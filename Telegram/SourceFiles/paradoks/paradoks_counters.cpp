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
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <array>

namespace Paradoks::Counters {
namespace {

struct Category {
	QString name;
	int chats = 0;
	int unreadChats = 0;
	int unreadMessages = 0;
	int muted = 0;
};

enum { kPrivate, kBots, kGroups, kChannels, kCategories };

[[nodiscard]] QString Bar(int value, int max) {
	const auto length = 16;
	const auto filled = (max > 0)
		? std::max(value > 0 ? 1 : 0, value * length / max)
		: 0;
	return QString(QChar(0x2588), filled)
		+ QString(QChar(0x2591), length - filled);
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
		const auto unread = history->unreadCount();
		if (unread > 0 || history->unreadMark()) {
			++category.unreadChats;
			++unreadTotal;
		}
		category.unreadMessages += std::max(unread, 0);
		if (history->muted()) {
			++category.muted;
			++mutedTotal;
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
	add(u"Arxivdagi chatlar hisobga olinmaydi."_q);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void Open(not_null<Window::SessionController*> controller) {
	controller->show(Box(CountersBox, controller));
}

} // namespace Paradoks::Counters
