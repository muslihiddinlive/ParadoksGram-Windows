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

[[nodiscard]] bool SevenZipBroken();
void SetSevenZipBroken();

[[nodiscard]] int AppIconVariant();
void SetAppIconVariant(int variant);

[[nodiscard]] int VoiceEffect();
void SetVoiceEffect(int mode);

} // namespace Paradoks
