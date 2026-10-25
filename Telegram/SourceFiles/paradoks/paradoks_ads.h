/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/history_item.h" // MTP types.

namespace Paradoks::Ads {

// Own ads are shown instead of Telegram's sponsored messages while those are
// hidden. The server address comes from the GitHub build (see
// paradoks_ads_server.h); nothing about the user or the chat is sent to it,
// only the ad number, the event kind and a random anonymous install id.
[[nodiscard]] bool Active();

// Starts a background refresh of the ad list (cheap to call often).
void Refresh();

// An ad packed like Telegram's answer, or an empty answer if nothing fits.
[[nodiscard]] MTPmessages_sponsoredMessages Build(bool bot);

[[nodiscard]] bool IsOurs(const QByteArray &randomId);
void OnView(const QByteArray &randomId);
void OnClick(const QByteArray &randomId);

} // namespace Paradoks::Ads
