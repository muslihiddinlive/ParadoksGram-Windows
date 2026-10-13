/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

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
	HiddenInSearch,

	kCount,
};

[[nodiscard]] bool Enabled(Flag flag);
void SetEnabled(Flag flag, bool value);

[[nodiscard]] int GhostScheduleStart();
[[nodiscard]] int GhostScheduleEnd();
void SetGhostSchedule(int startMinutes, int endMinutes);

[[nodiscard]] bool InGhostWindow();

} // namespace Paradoks
