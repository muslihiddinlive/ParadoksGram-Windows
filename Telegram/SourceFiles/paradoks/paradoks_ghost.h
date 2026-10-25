/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;

namespace Main {
class Session;
} // namespace Main

namespace Paradoks::Ghost {

[[nodiscard]] bool Read();
[[nodiscard]] bool Typing();
[[nodiscard]] bool Online();
[[nodiscard]] bool Stories();

void MarkReadNow(not_null<History*> history);

// While ghost "online" is active the "offline" status is sent only once
// (so "last seen" stays frozen) instead of at every status refresh.
[[nodiscard]] bool AllowStatusPacket(not_null<Main::Session*> session);
// Called when the "online" ghost part is switched on or off.
void OnOnlineSwitched(bool on);

} // namespace Paradoks::Ghost
