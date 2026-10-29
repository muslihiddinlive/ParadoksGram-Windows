/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "editor/video/video_editor_layer.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Paradoks::Story {

// Pick a photo, edit it, choose caption/privacy/duration and post a story.
void Start(not_null<Window::SessionController*> controller);

// Start a Live Story (RTMP) with an optional Stars price for chat messages.
void StartLive(not_null<Window::SessionController*> controller);

// An audio file (m4a, mp3, ...) becomes a story: cover art or a generated
// picture with the music. Returns false if the file has no playable audio.
[[nodiscard]] bool PrepareAudio(
	const QString &path,
	Fn<void(Editor::StoryMedia&&)> done);

} // namespace Paradoks::Story
