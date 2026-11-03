/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_folders.h"

#include "paradoks/paradoks_config.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "data/notify/data_notify_settings.h"
#include "data/notify/data_peer_notify_settings.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Paradoks::Folders {
namespace {

constexpr auto kDefaultKey = "ui/default_folder";

struct Period {
	QString text;
	int seconds = 0; // 0 = unmute, -1 = forever.
};

void MuteFolder(
		not_null<Window::SessionController*> controller,
		FilterId id,
		int seconds) {
	const auto session = &controller->session();
	const auto value = (seconds == 0)
		? Data::MuteValue{ .unmute = true }
		: (seconds < 0)
		? Data::MuteValue{ .forever = true }
		: Data::MuteValue{ .period = seconds };
	auto peers = std::vector<not_null<PeerData*>>();
	for (const auto &row : session->data().chatsFilters().chatsList(
			id)->indexed()->all()) {
		if (const auto history = row->history()) {
			peers.push_back(history->peer);
		}
	}
	for (const auto peer : peers) {
		session->data().notifySettings().update(peer, value);
	}
	controller->showToast((seconds == 0)
		? u"Jilddagi chatlar ovozi yoqildi (%1 ta)"_q.arg(peers.size())
		: u"Jilddagi chatlar ovozsiz qilindi (%1 ta)"_q.arg(peers.size()));
}

void MuteBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		FilterId id) {
	box->setTitle(rpl::single(u"Jildni ovozsiz qilish"_q));
	const auto periods = std::vector<Period>{
		{ u"1 soat"_q, 60 * 60 },
		{ u"8 soat"_q, 8 * 60 * 60 },
		{ u"2 kun"_q, 2 * 24 * 60 * 60 },
		{ u"7 kun"_q, 7 * 24 * 60 * 60 },
		{ u"Doimiy"_q, -1 },
		{ u"Ovozni qaytarish"_q, 0 },
	};
	for (const auto &period : periods) {
		const auto seconds = period.seconds;
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(period.text),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback([=] {
			MuteFolder(controller, id, seconds);
			box->closeBox();
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void AddFolderActions(
		const Ui::Menu::MenuCallback &addAction,
		not_null<Window::SessionController*> controller,
		int filterId) {
	if (!filterId) {
		return;
	}
	addAction(
		u"Sukut qilish..."_q,
		[=] { controller->show(Box(MuteBox, controller, FilterId(filterId))); },
		&st::menuIconMute);
	const auto isDefault = (IntSetting(QString::fromLatin1(kDefaultKey), 0)
		== filterId);
	addAction(
		isDefault
			? u"Odatiy jildni bekor qilish"_q
			: u"Odatiy jild qilib o'rnatish"_q,
		[=] {
			SetIntSetting(
				QString::fromLatin1(kDefaultKey),
				isDefault ? 0 : filterId);
			controller->showToast(isDefault
				? u"Odatiy jild bekor qilindi"_q
				: u"Ilova ochilganda bu jild birinchi ochiladi"_q);
		},
		&st::menuIconAddToFolder);
}

void ApplyDefaultFolder(not_null<Window::SessionController*> controller) {
	static auto applied = false;
	if (applied) {
		return;
	}
	const auto id = IntSetting(QString::fromLatin1(kDefaultKey), 0);
	if (!id) {
		applied = true;
		return;
	}
	for (const auto &filter : controller->session().data().chatsFilters(
			).list()) {
		if (filter.id() == id) {
			applied = true;
			crl::on_main(controller, [=] {
				controller->setActiveChatsFilter(FilterId(id));
			});
			return;
		}
	}
}

} // namespace Paradoks::Folders
