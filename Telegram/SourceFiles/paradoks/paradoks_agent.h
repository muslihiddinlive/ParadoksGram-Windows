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

namespace Paradoks::Agent {

// "Paradoks AI buyruqlari": lets the Paradoks AI bot perform actions in this
// app on the user's behalf. The bot's server cannot reach the computer, so
// this agent polls the server for jobs, runs them locally with the user's own
// account and posts the result back. Everything is off until the user turns
// the setting on and enters an https:// server address.
void StartForSession(not_null<Main::Session*> session);

// Settings: server address and the pairing state.
[[nodiscard]] QString ServerUrl();
void SetServerUrl(const QString &url);
[[nodiscard]] bool Paired();

} // namespace Paradoks::Agent
