/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Paradoks {

struct EditVersion {
	TimeId date = 0;
	QString text;
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
};

[[nodiscard]] bool KeepDeleted(not_null<HistoryItem*> item);
[[nodiscard]] bool IsDeleted(not_null<const HistoryItem*> item);
[[nodiscard]] std::vector<DeletedEntry> DeletedList();

void LogEdit(not_null<HistoryItem*> item, const QString &oldText);
[[nodiscard]] std::vector<EditVersion> EditsFor(
	not_null<const HistoryItem*> item);
// All edited messages, the most recently edited first.
[[nodiscard]] std::vector<EditedEntry> EditedList();

} // namespace Paradoks
