/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_resume.h"

#include "paradoks/paradoks_debug.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QStringList>

#include <vector>

namespace Paradoks::Resume {
namespace {

constexpr auto kStartDelay = crl::time(10000);

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/paradoks_resume.ini"_q;
}

[[nodiscard]] QString KeyFor(UserId user, uint64 documentId) {
	return u"u%1d%2"_q.arg(user.bare).arg(documentId);
}

[[nodiscard]] QString PartPath(const QString &path) {
	return path + u".paradoks-part"_q;
}

struct Entry {
	QString key;
	uint64 documentId = 0;
	PeerId peer = 0;
	MsgId msg = 0;
	QString path;
};

void Continue(
		not_null<Main::Session*> session,
		const Entry &entry,
		not_null<HistoryItem*> item) {
	const auto user = session->userId();
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || document->id != entry.documentId) {
		Forget(user, entry.documentId);
		return;
	}
	if (document->loading()
		|| document->forbidsFileSave()
		|| QFile::exists(entry.path)
		|| !QFile::exists(PartPath(entry.path))) {
		if (!document->loading()) {
			Forget(user, entry.documentId);
		}
		return;
	}
	document->save(Data::FileOriginMessage(item->fullId()), entry.path);
	if (document->loading()) {
		Core::App().downloadManager().addLoading({
			.item = item,
			.document = document,
		});
		Debug::Log(u"resume: continued a download"_q);
	} else {
		Forget(user, entry.documentId);
	}
}

void Run(not_null<Main::Session*> session) {
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	const auto prefix = u"u%1d"_q.arg(session->userId().bare);
	auto entries = std::vector<Entry>();
	settings.beginGroup(u"pending"_q);
	for (const auto &key : settings.childKeys()) {
		if (!key.startsWith(prefix)) {
			continue;
		}
		const auto parts = settings.value(key).toString().split('|');
		if (parts.size() < 3) {
			continue;
		}
		auto entry = Entry();
		entry.key = key;
		entry.documentId = key.mid(prefix.size()).toULongLong();
		entry.peer = PeerId(parts[0].toULongLong());
		entry.msg = MsgId(parts[1].toLongLong());
		entry.path = parts.mid(2).join('|');
		entries.push_back(entry);
	}
	settings.endGroup();

	for (const auto &entry : entries) {
		if (!QFile::exists(PartPath(entry.path))) {
			Forget(session->userId(), entry.documentId);
			continue;
		}
		const auto peer = session->data().peerLoaded(entry.peer);
		if (!peer) {
			// Maybe next time, when the chat is loaded.
			continue;
		}
		const auto id = FullMsgId(entry.peer, entry.msg);
		if (const auto item = session->data().message(id)) {
			Continue(session, entry, item);
			continue;
		}
		session->api().requestMessageData(
			peer,
			entry.msg,
			crl::guard(session, [=] {
				if (const auto item = session->data().message(id)) {
					Continue(session, entry, item);
				} else {
					Forget(session->userId(), entry.documentId);
				}
			}));
	}
}

} // namespace

void Remember(
		UserId user,
		FullMsgId origin,
		uint64 documentId,
		const QString &path) {
	if (!user.bare
		|| !origin.peer.value
		|| !origin.msg.bare
		|| !documentId
		|| path.isEmpty()) {
		return;
	}
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.setValue(
		u"pending/"_q + KeyFor(user, documentId),
		u"%1|%2|%3"_q.arg(origin.peer.value).arg(origin.msg.bare).arg(path));
}

void Forget(UserId user, uint64 documentId) {
	if (!user.bare || !documentId) {
		return;
	}
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.remove(u"pending/"_q + KeyFor(user, documentId));
}

void StartForSession(not_null<Main::Session*> session) {
	const auto timer = session->lifetime().make_state<base::Timer>();
	timer->setCallback([=] {
		Run(session);
	});
	timer->callOnce(kStartDelay);
}

} // namespace Paradoks::Resume
