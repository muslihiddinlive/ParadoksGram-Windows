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

namespace Paradoks::Backup {

// Settings -> JSON file ("paradoksgram-settings-backup") and back. Only a
// fixed list of keys is ever written or read; secrets (device token, PIN
// hash, hidden chat lists) are not included.
void Export(not_null<Window::SessionController*> controller);
void Import(not_null<Window::SessionController*> controller);

} // namespace Paradoks::Backup
