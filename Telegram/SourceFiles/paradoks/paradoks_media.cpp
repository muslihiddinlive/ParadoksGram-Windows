/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_media.h"

#include "paradoks/paradoks_ads.h"
#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "paradoks/paradoks_privacy.h"
#include "paradoks/paradoks_resume.h"
#include "paradoks/paradoks_stream.h"
#include "paradoks/paradoks_store.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_types.h"
#include "data/notify/data_notify_settings.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/widgets/popup_menu.h"
#include "window/notifications_manager.h"
#include "window/section_widget.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace Paradoks {
namespace {

constexpr auto kMaxMediaSize = int64(150) * 1024 * 1024;
constexpr auto kPreSaveMaxSize = int64(25) * 1024 * 1024;
constexpr auto kPreSaveKeepDays = 3;
constexpr auto kForwardInterval = crl::time(1500);
constexpr auto kMaxQueue = 100;
constexpr auto kMaxAwaiting = 50;
constexpr auto kMaxPendingPhotos = 50;
constexpr auto kNotifyWindow = crl::time(5000);
constexpr auto kNotifyPerWindow = 5;

using Key = std::pair<uint64, int64>;

[[nodiscard]] QString MediaFolder() {
	return cWorkingDir() + u"tdata/paradoks_media/"_q;
}

[[nodiscard]] QString PreFolder() {
	return MediaFolder() + u"pre/"_q;
}

[[nodiscard]] QString DbMapPath() {
	return cWorkingDir() + u"tdata/paradoks_dbmap.jsonl"_q;
}

[[nodiscard]] QString Prefix(uint64 peer, int64 id) {
	return QString::number(peer) + '_' + QString::number(id) + '_';
}

[[nodiscard]] QString SafeName(QString name) {
	if (name.isEmpty()) {
		return u"file"_q;
	}
	for (auto &ch : name) {
		if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?'
			|| ch == '"' || ch == '<' || ch == '>' || ch == '|') {
			ch = '_';
		}
	}
	return name;
}

[[nodiscard]] QString FindByPrefix(
		const QString &folder,
		const QString &prefix) {
	const auto list = QDir(folder).entryList(
		QStringList{ prefix + '*' },
		QDir::Files);
	return list.isEmpty() ? QString() : (folder + list.front());
}

[[nodiscard]] QDateTime FileTime(const QFileInfo &info) {
	const auto born = info.birthTime();
	return born.isValid() ? born : info.lastModified();
}

// Incoming media that can be backed up: photos and files, no stickers,
// no channels, no view-once media, no chats with saving restricted.
[[nodiscard]] bool BackupEligible(not_null<HistoryItem*> item) {
	if (item->out()
		|| !item->isRegular()
		|| item->isService()
		|| item->isPost()
		|| item->isScheduled()
		|| item->forbidsSaving()) {
		return false;
	}
	const auto media = item->media();
	if (!media || media->ttlSeconds() > 0) {
		return false;
	}
	if (const auto document = media->document()) {
		return !document->sticker();
	}
	return media->photo() != nullptr;
}

// Download of the original photo into the pre-save folder.
struct PendingPhoto {
	std::shared_ptr<Data::PhotoMedia> media;
	QString path;
};
std::deque<PendingPhoto> PendingPhotos;

void FlushPendingPhotos() {
	for (auto i = PendingPhotos.begin(); i != PendingPhotos.end();) {
		if (i->media->loaded()) {
			i->media->saveToFile(i->path);
			i = PendingPhotos.erase(i);
		} else {
			++i;
		}
	}
}

void PreSave(not_null<HistoryItem*> item) {
	if (!Enabled(Flag::KeepDeletedMedia)
		|| !Enabled(Flag::PreSaveMedia)
		|| !BackupEligible(item)) {
		return;
	}
	const auto media = item->media();
	const auto peer = item->history()->peer->id.value;
	const auto id = item->id.bare;
	QDir().mkpath(PreFolder());
	if (const auto document = media->document()) {
		if (document->size <= 0 || document->size > kPreSaveMaxSize) {
			return;
		}
		Debug::Log(u"presave document %1_%2 size=%3"_q.arg(peer).arg(id).arg(document->size));
		const auto path = PreFolder()
			+ Prefix(peer, id)
			+ SafeName(document->filename());
		if (!QFileInfo::exists(path)) {
			document->save(item->fullId(), path);
		}
	} else if (const auto photo = media->photo()) {
		const auto path = PreFolder() + Prefix(peer, id) + u"photo.jpg"_q;
		if (QFileInfo::exists(path)) {
			return;
		}
		auto view = photo->createMediaView();
		view->wanted(Data::PhotoSize::Large, item->fullId());
		Debug::Log(u"presave photo %1_%2"_q.arg(peer).arg(id));
		PendingPhotos.push_back({ std::move(view), path });
		while (PendingPhotos.size() > kMaxPendingPhotos) {
			PendingPhotos.pop_front();
		}
	}
}

// Deleted media database: forwards of incoming media.

struct Awaiting {
	base::weak_ptr<Main::Session> session;
	TimeId date = 0;
	Key source;
};

struct QueuedForward {
	base::weak_ptr<Main::Session> session;
	FullMsgId id;
	TimeId date = 0;
};

std::deque<QueuedForward> ForwardQueue;
std::deque<Awaiting> AwaitingEcho;
std::map<FullMsgId, Key> LocalEchoes;
std::set<std::pair<uint64, FullMsgId>> ForwardSeen;
std::map<uint64, PeerId> TargetCache;

[[nodiscard]] QString TargetKey(not_null<Main::Session*> session) {
	return u"db/target_"_q + QString::number(session->userId().bare);
}

[[nodiscard]] std::map<Key, Key> &DbMap() {
	static auto result = [] {
		auto map = std::map<Key, Key>();
		auto file = QFile(DbMapPath());
		if (!file.open(QIODevice::ReadOnly)) {
			return map;
		}
		while (!file.atEnd()) {
			const auto document = QJsonDocument::fromJson(file.readLine());
			if (!document.isObject()) {
				continue;
			}
			const auto object = document.object();
			map[Key(
				object.value(u"p"_q).toString().toULongLong(),
				object.value(u"i"_q).toString().toLongLong())] = Key(
				object.value(u"q"_q).toString().toULongLong(),
				object.value(u"r"_q).toString().toLongLong());
		}
		return map;
	}();
	return result;
}

void RememberCopy(Key source, Key copy) {
	DbMap()[source] = copy;
	auto file = QFile(DbMapPath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		return;
	}
	auto object = QJsonObject();
	object.insert(u"p"_q, QString::number(source.first));
	object.insert(u"i"_q, QString::number(source.second));
	object.insert(u"q"_q, QString::number(copy.first));
	object.insert(u"r"_q, QString::number(copy.second));
	file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	file.write("\n");
}

[[nodiscard]] base::Timer &ForwardTimer() {
	static auto timer = base::Timer();
	return timer;
}

void DrainForwards() {
	while (!ForwardQueue.empty()) {
		const auto next = std::move(ForwardQueue.front());
		ForwardQueue.pop_front();
		const auto session = next.session.get();
		if (!session) {
			continue;
		}
		const auto target = DbTarget(session);
		const auto item = session->data().message(next.id);
		const auto peer = target
			? session->data().peerLoaded(target)
			: nullptr;
		if (!peer || !item || item->forbidsForward()) {
			continue;
		}
		AwaitingEcho.push_back({
			.session = next.session,
			.date = next.date,
			.source = Key(item->history()->peer->id.value, item->id.bare),
		});
		while (AwaitingEcho.size() > kMaxAwaiting) {
			AwaitingEcho.pop_front();
		}
		session->api().forwardMessages(
			Data::ResolvedForwardDraft{
				.items = HistoryItemsList{ not_null(item) },
			},
			Api::SendAction(
				session->data().history(peer),
				Api::SendOptions{ .silent = true }));
		break;
	}
	if (!ForwardQueue.empty()) {
		ForwardTimer().callOnce(kForwardInterval);
	}
}

void EnqueueForward(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	if (!ForwardSeen.emplace(session->userId().bare, item->fullId()).second) {
		return;
	}
	if (ForwardSeen.size() > 500) {
		ForwardSeen.erase(ForwardSeen.begin());
	}
	ForwardQueue.push_back({
		.session = base::make_weak(session.get()),
		.id = item->fullId(),
		.date = item->date(),
	});
	while (ForwardQueue.size() > kMaxQueue) {
		ForwardQueue.pop_front();
	}
	if (!ForwardTimer().isActive()) {
		ForwardTimer().setCallback([] { DrainForwards(); });
		ForwardTimer().callOnce(500);
	}
}

// Our own forward shows up in the database chat as a local message first:
// pair it with its source, learn the real id when the server confirms it.
void MatchEcho(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	if (!item->out() || !item->isSending()) {
		return;
	}
	const auto forwarded = item->Get<HistoryMessageForwarded>();
	if (!forwarded) {
		return;
	}
	for (auto i = AwaitingEcho.begin(); i != AwaitingEcho.end(); ++i) {
		if (i->session.get() == session.get() && i->date == forwarded->originalDate) {
			LocalEchoes[item->fullId()] = i->source;
			AwaitingEcho.erase(i);
			if (LocalEchoes.size() > 200) {
				LocalEchoes.erase(LocalEchoes.begin());
			}
			return;
		}
	}
}

void OnIdChanged(Data::Session::IdChange change) {
	if (LocalEchoes.empty()) {
		return;
	}
	const auto old = FullMsgId(change.newId.peer, change.oldId);
	const auto i = LocalEchoes.find(old);
	if (i == LocalEchoes.end()) {
		return;
	}
	RememberCopy(i->second, Key(change.newId.peer.value, change.newId.msg.bare));
	LocalEchoes.erase(i);
}

[[nodiscard]] bool ForwardEligible(
		not_null<HistoryItem*> item,
		PeerId target) {
	return BackupEligible(item)
		&& !item->forbidsForward()
		&& (item->history()->peer->id != target);
}

void OnNewItem(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	const auto target = DbTarget(session);
	if (target && item->history()->peer->id == target) {
		MatchEcho(session, item);
		return;
	}
	PreSave(item);
	if (target
		&& Enabled(Flag::KeepDeletedMedia)
		&& ForwardEligible(item, target)) {
		EnqueueForward(session, item);
	}
}

} // namespace

void InstallForSession(not_null<Main::Session*> session) {
	Debug::Log(u"install session %1 (keepMedia=%2 preSave=%3 notify=%4 target=%5)"_q.arg(session->userId().bare).arg(Enabled(Flag::KeepDeletedMedia)).arg(Enabled(Flag::PreSaveMedia)).arg(Enabled(Flag::NotifyDeleted)).arg(DbTarget(session).value));
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		OnNewItem(session, item);
	}, session->lifetime());

	session->data().itemIdChanged(
	) | rpl::on_next([=](Data::Session::IdChange change) {
		OnIdChanged(change);
	}, session->lifetime());

	session->downloaderTaskFinished(
	) | rpl::on_next([] {
		if (!PendingPhotos.empty()) {
			FlushPendingPhotos();
		}
	}, session->lifetime());

	Ads::Refresh();
	Resume::StartForSession(session);

	static auto cleaned = false;
	if (!std::exchange(cleaned, true)) {
		crl::on_main([] { CleanupMedia(); });
	}
}

void KeepMediaFor(not_null<HistoryItem*> item) {
	if (!Enabled(Flag::KeepDeletedMedia)) {
		return;
	}
	const auto media = item->media();
	if (!media) {
		return;
	}
	const auto peer = item->history()->peer->id.value;
	const auto id = item->id.bare;
	const auto prefix = Prefix(peer, id);
	QDir().mkpath(MediaFolder());
	if (!FindByPrefix(MediaFolder(), prefix).isEmpty()) {
		return;
	}
	const auto pre = FindByPrefix(PreFolder(), prefix);
	if (!pre.isEmpty()) {
		const auto copied = QFile::copy(
			pre,
			MediaFolder() + QFileInfo(pre).fileName());
		Debug::Log(u"keep media %1_%2: from pre-save copy ok=%3"_q.arg(peer).arg(id).arg(copied));
		return;
	}
	Debug::Log(u"keep media %1_%2: no pre-save copy, document=%3 photo=%4"_q.arg(peer).arg(id).arg(media->document() != nullptr).arg(media->photo() != nullptr));
	if (const auto document = media->document()) {
		const auto path = document->filepath(true);
		if (path.isEmpty() || QFileInfo(path).size() > kMaxMediaSize) {
			return;
		}
		QFile::copy(
			path,
			MediaFolder() + prefix + SafeName(QFileInfo(path).fileName()));
	} else if (const auto photo = media->photo()) {
		const auto view = photo->activeMediaView();
		if (view && view->loaded()) {
			view->saveToFile(MediaFolder() + prefix + u"photo.jpg"_q);
		}
	}
}

QString SavedMediaPath(not_null<const HistoryItem*> item) {
	return SavedMediaPath(item->history()->peer->id.value, item->id.bare);
}

QString SavedMediaPath(uint64 peer, int64 id) {
	return FindByPrefix(MediaFolder(), Prefix(peer, id));
}

std::optional<std::pair<uint64, int64>> DbCopyFor(uint64 peer, int64 id) {
	const auto &map = DbMap();
	const auto i = map.find(Key(peer, id));
	return (i != map.end())
		? std::make_optional(i->second)
		: std::nullopt;
}

void RetryMedia(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media || !SavedMediaPath(item).isEmpty()) {
		return;
	}
	KeepMediaFor(item);
	if (!SavedMediaPath(item).isEmpty()) {
		return;
	}
	// Not downloaded yet: load it straight into the saved folder.
	const auto prefix = Prefix(item->history()->peer->id.value, item->id.bare);
	QDir().mkpath(MediaFolder());
	if (const auto document = media->document()) {
		if (document->size > 0 && document->size <= kMaxMediaSize) {
			document->save(
				item->fullId(),
				MediaFolder() + prefix + SafeName(document->filename()));
		}
	} else if (const auto photo = media->photo()) {
		auto view = photo->createMediaView();
		view->wanted(Data::PhotoSize::Large, item->fullId());
		PendingPhotos.push_back({
			std::move(view),
			MediaFolder() + prefix + u"photo.jpg"_q,
		});
	}
}

int64 SavedMediaBytes() {
	auto total = int64(0);
	for (const auto &folder : { MediaFolder(), PreFolder() }) {
		const auto list = QDir(folder).entryInfoList(QDir::Files);
		for (const auto &info : list) {
			total += info.size();
		}
	}
	return total;
}

void CleanupMedia() {
	Stream::Cleanup();
	struct Entry {
		QString path;
		QDateTime time;
		int64 size = 0;
	};
	const auto collect = [](const QString &folder) {
		auto result = std::vector<Entry>();
		const auto list = QDir(folder).entryInfoList(QDir::Files);
		for (const auto &info : list) {
			result.push_back({
				.path = info.absoluteFilePath(),
				.time = FileTime(info),
				.size = info.size(),
			});
		}
		return result;
	};
	const auto now = QDateTime::currentDateTime();

	// Pre-saved copies of messages that were never deleted.
	for (const auto &entry : collect(PreFolder())) {
		if (entry.time.daysTo(now) >= kPreSaveKeepDays) {
			QFile::remove(entry.path);
		}
	}

	const auto days = IntSetting(u"media_days"_q, 0);
	const auto maxMb = IntSetting(u"media_max_mb"_q, 0);
	if (days <= 0 && maxMb <= 0) {
		return;
	}
	auto entries = collect(MediaFolder());
	if (days > 0) {
		for (auto i = entries.begin(); i != entries.end();) {
			if (i->time.daysTo(now) >= days) {
				QFile::remove(i->path);
				i = entries.erase(i);
			} else {
				++i;
			}
		}
	}
	if (maxMb > 0) {
		std::sort(entries.begin(), entries.end(), [](
				const Entry &a,
				const Entry &b) {
			return a.time < b.time;
		});
		auto total = int64(0);
		for (const auto &entry : entries) {
			total += entry.size;
		}
		const auto limit = int64(maxMb) * 1024 * 1024;
		for (const auto &entry : entries) {
			if (total <= limit) {
				break;
			}
			total -= entry.size;
			QFile::remove(entry.path);
		}
	}
}

void NotifyDeleted(not_null<HistoryItem*> item) {
	if (!Enabled(Flag::NotifyDeleted)
		|| item->out()
		|| IsHidden(item->history())
		|| !Core::App().settings().desktopNotify()) {
		Debug::Log(u"notify deleted skipped: enabled=%1 out=%2 hidden=%3 desktopNotify=%4"_q.arg(Enabled(Flag::NotifyDeleted)).arg(item->out()).arg(IsHidden(item->history())).arg(Core::App().settings().desktopNotify()));
		return;
	}
	const auto history = item->history();
	if (history->owner().notifySettings().isMuted(history->peer)) {
		return;
	}
	static auto windowStart = crl::time(0);
	static auto count = 0;
	const auto now = crl::now();
	if (!windowStart || (now - windowStart) > kNotifyWindow) {
		windowStart = now;
		count = 0;
	}
	if (++count > kNotifyPerWindow) {
		return;
	}
	const auto sender = item->from()->name();
	const auto chat = history->peer->name();
	const auto who = (sender == chat)
		? sender
		: (sender + u" ("_q + chat + ')');
	const auto header = QString::fromUtf8("\xF0\x9F\x97\x91 ")
		+ who
		+ u" xabarni o'chirdi"_q;
	Debug::Log(u"notify deleted shown"_q);
	Core::App().notifications().manager().showNotification({
		.item = item,
		.paradoksHeader = header,
	});
}

PeerId DbTarget(not_null<Main::Session*> session) {
	const auto user = session->userId().bare;
	const auto i = TargetCache.find(user);
	if (i != TargetCache.end()) {
		return i->second;
	}
	const auto value = PeerId(StringSetting(TargetKey(session)).toULongLong());
	TargetCache[user] = value;
	return value;
}

void SetDbTarget(not_null<Main::Session*> session, PeerId peerId) {
	TargetCache[session->userId().bare] = peerId;
	SetStringSetting(TargetKey(session), QString::number(peerId.value));
}

QString DbTargetName(not_null<Main::Session*> session) {
	const auto target = DbTarget(session);
	if (!target) {
		return u"tanlanmagan"_q;
	}
	const auto peer = session->data().peerLoaded(target);
	return peer ? peer->name() : u"(chat yuklanmagan)"_q;
}

void ChooseDbTarget(
		not_null<Window::SessionController*> controller,
		Fn<void()> done) {
	const auto session = &controller->session();
	Window::ShowChooseRecipientBox(
		controller,
		[=](not_null<Data::Thread*> thread) {
			SetDbTarget(session, thread->peer()->id);
			if (done) {
				done();
			}
			return true;
		},
		rpl::single(u"Media bazasi chatini tanlang"_q));
}

void AddSavedMediaActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	if (!Enabled(Flag::RestoreBackupMedia)
		|| !item->media()
		|| !IsDeleted(item)) {
		return;
	}
	const auto path = SavedMediaPath(item);
	if (!path.isEmpty()) {
		menu->addAction(u"Saqlangan nusxani ochish"_q, [=] {
			File::Launch(path);
		}, &st::menuIconDownload);
		menu->addAction(u"Saqlangan nusxa papkada"_q, [=] {
			File::ShowInFolder(path);
		}, &st::menuIconShowInFolder);
		return;
	}
	if (const auto copy = DbCopyFor(
			item->history()->peer->id.value,
			item->id.bare)) {
		const auto peerId = PeerId(copy->first);
		const auto msgId = MsgId(copy->second);
		menu->addAction(u"Bazadagi nusxani ochish"_q, [=] {
			controller->showPeerHistory(
				peerId,
				Window::SectionShow::Way::Forward,
				msgId);
		}, &st::menuIconDownload);
	}
}

} // namespace Paradoks
