/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <memory>

class HistoryItem;

namespace Ui {
class PopupMenu;
class Show;
} // namespace Ui

namespace Paradoks::Viewer {

// "ParadoksGram ko'rgichida ochish": Word, Excel, PowerPoint, OpenDocument
// and text files open in a window inside the app, with search and zoom.
// Nothing is uploaded anywhere: the text is read from the downloaded file.
void AddViewerAction(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<Ui::Show> show,
	not_null<HistoryItem*> item);

} // namespace Paradoks::Viewer
