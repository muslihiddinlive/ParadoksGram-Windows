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

namespace Paradoks::Hotkey {

// Registers the saved global shortcut (if any). Safe to call repeatedly.
void Start();

// Shortcut text like "Ctrl+Shift+G", empty if none is set.
[[nodiscard]] QString Title();

// Flips the master ghost switch and flashes a small notice.
void ToggleGhost();

// Small always-on-top notice that disappears on its own.
void ShowOverlay(bool ghostOn);

// Box that waits for a key combination and saves it as the shortcut.
void CaptureBox(not_null<Ui::GenericBox*> box, Fn<void()> changed);

} // namespace Paradoks::Hotkey
