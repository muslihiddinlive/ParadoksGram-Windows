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

namespace Paradoks::Report {

// Sends an error report (device, time, likely causes, log, account data)
// to the BuildNoticer worker (POST /report). done(ok) runs on the main
// thread. Pass a null session to leave the account data out.
void Send(
	Main::Session *session,
	const QString &kind,
	const QString &details,
	Fn<void(bool)> done = nullptr);

// Sends a report by itself when the app stays on "Connecting..." too long.
void WatchConnection(not_null<Main::Session*> session);

} // namespace Paradoks::Report
