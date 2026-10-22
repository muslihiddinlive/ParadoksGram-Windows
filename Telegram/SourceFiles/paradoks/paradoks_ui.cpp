/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ui.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_hotkey.h"
#include "paradoks/paradoks_privacy.h"
#include "paradoks/paradoks_store.h"
#include "paradoks/paradoks_update.h"
#include "paradoks/paradoks_voice.h"
#include "window/window_session_controller.h"
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

void AddShortcutRow(
		not_null<Ui::GenericBox*> box,
		Hotkey::Slot slot,
		const QString &name) {
	const auto shortcutTitle = [=] {
		const auto title = Hotkey::Title(slot);
		return name + u": "_q
			+ (title.isEmpty() ? u"o'rnatilmagan"_q : title);
	};
	const auto shortcutLabel = std::make_shared<rpl::variable<QString>>(
		shortcutTitle());
	const auto shortcut = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			shortcutLabel->value(),
			st::settingsButtonNoIcon),
		style::margins());
	shortcut->setClickedCallback([=] {
		box->getDelegate()->show(Box(
			Hotkey::CaptureBox,
			slot,
			Fn<void()>([=] { *shortcutLabel = shortcutTitle(); })));
	});
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

void SettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"ParadoksGram"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto container = box->verticalLayout();

	const auto updateTitle = [] {
		const auto available = Update::AvailableBuild();
		return (available > 0)
			? u"Yangi versiya bor: build-%1. Yangilash"_q.arg(available)
			: (Update::CurrentBuild() > 0)
			? u"Yangilanishni tekshirish (hozir: build-%1)"_q.arg(
				Update::CurrentBuild())
			: u"Yangilanish (mahalliy build, o'chiq)"_q;
	};
	const auto updateLabel = std::make_shared<rpl::variable<QString>>(
		updateTitle());
	const auto update = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			updateLabel->value(),
			st::settingsButtonNoIcon),
		style::margins());
	update->setClickedCallback([=] {
		if (Update::AvailableBuild() > 0) {
			Update::ShowBox(controller);
			return;
		}
		Update::Check([=](Update::Result result) {
			*updateLabel = updateTitle();
			if (result == Update::Result::Newer) {
				Update::ShowBox(controller);
			} else if (result == Update::Result::Latest) {
				controller->showToast(u"Eng yangi versiyadasiz"_q);
			} else {
				controller->showToast(
					u"Tekshirib bo'lmadi. Internetni tekshiring"_q);
			}
		});
	});
	Ui::AddSkip(container);

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
	const auto master = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Ghost mode (hammasi birga)"_q),
			st::settingsButtonNoIcon),
		style::margins());
	master->toggleOn(
		GhostAllValue()
	)->toggledChanges(
	) | rpl::filter([=](bool on) {
		return (on != GhostAll());
	}) | rpl::on_next([=](bool on) {
		SetGhostAll(on);
	}, master->lifetime());
	AddShortcutRow(box, Hotkey::Slot::Ghost, u"Ghost shortcut"_q);
	AddText(
		box,
		u"Yoki alohida yoqing:"_q);
	AddToggle(box, Flag::GhostRead, u"O'qilgan belgisini yashirish"_q);
	AddToggle(box, Flag::GhostTyping, u"\"Yozyapti...\" ni yashirish"_q);
	AddToggle(box, Flag::GhostOnline, u"Online holatini yashirish"_q);
	AddToggle(box, Flag::GhostStories, u"Story ko'rganimni yashirish"_q);
	AddToggle(
		box,
		Flag::GhostSchedule,
		u"Faqat 22:00 dan 08:00 gacha ishlash"_q);

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Ovoz effekti"_q));
	const auto voiceTitle = [] {
		return u"Ovozli xabar effekti: "_q
			+ Voice::ModeTitle(Voice::CurrentMode());
	};
	const auto voiceLabel = box->lifetime().make_state<
		rpl::variable<QString>>(voiceTitle());
	const auto voice = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			voiceLabel->value(),
			st::settingsButtonNoIcon),
		style::margins());
	voice->setClickedCallback([=] {
		const auto next = (int(Voice::CurrentMode()) + 1)
			% (int(Voice::Mode::Villager) + 1);
		Voice::SetMode(Voice::Mode(next));
		*voiceLabel = voiceTitle();
	});
	AddText(
		box,
		u"Bosib Off, Robot, G'or va Minecraft Villager orasida almashtiring. "
		"Effekt yangi yozilayotgan ovozli xabarlarga qo'llanadi."_q);

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Maxfiylik"_q));
	const auto hidden = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Yashirin chatlar"_q),
			st::settingsButtonNoIcon),
		style::margins());
	hidden->setClickedCallback([=] {
		Paradoks::ShowHiddenChats(controller);
	});
	const auto pin = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Chat qulfi PIN"_q),
			st::settingsButtonNoIcon),
		style::margins());
	pin->setClickedCallback([=] {
		Paradoks::ManagePin(controller);
	});
	AddShortcutRow(
		box,
		Hotkey::Slot::Hidden,
		u"Yashirin chatlar shortcut"_q);
	AddText(
		box,
		u"Chatni yashirish yoki qulflash uchun chatlar ro'yxatida chat ustiga "
		"o'ng tugma bosing. Yashirin chatlarni ochish: qidiruv maydoniga "
		"PIN ni yozing, yoki \u2630 menyudagi Ghost mode belgisini uzoq bosib "
		"turing, yoki yuqoridagi shortcut."_q);

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
