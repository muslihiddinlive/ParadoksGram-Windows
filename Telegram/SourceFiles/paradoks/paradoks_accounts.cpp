/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_accounts.h"

#include "apiwrap.h"
#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/basic_click_handlers.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"

#include <QtCore/QSettings>

#include <map>

namespace Paradoks {
namespace {

[[nodiscard]] QString HubUsername() {
	return u"ParadoksHub"_q;
}

[[nodiscard]] QString IniPath() {
	return cWorkingDir() + u"tdata/paradoks.ini"_q;
}

[[nodiscard]] std::map<uint64, bool> &HubState() {
	static auto state = [] {
		auto result = std::map<uint64, bool>();
		auto settings = QSettings(IniPath(), QSettings::IniFormat);
		settings.beginGroup(u"hub"_q);
		for (const auto &key : settings.childKeys()) {
			result[key.toULongLong()] = settings.value(key).toBool();
		}
		return result;
	}();
	return state;
}

void SetHubMember(uint64 userId, bool member) {
	auto &state = HubState();
	const auto i = state.find(userId);
	if (i != state.end() && i->second == member) {
		return;
	}
	state[userId] = member;
	auto settings = QSettings(IniPath(), QSettings::IniFormat);
	settings.beginGroup(u"hub"_q);
	settings.setValue(QString::number(userId), member);
	settings.sync();
	Core::App().domain().updateMaxAccounts();
}

} // namespace

bool HubMember(uint64 userId) {
	const auto &state = HubState();
	const auto i = state.find(userId);
	return (i != state.end()) && i->second;
}

void RefreshHub(not_null<Main::Session*> session, Fn<void(bool)> done) {
	const auto userId = uint64(session->userId().bare);
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(HubUsername()),
		MTPstring()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(data.vpeer()));
		const auto channel = peer ? peer->asChannel() : nullptr;
		const auto member = channel && channel->amIn();
		SetHubMember(userId, member);
		if (done) {
			done(member);
		}
	}).fail([=] {
		if (done) {
			done(HubMember(userId));
		}
	}).send();
}

void AccountsGateBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setTitle(rpl::single(u"Ko'proq akkaunt"_q));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Standart limit: 3 ta akkaunt. 10 tagacha ochish uchun "
		"@%1 kanaliga obuna bo'ling va \"Tekshirish\" tugmasini bosing."_q
			.arg(HubUsername()),
		st::boxLabel));
	const auto show = box->uiShow();
	box->addButton(rpl::single(u"Tekshirish"_q), [=] {
		RefreshHub(session, [=](bool member) {
			if (member) {
				show->showToast(u"Obuna tasdiqlandi, limit 10 taga ochildi."_q);
			} else {
				show->showToast(u"Obuna topilmadi. Avval kanalga obuna bo'ling."_q);
			}
		});
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->addLeftButton(rpl::single(u"Obuna bo'lish"_q), [=] {
		UrlClickHandler::Open(u"https://t.me/"_q + HubUsername());
	});
}

} // namespace Paradoks
