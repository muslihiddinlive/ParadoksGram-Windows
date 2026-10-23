/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_config.h"

#include "settings.h"

#include "rpl/rpl.h"

#include <QtCore/QDateTime>
#include <QtCore/QSettings>

#include <array>
#include <chrono>
#include <deque>
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
} };

std::array<std::atomic<bool>, kCount> Values;
std::atomic<int> ScheduleStart = kDefaultScheduleStart;
std::atomic<int> ScheduleEnd = kDefaultScheduleEnd;
std::atomic<int> VoiceMode = 0;
std::atomic<int> IconVariant = 0;
std::once_flag LoadFlag;

[[nodiscard]] rpl::event_stream<> &GhostEvents() {
	static auto events = rpl::event_stream<>();
	return events;
}

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
		VoiceMode = settings.value(u"voice_effect"_q, 0).toInt();
		IconVariant = settings.value(u"app_icon"_q, 0).toInt();
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
	if (int(flag) <= int(Flag::GhostStories)) {
		GhostEvents().fire({});
	}
}

bool GhostAll() {
	return Enabled(Flag::GhostRead)
		&& Enabled(Flag::GhostTyping)
		&& Enabled(Flag::GhostOnline)
		&& Enabled(Flag::GhostStories);
}

void SetGhostAll(bool value) {
	SetEnabled(Flag::GhostRead, value);
	SetEnabled(Flag::GhostTyping, value);
	SetEnabled(Flag::GhostOnline, value);
	SetEnabled(Flag::GhostStories, value);
}

rpl::producer<bool> GhostAllValue() {
	return rpl::single(
		GhostAll()
	) | rpl::then(
		GhostEvents().events() | rpl::map([] {
			return GhostAll();
		})
	) | rpl::distinct_until_changed();
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

namespace {

struct SpeedSample {
	std::chrono::steady_clock::time_point time;
	int64 bytes = 0;
};

// Used from the main thread only.
std::deque<SpeedSample> SpeedSamples;
double SpeedPeak = 0.;

constexpr auto kSpeedWindow = std::chrono::milliseconds(1000);

void TrimSamples(std::chrono::steady_clock::time_point now) {
	while (!SpeedSamples.empty()
		&& (now - SpeedSamples.front().time) > kSpeedWindow) {
		SpeedSamples.pop_front();
	}
}

[[nodiscard]] double CurrentSpeed(std::chrono::steady_clock::time_point now) {
	TrimSamples(now);
	auto sum = int64(0);
	for (const auto &sample : SpeedSamples) {
		sum += sample.bytes;
	}
	return double(sum);
}

} // namespace

void NoteDownloaded(int64 bytes) {
	const auto now = std::chrono::steady_clock::now();
	SpeedSamples.push_back({ now, bytes });
	SpeedPeak = std::max(SpeedPeak, CurrentSpeed(now));
}

double DownloadSpeed() {
	return CurrentSpeed(std::chrono::steady_clock::now());
}

double DownloadPeak() {
	return SpeedPeak;
}

void ResetDownloadPeak() {
	SpeedPeak = 0.;
}

bool SevenZipBroken() {
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	return settings.value(u"update/sevenzip_broken"_q, false).toBool();
}

void SetSevenZipBroken() {
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.setValue(u"update/sevenzip_broken"_q, true);
}

int AppIconVariant() {
	EnsureLoaded();
	return IconVariant.load();
}

void SetAppIconVariant(int variant) {
	EnsureLoaded();
	IconVariant = variant;
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.setValue(u"app_icon"_q, variant);
}

int VoiceEffect() {
	EnsureLoaded();
	return VoiceMode.load();
}

void SetVoiceEffect(int mode) {
	EnsureLoaded();
	VoiceMode = mode;
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.setValue(u"voice_effect"_q, mode);
}

bool TakeFirstRun() {
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	if (settings.contains(u"first_run_done"_q)) {
		return false;
	}
	settings.setValue(u"first_run_done"_q, true);
	settings.sync();
	return true;
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
