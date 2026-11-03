/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

namespace Paradoks::Folders {

// Extra items of the folder tab menu (like the Android screen): mute the
// whole folder for a time, make the folder the one opened at start.
void AddFolderActions(
	const Ui::Menu::MenuCallback &addAction,
	not_null<Window::SessionController*> controller,
	int filterId);

// Opens the default folder once after the folder list is known.
void ApplyDefaultFolder(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Folders
