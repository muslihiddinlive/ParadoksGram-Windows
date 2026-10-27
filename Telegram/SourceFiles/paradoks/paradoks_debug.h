/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Paradoks::Debug {

// Appends a line to tdata/paradoks_debug.log (thread safe, size limited).
void Log(const QString &text);

[[nodiscard]] QString Path();
[[nodiscard]] QString Tail(int maxBytes = 60000);

} // namespace Paradoks::Debug
