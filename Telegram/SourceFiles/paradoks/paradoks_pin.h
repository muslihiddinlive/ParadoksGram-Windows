/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Paradoks::Pin {

// Fills the box with the PIN screen like on Android: dark background, lock
// icon, dots and a round number pad (keyboard digits work too).
// With `validate` it asks for the existing PIN, without it asks to choose
// a new PIN twice. `done` gets the entered PIN.
void BuildPinScreen(
	not_null<Ui::GenericBox*> box,
	const QString &title,
	const QString &subtitle,
	Fn<bool(const QString&)> validate,
	Fn<void(const QString&)> done);

// The same screen with a 3x3 dot grid: the code is drawn with the mouse.
// The code is the dot numbers in order ("01258"), at least 4 dots.
void BuildPatternScreen(
	not_null<Ui::GenericBox*> box,
	const QString &title,
	const QString &subtitle,
	Fn<bool(const QString&)> validate,
	Fn<void(const QString&)> done);

} // namespace Paradoks::Pin
