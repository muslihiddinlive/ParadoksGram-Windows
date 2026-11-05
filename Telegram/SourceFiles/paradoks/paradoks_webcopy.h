/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Paradoks::WebCopy {

// Saves the public files of a web page (HTML, CSS, scripts, images) into a
// new folder, with the links rewritten so index.html opens offline.
// Nothing private is sent: plain requests, no cookies, no initData (the
// address fragment is dropped). `done` gets the folder, empty on failure.
void Start(
	const QString &url,
	Fn<void(const QString &)> progress,
	Fn<void(const QString &)> done);

} // namespace Paradoks::WebCopy
