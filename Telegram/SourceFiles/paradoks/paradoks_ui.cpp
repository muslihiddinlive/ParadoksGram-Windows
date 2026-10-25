/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ui.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_hotkey.h"
#include "paradoks/paradoks_icon.h"
#include "paradoks/paradoks_privacy.h"
#include "paradoks/paradoks_store.h"
#include "paradoks/paradoks_update.h"
#include "paradoks/paradoks_voice.h"
#include "window/window_session_controller.h"
#include "base/unixtime.h"
#include "base/timer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/text/text_entity.h"
#include "window/section_widget.h"
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

void AddMarked(not_null<Ui::GenericBox*> box, TextWithEntities text) {
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(std::move(text)),
		st::boxLabel));
}

[[nodiscard]] QStringList SplitWords(const QString &text) {
	auto result = QStringList();
	auto token = QString();
	auto space = false;
	for (const auto ch : text) {
		const auto now = ch.isSpace();
		if (!token.isEmpty() && now != space) {
			result.push_back(token);
			token.clear();
		}
		space = now;
		token.append(ch);
	}
	if (!token.isEmpty()) {
		result.push_back(token);
	}
	return result;
}

// Word level difference: removed words are struck out, added ones are
// underlined.
[[nodiscard]] TextWithEntities DiffText(
		const QString &before,
		const QString &after) {
	const auto a = SplitWords(before);
	const auto b = SplitWords(after);
	const auto n = int(a.size());
	const auto m = int(b.size());
	auto result = TextWithEntities();
	if (n * m > 160000) {
		result.text = after;
		return result;
	}
	const auto append = [&](const QString &token, EntityType type, bool mark) {
		const auto offset = int(result.text.size());
		result.text.append(token);
		if (mark) {
			result.entities.push_back(
				EntityInText(type, offset, int(token.size())));
		}
	};
	auto lcs = std::vector<std::vector<int>>(
		n + 1,
		std::vector<int>(m + 1, 0));
	for (auto i = n - 1; i >= 0; --i) {
		for (auto j = m - 1; j >= 0; --j) {
			lcs[i][j] = (a[i] == b[j])
				? (lcs[i + 1][j + 1] + 1)
				: std::max(lcs[i + 1][j], lcs[i][j + 1]);
		}
	}
	auto i = 0;
	auto j = 0;
	while (i < n && j < m) {
		if (a[i] == b[j]) {
			append(b[j], EntityType::Bold, false);
			++i;
			++j;
		} else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
			append(a[i++], EntityType::StrikeOut, true);
		} else {
			append(b[j++], EntityType::Underline, true);
		}
	}
	while (i < n) {
		append(a[i++], EntityType::StrikeOut, true);
	}
	while (j < m) {
		append(b[j++], EntityType::Underline, true);
	}
	return result;
}

[[nodiscard]] bool Reachable(
		not_null<Window::SessionController*> controller,
		uint64 peer) {
	const auto session = &controller->session();
	const auto peerId = PeerId(peer);
	return !IsHiddenPeer(session, peerId) && !NeedsUnlock(session, peerId);
}

void RadarBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"O'chirilganlar radari"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto list = DeletedList();
	auto shown = 0;
	for (auto i = int(list.size()) - 1; i >= 0 && shown < kRadarLimit; --i) {
		const auto &entry = list[i];
		if (!Reachable(controller, entry.peer)) {
			continue;
		}
		++shown;
		const auto text = entry.text.isEmpty()
			? u"(matnsiz)"_q
			: entry.text;
		AddText(box, u"%1\n%2: %3\n(o'chirildi: %4)"_q.arg(
			FormatDate(entry.date),
			entry.from,
			text,
			FormatDate(entry.deletedAt)));
	}
	if (!shown) {
		AddText(box, u"Hozircha o'chirilgan xabar yo'q."_q);
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
	for (auto i = 0; i != int(versions.size()); ++i) {
		const auto &version = versions[i];
		const auto next = (i + 1 < int(versions.size()))
			? versions[i + 1].text
			: current;
		AddText(box, u"%1) %2\n%3"_q.arg(
			++index).arg(
			FormatDate(version.date),
			version.text.isEmpty() ? u"(matnsiz)"_q : version.text));
		auto change = TextWithEntities{ u"O'zgarish:  "_q };
		change.append(DiffText(version.text, next));
		AddMarked(box, std::move(change));
	}
	AddText(box, u"Hozirgi variant:\n"_q + current);
}

void EditedRadarBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Tahrirlanganlar radari"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto session = &controller->session();
	const auto list = EditedList();
	auto shown = 0;
	for (const auto &entry : list) {
		if (shown >= kRadarLimit) {
			break;
		} else if (!Reachable(controller, entry.peer) || entry.versions.empty()) {
			continue;
		}
		++shown;
		const auto peerId = PeerId(entry.peer);
		const auto msgId = MsgId(entry.id);
		const auto &last = entry.versions.back();
		const auto item = session->data().message(peerId, msgId);
		const auto current = item ? item->originalText().text : QString();
		const auto when = entry.changedAt ? entry.changedAt : last.date;

		AddText(box, u"%1\n%2 · %3\n%4 marta tahrirlangan"_q.arg(
			FormatDate(when),
			entry.chat.isEmpty() ? u"(chat)"_q : entry.chat,
			entry.from.isEmpty() ? u"(noma'lum)"_q : entry.from).arg(
			int(entry.versions.size())));
		if (item) {
			auto change = TextWithEntities{ u"So'nggi o'zgarish:  "_q };
			change.append(DiffText(last.text, current));
			AddMarked(box, std::move(change));
		} else {
			AddText(box, u"Oldingi variant:\n"_q
				+ (last.text.isEmpty() ? u"(matnsiz)"_q : last.text));
		}
		const auto versions = entry.versions;
		box->addRow(object_ptr<Ui::LinkButton>(
			box,
			u"Hamma versiyalar"_q))->setClickedCallback([=] {
			box->getDelegate()->show(Box(
				EditsBox,
				versions,
				current.isEmpty() ? u"(yuklanmagan)"_q : current));
		});
		box->addRow(object_ptr<Ui::LinkButton>(
			box,
			u"Xabarga o'tish"_q))->setClickedCallback([=] {
			box->closeBox();
			controller->showPeerHistory(
				peerId,
				Window::SectionShow::Way::Forward,
				msgId);
		});
		Ui::AddSkip(box->verticalLayout());
	}
	if (!shown) {
		AddText(box, u"Hozircha tahrirlangan xabar yo'q."_q);
	}
}

[[nodiscard]] QString SpeedText(double bytesPerSecond) {
	return u"%1 MB/s (%2 Mbit/s)"_q
		.arg(bytesPerSecond / (1024. * 1024.), 0, 'f', 1)
		.arg(bytesPerSecond * 8. / 1000000., 0, 'f', 0);
}

[[nodiscard]] QString SpeedLabel() {
	return u"Yuklash: "_q + SpeedText(DownloadSpeed())
		+ u"  \u00B7  eng yuqori: "_q + SpeedText(DownloadPeak());
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
			? u"Yangi versiya bor: V%1. Yangilash"_q.arg(available)
			: (Update::CurrentBuild() > 0)
			? u"Yangilanishni tekshirish (hozir: V%1)"_q.arg(
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
	Ui::AddSubsectionTitle(container, rpl::single(u"Ilova ikonkasi"_q));
	const auto iconButton = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Ikonkani almashtirish (Ghost / Telegram / Neon / Matrix)"_q),
			st::settingsButtonNoIcon),
		style::margins());
	iconButton->setClickedCallback([=] {
		controller->show(Box(Paradoks::AppIcon::PickerBox));
	});

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Yuklash tezligi"_q));
	const auto speedLabel = box->lifetime().make_state<
		rpl::variable<QString>>(SpeedLabel());
	const auto speedButton = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			speedLabel->value(),
			st::settingsButtonNoIcon),
		style::margins());
	speedButton->setClickedCallback([=] {
		ResetDownloadPeak();
		*speedLabel = SpeedLabel();
	});
	const auto speedTimer = box->lifetime().make_state<base::Timer>([=] {
		*speedLabel = SpeedLabel();
	});
	speedTimer->callEach(500);
	AddText(
		box,
		u"Telegram'dan fayl yuklab olinayotganda tezlik shu yerda "
		"ko'rinadi. Eng yuqori qiymatni nolga tushirish uchun bosing."_q);

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
		box->getDelegate()->show(Box(RadarBox, controller));
	});
	const auto editedRadar = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Tahrirlanganlar radari"_q),
			st::settingsButtonNoIcon),
		style::margins());
	editedRadar->setClickedCallback([=] {
		box->getDelegate()->show(Box(EditedRadarBox, controller));
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
	menu->addAction(u"Tahrirlar tarixi (%1)"_q.arg(versions.size()), [=] {
		show->showBox(Box(EditsBox, versions, current));
	}, &st::menuIconInfo);
}

} // namespace Paradoks
