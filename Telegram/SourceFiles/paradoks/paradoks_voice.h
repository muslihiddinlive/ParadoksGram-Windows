/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Paradoks::Voice {

enum class Mode : uchar {
	Off,
	Robot,
	Cave,
	Villager,
};

[[nodiscard]] Mode CurrentMode();
void SetMode(Mode mode);
[[nodiscard]] QString ModeTitle(Mode mode);

// Streaming effects on 16-bit mono PCM; state survives between calls.
void Reset();
void Process(short *samples, int count, int sampleRate);

} // namespace Paradoks::Voice
