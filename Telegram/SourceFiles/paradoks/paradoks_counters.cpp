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
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
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

void AddChart(
		not_null<Ui::GenericBox*> box,
		std::vector<std::pair<QString, int>> items) {
	constexpr auto kRowHeight = 30;
	constexpr auto kNameWidth = 110;
	constexpr auto kValueWidth = 52;
	const auto widget = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		st::boxRowPadding);
	widget->resize(
		widget->width(),
		int(items.size()) * kRowHeight + 8);
	auto maxValue = 0;
	for (const auto &item : items) {
		maxValue = std::max(maxValue, item.second);
	}
	widget->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(widget);
		p.setRenderHint(QPainter::Antialiasing);
		const auto barLeft = kNameWidth + 8;
		const auto barRight = widget->width() - kValueWidth - 8;
		const auto barWidth = std::max(0, barRight - barLeft);
		auto top = 4;
		for (const auto &item : items) {
			p.setFont(st::normalFont);
			p.setPen(st::windowFg);
			p.drawText(
				QRect(0, top, kNameWidth, kRowHeight),
				Qt::AlignVCenter | Qt::AlignLeft,
				st::normalFont->elided(item.first, kNameWidth));
			const auto barTop = top + (kRowHeight - 10) / 2;
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(QRectF(barLeft, barTop, barWidth, 10), 5, 5);
			if (maxValue > 0 && item.second > 0) {
				const auto filled = std::max(
					10.,
					double(barWidth) * item.second / maxValue);
				p.setBrush(st::activeButtonBg);
				p.drawRoundedRect(QRectF(barLeft, barTop, filled, 10), 5, 5);
			}
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(barRight + 8, top, kValueWidth, kRowHeight),
				Qt::AlignVCenter | Qt::AlignRight,
				QString::number(item.second));
			top += kRowHeight;
		}
	}, widget->lifetime());
}

void CountersBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Chat hisoblagichlari"_q));
	box->setWidth(st::boxWideWidth);
	auto list = std::array<Category, kCategories>();
	list[kPrivate].name = u"Shaxsiy chatlar"_q;
	list[kBots].name = u"Botlar"_q;
	list[kGroups].name = u"Guruhlar"_q;
	list[kChannels].name = u"Kanallar"_q;
	auto total = 0;
	auto unreadMessages = 0;
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
			unreadPeers.push_back(peer->id);
		}
		category.unreadMessages += std::max(unread, 0);
		unreadMessages += std::max(unread, 0);
		if (history->muted()) {
			++category.muted;
			mutedPeers.push_back(peer->id);
		}
	}

	const auto container = box->verticalLayout();
	const auto row = [&](
			const QString &title,
			int count,
			std::vector<PeerId> peers,
			bool openable) {
		const auto button = Settings::AddButtonWithLabel(
			container,
			rpl::single(title),
			rpl::single(QString::number(count)),
			st::settingsButtonNoIcon);
		if (openable) {
			button->setClickedCallback([=] {
				controller->show(Box(
					ListBox,
					controller,
					Shown{ title, peers }));
			});
		}
	};

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Chatlar"_q));
	row(u"Jami chatlar"_q, total, {}, false);
	for (const auto &category : list) {
		row(category.name, category.chats, category.peers, true);
	}

	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Xabarlar"_q));
	const auto unreadChats = int(unreadPeers.size());
	const auto mutedChats = int(mutedPeers.size());
	row(u"O'qilmagan chatlar"_q, unreadChats, std::move(unreadPeers), true);
	row(u"O'qilmagan xabarlar"_q, unreadMessages, {}, false);
	row(u"Ovozi o'chirilgan"_q, mutedChats, std::move(mutedPeers), true);

	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(u"O'qilmagan xabarlar taqsimoti"_q));
	auto chart = std::vector<std::pair<QString, int>>();
	for (const auto &category : list) {
		chart.emplace_back(category.name, category.unreadMessages);
	}
	AddChart(box, std::move(chart));
	Ui::AddSkip(container);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(u"Arxivdagi chatlar hisobga olinmaydi."_q),
			st::boxDividerLabel),
		st::boxRowPadding);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void Open(not_null<Window::SessionController*> controller) {
	controller->show(Box(CountersBox, controller));
}

} // namespace Paradoks::Counters
