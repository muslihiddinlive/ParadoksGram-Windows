/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_config.h"

#include "settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QSettings>

#include <array>
#include <atomic>
#include <mutex>

namespace Paradoks {
namespace {

constexpr auto kCount = int(Flag::kCount);
constexpr auto kDefaultScheduleStart = 22 * 60;
constexpr auto kDefaultScheduleEnd = 8 * 60;

struct FlagInfo {
	const char *key;
	bool byDefault;
};

constexpr auto kFlags = std::array<FlagInfo, kCount>{ {
	{ "ghost_read", false },
	{ "ghost_typing", false },
	{ "ghost_online", false },
	{ "ghost_stories", false },
	{ "ghost_sched", false },
	{ "show_id", true },
	{ "keep_deleted", true },
	{ "keep_deleted_media", true },
	{ "track_edits", true },
	{ "hide_sponsored", true },
	{ "hidden_in_search", false },
} };

std::array<std::atomic<bool>, kCount> Values;
std::atomic<int> ScheduleStart = kDefaultScheduleStart;
std::atomic<int> ScheduleEnd = kDefaultScheduleEnd;
std::once_flag LoadFlag;

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/paradoks.ini"_q;
}

void EnsureLoaded() {
	std::call_once(LoadFlag, [] {
		auto settings = QSettings(FilePath(), QSettings::IniFormat);
		for (auto i = 0; i != kCount; ++i) {
			Values[i] = settings.value(
				QString::fromLatin1(kFlags[i].key),
				kFlags[i].byDefault).toBool();
		}
		ScheduleStart = settings.value(
			u"ghost_sched_start"_q,
			kDefaultScheduleStart).toInt();
		ScheduleEnd = settings.value(
			u"ghost_sched_end"_q,
			kDefaultScheduleEnd).toInt();
	});
}

} // namespace

bool Enabled(Flag flag) {
	EnsureLoaded();
	return Values[int(flag)].load();
}

void SetEnabled(Flag flag, bool value) {
	EnsureLoaded();
	Values[int(flag)] = value;
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.setValue(QString::fromLatin1(kFlags[int(flag)].key), value);
}

int GhostScheduleStart() {
	EnsureLoaded();
	return ScheduleStart.load();
}

int GhostScheduleEnd() {
	EnsureLoaded();
	return ScheduleEnd.load();
}

void SetGhostSchedule(int startMinutes, int endMinutes) {
	EnsureLoaded();
	ScheduleStart = startMinutes;
	ScheduleEnd = endMinutes;
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.setValue(u"ghost_sched_start"_q, startMinutes);
	settings.setValue(u"ghost_sched_end"_q, endMinutes);
}

bool InGhostWindow() {
	if (!Enabled(Flag::GhostSchedule)) {
		return true;
	}
	const auto time = QDateTime::currentDateTime().time();
	const auto now = time.hour() * 60 + time.minute();
	const auto start = GhostScheduleStart();
	const auto end = GhostScheduleEnd();
	if (start == end) {
		return true;
	}
	return (start < end)
		? (now >= start && now < end)
		: (now >= start || now < end);
}

} // namespace Paradoks
