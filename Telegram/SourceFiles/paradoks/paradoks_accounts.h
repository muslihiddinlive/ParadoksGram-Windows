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

namespace Main {
class Session;
} // namespace Main

namespace Paradoks {

// Subscription to the hub channel unlocks the extended account limit.
[[nodiscard]] bool HubMember(uint64 userId);

void RefreshHub(
	not_null<Main::Session*> session,
	Fn<void(bool)> done = nullptr);

void AccountsGateBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session);

} // namespace Paradoks
