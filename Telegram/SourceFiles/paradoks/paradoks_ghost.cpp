/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ghost.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "core/application.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"

#include <set>

namespace Paradoks::Ghost {
namespace {

// Sessions that already got the single "offline" packet.
std::set<uint64> StatusSent;

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

bool AllowStatusPacket(not_null<Main::Session*> session) {
	if (!Online()) {
		StatusSent.clear();
		return true;
	}
	const auto first = StatusSent.emplace(session->uniqueId()).second;
	Debug::Log(u"ghost online: status packet %1"_q.arg(first ? u"sent once (offline)"_q : u"blocked"_q));
	return first;
}

void OnOnlineSwitched(bool on) {
	StatusSent.clear();
	if (!on || !Online()) {
		return;
	}
	for (const auto &entry : Core::App().domain().accounts()) {
		if (const auto session = entry.account->maybeSession()) {
			StatusSent.emplace(session->uniqueId());
			session->api().request(MTPaccount_UpdateStatus(
				MTP_bool(true)
			)).send();
		}
	}
}

} // namespace Paradoks::Ghost
