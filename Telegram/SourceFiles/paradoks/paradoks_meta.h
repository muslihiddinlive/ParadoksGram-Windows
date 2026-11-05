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

namespace Paradoks::Meta {

// "Metadata (EXIF)": Telegram removes the metadata of pictures sent as
// photos, but keeps it when the same picture is sent as a file. This shows
// what a received image file carries: camera, date, software and, if it was
// not removed, the GPS place. Everything is read from the downloaded file.
void AddMetaAction(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<Ui::Show> show,
	not_null<HistoryItem*> item);

} // namespace Paradoks::Meta
