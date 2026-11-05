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

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Paradoks::Report {

// Sends an error report (device, time, likely causes, log, account data)
// to the BuildNoticer worker (POST /report). done(ok) runs on the main
// thread. Pass a null session to leave the account data out.
void Send(
	Main::Session *session,
	const QString &kind,
	const QString &details,
	Fn<void(bool)> done = nullptr);

// Same, with the user's own words and files (pictures, documents).
void SendWithUser(
	Main::Session *session,
	const QString &kind,
	const QString &details,
	const QString &comment,
	const QStringList &files,
	Fn<void(bool)> done = nullptr);

// A box where the user describes the problem and attaches pictures or
// files, then sends the report by hand.
void ComposeBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller);

// Sends a report by itself when the app stays on "Connecting..." too long.
void WatchConnection(not_null<Main::Session*> session);

} // namespace Paradoks::Report
