/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ai.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "core/file_utilities.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_extensions.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QFileInfo>
#include <QtGui/QPainter>

#include <algorithm>
#include <vector>

namespace Paradoks::Ai {
namespace {

constexpr auto kHeight = 540;
constexpr auto kSessionsWidth = 190;
constexpr auto kGap = 8;
constexpr auto kButtonsWidth = 90;
constexpr auto kHistoryLimit = 60;
constexpr auto kTopicsLimit = 60;
constexpr auto kWaitTimeout = crl::time(120000);
constexpr auto kRefreshDelay = crl::time(120);

struct Topic {
	MsgId id = 0;
	QString title;
	int top = 0;
};

struct State {
	Window::SessionController *controller = nullptr;
	Main::Session *session = nullptr;
	UserData *bot = nullptr;
	// Root of the shown topic, zero for an ordinary (not forum) bot.
	MsgId root = 0;
	// The topic is just reserved, the server creates it with the first message.
	bool creating = false;
	std::vector<MsgId> ids;
	std::vector<Topic> topics;
	QString attachment;
	bool waiting = false;
	int token = 0;
	base::Timer refreshTimer;
	base::Timer waitTimer;
	Fn<void()> refreshMessages;
	Fn<void()> refreshSessions;
	Fn<void()> reloadTopics;
	Fn<void()> loadMessages;
	Fn<void()> updateAttachment;
};

[[nodiscard]] QString BotUsername() {
	const auto value = StringSetting(u"ai/bot"_q);
	return value.isEmpty() ? u"ParadoksAiBot"_q : value;
}

[[nodiscard]] Data::Thread *CurrentThread(not_null<State*> state) {
	const auto history = state->session->data().history(state->bot->id);
	if (!state->root.bare) {
		return history;
	}
	if (const auto forum = state->bot->forum()) {
		if (const auto topic = forum->enforceTopicFor(state->root)) {
			return topic;
		}
	}
	return history;
}

[[nodiscard]] bool InThread(
		not_null<State*> state,
		not_null<HistoryItem*> item) {
	if (item->history()->peer->id != state->bot->id) {
		return false;
	}
	return !state->root.bare || item->inThread(state->root);
}

[[nodiscard]] bool Visible(not_null<HistoryItem*> item) {
	if (item->isService()) {
		return false;
	}
	return !item->originalText().text.isEmpty() || (item->media() != nullptr);
}

void AddId(not_null<State*> state, MsgId id) {
	auto &ids = state->ids;
	if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
		ids.push_back(id);
		std::sort(ids.begin(), ids.end());
	}
}

void ScheduleRefresh(not_null<State*> state) {
	state->refreshTimer.callOnce(kRefreshDelay);
}

void GotReply(not_null<State*> state) {
	state->waiting = false;
	state->waitTimer.cancel();
}

// Takes the messages of a getReplies / getHistory answer.
void ProcessMessages(
		not_null<State*> state,
		const MTPmessages_Messages &result) {
	auto &owner = state->session->data();
	const auto list = result.match([&](
			const MTPDmessages_messagesNotModified &) {
		return QVector<MTPMessage>();
	}, [&](const auto &data) {
		owner.processUsers(data.vusers());
		owner.processChats(data.vchats());
		return data.vmessages().v;
	});
	for (const auto &message : list) {
		const auto item = owner.addNewMessage(
			message,
			MessageFlags(),
			NewMessageType::Existing);
		if (item && InThread(state, item) && Visible(item)) {
			AddId(state, item->id);
		}
	}
}

void AddMessage(
		not_null<Ui::VerticalLayout*> list,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item,
		Fn<void()> close) {
	const auto out = item->out();
	const auto bubble = list->add(
		object_ptr<Ui::VerticalLayout>(list),
		style::margins(out ? 56 : kGap, 4, out ? kGap : 56, 4));
	bubble->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(bubble);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(out ? st::msgOutBg : st::msgInBg);
		p.drawRoundedRect(QRectF(bubble->rect()), 12., 12.);
	}, bubble->lifetime());

	const auto inner = style::margins(12, 8, 12, 8);
	const auto &text = item->originalText();
	if (!text.text.isEmpty()) {
		const auto label = bubble->add(
			object_ptr<Ui::FlatLabel>(
				bubble,
				rpl::single(text),
				st::boxLabel),
			inner);
		label->setSelectable(true);
	}
	if (const auto media = item->media()) {
		auto title = QString();
		if (media->photo()) {
			title = u"Rasm"_q;
		} else if (const auto document = media->document()) {
			title = document->filename().isEmpty()
				? u"Fayl"_q
				: document->filename();
		} else {
			title = u"Media"_q;
		}
		const auto link = bubble->add(
			object_ptr<Ui::LinkButton>(
				bubble,
				title + u" (chatda ochish)"_q),
			inner);
		const auto peerId = item->history()->peer->id;
		const auto msgId = item->id;
		link->setClickedCallback([=] {
			controller->showPeerHistory(
				peerId,
				Window::SectionShow::Way::Forward,
				msgId);
			close();
		});
	}
}

void AiBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> bot) {
	box->setTitle(rpl::single(u"Paradoks AI"_q));
	box->setWidth(st::boxWideWidth);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto session = &controller->session();
	const auto state = box->lifetime().make_state<State>();
	state->controller = controller;
	state->session = session;
	state->bot = bot;

	const auto root = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		style::margins());
	root->resize(root->width(), kHeight);

	const auto sessionsScroll = Ui::CreateChild<Ui::ScrollArea>(root);
	Ui::VerticalLayout *sessionsList = sessionsScroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(sessionsScroll));
	const auto messagesScroll = Ui::CreateChild<Ui::ScrollArea>(root);
	Ui::VerticalLayout *messages = messagesScroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(messagesScroll));
	const auto input = Ui::CreateChild<Ui::InputField>(
		root,
		st::defaultInputField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(u"Savolingizni yozing..."_q));
	input->setMaxHeight(110);
	input->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
	const auto sendButton = Ui::CreateChild<Ui::LinkButton>(
		root,
		u"Yuborish"_q);
	const auto attachButton = Ui::CreateChild<Ui::LinkButton>(
		root,
		u"Fayl biriktirish"_q);
	const auto attachLink = Ui::CreateChild<Ui::LinkButton>(
		root,
		QString());
	attachLink->hide();

	const auto layout = [=] {
		const auto width = root->width();
		const auto height = root->height();
		const auto left = kSessionsWidth + kGap;
		const auto right = std::max(width - left, 100);
		const auto fieldWidth = std::max(right - kButtonsWidth - kGap, 50);
		input->resizeToWidth(fieldWidth);
		const auto inputTop = height - input->height();
		input->moveToLeft(left, inputTop);
		sendButton->moveToLeft(left + fieldWidth + kGap, inputTop);
		attachButton->moveToLeft(
			left + fieldWidth + kGap,
			inputTop + sendButton->height() + 4);
		auto bottom = inputTop - kGap;
		if (!state->attachment.isEmpty()) {
			attachLink->show();
			attachLink->moveToLeft(left, bottom - attachLink->height());
			bottom -= attachLink->height() + 4;
		} else {
			attachLink->hide();
		}
		messagesScroll->setGeometry(left, 0, right, std::max(bottom, 60));
		sessionsScroll->setGeometry(0, 0, kSessionsWidth, height);
		sessionsList->resizeToWidth(kSessionsWidth);
		messages->resizeToWidth(right);
	};
	rpl::combine(
		root->sizeValue(),
		input->heightValue()
	) | rpl::on_next([=](QSize, int) {
		layout();
	}, root->lifetime());

	// Always show the end of the conversation.
	messages->heightValue(
	) | rpl::on_next([=](int) {
		messagesScroll->scrollToY(messagesScroll->scrollTopMax());
	}, messages->lifetime());

	state->updateAttachment = [=] {
		attachLink->setText(state->attachment.isEmpty()
			? QString()
			: (u"Fayl: "_q
				+ QFileInfo(state->attachment).fileName()
				+ u" (olib tashlash)"_q));
		layout();
	};
	attachLink->setClickedCallback([=] {
		state->attachment.clear();
		state->updateAttachment();
	});
	attachButton->setClickedCallback([=] {
		FileDialog::GetOpenPath(
			root,
			u"Faylni tanlang"_q,
			FileDialog::AllFilesFilter(),
			crl::guard(root, [=](const FileDialog::OpenResult &result) {
				if (result.paths.isEmpty()) {
					return;
				}
				state->attachment = result.paths.front();
				state->updateAttachment();
			}));
	});

	const auto send = [=](QString text) {
		text = text.trimmed();
		const auto attachment = state->attachment;
		if (text.isEmpty() && attachment.isEmpty()) {
			return;
		}
		const auto thread = CurrentThread(state);
		if (!thread) {
			return;
		}
		if (!attachment.isEmpty()) {
			auto files = Storage::PrepareMediaList(
				QStringList(attachment),
				256,
				session->premium());
			if (files.error != Ui::PreparedList::Error::None
				|| files.files.empty()) {
				controller->showToast(u"Faylni yuborib bo'lmadi."_q);
				return;
			}
			files.files.front().caption = TextWithTags{ text };
			auto isImage = false;
			for (const auto &extension : Ui::ImageExtensions()) {
				if (attachment.endsWith(extension, Qt::CaseInsensitive)) {
					isImage = true;
					break;
				}
			}
			session->api().sendFiles(
				std::move(files),
				isImage ? SendMediaType::Photo : SendMediaType::File,
				nullptr,
				Api::SendAction(thread));
			state->attachment.clear();
			state->updateAttachment();
		} else {
			auto message = Api::MessageToSend(Api::SendAction(thread));
			message.textWithTags = TextWithTags{ text };
			session->api().sendMessage(std::move(message));
		}
		input->setText(QString());
		state->waiting = true;
		state->waitTimer.callOnce(kWaitTimeout);
		Debug::Log(u"ai window: message sent"_q);
		ScheduleRefresh(state);
	};
	sendButton->setClickedCallback([=] {
		send(input->getLastText());
	});
	input->submits(
	) | rpl::to_empty | rpl::on_next([=] {
		send(input->getLastText());
	}, input->lifetime());

	state->refreshTimer.setCallback([=] {
		state->refreshMessages();
	});
	state->waitTimer.setCallback([=] {
		state->waiting = false;
		state->refreshMessages();
	});

	state->refreshMessages = [=] {
		messages->clear();
		auto shown = 0;
		for (const auto id : state->ids) {
			const auto item = session->data().message(
				FullMsgId(bot->id, id));
			if (!item || !Visible(item)) {
				continue;
			}
			++shown;
			AddMessage(messages, controller, item, [=] {
				box->closeBox();
			});
		}
		if (!shown && !state->waiting) {
			messages->add(
				object_ptr<Ui::FlatLabel>(
					messages,
					u"Salom! Men Paradoks AI man. Savolingizni yozing "
					"yoki quyidagilardan birini tanlang:"_q,
					st::boxLabel),
				style::margins(kGap, 12, kGap, 4));
			const auto suggest = [&](const QString &text) {
				const auto link = messages->add(
					object_ptr<Ui::LinkButton>(messages, text),
					style::margins(kGap, 2, kGap, 2));
				link->setClickedCallback([=] {
					send(text);
				});
			};
			suggest(u"Bugun nimalar qila olasan?"_q);
			suggest(u"Menga qisqa reja tuzib ber"_q);
			suggest(u"Matnni o'zbekchaga tarjima qil"_q);
			suggest(u"Kod yozishda yordam ber"_q);
		}
		if (state->waiting) {
			messages->add(
				object_ptr<Ui::FlatLabel>(
					messages,
					u"Paradoks AI yozmoqda..."_q,
					st::boxLabel),
				style::margins(kGap + 6, 6, kGap, 6));
		}
		messages->resizeToWidth(messagesScroll->width());
	};

	state->refreshSessions = [=] {
		sessionsList->clear();
		const auto newChat = sessionsList->add(
			object_ptr<Ui::LinkButton>(sessionsList, u"+ Yangi suhbat"_q),
			style::margins(kGap, 6, kGap, 4));
		newChat->setClickedCallback([=] {
			const auto forum = bot->forum();
			if (!forum) {
				controller->showToast(
					u"Bu bot alohida suhbatlarni qo'llamaydi."_q);
				return;
			}
			const auto topic = forum->reserveNewBotTopic();
			if (!topic) {
				return;
			}
			state->root = topic->rootId();
			state->creating = true;
			state->ids.clear();
			state->waiting = false;
			++state->token;
			state->refreshMessages();
			state->refreshSessions();
			input->setFocus();
		});
		const auto open = sessionsList->add(
			object_ptr<Ui::LinkButton>(sessionsList, u"Bot chatini ochish"_q),
			style::margins(kGap, 2, kGap, 8));
		open->setClickedCallback([=] {
			controller->showPeerHistory(
				bot->id,
				Window::SectionShow::Way::Forward);
			box->closeBox();
		});
		if (state->creating) {
			sessionsList->add(
				object_ptr<Ui::FlatLabel>(
					sessionsList,
					u"> Yangi suhbat"_q,
					st::boxLabel),
				style::margins(kGap, 4, kGap, 4));
		}
		for (const auto &topic : state->topics) {
			const auto selected = !state->creating
				&& (topic.id == state->root);
			const auto row = sessionsList->add(
				object_ptr<Ui::SettingsButton>(
					sessionsList,
					rpl::single((selected ? u"> "_q : QString())
						+ topic.title),
					st::settingsButtonNoIcon));
			const auto id = topic.id;
			row->setClickedCallback([=] {
				if (!state->creating && state->root == id) {
					return;
				}
				state->root = id;
				state->creating = false;
				state->ids.clear();
				state->waiting = false;
				state->refreshSessions();
				state->loadMessages();
			});
		}
		sessionsList->resizeToWidth(kSessionsWidth);
	};

	state->loadMessages = [=] {
		const auto token = ++state->token;
		if (state->creating) {
			state->ids.clear();
			state->refreshMessages();
			return;
		}
		const auto done = [=](const MTPmessages_Messages &result) {
			if (token != state->token) {
				return;
			}
			ProcessMessages(state, result);
			state->refreshMessages();
		};
		const auto fail = [=] {
			if (token == state->token) {
				state->refreshMessages();
			}
		};
		if (state->root.bare) {
			session->api().request(MTPmessages_GetReplies(
				bot->input(),
				MTP_int(state->root.bare),
				MTP_int(0), // offset_id
				MTP_int(0), // offset_date
				MTP_int(0), // add_offset
				MTP_int(kHistoryLimit),
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0) // hash
			)).done(done).fail(fail).send();
		} else {
			session->api().request(MTPmessages_GetHistory(
				bot->input(),
				MTP_int(0), // offset_id
				MTP_int(0), // offset_date
				MTP_int(0), // add_offset
				MTP_int(kHistoryLimit),
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0) // hash
			)).done(done).fail(fail).send();
		}
	};

	state->reloadTopics = [=] {
		if (!bot->isForum()) {
			state->refreshSessions();
			return;
		}
		session->api().request(MTPmessages_GetForumTopics(
			MTP_flags(0),
			bot->input(),
			MTPstring(), // q
			MTP_int(0), // offset_date
			MTP_int(0), // offset_id
			MTP_int(0), // offset_topic
			MTP_int(kTopicsLimit)
		)).done([=](const MTPmessages_ForumTopics &result) {
			const auto &data = result.data();
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			auto list = std::vector<Topic>();
			for (const auto &topic : data.vtopics().v) {
				topic.match([&](const MTPDforumTopic &fields) {
					list.push_back({
						.id = MsgId(fields.vid().v),
						.title = fields.vtitle().v,
						.top = fields.vtop_message().v,
					});
				}, [](const auto &) {
				});
			}
			std::sort(list.begin(), list.end(), [](
					const Topic &a,
					const Topic &b) {
				return a.top > b.top;
			});
			state->topics = std::move(list);
			if (!state->root.bare
				&& !state->creating
				&& !state->topics.empty()) {
				state->root = state->topics.front().id;
				state->loadMessages();
			}
			state->refreshSessions();
		}).fail([=] {
			state->refreshSessions();
		}).send();
	};

	// Live updates.
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (item->history()->peer->id != bot->id) {
			return;
		}
		if (state->creating) {
			const auto forum = bot->forum();
			const auto top = item->topicRootId();
			if (forum
				&& top.bare
				&& top != state->root
				&& !forum->creating(top)) {
				// The topic was created by the server.
				state->root = top;
				state->creating = false;
				state->ids.clear();
				state->reloadTopics();
				state->loadMessages();
				return;
			}
		}
		if (!InThread(state, item) || !Visible(item)) {
			return;
		}
		AddId(state, item->id);
		if (!item->out()) {
			GotReply(state);
		}
		ScheduleRefresh(state);
	}, box->lifetime());

	session->data().itemIdChanged(
	) | rpl::on_next([=](Data::Session::IdChange change) {
		if (change.newId.peer != bot->id) {
			return;
		}
		auto &ids = state->ids;
		const auto i = std::find(ids.begin(), ids.end(), change.oldId);
		if (i != ids.end()) {
			*i = change.newId.msg;
			std::sort(ids.begin(), ids.end());
			ScheduleRefresh(state);
		}
	}, box->lifetime());

	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		const auto item = update.item;
		if (item->history()->peer->id != bot->id || !InThread(state, item)) {
			return;
		}
		if (!Visible(item)) {
			return;
		}
		AddId(state, item->id);
		if (!item->out()) {
			GotReply(state);
		}
		ScheduleRefresh(state);
	}, box->lifetime());

	layout();
	state->refreshMessages();
	state->refreshSessions();
	if (bot->isForum()) {
		if (const auto forum = bot->forum()) {
			forum->requestTopics();
		}
		state->reloadTopics();
	} else {
		state->loadMessages();
	}
	input->setFocusFast();
}

} // namespace

void Open(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	const auto name = BotUsername();
	const auto show = [=](not_null<PeerData*> peer) {
		const auto bot = peer->asUser();
		if (!bot || !bot->isBot()) {
			controller->showToast(u"Paradoks AI boti topilmadi."_q);
			return;
		}
		controller->show(Box(AiBox, controller, not_null<UserData*>(bot)));
	};
	if (const auto peer = session->data().peerByUsername(name)) {
		show(peer);
		return;
	}
	controller->showToast(u"Paradoks AI ulanmoqda..."_q);
	const auto weak = base::make_weak(controller);
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(name),
		MTPstring()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(data.vpeer()));
		if (!weak.get()) {
			return;
		}
		if (!peer) {
			weak.get()->showToast(u"Paradoks AI boti topilmadi."_q);
			return;
		}
		show(peer);
	}).fail([=] {
		if (const auto strong = weak.get()) {
			strong->showToast(u"Paradoks AI'ga ulanib bo'lmadi."_q);
		}
	}).send();
}

} // namespace Paradoks::Ai
