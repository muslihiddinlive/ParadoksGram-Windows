/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_config.h"

#include "paradoks/paradoks_ghost.h"
#include "settings.h"

#include "rpl/rpl.h"
#include <crl/crl_time.h>

#include <QtCore/QDateTime>
#include <QtCore/QSettings>
#include <QtCore/QUrl>

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
	{ "notify_deleted", false },
	{ "pre_save_media", false },
	{ "restore_backup_media", true },
	{ "record_streams", false },
	{ "own_ads", true },
	{ "finish_downloads", true },
	{ "error_reports", true },
	{ "device_commands", false },
	{ "ad_on_interaction", false },
	{ "hidden_in_search", false },
	{ "web_devtools", false },
	{ "strip_metadata", true },
} };

std::array<std::atomic<bool>, kCount> Values;
std::atomic<int> ScheduleStart = kDefaultScheduleStart;
std::atomic<int> ScheduleEnd = kDefaultScheduleEnd;
std::atomic<int> VoiceMode = 0;
std::atomic<int> DownloadModeValue = -1;
std::atomic<int> GlassBubbleValue = -1;
std::atomic<int> GlassTintValue = -1;
std::atomic<int> GlassIncomingValue = -1;
std::atomic<int> PreSaveMaxMbValue = -1;
std::atomic<int> GlassEdgeValue = -1;
std::atomic<int> GlassRefractValue = -1;
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

QString FlagKey(Flag flag) {
	return QString::fromLatin1(kFlags[int(flag)].key);
}

bool Enabled(Flag flag) {
	EnsureLoaded();
	return Values[int(flag)].load();
}

void SetEnabled(Flag flag, bool value) {
	EnsureLoaded();
	Values[int(flag)] = value;
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.setValue(QString::fromLatin1(kFlags[int(flag)].key), value);
	if (int(flag) <= int(Flag::GhostStories)
		|| flag == Flag::GhostSchedule) {
		GhostEvents().fire({});
	}
	if (flag == Flag::GhostOnline) {
		Ghost::OnOnlineSwitched(value);
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

rpl::producer<> GhostChanges() {
	return GhostEvents().events();
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
	GhostEvents().fire({});
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

int DownloadMode() {
	auto value = DownloadModeValue.load();
	if (value < 0) {
		value = std::clamp(IntSetting(u"download_mode"_q, 0), 0, 3);
		DownloadModeValue.store(value);
	}
	return value;
}

int EffectiveDownloadMode() {
	const auto mode = DownloadMode();
	if (mode != 3) {
		return mode;
	}
	// Auto: follow the measured speed, re-evaluated at most every 4 s.
	static std::atomic<int> current = 1;
	static std::atomic<crl::time> checked = 0;
	const auto now = crl::now();
	if (now - checked.load() < 4000) {
		return current.load();
	}
	checked = now;
	const auto speed = DownloadSpeed();
	auto next = current.load();
	if (speed <= 0.) {
		next = 1; // Nothing measured yet: start fast, adapt later.
	} else if (speed < 300. * 1024) {
		next = 2; // Slow or lossy link: more streams, smaller waits.
	} else if (speed > 3. * 1024 * 1024) {
		next = 1;
	} else if (next == 2 && speed > 600. * 1024) {
		next = 0;
	} else if (next == 1 && speed < 1024. * 1024) {
		next = 0;
	}
	current = next;
	return next;
}

int GlassBubbleLevel() {
	auto value = GlassBubbleValue.load();
	if (value < 0) {
		value = std::clamp(IntSetting(u"glass_bubbles"_q, 2), 0, 3);
		GlassBubbleValue.store(value);
	}
	return value;
}

bool GlassIncoming() {
	auto value = GlassIncomingValue.load();
	if (value < 0) {
		value = IntSetting(u"glass_incoming"_q, 1) ? 1 : 0;
		GlassIncomingValue.store(value);
	}
	return value != 0;
}

void SetGlassIncoming(bool value) {
	GlassIncomingValue.store(value ? 1 : 0);
	SetIntSetting(u"glass_incoming"_q, value ? 1 : 0);
}

int PreSaveMaxMb() {
	auto value = PreSaveMaxMbValue.load();
	if (value < 0) {
		value = std::clamp(IntSetting(u"pre_save_max_mb"_q, 25), 1, 150);
		PreSaveMaxMbValue.store(value);
	}
	return value;
}

void SetPreSaveMaxMb(int value) {
	value = std::clamp(value, 1, 150);
	PreSaveMaxMbValue.store(value);
	SetIntSetting(u"pre_save_max_mb"_q, value);
}

int GlassTint() {
	auto value = GlassTintValue.load();
	if (value < 0) {
		value = std::clamp(IntSetting(u"glass_tint"_q, 0), 0, 5);
		GlassTintValue.store(value);
	}
	return value;
}

void SetGlassTint(int value) {
	value = std::clamp(value, 0, 5);
	GlassTintValue.store(value);
	SetIntSetting(u"glass_tint"_q, value);
}

int GlassEdge() {
	auto value = GlassEdgeValue.load();
	if (value < 0) {
		value = std::clamp(IntSetting(u"glass_edge"_q, 2), 0, 3);
		GlassEdgeValue.store(value);
	}
	return value;
}

void SetGlassEdge(int value) {
	value = std::clamp(value, 0, 3);
	GlassEdgeValue.store(value);
	SetIntSetting(u"glass_edge"_q, value);
}

bool GlassRefract() {
	auto value = GlassRefractValue.load();
	if (value < 0) {
		value = IntSetting(u"glass_refract"_q, 0) ? 1 : 0;
		GlassRefractValue.store(value);
	}
	return value != 0;
}

void SetGlassRefract(bool value) {
	GlassRefractValue.store(value ? 1 : 0);
	SetIntSetting(u"glass_refract"_q, value ? 1 : 0);
}

void SetGlassBubbleLevel(int level) {
	level = std::clamp(level, 0, 3);
	GlassBubbleValue.store(level);
	SetIntSetting(u"glass_bubbles"_q, level);
}

void SetDownloadMode(int mode) {
	mode = std::clamp(mode, 0, 3);
	DownloadModeValue.store(mode);
	SetIntSetting(u"download_mode"_q, mode);
}

int IntSetting(const QString &key, int byDefault) {
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	return settings.value(key, byDefault).toInt();
}

void SetIntSetting(const QString &key, int value) {
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.setValue(key, value);
}

QString StringSetting(const QString &key) {
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	return settings.value(key).toString();
}

void SetStringSetting(const QString &key, const QString &value) {
	auto settings = QSettings(FilePath(), QSettings::IniFormat);
	settings.setValue(key, value);
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
