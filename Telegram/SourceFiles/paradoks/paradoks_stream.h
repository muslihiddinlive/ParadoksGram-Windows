/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Paradoks::Stream {

// Called for every successfully downloaded live stream part while
// watching a live stream / video chat with a broadcast. Does nothing
// unless the "record streams" option is on.
void OnPart(
	not_null<Main::Session*> session,
	uint64 callId,
	uint64 peerId,
	const QString &title,
	bool canSave,
	int64 timeMs,
	int videoChannel,
	int videoQuality,
	const std::vector<uint8_t> &data);

// Called when the group call object is destroyed, finishes the recording.
void OnCallEnded(uint64 callId);

// Removes raw segments and recordings that are too old.
void Cleanup();

} // namespace Paradoks::Stream
