/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_clone.h"

#include "api/api_updates.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

namespace Paradoks::Clone {
namespace {

constexpr auto kPageLimit = 100;
constexpr auto kForwardChunk = 100;
constexpr auto kChunkPause = crl::time(1500);
constexpr auto kHistoryPause = crl::time(120);

bool Running = false;

struct Item {
	int id = 0;
	uint64 grouped = 0;
};

struct Job {
	Job(not_null<Main::Session*> session, not_null<PeerData*> source)
	: session(session)
	, source(source) {
	}

	not_null<Main::Session*> session;
	not_null<PeerData*> source;
	QString sourceTitle;
	QString newTitle;
	std::vector<Item> items;
	ChannelData *target = nullptr;
	int forwarded = 0;
	int failed = 0;
	bool cancelled = false;
	rpl::variable<QString> text = QString();
	rpl::event_stream<> closeBox;
	Fn<void(const QString &)> finished;
};

using JobPtr = std::shared_ptr<Job>;

[[nodiscard]] QString CloneTitle(QString name) {
	const auto suffix = u"_clon"_q;
	if (name.size() + suffix.size() > 128) {
		name = name.left(128 - suffix.size());
	}
	return name + suffix;
}

[[nodiscard]] const QVector<MTPMessage> *MessagesOf(
		const MTPmessages_Messages &result,
		int &count) {
	switch (result.type()) {
	case mtpc_messages_messages: {
		const auto &data = result.c_messages_messages();
		count = data.vmessages().v.size();
		return &data.vmessages().v;
	}
	case mtpc_messages_messagesSlice: {
		const auto &data = result.c_messages_messagesSlice();
		count = data.vcount().v;
		return &data.vmessages().v;
	}
	case mtpc_messages_channelMessages: {
		const auto &data = result.c_messages_channelMessages();
		count = data.vcount().v;
		return &data.vmessages().v;
	}
	}
	return nullptr;
}

void Finish(const JobPtr &job, const QString &text) {
	Running = false;
	if (job->finished) {
		job->finished(text);
	}
	job->closeBox.fire({});
}

void Forward(const JobPtr &job, int index);

void Collect(const JobPtr &job, int offsetId, int seen) {
	if (job->cancelled) {
		Finish(job, u"Klon qilish to'xtatildi"_q);
		return;
	}
	job->session->api().request(MTPmessages_GetHistory(
		job->source->input(),
		MTP_int(offsetId),
		MTP_int(0),
		MTP_int(0),
		MTP_int(kPageLimit),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		auto count = 0;
		const auto list = MessagesOf(result, count);
		const auto createGroup = [=] {
			if (job->cancelled) {
				Finish(job, u"Klon qilish to'xtatildi"_q);
				return;
			}
			std::sort(
				job->items.begin(),
				job->items.end(),
				[](const Item &a, const Item &b) { return a.id < b.id; });
			if (job->items.empty()) {
				Finish(job, u"Forward qilinadigan xabar topilmadi"_q);
				return;
			}
			job->text = u"Yangi guruh yaratilmoqda..."_q;
			job->session->api().request(MTPchannels_CreateChannel(
				MTP_flags(MTPchannels_CreateChannel::Flag::f_megagroup),
				MTP_string(job->newTitle),
				MTP_string((u"Klon: "_q + job->sourceTitle).left(255)),
				MTPInputGeoPoint(),
				MTPstring(),
				MTP_int(0)
			)).done([=](const MTPUpdates &updates) {
				job->session->api().applyUpdates(updates);
				const auto chats = updates.match([](
						const MTPDupdates &data) {
					return data.vchats().v;
				}, [](const MTPDupdatesCombined &data) {
					return data.vchats().v;
				}, [](const auto &) {
					return QVector<MTPChat>();
				});
				for (const auto &chat : chats) {
					if (chat.type() == mtpc_channel) {
						job->target = job->session->data().channel(
							chat.c_channel().vid().v);
						break;
					}
				}
				if (!job->target) {
					Finish(job, u"Yangi guruh ma'lumoti olinmadi"_q);
					return;
				}
				Forward(job, 0);
			}).fail([=](const MTP::Error &error) {
				Finish(job, u"Guruh yaratilmadi: "_q + error.type());
			}).send();
		};
		if (!list || list->isEmpty()) {
			createGroup();
			return;
		}
		auto minId = std::numeric_limits<int>::max();
		for (const auto &message : *list) {
			message.match([&](const MTPDmessage &data) {
				const auto id = data.vid().v;
				minId = std::min(minId, id);
				if (!data.is_noforwards()) {
					job->items.push_back({
						.id = id,
						.grouped = uint64(
							data.vgrouped_id().value_or_empty()),
					});
				}
			}, [&](const MTPDmessageService &data) {
				minId = std::min(minId, data.vid().v);
			}, [&](const MTPDmessageEmpty &data) {
				minId = std::min(minId, data.vid().v);
			});
		}
		const auto total = seen + int(list->size());
		if (minId == std::numeric_limits<int>::max()
			|| (offsetId && minId >= offsetId)) {
			createGroup();
			return;
		}
		job->text = u"Xabarlar o'qilmoqda: "_q + QString::number(total)
			+ (count > total
				? (u" / "_q + QString::number(count))
				: QString());
		base::call_delayed(kHistoryPause, job->session, [=] {
			Collect(job, minId, total);
		});
	}).fail([=](const MTP::Error &error) {
		Finish(job, u"Xabarlarni o'qib bo'lmadi: "_q + error.type());
	}).send();
}

void Forward(const JobPtr &job, int index) {
	const auto total = int(job->items.size());
	if (job->cancelled) {
		Finish(job, u"Klon to'xtatildi: %1 / %2 forward qilindi"_q.arg(
			job->forwarded).arg(total));
		return;
	} else if (index >= total) {
		Finish(job, u"Klon tayyor: %1 ta xabar%2"_q.arg(
			job->forwarded).arg(job->failed
				? (u", %1 tasi o'tkazib yuborildi"_q.arg(job->failed))
				: QString()));
		return;
	}
	// Chunks of up to 100 messages, never splitting an album.
	auto end = std::min(index + kForwardChunk, total);
	while (end < total
		&& end - 1 > index
		&& job->items[end - 1].grouped != 0
		&& job->items[end - 1].grouped == job->items[end].grouped) {
		--end;
	}
	auto ids = QVector<MTPint>();
	auto randoms = QVector<MTPlong>();
	for (auto i = index; i != end; ++i) {
		ids.push_back(MTP_int(job->items[i].id));
		randoms.push_back(MTP_long(base::RandomValue<uint64>()));
	}
	const auto count = end - index;
	const auto progress = [=] {
		job->text = u"Forward qilinmoqda: %1 / %2"_q.arg(
			job->forwarded + job->failed).arg(total);
	};
	job->session->api().request(MTPmessages_ForwardMessages(
		MTP_flags(0),
		job->source->input(),
		MTP_vector<MTPint>(ids),
		MTP_vector<MTPlong>(randoms),
		job->target->input(),
		MTP_int(0),
		MTPInputReplyTo(),
		MTP_int(0),
		MTP_int(0),
		MTP_inputPeerEmpty(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTPint(),
		MTP_long(0),
		MTPSuggestedPost()
	)).done([=](const MTPUpdates &result) {
		job->session->api().applyUpdates(result);
		job->forwarded += count;
		progress();
		base::call_delayed(kChunkPause, job->session, [=] {
			Forward(job, end);
		});
	}).fail([=](const MTP::Error &error) {
		const auto type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			const auto seconds = std::max(type.mid(11).toInt(), 1);
			job->text = u"Telegram %1 soniya kutishni so'radi..."_q.arg(
				seconds);
			base::call_delayed(
				crl::time(seconds) * 1000 + 500,
				job->session,
				[=] { Forward(job, index); });
		} else if (type.contains(u"MESSAGE_ID_INVALID"_q)
			|| type.contains(u"MESSAGE_IDS_EMPTY"_q)) {
			job->failed += count;
			progress();
			base::call_delayed(kChunkPause, job->session, [=] {
				Forward(job, end);
			});
		} else {
			Finish(job, u"Forward xatosi: "_q + type);
		}
	}).send();
}

void ProgressBox(not_null<Ui::GenericBox*> box, JobPtr job) {
	box->setTitle(rpl::single(u"Chat klonlanmoqda"_q));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		job->text.value(),
		st::boxLabel));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(u"Katta chatlar uchun uzoq vaqt ketishi mumkin. "
			"Dasturni yopmang."_q),
		st::boxLabel));
	job->closeBox.events(
	) | rpl::on_next([=] {
		box->closeBox();
	}, box->lifetime());
	box->lifetime().add([=] {
		job->cancelled = true;
	});
	box->addButton(rpl::single(u"To'xtatish"_q), [=] {
		job->cancelled = true;
		job->text = u"To'xtatilmoqda..."_q;
	});
}

} // namespace

bool CanClone(not_null<PeerData*> peer) {
	if (const auto channel = peer->asChannel()) {
		return channel->isPublic()
			&& !channel->isMonoforum()
			&& peer->allowsForwarding();
	} else if (const auto user = peer->asUser()) {
		return !user->isInaccessible() && peer->allowsForwarding();
	}
	return false;
}

void Start(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	if (Running) {
		controller->showToast(u"Klon qilish allaqachon davom etmoqda"_q);
		return;
	} else if (!CanClone(peer)) {
		controller->showToast(
			u"Bu chatni klon qilib bo'lmaydi (yopiq yoki saqlash cheklangan)"_q);
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto begin = [=] {
		const auto strong = weak.get();
		if (!strong || Running) {
			return;
		}
		Running = true;
		const auto job = std::make_shared<Job>(
			&peer->session(),
			peer);
		job->sourceTitle = peer->name();
		job->newTitle = CloneTitle(job->sourceTitle);
		job->text = u"Boshlanmoqda..."_q;
		job->finished = [=](const QString &result) {
			if (const auto again = weak.get()) {
				again->showToast(result);
				if (job->target && job->forwarded > 0) {
					again->showPeerHistory(job->target);
				}
			}
		};
		strong->show(Box(ProgressBox, job));
		Collect(job, 0, 0);
	};
	controller->show(Ui::MakeConfirmBox({
		.text = u"\"%1\" chatidagi barcha xabarlar (media va fayllar bilan) "
			"yangi yopiq guruhga forward qilinadi. Guruh nomi: %2.\n\n"
			"Katta chatlar uchun uzoq vaqt ketishi mumkin."_q.arg(
				peer->name(),
				CloneTitle(peer->name())),
		.confirmed = [=](Fn<void()> &&close) {
			close();
			begin();
		},
		.confirmText = rpl::single(u"Boshlash"_q),
	}));
}

} // namespace Paradoks::Clone
