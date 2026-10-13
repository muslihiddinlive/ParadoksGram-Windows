/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ghost.h"

#include "paradoks/paradoks_config.h"
#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "main/main_session.h"

namespace Paradoks::Ghost {
namespace {

[[nodiscard]] bool Active(Flag flag) {
	return Enabled(flag) && InGhostWindow();
}

} // namespace

bool Read() {
	return Active(Flag::GhostRead);
}

bool Typing() {
	return Active(Flag::GhostTyping);
}

bool Online() {
	return Active(Flag::GhostOnline);
}

bool Stories() {
	return Active(Flag::GhostStories);
}

void MarkReadNow(not_null<History*> history) {
	const auto last = history->lastServerMessage();
	if (!last) {
		return;
	}
	const auto api = &history->session().api();
	if (const auto channel = history->peer->asChannel()) {
		api->request(MTPchannels_ReadHistory(
			channel->inputChannel(),
			MTP_int(last->id)
		)).send();
	} else {
		api->request(MTPmessages_ReadHistory(
			history->peer->input(),
			MTP_int(last->id)
		)).send();
	}
}

} // namespace Paradoks::Ghost
