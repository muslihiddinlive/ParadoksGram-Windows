/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Paradoks::Clone {

// Public groups/channels (forwarding allowed) and personal chats.
[[nodiscard]] bool CanClone(not_null<PeerData*> peer);

// Creates a private supergroup "<name>_clon" and forwards the whole
// history of the chat into it, oldest first.
void Start(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

// The list of finished and running clones ("Klonlangan chatlar").
void ShowList(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Clone
