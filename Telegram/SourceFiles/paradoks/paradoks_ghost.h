/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;

namespace Paradoks::Ghost {

[[nodiscard]] bool Read();
[[nodiscard]] bool Typing();
[[nodiscard]] bool Online();
[[nodiscard]] bool Stories();

void MarkReadNow(not_null<History*> history);

} // namespace Paradoks::Ghost
