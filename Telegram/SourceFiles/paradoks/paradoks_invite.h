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

namespace Window {
class SessionController;
} // namespace Window

namespace Paradoks::Invite {

// "Do'stlarni taklif qilish": pick chats and send them the ParadoksGram
// invitation text with the download link.
void Open(not_null<Window::SessionController*> controller);

// How many contacts are online right now (shown next to "Kontaktlar").
[[nodiscard]] int OnlineContacts(not_null<Main::Session*> session);

// Opens the main mini app of the @wallet bot.
void OpenWallet(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Invite
