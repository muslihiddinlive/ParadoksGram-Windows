/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_broadcast.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/weak_ptr.h"
#include "boxes/share_box.h"
#include "core/file_utilities.h"
#include "data/data_chat_filters.h"
#include "data/data_chat_participant_status.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "rpl/variable.h"
#include "settings.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_extensions.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <memory>
#include <vector>

namespace Paradoks::Broadcast {
namespace {

constexpr auto kMaxHistory = 20;
constexpr auto kCaptionLimit = 1024;
constexpr auto kPause = crl::time(700);

struct Recipient {
	uint64 peer = 0;
	QString title;
};

struct Draft {
	QString text;
	QString media;
	std::vector<Recipient> recipients;

	[[nodiscard]] bool hasMedia() const {
		return !media.isEmpty() && QFileInfo::exists(media);
	}
	[[nodiscard]] bool contains(uint64 peer) const {
		return std::any_of(
			recipients.begin(),
			recipients.end(),
			[&](const Recipient &r) { return r.peer == peer; });
	}
};

struct Entry {
	qint64 time = 0;
	int sent = 0;
	Draft draft;
};

[[nodiscard]] QString StorePath() {
	return cWorkingDir() + u"tdata/paradoks_broadcast.json"_q;
}

[[nodiscard]] QString MediaDir() {
	const auto path = cWorkingDir() + u"tdata/paradoks_broadcast/"_q;
	QDir().mkpath(path);
	return path;
}

[[nodiscard]] std::vector<Entry> LoadHistory() {
	auto result = std::vector<Entry>();
	auto file = QFile(StorePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto array = QJsonDocument::fromJson(file.readAll()).array();
	for (const auto &value : array) {
		const auto object = value.toObject();
		auto entry = Entry();
		entry.time = qint64(object.value(u"time"_q).toDouble());
		entry.sent = object.value(u"sent"_q).toInt();
		entry.draft.text = object.value(u"text"_q).toString();
		entry.draft.media = object.value(u"media"_q).toString();
		for (const auto &item : object.value(u"recipients"_q).toArray()) {
			const auto r = item.toObject();
			const auto peer = r.value(u"id"_q).toString().toULongLong();
			if (peer) {
				entry.draft.recipients.push_back({
					.peer = peer,
					.title = r.value(u"title"_q).toString(),
				});
			}
		}
		result.push_back(std::move(entry));
	}
	return result;
}

void SaveHistory(const std::vector<Entry> &list) {
	auto array = QJsonArray();
	for (const auto &entry : list) {
		auto object = QJsonObject();
		object.insert(u"time"_q, double(entry.time));
		object.insert(u"sent"_q, entry.sent);
		object.insert(u"text"_q, entry.draft.text);
		if (!entry.draft.media.isEmpty()) {
			object.insert(u"media"_q, entry.draft.media);
		}
		auto recipients = QJsonArray();
		for (const auto &r : entry.draft.recipients) {
			auto item = QJsonObject();
			item.insert(u"id"_q, QString::number(r.peer));
			item.insert(u"title"_q, r.title);
			recipients.push_back(item);
		}
		object.insert(u"recipients"_q, recipients);
		array.push_back(object);
	}
	auto file = QFile(StorePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
	}
}

[[nodiscard]] QString Signature(const Draft &draft) {
	auto ids = std::vector<uint64>();
	for (const auto &r : draft.recipients) {
		ids.push_back(r.peer);
	}
	std::sort(ids.begin(), ids.end());
	auto result = draft.text + u"|"_q + draft.media + u"|"_q;
	for (const auto id : ids) {
		result += QString::number(id) + u","_q;
	}
	return result;
}

void DeleteMediaIfUnused(
		const QString &path,
		const std::vector<Entry> &history) {
	if (path.isEmpty()) {
		return;
	}
	for (const auto &entry : history) {
		if (entry.draft.media == path) {
			return;
		}
	}
	// Only ever touch files inside our own folder.
	if (QFileInfo(path).absoluteDir() == QDir(MediaDir())) {
		QFile::remove(path);
	}
}

void Remember(const Draft &draft, int sent) {
	auto list = LoadHistory();
	const auto signature = Signature(draft);
	list.erase(
		std::remove_if(list.begin(), list.end(), [&](const Entry &e) {
			return Signature(e.draft) == signature;
		}),
		list.end());
	list.insert(list.begin(), Entry{
		.time = QDateTime::currentSecsSinceEpoch(),
		.sent = sent,
		.draft = draft,
	});
	auto dropped = std::vector<QString>();
	while (list.size() > kMaxHistory) {
		dropped.push_back(list.back().draft.media);
		list.pop_back();
	}
	SaveHistory(list);
	for (const auto &path : dropped) {
		DeleteMediaIfUnused(path, list);
	}
}

void DeleteEntry(int index) {
	auto list = LoadHistory();
	if (index < 0 || index >= int(list.size())) {
		return;
	}
	const auto media = list[index].draft.media;
	list.erase(list.begin() + index);
	SaveHistory(list);
	DeleteMediaIfUnused(media, list);
}

void ClearHistory() {
	const auto list = LoadHistory();
	SaveHistory({});
	for (const auto &entry : list) {
		DeleteMediaIfUnused(entry.draft.media, {});
	}
}

// Copies the chosen file into our own folder so the history keeps it.
[[nodiscard]] QString CopyMedia(const QString &source) {
	const auto info = QFileInfo(source);
	if (!info.exists()) {
		return QString();
	}
	const auto target = MediaDir()
		+ QString::number(QDateTime::currentMSecsSinceEpoch())
		+ u"_"_q
		+ info.fileName();
	return QFile::copy(source, target) ? target : QString();
}

[[nodiscard]] QString TitleOf(not_null<PeerData*> peer) {
	return peer->isSelf() ? u"Saqlangan xabarlar"_q : peer->name();
}

[[nodiscard]] bool Sendable(not_null<PeerData*> peer) {
	return Data::CanSendAnything(peer);
}

// Adds a peer if it can receive messages. Returns false if not added.
bool AddPeer(Draft &draft, not_null<PeerData*> peer) {
	if (draft.contains(peer->id.value) || !Sendable(peer)) {
		return false;
	}
	draft.recipients.push_back({
		.peer = peer->id.value,
		.title = TitleOf(peer),
	});
	return true;
}

[[nodiscard]] bool IsImage(const QString &path) {
	for (const auto &extension : Ui::ImageExtensions()) {
		if (path.endsWith(extension, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

struct SendJob {
	SendJob(not_null<Main::Session*> session, Draft draft)
	: session(session)
	, draft(std::move(draft)) {
	}

	not_null<Main::Session*> session;
	Draft draft;
	int index = 0;
	int sent = 0;
	int skipped = 0;
	bool cancelled = false;
	rpl::variable<QString> text = QString();
	rpl::event_stream<> closeBox;
	Fn<void(const QString &)> finished;
};

using JobPtr = std::shared_ptr<SendJob>;

void SendOne(
		not_null<Main::Session*> session,
		const Draft &draft,
		not_null<Data::Thread*> thread) {
	const auto text = draft.text.trimmed();
	const auto asCaption = draft.hasMedia()
		&& !text.isEmpty()
		&& text.size() <= kCaptionLimit;
	if (!text.isEmpty() && !asCaption) {
		auto message = Api::MessageToSend(Api::SendAction(thread));
		message.textWithTags = TextWithTags{ text };
		session->api().sendMessage(std::move(message));
	}
	if (draft.hasMedia()) {
		auto files = Storage::PrepareMediaList(
			QStringList(draft.media),
			256,
			session->premium());
		if (files.error != Ui::PreparedList::Error::None
			|| files.files.empty()) {
			return;
		}
		if (asCaption) {
			files.files.front().caption = TextWithTags{ text };
		}
		session->api().sendFiles(
			std::move(files),
			IsImage(draft.media)
				? SendMediaType::Photo
				: SendMediaType::File,
			nullptr,
			Api::SendAction(thread));
	}
}

void Step(const JobPtr &job) {
	const auto total = int(job->draft.recipients.size());
	if (job->cancelled || job->index >= total) {
		if (job->sent > 0) {
			Remember(job->draft, job->sent);
		}
		const auto text = job->cancelled
			? u"To'xtatildi: %1 ta chatga yuborildi"_q.arg(job->sent)
			: u"Yuborildi: %1 ta chat%2"_q.arg(job->sent).arg(
				job->skipped
					? (u", %1 tasi o'tkazib yuborildi"_q.arg(job->skipped))
					: QString());
		if (job->finished) {
			job->finished(text);
		}
		job->closeBox.fire({});
		return;
	}
	const auto &recipient = job->draft.recipients[job->index++];
	job->text = u"Yuborilmoqda: %1 / %2\n%3"_q.arg(
		job->index).arg(total).arg(recipient.title);
	const auto peer = job->session->data().peerLoaded(PeerId(recipient.peer));
	if (!peer || !Sendable(peer)) {
		++job->skipped;
		Step(job);
		return;
	}
	SendOne(
		job->session,
		job->draft,
		job->session->data().history(peer));
	++job->sent;
	base::call_delayed(kPause, job->session, [=] { Step(job); });
}

void ProgressBox(not_null<Ui::GenericBox*> box, JobPtr job) {
	box->setTitle(rpl::single(u"Broadcast yuborilmoqda"_q));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		job->text.value(),
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
	});
}

void StartSending(
		not_null<Window::SessionController*> controller,
		const Draft &draft) {
	const auto weak = base::make_weak(controller);
	const auto job = std::make_shared<SendJob>(
		&controller->session(),
		draft);
	job->finished = [=](const QString &text) {
		if (const auto strong = weak.get()) {
			strong->showToast(text);
		}
	};
	controller->show(Box(ProgressBox, job));
	Step(job);
}

void PickChats(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Draft> draft,
		Fn<void()> changed) {
	const auto holder = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
	const auto toast = base::make_weak(controller);
	auto box = Box<ShareBox>(ShareBox::Descriptor{
		.session = &controller->session(),
		.submitCallback = [=](
				std::vector<not_null<Data::Thread*>> &&threads,
				Fn<bool()>,
				TextWithTags &&,
				Api::SendOptions,
				Data::ForwardOptions) {
			auto added = 0;
			for (const auto &thread : threads) {
				if (AddPeer(*draft, thread->peer())) {
					++added;
				}
			}
			changed();
			if (const auto strong = holder->get()) {
				strong->closeBox();
			}
			if (const auto strong = toast.get()) {
				strong->showToast(u"Qo'shildi: %1 ta chat"_q.arg(added));
			}
		},
		.filterCallback = [](not_null<Data::Thread*> thread) {
			return Sendable(thread->peer());
		},
		.titleOverride = rpl::single(u"Qabul qiluvchilarni tanlang"_q),
	});
	*holder = base::make_weak(box.data());
	controller->show(std::move(box));
}

void FolderBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Draft> draft,
		Fn<void()> changed) {
	box->setTitle(rpl::single(u"Papkadan qo'shish"_q));
	const auto session = &controller->session();
	const auto &filters = session->data().chatsFilters().list();
	if (filters.empty()) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(u"Papkalar yo'q."_q),
			st::boxLabel));
	}
	for (const auto &filter : filters) {
		const auto copy = filter;
		const auto title = copy.title().text.text;
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(title.isEmpty() ? u"Papka"_q : title),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback([=] {
			auto added = 0;
			for (const auto &row : session->data().chatsList()->indexed()->all()) {
				const auto history = row->history();
				if (history
					&& copy.contains(history)
					&& AddPeer(*draft, history->peer)) {
					++added;
				}
			}
			changed();
			controller->showToast(u"Papkadan qo'shildi: %1 ta chat"_q.arg(added));
			box->closeBox();
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void HistoryBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Draft> draft,
		Fn<void()> changed) {
	box->setTitle(rpl::single(u"Broadcast tarixi"_q));
	const auto list = LoadHistory();
	if (list.empty()) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(u"Tarix bo'sh."_q),
			st::boxLabel));
	}
	for (auto i = 0; i != int(list.size()); ++i) {
		const auto &entry = list[i];
		auto preview = entry.draft.text.simplified().left(40);
		if (preview.isEmpty()) {
			preview = QFileInfo(entry.draft.media).fileName();
		}
		const auto label = u"%1  ·  %2 chat  ·  %3"_q.arg(
			QDateTime::fromSecsSinceEpoch(entry.time).toString(
				u"dd.MM HH:mm"_q)).arg(
			entry.sent).arg(preview);
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(label),
				st::settingsButtonNoIcon),
			style::margins());
		const auto saved = entry.draft;
		button->setClickedCallback([=] {
			*draft = saved;
			changed();
			controller->showToast(u"Tarixdan yuklandi"_q);
			box->closeBox();
		});
	}
	if (!list.empty()) {
		box->addLeftButton(rpl::single(u"Tarixni tozalash"_q), [=] {
			ClearHistory();
			controller->showToast(u"Tarix tozalandi"_q);
			box->closeBox();
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void MainBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Broadcast"_q));
	const auto container = box->verticalLayout();

	struct State {
		std::shared_ptr<Draft> draft = std::make_shared<Draft>();
		rpl::variable<QString> summary = QString();
		rpl::variable<QString> media = QString();
	};
	const auto state = box->lifetime().make_state<State>();
	const auto draft = state->draft;
	const auto refresh = [=] {
		state->summary = u"Qabul qiluvchilar: %1 ta chat"_q.arg(
			draft->recipients.size());
		state->media = draft->hasMedia()
			? (u"Fayl: "_q + QFileInfo(draft->media).fileName()
				+ u" (olib tashlash uchun bosing)"_q)
			: u"Fayl biriktirish"_q;
	};
	refresh();

	box->addRow(
		object_ptr<Ui::FlatLabel>(box, state->summary.value(), st::boxLabel),
		st::boxRowPadding);
	Ui::AddSkip(container);
	const auto input = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Xabar matni"_q)),
		st::boxRowPadding);
	Ui::AddSkip(container);

	const auto addButton = [&](rpl::producer<QString> text, Fn<void()> done) {
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				std::move(text),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback(std::move(done));
	};
	addButton(state->media.value(), [=] {
		if (!draft->media.isEmpty()) {
			DeleteMediaIfUnused(draft->media, LoadHistory());
			draft->media.clear();
			refresh();
			return;
		}
		FileDialog::GetOpenPath(
			QPointer<QWidget>(box.get()),
			u"Faylni tanlang"_q,
			QString(),
			crl::guard(box, [=](FileDialog::OpenResult &&result) {
				if (result.paths.isEmpty()) {
					return;
				}
				const auto copied = CopyMedia(result.paths.front());
				if (copied.isEmpty()) {
					controller->showToast(u"Faylni olib bo'lmadi"_q);
					return;
				}
				draft->media = copied;
				refresh();
			}));
	});
	addButton(rpl::single(u"Chatlarni tanlash"_q), [=] {
		PickChats(controller, draft, refresh);
	});
	addButton(rpl::single(u"Papkadan qo'shish"_q), [=] {
		controller->show(Box(FolderBox, controller, draft, refresh));
	});
	addButton(rpl::single(u"Tanlanganlarni tozalash"_q), [=] {
		draft->recipients.clear();
		refresh();
	});
	addButton(rpl::single(u"Tarix"_q), [=] {
		controller->show(Box(HistoryBox, controller, draft, [=] {
			refresh();
			input->setText(draft->text);
		}));
	});

	box->addButton(rpl::single(u"Yuborish"_q), [=] {
		draft->text = input->getLastText().trimmed();
		if (draft->recipients.empty()) {
			controller->showToast(u"Avval chatlarni tanlang"_q);
			return;
		} else if (draft->text.isEmpty() && !draft->hasMedia()) {
			controller->showToast(u"Xabar matni yoki fayl kerak"_q);
			return;
		}
		const auto copy = *draft;
		box->closeBox();
		StartSending(controller, copy);
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void Open(not_null<Window::SessionController*> controller) {
	controller->show(Box(MainBox, controller));
}

} // namespace Paradoks::Broadcast
