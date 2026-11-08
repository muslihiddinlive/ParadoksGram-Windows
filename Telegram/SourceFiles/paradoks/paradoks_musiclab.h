/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class DocumentData;

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Paradoks::MusicLab {

// "Musiqiy ijodxona": a native (not a mini app) music studio. Opens for a
// song document; downloads it first if it is not on the disk yet.
void Open(
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	FullMsgId contextId);

// "Musiqiy ijodxonani ochish" item for context menus (message, player bar).
void AddMenuAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	FullMsgId contextId);

} // namespace Paradoks::MusicLab
