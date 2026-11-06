/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class QJsonObject;

namespace Paradoks::WebDev {

// Mini-app developer tools: a log of everything the mini-app and Telegram
// say to each other, the initData of the open mini-app, WebView DevTools.

// Mini-app -> Telegram (a "web_app_*" command with its arguments).
void LogIncoming(const QString &command, const QJsonObject &arguments);
// Telegram -> mini-app (an event with its payload).
void LogOutgoing(const QString &event, const QString &payload);
// The address the mini-app was opened with (initData is in its fragment).
void SetUrl(const QString &url);

// Opens (or raises) the developer window.
void Open();

// Saves the open mini-app page as a ZIP and sends it to Saved Messages
// (the "Mini-appni nusxalash" item of the mini-app three-dot menu).
void CloneCurrent();

// Applies the saved "WebView DevTools" switch (call at start).
void ApplyDevTools();
void SetDevTools(bool enabled);

} // namespace Paradoks::WebDev
