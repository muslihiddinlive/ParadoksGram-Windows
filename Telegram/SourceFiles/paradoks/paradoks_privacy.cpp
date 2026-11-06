/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_privacy.h"

#include "paradoks/paradoks_clone.h"
#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_ghost.h"
#include "paradoks/paradoks_pin.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "ui/rp_widget.h"
#include "ui/userpic_view.h"
#include "history/history_item.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "styles/style_basic.h"
#include "styles/style_widgets.h"
#include <range/v3/algorithm/sort.hpp>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSettings>

#include <set>

namespace Paradoks {
namespace {

constexpr auto kMinPinLength = 4;

struct State {
	std::set<QString> hidden;
	std::set<QString> locked;
	QString salt;
	QString hash;
	QString unlocked;
	bool pattern = false; // Drawn pattern instead of a typed PIN.
};

[[nodiscard]] QString IniPath() {
	return cWorkingDir() + u"tdata/paradoks.ini"_q;
}

[[nodiscard]] State &Data() {
	static auto state = [] {
		auto result = State();
		auto settings = QSettings(IniPath(), QSettings::IniFormat);
		for (const auto &key
				: settings.value(u"privacy/hidden"_q).toStringList()) {
			result.hidden.insert(key);
		}
		for (const auto &key
				: settings.value(u"privacy/locked"_q).toStringList()) {
			result.locked.insert(key);
		}
		result.salt = settings.value(u"privacy/salt"_q).toString();
		result.hash = settings.value(u"privacy/hash"_q).toString();
		result.pattern = settings.value(u"privacy/pattern"_q, false).toBool();
		return result;
	}();
	return state;
}

[[nodiscard]] QStringList ToList(const std::set<QString> &keys) {
	auto result = QStringList();
	for (const auto &key : keys) {
		result.push_back(key);
	}
	return result;
}

void Save() {
	const auto &state = Data();
	auto settings = QSettings(IniPath(), QSettings::IniFormat);
	settings.setValue(u"privacy/hidden"_q, ToList(state.hidden));
	settings.setValue(u"privacy/locked"_q, ToList(state.locked));
	settings.setValue(u"privacy/salt"_q, state.salt);
	settings.setValue(u"privacy/hash"_q, state.hash);
	settings.setValue(u"privacy/pattern"_q, state.pattern);
	settings.sync();
}

[[nodiscard]] QString KeyFor(
		not_null<Main::Session*> session,
		PeerId peerId) {
	return QString::number(session->userId().bare)
		+ ':'
		+ QString::number(peerId.value);
}

[[nodiscard]] bool HasPin() {
	return !Data().hash.isEmpty();
}

[[nodiscard]] QString HashPin(const QString &salt, const QString &pin) {
	const auto bytes = (salt + ':' + pin).toUtf8();
	return QString::fromLatin1(QCryptographicHash::hash(
		bytes,
		QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] bool CheckPin(const QString &pin) {
	const auto &state = Data();
	return !state.hash.isEmpty()
		&& state.hash == HashPin(state.salt, pin);
}

void SavePin(const QString &pin) {
	auto &state = Data();
	state.salt = QString::number(
		QRandomGenerator::system()->generate64(),
		16);
	state.hash = HashPin(state.salt, pin);
	Save();
}

void ClearPin() {
	auto &state = Data();
	state.salt.clear();
	state.hash.clear();
	state.pattern = false;
	state.locked.clear();
	state.unlocked.clear();
	Save();
}

void PinBox(
		not_null<Ui::GenericBox*> box,
		QString title,
		QString description,
		Fn<bool(const QString&)> validate,
		Fn<void()> done) {
	const auto finish = [=](const QString &) {
		if (done) {
			done();
		}
	};
	if (Data().pattern) {
		Pin::BuildPatternScreen(
			box,
			title,
			u"Chizmani chizing"_q,
			std::move(validate),
			finish);
	} else {
		Pin::BuildPinScreen(box, title, description, std::move(validate), finish);
	}
}

void NewPinBox(
		not_null<Ui::GenericBox*> box,
		QString title,
		Fn<void(const QString&)> done,
		bool pattern) {
	if (pattern) {
		Pin::BuildPatternScreen(
			box,
			title,
			u"Kamida 4 ta nuqtani ulab chizma chizing va takrorlang"_q,
			nullptr,
			std::move(done));
	} else {
		Pin::BuildPinScreen(
			box,
			title,
			u"Kamida 4 ta raqam kiriting va takrorlang"_q,
			nullptr,
			std::move(done));
	}
}

// First use: let the user pick a typed PIN or a drawn pattern, then set it.
void SetupNewCode(
		not_null<Window::SessionController*> controller,
		Fn<void()> done) {
	static const auto options = std::vector<QString>{
		u"PIN kod (raqamlar)"_q,
		u"Chizma (nuqtalarni ulab chizish)"_q,
	};
	controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
		.title = rpl::single(u"Himoya usulini tanlang"_q),
		.options = options,
		.initialSelection = 1,
		.callback = Fn<void(int)>([=](int value) {
			const auto pattern = (value == 1);
			crl::on_main(controller, [=] {
				controller->show(Box(
					NewPinBox,
					pattern ? u"Yangi chizma"_q : u"Yangi PIN"_q,
					Fn<void(const QString&)>([=](const QString &code) {
						Data().pattern = pattern;
						SavePin(code);
						controller->showToast(pattern
							? u"Chizma o'rnatildi"_q
							: u"PIN o'rnatildi"_q);
						if (done) {
							done();
						}
					}),
					pattern));
			});
		}),
	}));
}

// Asks for the PIN (creating one first if there is none), then runs `done`.
void Verify(
		not_null<Window::SessionController*> controller,
		const QString &title,
		Fn<void()> done) {
	if (!HasPin()) {
		SetupNewCode(controller, std::move(done));
		return;
	}
	controller->show(Box(
		PinBox,
		title,
		u"PIN kodni kiriting"_q,
		Fn<bool(const QString&)>(CheckPin),
		std::move(done)));
}

void ToggleHidden(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	Verify(controller, u"Yashirin chat"_q, [=] {
		auto &state = Data();
		const auto key = KeyFor(&controller->session(), peer->id);
		if (state.hidden.erase(key)) {
			controller->showToast(u"Chat ro'yxatga qaytarildi"_q);
		} else {
			state.hidden.insert(key);
			controller->showToast(
				u"Chat yashirildi. Xabarlari bildirishnoma chiqarmaydi"_q);
		}
		Save();
		peer->owner().history(peer)->updateChatListExistence();
	});
}

void ToggleLocked(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	if (!HasPin()) {
		SetupNewCode(controller, [=] {
			ToggleLocked(controller, peer);
		});
		return;
	}
	auto &state = Data();
	const auto key = KeyFor(&controller->session(), peer->id);
	if (state.locked.contains(key)) {
		controller->show(Box(
			PinBox,
			u"Qulfni olish"_q,
			u"PIN kodni kiriting"_q,
			Fn<bool(const QString&)>(CheckPin),
			Fn<void()>([=] {
				auto &state = Data();
				state.locked.erase(key);
				state.unlocked.clear();
				Save();
				controller->showToast(u"Chat qulfi olib tashlandi"_q);
			})));
	} else {
		state.locked.insert(key);
		state.unlocked = key;
		Save();
		controller->showToast(
			u"Chat qulflandi. Keyingi safar ochganda PIN so'raladi"_q);
	}
}

// The hidden chats live in their own window, like a separate chat list.
void ShowHiddenWindow(not_null<Window::SessionController*> controller) {
	static auto Existing = QPointer<QWidget>();
	if (Existing) {
		Existing->show();
		Existing->raise();
		Existing->activateWindow();
		return;
	}
	constexpr auto kHeader = 76;
	constexpr auto kRowHeight = 64;
	constexpr auto kAvatar = 48;
	constexpr auto kRemoveWidth = 96;

	struct Row {
		not_null<PeerData*> peer;
		Ui::PeerUserpicView view;
		QString key;
		QString name;
		QString preview;
		QString when;
		TimeId date = 0;
	};
	struct State {
		std::vector<Row> rows;
		int hover = -1;
		bool hoverRemove = false;
	};

	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	const auto window = new Ui::RpWidget();
	Existing = window;
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->setWindowTitle(u"Yashirin chatlar — ParadoksGram"_q);
	window->setMinimumSize(340, 420);
	window->resize(420, 640);
	const auto guard = QPointer<QWidget>(window);
	session->lifetime().add([=] {
		if (guard) {
			guard->close();
		}
	});

	const auto state = window->lifetime().make_state<State>();
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(window);
	const auto list = scroll->setOwnedWidget(
		object_ptr<Ui::RpWidget>(scroll));
	list->setMouseTracking(true);

	const auto relayout = [=] {
		const auto height = std::max(
			int(state->rows.size()) * kRowHeight,
			scroll->height());
		list->resize(scroll->width(), height);
		list->update();
		window->update();
	};
	const auto reload = [=] {
		state->rows.clear();
		const auto prefix = QString::number(session->userId().bare) + ':';
		for (const auto &key : Data().hidden) {
			if (!key.startsWith(prefix)) {
				continue;
			}
			const auto peerId = PeerId(key.mid(prefix.size()).toULongLong());
			const auto peer = session->data().peer(peerId);
			auto row = Row{ .peer = peer, .key = key, .name = peer->name() };
			peer->loadUserpic();
			if (const auto item = session->data().history(peer)
					->chatListMessage()) {
				const auto text = item->originalText().text.simplified();
				row.preview = text.isEmpty() ? u"[media]"_q : text;
				row.date = item->date();
				row.when = base::unixtime::parse(item->date()).toString(
					u"dd.MM HH:mm"_q);
			}
			state->rows.push_back(std::move(row));
		}
		ranges::sort(state->rows, [](const Row &a, const Row &b) {
			return a.date > b.date;
		});
		relayout();
	};

	window->sizeValue(
	) | rpl::on_next([=](QSize size) {
		scroll->setGeometry(
			0,
			kHeader,
			size.width(),
			std::max(size.height() - kHeader, 0));
		relayout();
	}, window->lifetime());

	window->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(window);
		p.fillRect(clip, st::windowBg);
		p.setPen(st::windowFg);
		auto big = st::semiboldFont->f;
		big.setPixelSize(20);
		big.setBold(true);
		p.setFont(big);
		p.drawText(
			QRect(20, 12, window->width() - 40, 30),
			Qt::AlignLeft | Qt::AlignVCenter,
			u"Yashirin chatlar"_q);
		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		p.drawText(
			QRect(20, 44, window->width() - 40, 22),
			Qt::AlignLeft | Qt::AlignVCenter,
			u"%1 ta chat. Faqat sizga ko'rinadi, bildirishnoma kelmaydi."_q
				.arg(state->rows.size()));
	}, window->lifetime());

	list->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(list);
		p.fillRect(clip, st::windowBg);
		const auto width = list->width();
		if (state->rows.empty()) {
			p.setPen(st::windowSubTextFg);
			p.setFont(st::normalFont);
			p.drawText(
				QRect(24, 24, width - 48, 120),
				Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
				u"Yashirin chat yo'q.\nChatni yashirish uchun chatlar "
				"ro'yxatida chat ustiga o'ng tugma bosing va "
				"\"Chatni yashirish\" ni tanlang."_q);
			return;
		}
		for (auto i = 0; i != int(state->rows.size()); ++i) {
			const auto top = i * kRowHeight;
			if (top + kRowHeight < clip.top() || top > clip.bottom()) {
				continue;
			}
			auto &row = state->rows[i];
			if (state->hover == i) {
				p.fillRect(0, top, width, kRowHeight, st::windowBgOver);
			}
			row.peer->paintUserpicLeft(
				p,
				row.view,
				14,
				top + (kRowHeight - kAvatar) / 2,
				width,
				kAvatar);
			const auto left = 14 + kAvatar + 14;
			const auto textWidth = width - left - kRemoveWidth - 14;
			p.setPen(st::windowFg);
			p.setFont(st::semiboldFont);
			p.drawText(
				QRect(left, top + 9, textWidth, 22),
				Qt::AlignLeft | Qt::AlignVCenter,
				st::semiboldFont->elided(row.name, textWidth));
			p.setPen(st::windowSubTextFg);
			p.setFont(st::normalFont);
			p.drawText(
				QRect(left, top + 33, textWidth, 22),
				Qt::AlignLeft | Qt::AlignVCenter,
				st::normalFont->elided(
					row.preview.isEmpty()
						? u"(xabar yo'q)"_q
						: row.preview,
					textWidth));
			p.drawText(
				QRect(width - kRemoveWidth - 8, top + 9, kRemoveWidth, 20),
				Qt::AlignRight | Qt::AlignVCenter,
				row.when);
			p.setPen((state->hover == i && state->hoverRemove)
				? st::windowActiveTextFg
				: st::windowSubTextFg);
			p.drawText(
				QRect(width - kRemoveWidth - 8, top + 33, kRemoveWidth, 22),
				Qt::AlignRight | Qt::AlignVCenter,
				u"Chiqarish"_q);
		}
	}, list->lifetime());

	const auto rowAt = [=](QPoint point) {
		const auto index = point.y() / kRowHeight;
		return (point.y() >= 0 && index < int(state->rows.size()))
			? index
			: -1;
	};
	const auto removeZone = [=](QPoint point) {
		return point.x() >= list->width() - kRemoveWidth - 8
			&& (point.y() % kRowHeight) >= 28;
	};
	list->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		switch (e->type()) {
		case QEvent::MouseMove: {
			const auto point = static_cast<QMouseEvent*>(e.get())
				->position().toPoint();
			state->hover = rowAt(point);
			state->hoverRemove = (state->hover >= 0) && removeZone(point);
			list->setCursor((state->hover >= 0)
				? style::cur_pointer
				: style::cur_default);
			list->update();
		} break;
		case QEvent::Leave:
			state->hover = -1;
			list->update();
			break;
		case QEvent::MouseButtonPress: {
			const auto event = static_cast<QMouseEvent*>(e.get());
			if (event->button() != Qt::LeftButton) {
				return;
			}
			const auto point = event->position().toPoint();
			const auto index = rowAt(point);
			if (index < 0) {
				return;
			}
			const auto row = state->rows[index];
			if (removeZone(point)) {
				Data().hidden.erase(row.key);
				Save();
				row.peer->owner().history(row.peer)->updateChatListExistence();
				state->hover = -1;
				reload();
				return;
			}
			Data().unlocked = row.key;
			if (const auto strong = weak.get()) {
				strong->showPeerHistory(row.peer);
				strong->window().activate();
			}
		} break;
		default: break;
		}
	}, list->lifetime());

	reload();
	window->show();
}

void ManagePinBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Chat qulfi PIN"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto change = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"PIN ni o'zgartirish"_q),
			st::settingsButtonNoIcon),
		style::margins());
	change->setClickedCallback([=] {
		box->closeBox();
		controller->show(Box(
			PinBox,
			u"Joriy PIN"_q,
			u"Davom etish uchun joriy PIN ni kiriting"_q,
			Fn<bool(const QString&)>(CheckPin),
			Fn<void()>([=] {
				controller->show(Box(
					NewPinBox,
					u"Yangi PIN"_q,
					Fn<void(const QString&)>([=](const QString &pin) {
						SavePin(pin);
						controller->showToast(Data().pattern
							? u"Chizma o'zgartirildi"_q
							: u"PIN o'zgartirildi"_q);
					}),
					Data().pattern));
			})));
	});

	const auto method = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(Data().pattern
				? u"Kirish usulini almashtirish: Chizma \u2192 PIN"_q
				: u"Kirish usulini almashtirish: PIN \u2192 Chizma"_q),
			st::settingsButtonNoIcon),
		style::margins());
	method->setClickedCallback([=] {
		const auto toPattern = !Data().pattern;
		box->closeBox();
		controller->show(Box(
			PinBox,
			u"Kirish usulini almashtirish"_q,
			u"Davom etish uchun joriy kodni kiriting"_q,
			Fn<bool(const QString&)>(CheckPin),
			Fn<void()>([=] {
				controller->show(Box(
					NewPinBox,
					toPattern ? u"Yangi chizma"_q : u"Yangi PIN"_q,
					Fn<void(const QString&)>([=](const QString &code) {
						Data().pattern = toPattern;
						SavePin(code);
						controller->showToast(toPattern
							? u"Endi chizma bilan ochiladi"_q
							: u"Endi PIN bilan ochiladi"_q);
					}),
					toPattern));
			})));
	});

	const auto remove = box->addRow(
		object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"PIN ni o'chirish (barcha qulflar olinadi)"_q),
			st::settingsButtonNoIcon),
		style::margins());
	remove->setClickedCallback([=] {
		box->closeBox();
		controller->show(Box(
			PinBox,
			u"PIN ni o'chirish"_q,
			u"PIN kodni kiriting"_q,
			Fn<bool(const QString&)>(CheckPin),
			Fn<void()>([=] {
				ClearPin();
				controller->showToast(u"PIN va qulflar o'chirildi"_q);
			})));
	});
}

} // namespace

bool IsHidden(not_null<const History*> history) {
	const auto &state = Data();
	return !state.hidden.empty()
		&& state.hidden.contains(
			KeyFor(&history->session(), history->peer->id));
}

bool IsHiddenPeer(not_null<Main::Session*> session, PeerId peerId) {
	const auto &state = Data();
	return !state.hidden.empty()
		&& state.hidden.contains(KeyFor(session, peerId));
}

bool NeedsUnlock(not_null<Main::Session*> session, PeerId peerId) {
	const auto &state = Data();
	if (state.hash.isEmpty()) {
		return false;
	}
	const auto key = KeyFor(session, peerId);
	return (state.unlocked != key)
		&& (state.locked.contains(key) || state.hidden.contains(key));
}

void NoteChatShown(not_null<Main::Session*> session, PeerId peerId) {
	auto &state = Data();
	if (!state.unlocked.isEmpty()
		&& state.unlocked != KeyFor(session, peerId)) {
		state.unlocked.clear();
	}
}

void AskUnlock(
		not_null<Window::SessionController*> controller,
		PeerId peerId,
		Fn<void()> done) {
	const auto key = KeyFor(&controller->session(), peerId);
	controller->show(Box(
		PinBox,
		u"Chat qulflangan"_q,
		u"PIN kodni kiriting"_q,
		Fn<bool(const QString&)>(CheckPin),
		Fn<void()>([=] {
			Data().unlocked = key;
			done();
		})));
}

void AddChatActions(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		const Ui::Menu::MenuCallback &add) {
	const auto key = KeyFor(&controller->session(), peer->id);
	const auto &state = Data();
	const auto hidden = state.hidden.contains(key);
	const auto locked = state.locked.contains(key);
	add(
		hidden ? u"Yashirinni chiqarish"_q : u"Chatni yashirish"_q,
		[=] { ToggleHidden(controller, peer); },
		&st::menuIconStealth);
	add(
		locked ? u"Qulfni olish"_q : u"Chatni qulflash"_q,
		[=] { ToggleLocked(controller, peer); },
		&st::menuIconLock);
	if (Clone::CanClone(peer)) {
		add(
			u"Chatni klon qilish"_q,
			[=] { Clone::Start(controller, peer); },
			&st::menuIconForward);
	}
	if (Enabled(Flag::GhostRead)) {
		add(
			u"O'qilgan deb belgilash (serverga)"_q,
			[=] {
				Ghost::MarkReadNow(
					controller->session().data().history(peer));
				controller->showToast(u"O'qildi belgisi yuborildi"_q);
			},
			&st::menuIconMarkRead);
	}
}

void ShowHiddenChats(not_null<Window::SessionController*> controller) {
	Verify(controller, u"Yashirin chatlar"_q, [=] {
		ShowHiddenWindow(controller);
	});
}

bool TryOpenByPin(
		not_null<Window::SessionController*> controller,
		const QString &text) {
	if (Data().pattern
		|| text.size() < kMinPinLength
		|| !CheckPin(text)) {
		return false;
	}
	ShowHiddenWindow(controller);
	return true;
}

void ManagePin(not_null<Window::SessionController*> controller) {
	if (!HasPin()) {
		SetupNewCode(controller, nullptr);
		return;
	}
	controller->show(Box(ManagePinBox, controller));
}

} // namespace Paradoks
