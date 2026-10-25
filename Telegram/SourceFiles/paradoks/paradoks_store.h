/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/text/text_entity.h"

class HistoryItem;

namespace Data {
class PhotoMedia;
} // namespace Data

namespace Paradoks {

struct EditVersion {
	TimeId date = 0;
	QString text;
	EntitiesInText entities;
	QString media; // Saved copy of a replaced photo / file, if any.
};

// One edited message for the "edited radar".
struct EditedEntry {
	uint64 peer = 0;
	int64 id = 0;
	TimeId changedAt = 0; // When the last edit was noticed.
	QString from;
	QString chat;
	std::vector<EditVersion> versions; // Old versions, oldest first.
};

struct DeletedEntry {
	uint64 peer = 0;
	int64 id = 0;
	TimeId date = 0;
	TimeId deletedAt = 0;
	QString from;
	QString text;
	QString chat;
	bool hasMedia = false;
};

// State of a message right before an edit is applied.
struct EditSnapshot {
	TextWithEntities text;
	QString mediaKey;
	QString mediaPath;
	std::shared_ptr<Data::PhotoMedia> photo;
	TimeId date = 0;
};

[[nodiscard]] bool KeepDeleted(not_null<HistoryItem*> item);
[[nodiscard]] bool IsDeleted(not_null<const HistoryItem*> item);
[[nodiscard]] std::vector<DeletedEntry> DeletedList();

// Call before applyEdition() and after it.
[[nodiscard]] EditSnapshot BeginEdit(not_null<HistoryItem*> item);
void FinishEdit(not_null<HistoryItem*> item, EditSnapshot &&before);

[[nodiscard]] std::vector<EditVersion> EditsFor(
	not_null<const HistoryItem*> item);
[[nodiscard]] std::vector<EditVersion> EditsFor(uint64 peer, int64 id);
// All edited messages, the most recently edited first.
[[nodiscard]] std::vector<EditedEntry> EditedList();

} // namespace Paradoks
