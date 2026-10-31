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

namespace Paradoks::Counters {

// "Chat hisoblagichlari": how many private chats, bots, groups and channels,
// and how many unread / muted ones (like the Android screen).
void Open(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Counters
