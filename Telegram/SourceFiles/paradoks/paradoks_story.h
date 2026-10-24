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

namespace Paradoks::Story {

// Pick a photo, edit it, choose caption/privacy/duration and post a story.
void Start(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Story
