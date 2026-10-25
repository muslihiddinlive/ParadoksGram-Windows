/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Paradoks::Crash {

// Writes tdata/paradoks_crash.txt when the app dies from an unhandled
// exception (Telegram's own crash reports are disabled in this build).
void Install();

// Shows the report of the previous run (if any) with a copy button.
void ShowLastIfAny(not_null<Main::Session*> session);

} // namespace Paradoks::Crash
