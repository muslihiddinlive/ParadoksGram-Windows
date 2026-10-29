/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Paradoks::Broadcast {

// One message (text and/or one file) sent to many chats or a whole folder,
// with a history of previous sends that can be repeated.
void Open(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Broadcast
