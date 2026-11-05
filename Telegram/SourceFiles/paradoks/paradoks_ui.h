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

class DocumentData;

namespace Paradoks {

// Stickers: asks whether to save only this sticker or the whole pack.
void SaveStickerChoice(
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	Fn<void()> saveSingle);

void SettingsBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller);

// Copies the ID of a sticker / GIF / custom emoji of the message.
void AddIdActions(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<Ui::Show> show,
	not_null<HistoryItem*> item);

void AddEditHistoryAction(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<Ui::Show> show,
	not_null<HistoryItem*> item);

// "Xabar xususiyatlari": ids, dates, forward info and media details.
void AddInspectAction(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<Ui::Show> show,
	not_null<HistoryItem*> item);

} // namespace Paradoks
