/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_peer_id.h"
#include "ui/widgets/menu/menu_add_action_callback.h"

class History;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Paradoks {

// Hidden chats leave the chat list and never notify (app-level only).
[[nodiscard]] bool IsHidden(not_null<const History*> history);

// Locked chats ask for the PIN each time they are opened.
[[nodiscard]] bool NeedsUnlock(
	not_null<Main::Session*> session,
	PeerId peerId);
void NoteChatShown(not_null<Main::Session*> session, PeerId peerId);
void AskUnlock(
	not_null<Window::SessionController*> controller,
	PeerId peerId,
	Fn<void()> done);

void AddChatActions(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	const Ui::Menu::MenuCallback &add);

void ShowHiddenChats(not_null<Window::SessionController*> controller);

// If `text` is exactly the PIN, opens the hidden chats list and returns true.
[[nodiscard]] bool TryOpenByPin(
	not_null<Window::SessionController*> controller,
	const QString &text);
void ManagePin(not_null<Window::SessionController*> controller);

} // namespace Paradoks
