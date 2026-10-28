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

namespace Paradoks::Ai {

// The Paradoks AI window: a chat screen over the conversation with
// @ParadoksAiBot. Messages are sent from the user's own account, so the
// whole agent (memory, tools, limits) keeps working. The "topics" of the bot
// are the user's sessions: they are listed on the left, can be opened and
// started here.
void Open(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Ai
