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
// The network request itself stays private: plain requests, no cookies, no
// initData (the address fragment is never sent to the server).
// `done` gets the folder, empty on failure. With `single` the result is not
// a ZIP but one self-contained .html file (CSS, scripts, images, fonts and
// video inlined as data: URIs); `done` then gets the path of that file.
//
// `initData` is never sent anywhere either: when not empty, it is only
// baked into the saved copy itself, as a small script that sets
// location.hash before any other script on the page runs, the same way the
// value of #tgWebAppData=... normally reaches a mini-app. This lets the
// saved copy run with real-looking initData when it is later reopened in a
// browser (outside Telegram). Pass the person's own captured value (the
// clone keeps working, hash included) or a value with the identifying
// fields replaced (the clone runs, but its hash and signature are no
// longer valid, so a server that checks them will reject it).
void Start(
	const QString &url,
	bool single,
	const QString &initData,
	Fn<void(const QString &)> progress,
	Fn<void(const QString &)> done);

} // namespace Paradoks::WebCopy
