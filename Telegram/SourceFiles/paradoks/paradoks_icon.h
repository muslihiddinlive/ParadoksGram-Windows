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

namespace Paradoks::AppIcon {

constexpr auto kVariants = 3; // Ghost, Telegram, Neon ghost.

// Selected variant (0 .. kVariants - 1).
[[nodiscard]] int Current();

// Qt resource path of the logo for a variant.
[[nodiscard]] QString ResourcePath(int variant, bool noMargin);

// Saves the choice and refreshes window, tray and shortcut icons.
void Apply(int variant);

// Box with the icon choices.
void PickerBox(not_null<Ui::GenericBox*> box);

} // namespace Paradoks::AppIcon
