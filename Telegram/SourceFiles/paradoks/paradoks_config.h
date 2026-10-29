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

	kCount,
};

[[nodiscard]] bool Enabled(Flag flag);
void SetEnabled(Flag flag, bool value);

// Master switch: all four ghost parts together (read, typing, online, stories).
[[nodiscard]] bool GhostAll();
void SetGhostAll(bool value);
[[nodiscard]] rpl::producer<bool> GhostAllValue();

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

[[nodiscard]] int VoiceEffect();
void SetVoiceEffect(int mode);

} // namespace Paradoks
