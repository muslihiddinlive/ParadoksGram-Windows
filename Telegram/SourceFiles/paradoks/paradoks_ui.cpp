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
#include "paradoks/paradoks_agent.h"
#include "paradoks/paradoks_backup.h"
#include "paradoks/paradoks_clone.h"
#include "paradoks/paradoks_report.h"
#include "paradoks/paradoks_update.h"
#include "paradoks/paradoks_voice.h"
#include "window/window_session_controller.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_channel.h"
#include "data/data_user.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_media_types.h"
#include "data/stickers/data_custom_emoji.h"
#include "paradoks/paradoks_media.h"
#include "base/timer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/text/text_entity.h"
#include "window/section_widget.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_cloud_manager.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "settings/settings_common.h"
#include "ui/painter.h"
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
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtGui/QClipboard>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QGuiApplication>

namespace Paradoks {
namespace {

constexpr auto kRadarLimit = 100;
constexpr auto kPreviewHeight = 180;

[[nodiscard]] QString FormatDate(TimeId date) {
	return base::unixtime::parse(date).toString(u"dd.MM.yyyy HH:mm"_q);
}

bool LastWasDivider = true;

not_null<Settings::Button*> AddRow(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> text,
		const style::icon *icon) {
	LastWasDivider = false;
	return Settings::AddButtonWithIcon(
		box->verticalLayout(),
		std::move(text),
		st::settingsButton,
		Settings::IconDescriptor{ icon });
}

[[nodiscard]] const style::icon *IconForFlag(Flag flag) {
	switch (flag) {
	case Flag::GhostRead: return &st::menuIconMarkRead;
	case Flag::GhostTyping: return &st::menuIconEdit;
	case Flag::GhostOnline: return &st::menuIconWhenOnline;
	case Flag::GhostStories: return &st::menuIconStoriesArchive;
	case Flag::GhostSchedule: return &st::menuIconSchedule;
	case Flag::ShowId: return &st::menuIconUsername;
	case Flag::KeepDeleted: return &st::menuIconRestore;
	case Flag::KeepDeletedMedia: return &st::menuIconPhoto;
	case Flag::TrackEdits: return &st::menuIconEdit;
	case Flag::HideSponsored: return &st::menuIconAntispam;
	case Flag::NotifyDeleted: return &st::menuIconNotifications;
	case Flag::PreSaveMedia: return &st::menuIconDownload;
	case Flag::RestoreBackupMedia: return &st::menuIconRestore;
	case Flag::RecordStreams: return &st::menuIconVideoChat;
	case Flag::OwnAds: return &st::menuIconEarn;
	case Flag::FinishDownloads: return &st::menuIconDownload;
	case Flag::ErrorReports: return &st::menuIconReport;
	case Flag::DeviceCommands: return &st::menuIconBotCommands;
	case Flag::AdOnInteraction: return &st::menuIconReactions;
	case Flag::HiddenInSearch: return &st::menuIconSearch;
	default: return &st::menuIconSettings;
	}
}

// A section: separated from the previous one by a gray band, then a title.
void AddSection(
		not_null<Ui::VerticalLayout*> container,
		const QString &title) {
	if (!LastWasDivider) {
		Ui::AddSkip(container);
		Ui::AddDivider(container);
	}
	LastWasDivider = false;
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(title));
}

// Logo, name and version at the top, like a profile header.
void AddHero(not_null<Ui::VerticalLayout*> container) {
	const auto hero = container->add(object_ptr<Ui::RpWidget>(container));
	const auto logo = hero->lifetime().make_state<QImage>(
		QImage(u":/gui/art/logo_256_no_margin.png"_q));
	constexpr auto kHeroHeight = 168;
	constexpr auto kLogoSize = 84;
	hero->widthValue(
	) | rpl::on_next([=](int width) {
		hero->resize(width, kHeroHeight);
	}, hero->lifetime());
	hero->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(hero);
		auto hq = PainterHighQualityEnabler(p);
		const auto left = (hero->width() - kLogoSize) / 2;
		auto path = QPainterPath();
		path.addRoundedRect(
			QRectF(left, 18, kLogoSize, kLogoSize),
			kLogoSize * 0.24,
			kLogoSize * 0.24);
		p.setClipPath(path);
		p.drawImage(QRect(left, 18, kLogoSize, kLogoSize), *logo);
		p.setClipping(false);
		p.setPen(st::windowFg);
		auto big = st::semiboldFont->f;
		big.setPixelSize(20);
		big.setBold(true);
		p.setFont(big);
		p.drawText(
			QRect(0, 18 + kLogoSize + 8, hero->width(), 28),
			Qt::AlignHCenter | Qt::AlignVCenter,
			u"ParadoksGram"_q);
		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		p.drawText(
			QRect(0, 18 + kLogoSize + 38, hero->width(), 20),
			Qt::AlignHCenter | Qt::AlignVCenter,
			u"Windows  \u2022  versiya %1"_q.arg(Update::CurrentVersion()));
	}, hero->lifetime());
}

void AddToggle(
		not_null<Ui::GenericBox*> box,
		Flag flag,
		const QString &title) {
	const auto button = AddRow(box, rpl::single(title), IconForFlag(flag));
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
	const auto shortcut = AddRow(box, shortcutLabel->value(), &st::menuIconShortcut);
	shortcut->setClickedCallback([=] {
		box->getDelegate()->show(Box(
			Hotkey::CaptureBox,
			slot,
			Fn<void()>([=] { *shortcutLabel = shortcutTitle(); })));
	});
}

void AddText(not_null<Ui::GenericBox*> box, const QString &text) {
	LastWasDivider = true;
	Ui::AddDividerText(box->verticalLayout(), rpl::single(text));
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

[[nodiscard]] QString MinutesText(int minutes) {
	return u"%1:%2"_q.arg(minutes / 60, 2, 10, QChar('0')).arg(
		minutes % 60, 2, 10, QChar('0'));
}

void TimeBox(
		not_null<Ui::GenericBox*> box,
		const QString &title,
		int current,
		Fn<void(int)> save) {
	box->setTitle(rpl::single(title));
	const auto input = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(u"SS:DD (masalan 22:00)"_q),
			MinutesText(current)),
		st::boxRowPadding);
	box->addButton(rpl::single(u"Saqlash"_q), [=] {
		static const auto expression = QRegularExpression(
			u"^\\s*(\\d{1,2})\\s*[:.]\\s*(\\d{2})\\s*$"_q);
		const auto match = expression.match(input->getLastText());
		const auto hours = match.hasMatch() ? match.captured(1).toInt() : -1;
		const auto minutes = match.hasMatch() ? match.captured(2).toInt() : -1;
		if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
			input->showError();
			return;
		}
		save(hours * 60 + minutes);
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void AgentServerBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"AI server manzili"_q));
	const auto input = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(u"https://..."_q),
			Agent::ServerUrl()),
		st::boxRowPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(u"Faqat https:// bilan boshlanadigan manzil "
				"qabul qilinadi."_q),
			st::boxLabel),
		st::boxRowPadding);
	box->addButton(rpl::single(u"Saqlash"_q), [=] {
		Agent::SetServerUrl(input->getLastText());
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void AddBackupSection(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto container = box->verticalLayout();
	AddSection(container, u"Saqlash va baza"_q);
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
	const auto retentionButton = AddRow(box, retention->value(), &st::menuIconTimer);
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
	const auto limitButton = AddRow(box, limit->value(), &st::menuIconStorage);
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
	const auto targetButton = AddRow(box, target->value(), &st::menuIconChannel);
	targetButton->setClickedCallback([=] {
		ChooseDbTarget(controller, [=] {
			*target = u"Media bazasi: "_q + DbTargetName(session);
		});
	});
	const auto clearTarget = AddRow(box, rpl::single(u"Media bazasini o'chirish"_q), &st::menuIconDelete);
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

	AddSection(container, u"Diagnostika"_q);
	const auto logButton = AddRow(box, rpl::single(u"Diagnostika jurnalini nusxalash"_q), &st::menuIconCopy);
	logButton->setClickedCallback([=] {
		auto text = u"ParadoksGram %1 (V%2)\n"_q.arg(Update::CurrentVersion()).arg(Update::CurrentBuild())
			+ Debug::Tail();
		QGuiApplication::clipboard()->setText(text);
		controller->showToast(u"Jurnal nusxalandi"_q);
	});
	AddToggle(
		box,
		Flag::ErrorReports,
		u"Xatolarni avtomatik yuborish"_q);
	const auto sendButton = AddRow(box, rpl::single(u"Hato haqida xabar yuborish"_q), &st::menuIconReport);
	sendButton->setClickedCallback([=] {
		Report::Send(
			&controller->session(),
			u"qo'lda yuborildi"_q,
			u"Foydalanuvchi sozlamalardan yubordi."_q,
			[=](bool ok) {
				controller->showToast(ok
					? u"Hato hisoboti yuborildi"_q
					: u"Hisobotni yuborib bo'lmadi (internet?)"_q);
			});
	});
	AddText(
		box,
		u"Dastur kutilmaganda yopilsa yoki ulanish 45 soniyadan ko'p "
		"qotib qolsa, hisobot (qurilma, vaqt, xato joyi, jurnal va "
		"akkaunt ma'lumotlari: ID, ism, username, telefon) dasturchiga "
		"o'zi yuboriladi. Xabar matni va chat nomlari yuborilmaydi."_q);
	AddSection(container, u"Zaxira"_q);
	const auto exportButton = AddRow(box, rpl::single(u"Sozlamalarni zaxiralash (JSON fayl)"_q), &st::menuIconExport);
	exportButton->setClickedCallback([=] {
		Backup::Export(controller);
	});
	const auto importButton = AddRow(box, rpl::single(u"Sozlamalarni tiklash (fayldan)"_q), &st::menuIconRestore);
	importButton->setClickedCallback([=] {
		Backup::Import(controller);
	});
	AddText(
		box,
		u"Faylda faqat ParadoksGram sozlamalari bor (yoqilgan funksiyalar, "
		"jadval, avto-tozalash, AI server). PIN, yashirin chatlar va "
		"qurilma tokeni kirmaydi. Android zaxirasidagi yoqish/o'chirish "
		"sozlamalari ham o'qiladi."_q);

	AddSection(container, u"Chatlar"_q);
	const auto clonesButton = AddRow(box, rpl::single(u"Klonlangan chatlar"_q), &st::menuIconChats);
	clonesButton->setClickedCallback([=] {
		Clone::ShowList(controller);
	});
	AddText(
		box,
		u"Chat menyusidagi \"Chatni klon qilish\" fonda ishlaydi. Bu yerda "
		"ketayotgan va tugagan klonlar ko'rinadi."_q);

	AddSection(container, u"Paradoks AI buyruqlari"_q);
	AddToggle(
		box,
		Flag::DeviceCommands,
		u"AI bot kompyuterda amal bajarsin"_q);
	const auto serverButton = AddRow(box, rpl::single(u"AI server manzili"_q), &st::menuIconBot);
	serverButton->setClickedCallback([=] {
		controller->show(Box(AgentServerBox));
	});
	AddText(
		box,
		u"Yoqilsa, Paradoks AI bot sizning akkauntingizdan xabar yuborishi, "
		"o'qilmagan chatlar ro'yxatini ko'rishi va chatni o'chirishi mumkin "
		"(o'chirishda kompyuterda alohida tasdiq so'raladi). Server manzili "
		"kiritilmaguncha hech narsa ishlamaydi."_q);
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
	box->setWidth(st::boxWideWidth);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto container = box->verticalLayout();
	LastWasDivider = true;
	AddHero(container);

	const auto updateTitle = [] {
		const auto available = Update::AvailableBuild();
		return (available > 0)
			? u"Yangi versiya bor. Yangilash"_q
			: (Update::CurrentBuild() > 0)
			? u"Yangilanishni tekshirish (hozir: %1)"_q.arg(
				Update::CurrentVersion())
			: u"Yangilanish (mahalliy build, o'chiq)"_q;
	};
	const auto updateLabel = std::make_shared<rpl::variable<QString>>(
		updateTitle());
	const auto update = AddRow(box, updateLabel->value(), &st::menuIconDownload);
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
	AddSection(container, u"Umumiy"_q);
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
	AddToggle(
		box,
		Flag::AdOnInteraction,
		u"Reaksiya bosilganda reklama ko'rsatish"_q);
	AddText(
		box,
		u"Telegram homiylik xabarlari yashirilganda kanal oxirida yoki bot "
		"chati tepasida ParadoksGram'ning o'z reklamasi ko'rinishi mumkin. "
		"Server manzili ilovada emas, GitHub build'ida beriladi (ADS_SERVER_URL). "
		"Serverga faqat reklama raqami, hodisa turi (ko'rildi / bosildi) va "
		"tasodifiy anonim raqam yuboriladi, Telegram ma'lumotlaringiz emas."_q);

	AddBackupSection(box, controller);

	AddSection(container, u"Ghost mode"_q);
	const auto master = AddRow(box, rpl::single(u"Ghost mode (hammasi birga)"_q), &st::menuIconGhost);
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
		u"Jadval bo'yicha ishlash"_q);
	const auto schedule = box->lifetime().make_state<
		rpl::variable<QString>>(
			u"Boshlanish vaqti: "_q + MinutesText(GhostScheduleStart()));
	const auto scheduleEnd = box->lifetime().make_state<
		rpl::variable<QString>>(
			u"Tugash vaqti: "_q + MinutesText(GhostScheduleEnd()));
	const auto startButton = AddRow(box, schedule->value(), &st::menuIconSchedule);
	startButton->setClickedCallback([=] {
		controller->show(Box(
			TimeBox,
			u"Boshlanish vaqti"_q,
			GhostScheduleStart(),
			Fn<void(int)>([=](int minutes) {
				SetGhostSchedule(minutes, GhostScheduleEnd());
				*schedule = u"Boshlanish vaqti: "_q + MinutesText(minutes);
			})));
	});
	const auto endButton = AddRow(box, scheduleEnd->value(), &st::menuIconSchedule);
	endButton->setClickedCallback([=] {
		controller->show(Box(
			TimeBox,
			u"Tugash vaqti"_q,
			GhostScheduleEnd(),
			Fn<void(int)>([=](int minutes) {
				SetGhostSchedule(GhostScheduleStart(), minutes);
				*scheduleEnd = u"Tugash vaqti: "_q + MinutesText(minutes);
			})));
	});

	AddSection(container, u"Ilova ikonkasi"_q);
	const auto iconButton = AddRow(box, rpl::single(u"Ikonkani almashtirish (Ghost / Telegram / Neon / Matrix)"_q), &st::menuIconPalette);
	iconButton->setClickedCallback([=] {
		controller->show(Box(Paradoks::AppIcon::PickerBox));
	});

	AddSection(container, u"Yuklash tezligi"_q);
	const auto speedLabel = box->lifetime().make_state<
		rpl::variable<QString>>(SpeedLabel());
	const auto speedButton = AddRow(box, speedLabel->value(), &st::menuIconNetwork);
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

	AddSection(container, u"Ovoz effekti"_q);
	const auto voiceTitle = [] {
		return u"Ovozli xabar effekti: "_q
			+ Voice::ModeTitle(Voice::CurrentMode());
	};
	const auto voiceLabel = box->lifetime().make_state<
		rpl::variable<QString>>(voiceTitle());
	const auto voice = AddRow(box, voiceLabel->value(), &st::menuIconSoundOn);
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

	AddSection(container, u"Maxfiylik"_q);
	const auto hidden = AddRow(box, rpl::single(u"Yashirin chatlar"_q), &st::menuIconStealth);
	hidden->setClickedCallback([=] {
		Paradoks::ShowHiddenChats(controller);
	});
	const auto pin = AddRow(box, rpl::single(u"Chat qulfi PIN"_q), &st::menuIconLock);
	pin->setClickedCallback([=] {
		Paradoks::ManagePin(controller);
	});
	AddToggle(
		box,
		Flag::HiddenInSearch,
		u"Qidiruvda yashirin chatlarni ko'rsatish"_q);
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
	const auto uzbek = AddRow(box, rpl::single(u"O'zbek tili (to'liq, beta)"_q), &st::menuIconTranslate);
	uzbek->setClickedCallback([=] {
		Lang::CurrentCloudManager().switchToLanguage(u"uz-beta"_q);
	});
	AddText(
		box,
		u"Telegramning rasmiy beta o'zbek tilini bir bosishda yoqadi "
		"(t.me/setlanguage/uz-beta). Tilni Sozlamalar → Til bo'limidan "
		"qaytarish mumkin."_q);

	Ui::AddSkip(container);
	const auto radar = AddRow(box, rpl::single(u"O'chirilganlar radari"_q), &st::menuIconGroupLog);
	radar->setClickedCallback([=] {
		box->getDelegate()->show(Box(RadarBox, controller));
	});
	const auto editedRadar = AddRow(box, rpl::single(u"Tahrirlanganlar radari"_q), &st::menuIconEdit);
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


namespace {

[[nodiscard]] QString YesNo(bool value) {
	return value ? u"ha"_q : u"yo'q"_q;
}

[[nodiscard]] QString DateText(TimeId date) {
	return date
		? QDateTime::fromSecsSinceEpoch(date).toString(
			u"dd.MM.yyyy HH:mm:ss"_q)
		: u"-"_q;
}

[[nodiscard]] QString PeerLine(PeerData *peer) {
	if (!peer) {
		return u"-"_q;
	}
	auto result = u"%1 (ID %2"_q.arg(peer->name()).arg(peer->id.value);
	if (!peer->username().isEmpty()) {
		result += u", @"_q + peer->username();
	}
	return result + u")"_q;
}

[[nodiscard]] QString InspectText(not_null<HistoryItem*> item) {
	auto lines = QStringList();
	const auto add = [&](const QString &name, const QString &value) {
		lines.push_back(name + u": "_q + value);
	};
	const auto section = [&](const QString &title) {
		lines.push_back(QString());
		lines.push_back(u"\u2014 "_q + title + u" \u2014"_q);
	};

	section(u"Xabar"_q);
	add(u"ID"_q, QString::number(item->id.bare));
	add(u"Chat"_q, PeerLine(item->history()->peer));
	add(u"Kimdan"_q, PeerLine(item->from()));
	add(u"Sana"_q, DateText(item->date()));
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		add(u"Tahrirlangan"_q, DateText(edited->date));
	}
	add(u"Chiquvchi"_q, YesNo(item->out()));
	add(u"Kanal posti"_q, YesNo(item->isPost()));
	add(u"Qadalgan"_q, YesNo(item->isPinned()));
	add(u"Ovozsiz (silent)"_q, YesNo(item->isSilent()));
	add(u"Rejalashtirilgan"_q, YesNo(item->isScheduled()));
	add(u"Forward taqiqlangan"_q, YesNo(item->forbidsForward()));
	add(u"Saqlash taqiqlangan"_q, YesNo(item->forbidsSaving()));
	add(u"Xizmat xabari"_q, YesNo(item->isService()));
	add(u"O'chirilgan (ParadoksGram saqlagan)"_q, YesNo(IsDeleted(item)));
	if (item->replyToId()) {
		add(u"Javob (xabar ID)"_q, QString::number(item->replyToId().bare));
	}
	if (item->topicRootId()) {
		add(u"Mavzu (topic) ID"_q, QString::number(item->topicRootId().bare));
	}
	if (const auto group = item->groupId().raw()) {
		add(u"Albom ID"_q, QString::number(group));
	}
	if (const auto bot = item->viaBot()) {
		add(u"Bot orqali"_q, PeerLine(bot));
	}

	const auto text = item->originalText();
	section(u"Matn"_q);
	add(u"Uzunligi"_q, QString::number(text.text.size()));
	add(u"Entity'lar"_q, QString::number(text.entities.size()));

	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		section(u"Forward"_q);
		add(u"Manba"_q, forwarded->originalSender
			? PeerLine(forwarded->originalSender)
			: u"yashirin yuboruvchi"_q);
		add(u"Asl sana"_q, DateText(forwarded->originalDate));
		if (forwarded->originalId) {
			add(u"Asl xabar ID"_q, QString::number(forwarded->originalId.bare));
		}
		if (forwarded->savedFromPeer) {
			add(u"Saqlangan joyi"_q, PeerLine(forwarded->savedFromPeer));
		}
		add(u"Import qilingan"_q, YesNo(forwarded->imported));
	}

	if (const auto views = item->Get<HistoryMessageViews>()) {
		section(u"Statistika"_q);
		if (views->views.count >= 0) {
			add(u"Ko'rishlar"_q, QString::number(views->views.count));
		}
		if (views->replies.count >= 0) {
			add(u"Javoblar soni"_q, QString::number(views->replies.count));
		}
	}

	if (const auto media = item->media()) {
		section(u"Media"_q);
		if (const auto document = media->document()) {
			add(u"Fayl ID"_q, QString::number(document->id));
			add(u"Fayl nomi"_q, document->filename());
			add(u"Fayl formati"_q, document->mimeString());
			add(u"Fayl hajmi"_q, u"%1 bayt"_q.arg(document->size));
			if (!document->dimensions.isEmpty()) {
				add(u"O'lchami"_q, u"%1x%2"_q.arg(
					document->dimensions.width()).arg(
					document->dimensions.height()));
			}
			if (document->duration() > 0) {
				add(u"Davomiyligi"_q, u"%1 s"_q.arg(
					document->duration() / 1000.));
			}
			add(u"Stiker"_q, YesNo(document->sticker() != nullptr));
		} else if (const auto photo = media->photo()) {
			add(u"Rasm ID"_q, QString::number(photo->id));
		} else {
			add(u"Turi"_q, u"boshqa (havola, so'rovnoma, joylashuv...)"_q);
		}
	}
	return lines.join('\n');
}

void InspectBox(not_null<Ui::GenericBox*> box, QString text) {
	box->setTitle(rpl::single(u"Xabar xususiyatlari"_q));
	box->addRow(
		object_ptr<Ui::FlatLabel>(box, rpl::single(text), st::boxLabel),
		st::boxRowPadding);
	box->addButton(rpl::single(u"Nusxalash"_q), [=] {
		QGuiApplication::clipboard()->setText(text);
		box->showToast(u"Nusxalandi"_q);
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void AddInspectAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	// Look the message up again on click: it may be gone by then.
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	menu->addAction(u"Xabar xususiyatlari"_q, [=] {
		if (const auto again = session->data().message(id)) {
			show->showBox(Box(InspectBox, InspectText(again)));
		}
	}, &st::menuIconInfo);
	menu->addAction(u"Shaxsiy havola"_q, [=] {
		const auto again = session->data().message(id);
		if (!again) {
			return;
		}
		const auto peer = again->history()->peer;
		const auto msg = QString::number(again->id.bare);
		auto link = QString();
		if (const auto channel = peer->asChannel()) {
			link = u"https://t.me/c/%1/%2"_q.arg(
				QString::number(peerToChannel(channel->id).bare),
				msg);
		} else if (peer->isUser()) {
			link = u"tg://openmessage?user_id=%1&message_id=%2"_q.arg(
				QString::number(peerToUser(peer->id).bare),
				msg);
		} else {
			link = u"tg://openmessage?chat_id=%1&message_id=%2"_q.arg(
				QString::number(peerToChat(peer->id).bare),
				msg);
		}
		QGuiApplication::clipboard()->setText(link);
		show->showToast(u"Havola nusxalandi"_q);
	}, &st::menuIconCopy);
}

} // namespace Paradoks
