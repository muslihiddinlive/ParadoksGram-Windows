/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ui.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_store.h"
#include "base/unixtime.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Paradoks {
namespace {

constexpr auto kRadarLimit = 100;

[[nodiscard]] QString FormatDate(TimeId date) {
	return base::unixtime::parse(date).toString(u"dd.MM.yyyy HH:mm"_q);
}

void AddToggle(
		not_null<Ui::GenericBox*> box,
		Flag flag,
		const QString &title) {
	const auto button = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(title),
			st::settingsButtonNoIcon),
		style::margins());
	button->toggleOn(
		rpl::single(Enabled(flag))
	)->toggledValue() | rpl::on_next([=](bool value) {
		SetEnabled(flag, value);
	}, button->lifetime());
}

void AddText(not_null<Ui::GenericBox*> box, const QString &text) {
	box->addRow(object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
}

void RadarBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"O'chirilganlar radari"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto list = DeletedList();
	if (list.empty()) {
		AddText(box, u"Hozircha o'chirilgan xabar yo'q."_q);
		return;
	}
	const auto from = std::max(int(list.size()) - kRadarLimit, 0);
	for (auto i = int(list.size()) - 1; i >= from; --i) {
		const auto &entry = list[i];
		const auto text = entry.text.isEmpty()
			? u"(matnsiz)"_q
			: entry.text;
		AddText(box, u"%1\n%2: %3\n(o'chirildi: %4)"_q.arg(
			FormatDate(entry.date),
			entry.from,
			text,
			FormatDate(entry.deletedAt)));
	}
}

void EditsBox(
		not_null<Ui::GenericBox*> box,
		std::vector<EditVersion> versions,
		QString current) {
	box->setTitle(rpl::single(
		u"Tahrirlar tarixi (%1)"_q.arg(versions.size())));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	auto index = 0;
	for (const auto &version : versions) {
		AddText(box, u"%1) %2\n%3"_q.arg(
			++index).arg(
			FormatDate(version.date),
			version.text.isEmpty() ? u"(matnsiz)"_q : version.text));
	}
	AddText(box, u"Hozirgi variant:\n"_q + current);
}

} // namespace

void SettingsBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"ParadoksGram"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto container = box->verticalLayout();
	Ui::AddSubsectionTitle(container, rpl::single(u"Umumiy"_q));
	AddToggle(box, Flag::ShowId, u"ID ni ko'rsatish"_q);
	AddToggle(
		box,
		Flag::KeepDeleted,
		u"O'chirilgan xabarlarni saqlash"_q);
	AddToggle(
		box,
		Flag::KeepDeletedMedia,
		u"O'chirilgan media'ni saqlash"_q);
	AddToggle(box, Flag::TrackEdits, u"Tahrirlar tarixini saqlash"_q);
	AddToggle(
		box,
		Flag::HideSponsored,
		u"Reklamani yashirish (kanal va botlar)"_q);

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Ghost mode"_q));
	AddToggle(box, Flag::GhostRead, u"O'qilgan belgisini yashirish"_q);
	AddToggle(box, Flag::GhostTyping, u"\"Yozyapti...\" ni yashirish"_q);
	AddToggle(box, Flag::GhostOnline, u"Online holatini yashirish"_q);
	AddToggle(box, Flag::GhostStories, u"Story ko'rganimni yashirish"_q);
	AddToggle(
		box,
		Flag::GhostSchedule,
		u"Faqat 22:00 dan 08:00 gacha ishlash"_q);

	Ui::AddSkip(container);
	const auto radar = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"O'chirilganlar radari"_q),
			st::settingsButtonNoIcon),
		style::margins());
	radar->setClickedCallback([=] {
		box->getDelegate()->show(Box(RadarBox));
	});
	AddText(
		box,
		u"Sozlamalar qayta ishga tushirmasdan darhol ishlaydi. "
		"Ba'zi o'zgarishlar (masalan, profildagi ID) keyingi ochilishda ko'rinadi."_q);
}

void AddEditHistoryAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	auto versions = EditsFor(item);
	if (versions.empty()) {
		return;
	}
	const auto current = item->originalText().text;
	menu->addAction(u"Tahrirlar tarixi"_q, [=] {
		show->showBox(Box(EditsBox, versions, current));
	}, &st::menuIconInfo);
}

} // namespace Paradoks
