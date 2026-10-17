/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_store.h"

#include "paradoks/paradoks_config.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <map>
#include <set>

namespace Paradoks {
namespace {

constexpr auto kMaxMediaSize = int64(150) * 1024 * 1024;

using Key = std::pair<uint64, int64>;

struct State {
	std::set<Key> deletedKeys;
	std::vector<DeletedEntry> deleted;
	std::map<Key, std::vector<EditVersion>> edits;
};

[[nodiscard]] QString LogPath() {
	return cWorkingDir() + u"tdata/paradoks_log.jsonl"_q;
}

[[nodiscard]] Key KeyFor(not_null<const HistoryItem*> item) {
	return { item->history()->peer->id.value, item->id.bare };
}

[[nodiscard]] State &Load() {
	static auto state = [] {
		auto result = State();
		auto file = QFile(LogPath());
		if (!file.open(QIODevice::ReadOnly)) {
			return result;
		}
		while (!file.atEnd()) {
			const auto document = QJsonDocument::fromJson(file.readLine());
			if (!document.isObject()) {
				continue;
			}
			const auto object = document.object();
			const auto key = Key(
				object.value(u"p"_q).toString().toULongLong(),
				object.value(u"i"_q).toString().toLongLong());
			const auto kind = object.value(u"k"_q).toString();
			if (kind == u"d"_q) {
				result.deletedKeys.emplace(key);
				result.deleted.push_back({
					.peer = key.first,
					.id = key.second,
					.date = TimeId(object.value(u"d"_q).toInt()),
					.deletedAt = TimeId(object.value(u"x"_q).toInt()),
					.from = object.value(u"f"_q).toString(),
					.text = object.value(u"t"_q).toString(),
				});
			} else if (kind == u"e"_q) {
				result.edits[key].push_back({
					.date = TimeId(object.value(u"d"_q).toInt()),
					.text = object.value(u"t"_q).toString(),
				});
			}
		}
		return result;
	}();
	return state;
}

void Append(const QJsonObject &object) {
	auto file = QFile(LogPath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		return;
	}
	file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	file.write("\n");
}

void SaveMedia(not_null<HistoryItem*> item) {
	if (!Enabled(Flag::KeepDeletedMedia)) {
		return;
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document) {
		return;
	}
	const auto path = document->filepath(true);
	if (path.isEmpty() || QFileInfo(path).size() > kMaxMediaSize) {
		return;
	}
	const auto folder = cWorkingDir() + u"tdata/paradoks_media/"_q;
	QDir().mkpath(folder);
	const auto name = QString::number(item->history()->peer->id.value)
		+ '_'
		+ QString::number(item->id.bare)
		+ '_'
		+ QFileInfo(path).fileName();
	QFile::copy(path, folder + name);
}

} // namespace

bool KeepDeleted(not_null<HistoryItem*> item) {
	if (!Enabled(Flag::KeepDeleted)
		|| !item->isRegular()
		|| item->isService()) {
		return false;
	}
	auto &state = Load();
	const auto key = KeyFor(item);
	if (!state.deletedKeys.emplace(key).second) {
		return true;
	}
	const auto entry = DeletedEntry{
		.peer = key.first,
		.id = key.second,
		.date = item->date(),
		.deletedAt = base::unixtime::now(),
		.from = item->from()->name(),
		.text = item->originalText().text,
	};
	state.deleted.push_back(entry);
	auto object = QJsonObject();
	object.insert(u"k"_q, u"d"_q);
	object.insert(u"p"_q, QString::number(entry.peer));
	object.insert(u"i"_q, QString::number(entry.id));
	object.insert(u"d"_q, int(entry.date));
	object.insert(u"x"_q, int(entry.deletedAt));
	object.insert(u"f"_q, entry.from);
	object.insert(u"t"_q, entry.text);
	Append(object);
	SaveMedia(item);
	item->history()->owner().requestItemViewRefresh(item);
	return true;
}

bool IsDeleted(not_null<const HistoryItem*> item) {
	const auto &state = Load();
	return !state.deletedKeys.empty()
		&& state.deletedKeys.contains(KeyFor(item));
}

std::vector<DeletedEntry> DeletedList() {
	return Load().deleted;
}

void LogEdit(not_null<HistoryItem*> item, const QString &oldText) {
	if (!Enabled(Flag::TrackEdits)) {
		return;
	}
	const auto edited = item->Get<HistoryMessageEdited>();
	const auto date = (edited && edited->date) ? edited->date : item->date();
	const auto key = KeyFor(item);
	Load().edits[key].push_back({ .date = date, .text = oldText });
	auto object = QJsonObject();
	object.insert(u"k"_q, u"e"_q);
	object.insert(u"p"_q, QString::number(key.first));
	object.insert(u"i"_q, QString::number(key.second));
	object.insert(u"d"_q, int(date));
	object.insert(u"t"_q, oldText);
	Append(object);
}

std::vector<EditVersion> EditsFor(not_null<const HistoryItem*> item) {
	const auto &state = Load();
	const auto i = state.edits.find(KeyFor(item));
	return (i != state.edits.end()) ? i->second : std::vector<EditVersion>();
}

} // namespace Paradoks
