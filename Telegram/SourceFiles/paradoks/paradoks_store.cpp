/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_store.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "paradoks/paradoks_media.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <map>
#include <set>

namespace Paradoks {
namespace {

constexpr auto kMaxEditMediaSize = int64(100) * 1024 * 1024;

using Key = std::pair<uint64, int64>;

struct EditMeta {
	TimeId changedAt = 0;
	QString from;
	QString chat;
};

struct State {
	std::set<Key> deletedKeys;
	std::map<Key, EditMeta> editMeta;
	std::vector<DeletedEntry> deleted;
	std::map<Key, std::vector<EditVersion>> edits;
};

[[nodiscard]] QString LogPath() {
	return cWorkingDir() + u"tdata/paradoks_log.jsonl"_q;
}

[[nodiscard]] Key KeyFor(not_null<const HistoryItem*> item) {
	return { item->history()->peer->id.value, item->id.bare };
}

[[nodiscard]] QString MediaKey(not_null<const HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return QString();
	} else if (const auto document = media->document()) {
		return u"d"_q + QString::number(document->id);
	} else if (const auto photo = media->photo()) {
		return u"p"_q + QString::number(photo->id);
	}
	return u"o"_q;
}

[[nodiscard]] bool SameEntities(
		const EntitiesInText &a,
		const EntitiesInText &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto i = 0; i != a.size(); ++i) {
		if (a[i].type() != b[i].type()
			|| a[i].offset() != b[i].offset()
			|| a[i].length() != b[i].length()
			|| a[i].data() != b[i].data()) {
			return false;
		}
	}
	return true;
}

// Private copy of the photo / file that was replaced by an edit.
[[nodiscard]] QString KeepReplacedMedia(
		Key key,
		TimeId date,
		const QString &path,
		const std::shared_ptr<Data::PhotoMedia> &photo) {
	const auto folder = cWorkingDir() + u"tdata/paradoks_media/edits/"_q;
	const auto prefix = QString::number(key.first)
		+ '_'
		+ QString::number(key.second)
		+ '_'
		+ QString::number(date)
		+ '_';
	if (!path.isEmpty()) {
		const auto info = QFileInfo(path);
		if (!info.exists() || info.size() > kMaxEditMediaSize) {
			return QString();
		}
		QDir().mkpath(folder);
		const auto name = prefix + info.fileName();
		return QFile::copy(path, folder + name) ? (folder + name) : QString();
	} else if (photo && photo->loaded()) {
		QDir().mkpath(folder);
		const auto name = prefix + u"photo.jpg"_q;
		return photo->saveToFile(folder + name) ? (folder + name) : QString();
	}
	return QString();
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
					.chat = object.value(u"c"_q).toString(),
					.hasMedia = object.value(u"m"_q).toBool(),
				});
			} else if (kind == u"e"_q) {
				auto entities = EntitiesInText();
				const auto list = object.value(u"n"_q).toArray();
				for (const auto &value : list) {
					const auto entity = value.toObject();
					entities.push_back(EntityInText(
						EntityType(entity.value(u"t"_q).toInt()),
						entity.value(u"o"_q).toInt(),
						entity.value(u"l"_q).toInt(),
						entity.value(u"d"_q).toString()));
				}
				result.edits[key].push_back({
					.date = TimeId(object.value(u"d"_q).toInt()),
					.text = object.value(u"t"_q).toString(),
					.entities = std::move(entities),
					.media = object.value(u"m"_q).toString(),
				});
				result.editMeta[key] = {
					.changedAt = TimeId(object.value(u"x"_q).toInt()),
					.from = object.value(u"f"_q).toString(),
					.chat = object.value(u"c"_q).toString(),
				};
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
		.chat = item->history()->peer->name(),
		.hasMedia = (item->media() != nullptr),
	};
	state.deleted.push_back(entry);
	Debug::Log(u"kept deleted %1_%2 hasMedia=%3"_q.arg(entry.peer).arg(entry.id).arg(entry.hasMedia));
	auto object = QJsonObject();
	object.insert(u"k"_q, u"d"_q);
	object.insert(u"p"_q, QString::number(entry.peer));
	object.insert(u"i"_q, QString::number(entry.id));
	object.insert(u"d"_q, int(entry.date));
	object.insert(u"x"_q, int(entry.deletedAt));
	object.insert(u"f"_q, entry.from);
	object.insert(u"t"_q, entry.text);
	object.insert(u"c"_q, entry.chat);
	object.insert(u"m"_q, entry.hasMedia);
	Append(object);
	KeepMediaFor(item);
	NotifyDeleted(item);
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

EditSnapshot BeginEdit(not_null<HistoryItem*> item) {
	const auto edited = item->Get<HistoryMessageEdited>();
	auto result = EditSnapshot{
		.text = item->originalText(),
		.mediaKey = MediaKey(item),
		.date = (edited && edited->date) ? edited->date : item->date(),
	};
	if (Enabled(Flag::TrackEdits) && item->media()) {
		if (const auto document = item->media()->document()) {
			result.mediaPath = document->filepath(true);
		} else if (const auto photo = item->media()->photo()) {
			result.photo = photo->activeMediaView();
		}
	}
	return result;
}

void FinishEdit(not_null<HistoryItem*> item, EditSnapshot &&before) {
	if (!Enabled(Flag::TrackEdits)) {
		return;
	}
	const auto &now = item->originalText();
	const auto textChanged = (before.text.text != now.text)
		|| !SameEntities(before.text.entities, now.entities);
	const auto mediaChanged = (before.mediaKey != MediaKey(item));
	if (!textChanged && !mediaChanged) {
		return;
	}
	const auto key = KeyFor(item);
	auto version = EditVersion{
		.date = before.date,
		.text = before.text.text,
		.entities = before.text.entities,
	};
	if (mediaChanged) {
		version.media = KeepReplacedMedia(
			key,
			before.date,
			before.mediaPath,
			before.photo);
	}
	Debug::Log(u"edit tracked %1_%2 textChanged=%3 mediaChanged=%4"_q.arg(key.first).arg(key.second).arg(textChanged).arg(mediaChanged));
	auto &state = Load();
	state.edits[key].push_back(version);
	const auto meta = EditMeta{
		.changedAt = base::unixtime::now(),
		.from = item->from()->name(),
		.chat = item->history()->peer->name(),
	};
	state.editMeta[key] = meta;
	auto object = QJsonObject();
	object.insert(u"k"_q, u"e"_q);
	object.insert(u"p"_q, QString::number(key.first));
	object.insert(u"i"_q, QString::number(key.second));
	object.insert(u"d"_q, int(version.date));
	object.insert(u"t"_q, version.text);
	object.insert(u"x"_q, int(meta.changedAt));
	object.insert(u"f"_q, meta.from);
	object.insert(u"c"_q, meta.chat);
	if (!version.media.isEmpty()) {
		object.insert(u"m"_q, version.media);
	}
	if (!version.entities.isEmpty()) {
		auto array = QJsonArray();
		for (const auto &entity : version.entities) {
			auto value = QJsonObject();
			value.insert(u"t"_q, int(entity.type()));
			value.insert(u"o"_q, entity.offset());
			value.insert(u"l"_q, entity.length());
			if (!entity.data().isEmpty()) {
				value.insert(u"d"_q, entity.data());
			}
			array.push_back(value);
		}
		object.insert(u"n"_q, array);
	}
	Append(object);
}

std::vector<EditedEntry> EditedList() {
	const auto &state = Load();
	auto result = std::vector<EditedEntry>();
	result.reserve(state.edits.size());
	for (const auto &[key, versions] : state.edits) {
		auto entry = EditedEntry{
			.peer = key.first,
			.id = key.second,
			.versions = versions,
		};
		const auto i = state.editMeta.find(key);
		if (i != state.editMeta.end()) {
			entry.changedAt = i->second.changedAt;
			entry.from = i->second.from;
			entry.chat = i->second.chat;
		}
		result.push_back(std::move(entry));
	}
	std::sort(result.begin(), result.end(), [](
			const EditedEntry &a,
			const EditedEntry &b) {
		return a.changedAt > b.changedAt;
	});
	return result;
}

std::vector<EditVersion> EditsFor(not_null<const HistoryItem*> item) {
	return EditsFor(item->history()->peer->id.value, item->id.bare);
}

std::vector<EditVersion> EditsFor(uint64 peer, int64 id) {
	const auto &state = Load();
	const auto i = state.edits.find(Key(peer, id));
	return (i != state.edits.end()) ? i->second : std::vector<EditVersion>();
}

} // namespace Paradoks
