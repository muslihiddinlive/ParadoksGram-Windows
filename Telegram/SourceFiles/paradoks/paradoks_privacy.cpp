/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_privacy.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
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
	state.locked.clear();
	state.unlocked.clear();
	Save();
}

[[nodiscard]] not_null<Ui::PasswordInput*> AddPinField(
		not_null<Ui::GenericBox*> box,
		const QString &placeholder) {
	const auto &st = st::settingLocalPasscodeInputField;
	const auto wrap = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		st::boxRowPadding);
	wrap->resize(wrap->width(), st.heightMin);
	const auto field = Ui::CreateChild<Ui::PasswordInput>(
		wrap,
		st,
		rpl::single(placeholder));
	wrap->widthValue(
	) | rpl::on_next([=](int width) {
		field->resize(width, field->height());
		field->moveToLeft(0, 0);
	}, wrap->lifetime());
	return not_null(field);
}

[[nodiscard]] not_null<Ui::FlatLabel*> AddErrorLabel(
		not_null<Ui::GenericBox*> box) {
	const auto error = box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::settingLocalPasscodeError),
		st::boxRowPadding);
	error->hide();
	return error;
}

void PinBox(
		not_null<Ui::GenericBox*> box,
		QString title,
		QString description,
		Fn<bool(const QString&)> validate,
		Fn<void()> done) {
	box->setTitle(rpl::single(title));
	box->addRow(object_ptr<Ui::FlatLabel>(box, description, st::boxLabel));
	const auto field = AddPinField(box, u"PIN"_q);
	const auto error = AddErrorLabel(box);
	const auto submit = [=] {
		if (validate(field->text())) {
			box->closeBox();
			if (done) {
				done();
			}
			return;
		}
		error->show();
		error->setText(u"PIN noto'g'ri"_q);
		field->setFocus();
		field->showError();
	};
	QObject::connect(field, &Ui::MaskedInputField::submitted, submit);
	box->addButton(rpl::single(u"OK"_q), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
}

void NewPinBox(
		not_null<Ui::GenericBox*> box,
		QString title,
		Fn<void(const QString&)> done) {
	box->setTitle(rpl::single(title));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Kamida 4 ta belgi kiriting va takrorlang."_q,
		st::boxLabel));
	const auto first = AddPinField(box, u"Yangi PIN"_q);
	const auto second = AddPinField(box, u"PIN ni takrorlang"_q);
	const auto error = AddErrorLabel(box);
	const auto submit = [=] {
		const auto pin = first->text();
		if (pin.size() < kMinPinLength) {
			error->show();
			error->setText(u"PIN kamida 4 ta belgidan iborat bo'lsin"_q);
			first->setFocus();
			first->showError();
		} else if (pin != second->text()) {
			error->show();
			error->setText(u"PIN lar bir xil emas"_q);
			second->setFocus();
			second->showError();
		} else {
			box->closeBox();
			done(pin);
		}
	};
	QObject::connect(first, &Ui::MaskedInputField::submitted, [=] {
		second->setFocus();
	});
	QObject::connect(second, &Ui::MaskedInputField::submitted, submit);
	box->addButton(rpl::single(u"Saqlash"_q), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->setFocusCallback([=] {
		first->setFocusFast();
	});
}

// Asks for the PIN (creating one first if there is none), then runs `done`.
void Verify(
		not_null<Window::SessionController*> controller,
		const QString &title,
		Fn<void()> done) {
	if (!HasPin()) {
		controller->show(Box(
			NewPinBox,
			u"Yangi PIN"_q,
			Fn<void(const QString&)>([=](const QString &pin) {
				SavePin(pin);
				controller->showToast(u"PIN o'rnatildi"_q);
				done();
			})));
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
		controller->show(Box(
			NewPinBox,
			u"Yangi PIN"_q,
			Fn<void(const QString&)>([=](const QString &pin) {
				SavePin(pin);
				ToggleLocked(controller, peer);
			})));
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

void HiddenChatsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Yashirin chatlar"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto session = &controller->session();
	const auto prefix = QString::number(session->userId().bare) + ':';
	auto any = false;
	for (const auto &key : Data().hidden) {
		if (!key.startsWith(prefix)) {
			continue;
		}
		any = true;
		const auto peerId = PeerId(key.mid(prefix.size()).toULongLong());
		const auto peer = session->data().peer(peerId);
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(peer->name()),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback([=] {
			Data().unlocked = key;
			box->closeBox();
			controller->showPeerHistory(peer);
		});
	}
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		any
			? u"Chatni ro'yxatga qaytarish: chat menyusi, "
				"\"Yashirinni chiqarish\"."_q
			: u"Yashirin chat yo'q. Chatni yashirish uchun chat "
				"ustida o'ng tugma bosing."_q,
		st::boxLabel));
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
						controller->showToast(u"PIN o'zgartirildi"_q);
					})));
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
}

void ShowHiddenChats(not_null<Window::SessionController*> controller) {
	Verify(controller, u"Yashirin chatlar"_q, [=] {
		controller->show(Box(HiddenChatsBox, controller));
	});
}

bool TryOpenByPin(
		not_null<Window::SessionController*> controller,
		const QString &text) {
	if (text.size() < kMinPinLength || !CheckPin(text)) {
		return false;
	}
	controller->show(Box(HiddenChatsBox, controller));
	return true;
}

void ManagePin(not_null<Window::SessionController*> controller) {
	if (!HasPin()) {
		controller->show(Box(
			NewPinBox,
			u"Yangi PIN"_q,
			Fn<void(const QString&)>([=](const QString &pin) {
				SavePin(pin);
				controller->showToast(u"PIN o'rnatildi"_q);
			})));
		return;
	}
	controller->show(Box(ManagePinBox, controller));
}

} // namespace Paradoks
