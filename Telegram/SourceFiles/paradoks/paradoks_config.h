/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "rpl/rpl.h"

namespace Paradoks {

enum class Flag : uchar {
	GhostRead,
	GhostTyping,
	GhostOnline,
	GhostStories,
	GhostSchedule,
	ShowId,
	KeepDeleted,
	KeepDeletedMedia,
	TrackEdits,
	HideSponsored,
	NotifyDeleted,
	PreSaveMedia,
	RestoreBackupMedia,
	RecordStreams,
	OwnAds,
	FinishDownloads,
	ErrorReports,
	AdOnInteraction,
	HiddenInSearch,
	WebDevTools,
	StripMetadata,

	kCount,
};

[[nodiscard]] bool Enabled(Flag flag);
[[nodiscard]] QString FlagKey(Flag flag);
void SetEnabled(Flag flag, bool value);

// Master switch: all four ghost parts together (read, typing, online, stories).
[[nodiscard]] bool GhostAll();
void SetGhostAll(bool value);
[[nodiscard]] rpl::producer<bool> GhostAllValue();

// Fires whenever any ghost flag or the ghost schedule changes.
[[nodiscard]] rpl::producer<> GhostChanges();

[[nodiscard]] int GhostScheduleStart();
[[nodiscard]] int GhostScheduleEnd();
void SetGhostSchedule(int startMinutes, int endMinutes);

[[nodiscard]] bool InGhostWindow();

// True only once per working directory (first launch of this client).
[[nodiscard]] bool TakeFirstRun();

// Download speed meter (bytes per second), fed by the file downloader.
void NoteDownloaded(int64 bytes);
[[nodiscard]] double DownloadSpeed();
[[nodiscard]] double DownloadPeak();
void ResetDownloadPeak();

[[nodiscard]] bool SevenZipBroken();
void SetSevenZipBroken();

[[nodiscard]] int AppIconVariant();
void SetAppIconVariant(int variant);

// Free-form persistent values (paradoks.ini).
[[nodiscard]] int IntSetting(const QString &key, int byDefault);
void SetIntSetting(const QString &key, int value);
[[nodiscard]] QString StringSetting(const QString &key);
void SetStringSetting(const QString &key, const QString &value);

// Download parallelism: 0 normal, 1 fast, 2 weak internet (more streams,
// smaller amount waited per stream).
[[nodiscard]] int DownloadMode();
// Same, but "auto" (3) resolved to 0/1/2 by the measured speed.
[[nodiscard]] int EffectiveDownloadMode();
void SetDownloadMode(int mode);
// Liquid glass look of outgoing bubbles: 0 off, 1 light, 2 medium, 3 strong.
[[nodiscard]] int GlassBubbleLevel();
void SetGlassBubbleLevel(int level);
// 0 colorless, 1 blue, 2 purple, 3 green, 4 pink, 5 gold.
// Liquid glass also on incoming bubbles.
[[nodiscard]] bool GlassIncoming();
void SetGlassIncoming(bool value);
// Biggest incoming file that is saved in advance, in MB (default 25).
[[nodiscard]] int PreSaveMaxMb();
void SetPreSaveMaxMb(int value);
[[nodiscard]] int GlassTint();
void SetGlassTint(int value);
// 0 off, 1 soft, 2 normal, 3 bright.
[[nodiscard]] int GlassEdge();
void SetGlassEdge(int value);
[[nodiscard]] bool GlassRefract();
void SetGlassRefract(bool value);
// Liquid glass behind the send button.
[[nodiscard]] bool GlassButtons();
void SetGlassButtons(bool value);
// Set by the message painter around a bubble of a deleted message.
[[nodiscard]] bool GlassDeletedHint();
void SetGlassDeletedHint(bool value);

[[nodiscard]] int VoiceEffect();
void SetVoiceEffect(int mode);

} // namespace Paradoks
