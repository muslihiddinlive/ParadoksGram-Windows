/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class GenericBox;
class PopupMenu;
class Show;
} // namespace Ui

namespace Paradoks {

void SettingsBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller);

void AddEditHistoryAction(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<Ui::Show> show,
	not_null<HistoryItem*> item);

} // namespace Paradoks
