/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "rpl/rpl.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Paradoks::Update {

enum class Result {
	Failed,
	Latest,
	Newer,
};

// Build number baked in at compile time (0 for local builds).
[[nodiscard]] int CurrentBuild();

// Newer build found by the last check, or 0.
[[nodiscard]] int AvailableBuild();

// Newer build number as a stream (0 while there is none).
[[nodiscard]] rpl::producer<int> AvailableValue();

// Delayed one-time check after the app starts, then periodic.
void CheckOnStart();

void Check(Fn<void(Result)> done);

// "New version" box: downloads, replaces Telegram.exe and restarts.
void ShowBox(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Update
