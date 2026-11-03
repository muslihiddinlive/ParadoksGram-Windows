/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_counters.h"

#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <array>
#include <vector>

namespace Paradoks::Counters {
namespace {

struct Category {
	QString name;
	int chats = 0;
	int unreadChats = 0;
	int unreadMessages = 0;
	int muted = 0;
	std::vector<PeerId> peers;
};

struct Shown {
	QString title;
	std::vector<PeerId> peers;
};

enum { kPrivate, kBots, kGroups, kChannels, kCategories };

[[nodiscard]] QString Bar(int value, int max) {
	const auto length = 16;
	const auto filled = (max > 0)
		? std::max(value > 0 ? 1 : 0, value * length / max)
		: 0;
	return QString(filled, QChar(0x2588))
		+ QString(length - filled, QChar(0x2591));
}

void ListBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		Shown shown) {
	box->setTitle(rpl::single(shown.title));
	box->setWidth(st::boxWideWidth);
	auto count = 0;
	for (const auto id : shown.peers) {
		const auto peer = controller->session().data().peerLoaded(id);
		if (!peer) {
			continue;
		}
		const auto history = controller->session().data().history(peer);
		const auto unread = history->unreadCount();
		auto text = peer->name();
		if (unread > 0) {
			text += u"  \u2022  "_q + QString::number(unread);
		}
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(text),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback([=] {
			controller->showPeerHistory(peer);
			box->closeBox();
		});
		if (++count >= 300) {
			break;
		}
	}
	if (!count) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(u"Bo'sh."_q),
				st::boxLabel),
			st::boxRowPadding);
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void CountersBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Chat hisoblagichlari"_q));
	auto list = std::array<Category, kCategories>();
	list[kPrivate].name = u"Shaxsiy chatlar"_q;
	list[kBots].name = u"Botlar"_q;
	list[kGroups].name = u"Guruhlar"_q;
	list[kChannels].name = u"Kanallar"_q;
	auto total = 0;
	auto unreadTotal = 0;
	auto mutedTotal = 0;
	auto unreadPeers = std::vector<PeerId>();
	auto mutedPeers = std::vector<PeerId>();
	for (const auto &row : controller->session().data().chatsList()->indexed()->all()) {
		const auto history = row->history();
		if (!history) {
			continue;
		}
		const auto peer = history->peer;
		const auto user = peer->asUser();
		const auto channel = peer->asChannel();
		auto index = kPrivate;
		if (user) {
			index = user->isBot() ? kBots : kPrivate;
		} else if (channel && !channel->isMegagroup()) {
			index = kChannels;
		} else {
			index = kGroups;
		}
		auto &category = list[index];
		++category.chats;
		++total;
		category.peers.push_back(peer->id);
		const auto unread = history->unreadCount();
		if (unread > 0 || history->unreadMark()) {
			++category.unreadChats;
			++unreadTotal;
			unreadPeers.push_back(peer->id);
		}
		category.unreadMessages += std::max(unread, 0);
		if (history->muted()) {
			++category.muted;
			++mutedTotal;
			mutedPeers.push_back(peer->id);
		}
	}
	const auto add = [&](const QString &text) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(text),
				st::boxLabel),
			st::boxRowPadding);
	};
	add(u"Jami: %1 ta chat  •  o'qilmagan: %2  •  ovozsiz: %3"_q.arg(
		total).arg(unreadTotal).arg(mutedTotal));
	auto maxUnread = 0;
	for (const auto &category : list) {
		maxUnread = std::max(maxUnread, category.unreadMessages);
	}
	for (const auto &category : list) {
		add(u"%1: %2 ta\n   o'qilmagan chat: %3, xabar: %4, ovozsiz: %5\n   %6"_q
			.arg(category.name)
			.arg(category.chats)
			.arg(category.unreadChats)
			.arg(category.unreadMessages)
			.arg(category.muted)
			.arg(Bar(category.unreadMessages, maxUnread)));
	}
	add(u"Ro'yxatni ochish (chatni bosib o'tasiz):"_q);
	const auto open = [&](const QString &title, std::vector<PeerId> peers) {
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(u"%1 (%2)"_q.arg(title).arg(peers.size())),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback([=] {
			controller->show(Box(
				ListBox,
				controller,
				Shown{ title, peers }));
		});
	};
	for (const auto &category : list) {
		open(category.name, category.peers);
	}
	open(u"O'qilmagan chatlar"_q, std::move(unreadPeers));
	open(u"Ovozsiz chatlar"_q, std::move(mutedPeers));
	add(u"Arxivdagi chatlar hisobga olinmaydi."_q);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void Open(not_null<Window::SessionController*> controller) {
	controller->show(Box(CountersBox, controller));
}

} // namespace Paradoks::Counters
