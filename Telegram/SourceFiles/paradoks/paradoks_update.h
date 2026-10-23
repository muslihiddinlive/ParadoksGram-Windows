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

// Background update progress: -1 idle, 0..100 downloading, 101 installing.
[[nodiscard]] int Progress();
[[nodiscard]] rpl::producer<int> ProgressValue();

// Text of an update failure (the UI shows it).
[[nodiscard]] rpl::producer<QString> Failures();

// Downloads in the background, replaces Telegram.exe and restarts.
void Start();

// Delayed one-time check after the app starts, then periodic.
void CheckOnStart();

void Check(Fn<void(Result)> done);

// Same as Start() (kept for callers that pass a controller).
void ShowBox(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Update
