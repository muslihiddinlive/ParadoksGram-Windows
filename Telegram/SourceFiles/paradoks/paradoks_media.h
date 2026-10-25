/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Paradoks {

// Pre-saving of incoming media, forwarding to the "deleted media database"
// chat and the first cleanup. Called once per logged in session.
void InstallForSession(not_null<Main::Session*> session);

// Called when a deletion of someone else's message was intercepted.
void KeepMediaFor(not_null<HistoryItem*> item);
void NotifyDeleted(not_null<HistoryItem*> item);

// Permanent copy of the media of a deleted message (empty if none).
[[nodiscard]] QString SavedMediaPath(not_null<const HistoryItem*> item);
[[nodiscard]] QString SavedMediaPath(uint64 peer, int64 id);
// Message of the "deleted media database" that holds a copy (peer, id).
[[nodiscard]] std::optional<std::pair<uint64, int64>> DbCopyFor(
	uint64 peer,
	int64 id);
// Tries again to save the media of a deleted message.
void RetryMedia(not_null<HistoryItem*> item);
[[nodiscard]] int64 SavedMediaBytes();
void CleanupMedia();

[[nodiscard]] PeerId DbTarget(not_null<Main::Session*> session);
void SetDbTarget(not_null<Main::Session*> session, PeerId peerId);
[[nodiscard]] QString DbTargetName(not_null<Main::Session*> session);
void ChooseDbTarget(
	not_null<Window::SessionController*> controller,
	Fn<void()> done);

// "Open saved copy" / "Show in folder" / "Open database copy" for deleted
// messages with media.
void AddSavedMediaActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Paradoks
