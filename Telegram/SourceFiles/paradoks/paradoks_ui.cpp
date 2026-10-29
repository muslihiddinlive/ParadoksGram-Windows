/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ui.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "paradoks/paradoks_hotkey.h"
#include "paradoks/paradoks_icon.h"
#include "paradoks/paradoks_privacy.h"
#include "paradoks/paradoks_store.h"
#include "paradoks/paradoks_update.h"
#include "paradoks/paradoks_voice.h"
#include "window/window_session_controller.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/stickers/data_custom_emoji.h"
#include "paradoks/paradoks_media.h"
#include "base/timer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/text/text_entity.h"
#include "window/section_widget.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_cloud_manager.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/rp_widget.h"
#include "core/file_utilities.h"
#include "styles/style_boxes.h"
#include "styles/style_widgets.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QGuiApplication>

namespace Paradoks {
namespace {

constexpr auto kRadarLimit = 100;
constexpr auto kPreviewHeight = 180;

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

[[nodiscard]] bool Matches(const QString &query, const QStringList &fields) {
	if (query.isEmpty()) {
		return true;
	}
	for (const auto &field : fields) {
		if (field.contains(query, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

void AddSearch(
		not_null<Ui::GenericBox*> box,
		Fn<void(QString)> changed) {
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		rpl::single(u"Qidirish..."_q)));
	field->changes(
	) | rpl::on_next([=] {
		changed(field->getLastText().trimmed());
	}, field->lifetime());
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
}

void AddLine(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	container->add(
		object_ptr<Ui::FlatLabel>(container, text, st::boxLabel),
		st::boxRowPadding);
}

void AddLink(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		Fn<void()> callback) {
	const auto link = container->add(
		object_ptr<Ui::LinkButton>(container, text),
		st::boxRowPadding);
	link->setClickedCallback(std::move(callback));
}

// A message-like rounded block: date, text with its formatting, saved media.
void AddBubble(
		not_null<Ui::VerticalLayout*> container,
		const QString &header,
		TextWithEntities text,
		const QString &diffTitle,
		TextWithEntities diff,
		const QString &mediaPath) {
	const auto bubble = container->add(
		object_ptr<Ui::VerticalLayout>(container),
		style::margins(
			st::boxRowPadding.left(),
			4,
			st::boxRowPadding.right(),
			4));
	bubble->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(bubble);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(QRectF(bubble->rect()), 12., 12.);
	}, bubble->lifetime());

	const auto inner = style::margins(12, 6, 12, 6);
	bubble->add(
		object_ptr<Ui::FlatLabel>(bubble, header, st::boxLabel),
		inner);
	if (text.text.isEmpty()) {
		text.text = u"(matnsiz)"_q;
		text.entities.clear();
	}
	bubble->add(
		object_ptr<Ui::FlatLabel>(
			bubble,
			rpl::single(std::move(text)),
			st::boxLabel),
		inner);
	if (!mediaPath.isEmpty()) {
		const auto image = QImage(mediaPath);
		if (!image.isNull()) {
			const auto preview = bubble->add(
				object_ptr<Ui::RpWidget>(bubble),
				inner);
			preview->resize(preview->width(), kPreviewHeight);
			preview->paintRequest(
			) | rpl::on_next([=] {
				auto p = QPainter(preview);
				const auto box = QSize(preview->width(), kPreviewHeight);
				const auto scaled = image.scaled(
					box,
					Qt::KeepAspectRatio,
					Qt::SmoothTransformation);
				p.drawImage(0, 0, scaled);
			}, preview->lifetime());
		} else {
			const auto link = bubble->add(
				object_ptr<Ui::LinkButton>(
					bubble,
					u"Eski fayl: "_q + QFileInfo(mediaPath).fileName()),
				inner);
			link->setClickedCallback([=] {
				File::Launch(mediaPath);
			});
		}
	}
	if (!diffTitle.isEmpty()) {
		auto change = TextWithEntities{ diffTitle };
		change.append(std::move(diff));
		bubble->add(
			object_ptr<Ui::FlatLabel>(
				bubble,
				rpl::single(std::move(change)),
				st::boxLabel),
			inner);
	}
}

void EditsBox(
		not_null<Ui::GenericBox*> box,
		std::vector<EditVersion> versions,
		TextWithEntities current,
		bool currentKnown) {
	box->setTitle(rpl::single(
		u"Tahrirlar tarixi (%1)"_q.arg(versions.size())));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto container = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	for (auto i = 0; i != int(versions.size()); ++i) {
		const auto &version = versions[i];
		const auto next = (i + 1 < int(versions.size()))
			? versions[i + 1].text
			: current.text;
		const auto knownNext = (i + 1 < int(versions.size()))
			|| currentKnown;
		AddBubble(
			container,
			u"%1) %2"_q.arg(i + 1).arg(FormatDate(version.date)),
			TextWithEntities{ version.text, version.entities },
			knownNext ? u"O'zgarish:  "_q : QString(),
			knownNext ? DiffText(version.text, next) : TextWithEntities(),
			version.media);
	}
	AddBubble(
		container,
		u"Hozirgi variant"_q,
		currentKnown
			? std::move(current)
			: TextWithEntities{ u"(xabar yuklanmagan)"_q },
		QString(),
		TextWithEntities(),
		QString());
}

void RadarBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"O'chirilganlar radari"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	struct State {
		QString query;
		int filter = 0;
		Fn<void()> rebuild;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto filterLabel = box->lifetime().make_state<
		rpl::variable<QString>>(u"Filtr: hammasi"_q);
	const auto session = &controller->session();

	AddSearch(box, [=](QString query) {
		state->query = std::move(query);
		state->rebuild();
	});
	box->addLeftButton(filterLabel->value(), [=] {
		state->filter = (state->filter + 1) % 3;
		*filterLabel = (state->filter == 0)
			? u"Filtr: hammasi"_q
			: (state->filter == 1)
			? u"Filtr: media bor"_q
			: u"Filtr: media saqlanmagan"_q;
		state->rebuild();
	});

	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	state->rebuild = [=] {
		list->clear();
		const auto entries = DeletedList();
		auto shown = 0;
		for (auto i = int(entries.size()) - 1;
			i >= 0 && shown < kRadarLimit;
			--i) {
			const auto &entry = entries[i];
			if (!Reachable(controller, entry.peer)
				|| (state->filter >= 1 && !entry.hasMedia)
				|| !Matches(state->query, { entry.text, entry.from, entry.chat })) {
				continue;
			}
			const auto saved = entry.hasMedia
				? SavedMediaPath(entry.peer, entry.id)
				: QString();
			const auto copy = entry.hasMedia
				? DbCopyFor(entry.peer, entry.id)
				: std::nullopt;
			if (state->filter == 2 && (!saved.isEmpty() || copy)) {
				continue;
			}
			++shown;
			const auto who = entry.chat.isEmpty() || entry.chat == entry.from
				? entry.from
				: (entry.from + u" ("_q + entry.chat + ')');
			AddLine(list, u"%1\n%2: %3\n(o'chirildi: %4)"_q.arg(
				FormatDate(entry.date),
				who,
				entry.text.isEmpty()
					? (entry.hasMedia ? u"(media)"_q : u"(matnsiz)"_q)
					: entry.text,
				FormatDate(entry.deletedAt)));
			const auto peerId = PeerId(entry.peer);
			const auto msgId = MsgId(entry.id);
			AddLink(list, u"Xabarga o'tish"_q, [=] {
				controller->showPeerHistory(
					peerId,
					Window::SectionShow::Way::Forward,
					msgId);
				box->closeBox();
			});
			if (!saved.isEmpty()) {
				AddLink(list, u"Saqlangan nusxani ochish"_q, [=] {
					File::Launch(saved);
				});
			} else if (copy) {
				const auto copyPeer = PeerId(copy->first);
				const auto copyMsg = MsgId(copy->second);
				AddLink(list, u"Bazadagi nusxani ochish"_q, [=] {
					controller->showPeerHistory(
						copyPeer,
						Window::SectionShow::Way::Forward,
						copyMsg);
					box->closeBox();
				});
			} else if (entry.hasMedia) {
				AddLink(list, u"Qayta urinish (media)"_q, [=] {
					if (const auto item = session->data().message(
							peerId,
							msgId)) {
						RetryMedia(item);
						controller->showToast(
							u"Yuklash boshlandi, birozdan keyin qayta oching"_q);
					} else {
						controller->showToast(u"Xabar yuklanmagan"_q);
					}
				});
			}
			Ui::AddSkip(list);
		}
		if (!shown) {
			AddLine(list, u"Hech narsa topilmadi."_q);
		}
	};
	state->rebuild();
}

void EditedRadarBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Tahrirlanganlar radari"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	struct State {
		QString query;
		Fn<void()> rebuild;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto session = &controller->session();

	AddSearch(box, [=](QString query) {
		state->query = std::move(query);
		state->rebuild();
	});
	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	state->rebuild = [=] {
		list->clear();
		const auto entries = EditedList();
		auto shown = 0;
		for (const auto &entry : entries) {
			if (shown >= kRadarLimit) {
				break;
			} else if (entry.versions.empty()
				|| !Reachable(controller, entry.peer)) {
				continue;
			}
			auto searchable = QStringList{ entry.from, entry.chat };
			for (const auto &version : entry.versions) {
				searchable.push_back(version.text);
			}
			if (!Matches(state->query, searchable)) {
				continue;
			}
			++shown;
			const auto peerId = PeerId(entry.peer);
			const auto msgId = MsgId(entry.id);
			const auto &last = entry.versions.back();
			const auto item = session->data().message(peerId, msgId);
			const auto when = entry.changedAt ? entry.changedAt : last.date;
			AddLine(list, u"%1\n%2 · %3\n%4 marta tahrirlangan"_q.arg(
				FormatDate(when),
				entry.chat.isEmpty() ? u"(chat)"_q : entry.chat,
				entry.from.isEmpty() ? u"(noma'lum)"_q : entry.from).arg(
				int(entry.versions.size())));
			if (item) {
				auto change = TextWithEntities{ u"So'nggi o'zgarish:  "_q };
				change.append(DiffText(last.text, item->originalText().text));
				list->add(
					object_ptr<Ui::FlatLabel>(
						list,
						rpl::single(std::move(change)),
						st::boxLabel),
					st::boxRowPadding);
			} else {
				AddLine(list, u"Oldingi variant:\n"_q
					+ (last.text.isEmpty() ? u"(matnsiz)"_q : last.text));
			}
			const auto versions = entry.versions;
			AddLink(list, u"Hamma versiyalar"_q, [=] {
				const auto live = session->data().message(peerId, msgId);
				box->getDelegate()->show(Box(
					EditsBox,
					versions,
					live ? live->originalText() : TextWithEntities(),
					live != nullptr));
			});
			AddLink(list, u"Xabarga o'tish"_q, [=] {
				controller->showPeerHistory(
					peerId,
					Window::SectionShow::Way::Forward,
					msgId);
				box->closeBox();
			});
			Ui::AddSkip(list);
		}
		if (!shown) {
			AddLine(list, u"Hech narsa topilmadi."_q);
		}
	};
	state->rebuild();
}

[[nodiscard]] QString SpeedText(double bytesPerSecond) {
	return u"%1 MB/s (%2 Mbit/s)"_q
		.arg(bytesPerSecond / (1024. * 1024.), 0, 'f', 1)
		.arg(bytesPerSecond * 8. / 1000000., 0, 'f', 0);
}

[[nodiscard]] QString RetentionLabel() {
	const auto days = IntSetting(u"media_days"_q, 0);
	return u"Avto-tozalash (vaqt): "_q
		+ (days > 0 ? u"%1 kundan keyin"_q.arg(days) : u"o'chirilmaydi"_q);
}

[[nodiscard]] QString SizeLimitLabel() {
	const auto mb = IntSetting(u"media_max_mb"_q, 0);
	return u"Avto-tozalash (hajm): "_q
		+ (mb > 0
			? u"%1 GB dan oshsa eskisini o'chirish"_q.arg(mb / 1024)
			: u"cheklanmagan"_q);
}

void AddBackupSection(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto container = box->verticalLayout();
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Saqlash va baza"_q));
	AddToggle(
		box,
		Flag::NotifyDeleted,
		u"Xabar o'chirilganda bildirishnoma"_q);
	AddToggle(
		box,
		Flag::PreSaveMedia,
		u"Media'ni oldindan saqlash (25 MB gacha)"_q);
	AddToggle(
		box,
		Flag::RestoreBackupMedia,
		u"Saqlangan nusxani xabar menyusida ko'rsatish"_q);
	AddToggle(
		box,
		Flag::RecordStreams,
		u"Jonli efirlarni yozib olish"_q);
	AddText(
		box,
		u"Jonli efir yoki video chatni ko'rayotganingizda efir fon rejimida "
		"yozib boriladi va efir tugagach (yoki 45 soniya jimlikdan keyin) "
		"Saqlangan xabarlarga .webm yoki .mp4 fayl sifatida yuboriladi. Nusxa qo'shimcha "
		"ravishda tdata/paradoks_streams papkasida 7 kun saqlanadi. Yuborish "
		"taqiqlangan (noforwards) chatlar yozilmaydi."_q);

	const auto retention = box->lifetime().make_state<
		rpl::variable<QString>>(RetentionLabel());
	const auto retentionButton = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			retention->value(),
			st::settingsButtonNoIcon),
		style::margins());
	retentionButton->setClickedCallback([=] {
		const auto days = IntSetting(u"media_days"_q, 0);
		SetIntSetting(
			u"media_days"_q,
			(days == 0) ? 30 : (days == 30) ? 90 : (days == 90) ? 180 : 0);
		*retention = RetentionLabel();
		CleanupMedia();
	});
	const auto limit = box->lifetime().make_state<
		rpl::variable<QString>>(SizeLimitLabel());
	const auto limitButton = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			limit->value(),
			st::settingsButtonNoIcon),
		style::margins());
	limitButton->setClickedCallback([=] {
		const auto mb = IntSetting(u"media_max_mb"_q, 0);
		SetIntSetting(
			u"media_max_mb"_q,
			(mb == 0) ? 1024 : (mb == 1024) ? 5120 : 0);
		*limit = SizeLimitLabel();
		CleanupMedia();
	});
	AddText(
		box,
		u"Saqlangan media: %1 MB. Avto-tozalash faqat saqlangan fayllarni "
		"o'chiradi (xabar matni va radar yozuvi qoladi). Oldindan saqlangan "
		"nusxalar o'chirilmasa 3 kundan keyin tozalanadi."_q.arg(
			SavedMediaBytes() / (1024 * 1024)));

	const auto session = &controller->session();
	const auto target = box->lifetime().make_state<
		rpl::variable<QString>>(
			u"Media bazasi: "_q + DbTargetName(session));
	const auto targetButton = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			target->value(),
			st::settingsButtonNoIcon),
		style::margins());
	targetButton->setClickedCallback([=] {
		ChooseDbTarget(controller, [=] {
			*target = u"Media bazasi: "_q + DbTargetName(session);
		});
	});
	const auto clearTarget = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Media bazasini o'chirish"_q),
			st::settingsButtonNoIcon),
		style::margins());
	clearTarget->setClickedCallback([=] {
		SetDbTarget(session, PeerId());
		*target = u"Media bazasi: "_q + DbTargetName(session);
	});
	AddText(
		box,
		u"Baza: boshqalar yuborgan rasm, video va fayllar avtomatik siz "
		"tanlagan shaxsiy kanal yoki guruhga jimgina forward qilinadi. "
		"Yuboruvchi o'chirsa ham nusxa qoladi. Faqat shaxsiy chat va "
		"guruhlardagi, forward taqiqlanmagan xabarlar yuboriladi."_q);

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Diagnostika"_q));
	const auto logButton = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Diagnostika jurnalini nusxalash"_q),
			st::settingsButtonNoIcon),
		style::margins());
	logButton->setClickedCallback([=] {
		auto text = u"ParadoksGram V%1\n"_q.arg(Update::CurrentBuild())
			+ Debug::Tail();
		QGuiApplication::clipboard()->setText(text);
		controller->showToast(u"Jurnal nusxalandi"_q);
	});
	AddText(
		box,
		u"Funksiya ishlamasa: jurnalni nusxalab, dasturchiga yuboring. "
		"Unda faqat funksiyalar holati bor (chat nomlari va xabar matni "
		"yo'q)."_q);
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
	AddToggle(
		box,
		Flag::OwnAds,
		u"ParadoksGram reklamasi (reklama yashirilganda)"_q);
	AddText(
		box,
		u"Telegram homiylik xabarlari yashirilganda kanal oxirida yoki bot "
		"chati tepasida ParadoksGram'ning o'z reklamasi ko'rinishi mumkin. "
		"Server manzili ilovada emas, GitHub build'ida beriladi (ADS_SERVER_URL). "
		"Serverga faqat reklama raqami, hodisa turi (ko'rildi / bosildi) va "
		"tasodifiy anonim raqam yuboriladi, Telegram ma'lumotlaringiz emas."_q);

	AddBackupSection(box, controller);

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
	AddToggle(
		box,
		Flag::FinishDownloads,
		u"Yopilganda yuklashni tugatish"_q);
	AddText(
		box,
		u"Dasturdan chiqayotganingizda yuklash ketayotgan bo'lsa, oyna "
		"yopiladi, lekin jarayon fonda yuklashni tugatadi va o'zi chiqadi. "
		"Yana bir marta chiqishni bossangiz, yuklashni to'xtatish so'raladi. "
		"Yuklash buzilsa, yuklangan qismi saqlanadi va keyingi urinishda "
		"davom etadi."_q);

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
	const auto uzbek = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"O'zbek tili (to'liq, beta)"_q),
			st::settingsButtonNoIcon),
		style::margins());
	uzbek->setClickedCallback([=] {
		Lang::CurrentCloudManager().switchToLanguage(u"uz-beta"_q);
	});
	AddText(
		box,
		u"Telegramning rasmiy beta o'zbek tilini bir bosishda yoqadi "
		"(t.me/setlanguage/uz-beta). Tilni Sozlamalar → Til bo'limidan "
		"qaytarish mumkin."_q);

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

void AddIdActions(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	if (!Enabled(Flag::ShowId)) {
		return;
	}
	auto ids = QStringList();
	auto kind = QString();
	const auto media = item->media();
	if (const auto document = media ? media->document() : nullptr) {
		if (document->sticker()) {
			kind = u"Sticker"_q;
		} else if (document->isGifv() || document->isAnimation()) {
			kind = u"GIF"_q;
		}
		if (!kind.isEmpty()) {
			ids.push_back(QString::number(document->id));
		}
	}
	for (const auto &entity : item->originalText().entities) {
		if (entity.type() != EntityType::CustomEmoji) {
			continue;
		}
		const auto id = QString::number(
			Data::ParseCustomEmojiData(entity.data()));
		if (!ids.contains(id)) {
			ids.push_back(id);
		}
		if (kind.isEmpty()) {
			kind = u"Emoji"_q;
		}
	}
	if (ids.isEmpty()) {
		return;
	}
	const auto text = ids.join('\n');
	const auto title = (ids.size() > 1)
		? u"Emoji ID'larini nusxalash (%1)"_q.arg(ids.size())
		: u"%1 ID: %2"_q.arg(kind, ids.front());
	menu->addAction(title, [=] {
		QGuiApplication::clipboard()->setText(text);
		show->showToast(u"ID nusxalandi"_q);
	}, &st::menuIconInfo);
}

void AddEditHistoryAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	auto versions = EditsFor(item);
	if (versions.empty()) {
		if (item->Has<HistoryMessageEdited>() && !item->hideEditedBadge()) {
			menu->addAction(u"Tahrirlar tarixi (yo'q)"_q, [=] {
				show->showToast(
					u"Bu xabar ParadoksGram kuzatuvidan oldin tahrirlangan, "
					"eski matni saqlanmagan. Keyingi tahrirlar saqlanadi."_q);
			}, &st::menuIconInfo);
		}
		return;
	}
	const auto current = item->originalText();
	menu->addAction(u"Tahrirlar tarixi (%1)"_q.arg(versions.size()), [=] {
		show->showBox(Box(EditsBox, versions, current, true));
	}, &st::menuIconInfo);
}

} // namespace Paradoks
