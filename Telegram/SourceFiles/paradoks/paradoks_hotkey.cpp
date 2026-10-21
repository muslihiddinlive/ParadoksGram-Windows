/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_hotkey.h"

#include "paradoks/paradoks_config.h"
#include "lang/lang_keys.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtWidgets/QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Paradoks::Hotkey {
namespace {

constexpr auto kHotkeyId = 0x5047; // "PG"
constexpr auto kOverlayShowMs = 1000;
constexpr auto kOverlayFadeSteps = 8;
constexpr auto kOverlayFadeStepMs = 25;

struct Saved {
	uint modifiers = 0;
	uint vk = 0;
	QString title;
};

[[nodiscard]] QString IniPath() {
	return cWorkingDir() + u"tdata/paradoks.ini"_q;
}

[[nodiscard]] Saved &Current() {
	static auto saved = [] {
		auto result = Saved();
		auto settings = QSettings(IniPath(), QSettings::IniFormat);
		result.modifiers = settings.value(u"hotkey/mod"_q, 0).toUInt();
		result.vk = settings.value(u"hotkey/vk"_q, 0).toUInt();
		result.title = settings.value(u"hotkey/title"_q).toString();
		return result;
	}();
	return saved;
}

void Save() {
	const auto &saved = Current();
	auto settings = QSettings(IniPath(), QSettings::IniFormat);
	settings.setValue(u"hotkey/mod"_q, saved.modifiers);
	settings.setValue(u"hotkey/vk"_q, saved.vk);
	settings.setValue(u"hotkey/title"_q, saved.title);
	settings.sync();
}

#ifdef Q_OS_WIN

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
using FilterResult = qintptr;
#else // Qt >= 6
using FilterResult = long;
#endif // Qt >= 6

class NativeFilter final : public QAbstractNativeEventFilter {
public:
	bool nativeEventFilter(
			const QByteArray &eventType,
			void *message,
			FilterResult *result) override {
		const auto msg = static_cast<MSG*>(message);
		if (msg && msg->message == WM_HOTKEY && msg->wParam == kHotkeyId) {
			QTimer::singleShot(0, [] {
				ToggleGhost();
			});
			return true;
		}
		return false;
	}
};

[[nodiscard]] bool Register(uint modifiers, uint vk) {
	UnregisterHotKey(nullptr, kHotkeyId);
	if (!vk) {
		return true;
	}
	return RegisterHotKey(
		nullptr,
		kHotkeyId,
		modifiers | MOD_NOREPEAT,
		vk) != FALSE;
}

[[nodiscard]] uint WinModifiers(Qt::KeyboardModifiers modifiers) {
	auto result = uint(0);
	if (modifiers & Qt::ControlModifier) {
		result |= MOD_CONTROL;
	}
	if (modifiers & Qt::AltModifier) {
		result |= MOD_ALT;
	}
	if (modifiers & Qt::ShiftModifier) {
		result |= MOD_SHIFT;
	}
	if (modifiers & Qt::MetaModifier) {
		result |= MOD_WIN;
	}
	return result;
}

[[nodiscard]] bool IsFunctionKey(uint vk) {
	return (vk >= VK_F1) && (vk <= VK_F24);
}

#else // Q_OS_WIN

[[nodiscard]] bool Register(uint modifiers, uint vk) {
	return false;
}

#endif // Q_OS_WIN

class Overlay final : public QWidget {
public:
	explicit Overlay(bool on)
	: QWidget(
		nullptr,
		Qt::Tool
			| Qt::FramelessWindowHint
			| Qt::WindowStaysOnTopHint
			| Qt::WindowDoesNotAcceptFocus
			| Qt::NoDropShadowWindowHint)
	, _text(on
		? u"Ghost mode ishga tushdi"_q
		: u"Ghost mode o'chirildi"_q)
	, _on(on) {
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_DeleteOnClose);

		_font = QFont(u"Segoe UI"_q);
		_font.setPixelSize(15);
		_font.setWeight(QFont::DemiBold);
		const auto textWidth = QFontMetrics(_font).horizontalAdvance(_text);
		resize(kPadding * 3 + kIcon + textWidth, kIcon + kPadding * 2);

		if (const auto screen = QGuiApplication::primaryScreen()) {
			const auto area = screen->availableGeometry();
			move(
				area.x() + (area.width() - width()) / 2,
				area.y() + area.height() - height() - 90);
		}

		_hide.setSingleShot(true);
		QObject::connect(&_hide, &QTimer::timeout, this, [=] {
			_fade.start(kOverlayFadeStepMs);
		});
		QObject::connect(&_fade, &QTimer::timeout, this, [=] {
			if (++_step >= kOverlayFadeSteps) {
				_fade.stop();
				close();
				return;
			}
			setWindowOpacity(1. - double(_step) / kOverlayFadeSteps);
		});
	}

	void flash() {
		show();
		_hide.start(kOverlayShowMs);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(22, 22, 26, 236));
		p.drawRoundedRect(rect(), 16, 16);

		const auto accent = _on ? QColor(120, 220, 140) : QColor(200, 200, 205);
		st::menuIconGhost.paint(
			p,
			kPadding,
			(height() - kIcon) / 2,
			width(),
			accent);
		p.setFont(_font);
		p.setPen(QColor(240, 240, 245));
		p.drawText(
			QRect(
				kPadding * 2 + kIcon,
				0,
				width() - kPadding * 3 - kIcon,
				height()),
			Qt::AlignVCenter | Qt::AlignLeft,
			_text);
	}

private:
	static constexpr auto kPadding = 16;
	static constexpr auto kIcon = 24;

	QString _text;
	bool _on = false;
	QFont _font;
	QTimer _hide;
	QTimer _fade;
	int _step = 0;

};

class CaptureFilter final : public QObject {
public:
	CaptureFilter(
		QObject *parent,
		Fn<void(QKeyEvent*)> callback)
	: QObject(parent)
	, _callback(std::move(callback)) {
	}

	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event->type() == QEvent::KeyPress
			|| event->type() == QEvent::ShortcutOverride) {
			if (event->type() == QEvent::KeyPress) {
				_callback(static_cast<QKeyEvent*>(event));
			} else {
				event->accept();
			}
			return true;
		}
		return false;
	}

private:
	Fn<void(QKeyEvent*)> _callback;

};

} // namespace

void Start() {
	static auto started = false;
	if (started) {
		return;
	}
	started = true;
#ifdef Q_OS_WIN
	QCoreApplication::instance()->installNativeEventFilter(
		new NativeFilter());
#endif // Q_OS_WIN
	const auto &saved = Current();
	if (saved.vk) {
		(void)Register(saved.modifiers, saved.vk);
	}
}

QString Title() {
	return Current().title;
}

void ShowOverlay(bool ghostOn) {
	static auto current = QPointer<Overlay>();
	if (current) {
		current->close();
	}
	current = new Overlay(ghostOn);
	current->flash();
}

void ToggleGhost() {
	SetGhostAll(!GhostAll());
	ShowOverlay(GhostAll());
}

void CaptureBox(not_null<Ui::GenericBox*> box, Fn<void()> changed) {
	box->setTitle(rpl::single(u"Ghost mode shortcut"_q));
	const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Kerakli tugmalar birikmasini bosing (Ctrl, Alt yoki Shift "
		"bilan birga). Bekor qilish: Esc."_q,
		st::boxLabel));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Bu birikma Telegram yopiq yoki oynasi orqada bo'lsa ham ishlaydi."_q,
		st::boxLabel));

	const auto weak = QPointer<Ui::GenericBox>(box.get());
	const auto filter = new CaptureFilter(box.get(), [=](QKeyEvent *e) {
		if (e->isAutoRepeat() || !weak) {
			return;
		}
		switch (e->key()) {
		case Qt::Key_Control:
		case Qt::Key_Shift:
		case Qt::Key_Alt:
		case Qt::Key_AltGr:
		case Qt::Key_Meta:
		case Qt::Key_Super_L:
		case Qt::Key_Super_R:
			return;
		case Qt::Key_Escape:
			weak->closeBox();
			return;
		}
#ifdef Q_OS_WIN
		const auto mask = Qt::ControlModifier
			| Qt::AltModifier
			| Qt::ShiftModifier
			| Qt::MetaModifier;
		const auto modifiers = e->modifiers() & mask;
		const auto vk = uint(e->nativeVirtualKey());
		if (!vk || (!modifiers && !IsFunctionKey(vk))) {
			status->setText(
				u"Ctrl, Alt yoki Shift bilan birga bosing."_q);
			return;
		}
		auto &saved = Current();
		const auto previous = saved;
		const auto winModifiers = WinModifiers(modifiers);
		if (!Register(winModifiers, vk)) {
			(void)Register(previous.modifiers, previous.vk);
			status->setText(
				u"Bu birikma band. Boshqasini bosing."_q);
			return;
		}
		auto title = QString();
		if (modifiers & Qt::ControlModifier) {
			title += u"Ctrl+"_q;
		}
		if (modifiers & Qt::AltModifier) {
			title += u"Alt+"_q;
		}
		if (modifiers & Qt::ShiftModifier) {
			title += u"Shift+"_q;
		}
		if (modifiers & Qt::MetaModifier) {
			title += u"Win+"_q;
		}
		title += QKeySequence(int(e->key())).toString(
			QKeySequence::NativeText);
		saved.modifiers = winModifiers;
		saved.vk = vk;
		saved.title = title;
		Save();
		weak->closeBox();
		if (changed) {
			changed();
		}
#else // Q_OS_WIN
		status->setText(u"Bu funksiya faqat Windows'da ishlaydi."_q);
#endif // Q_OS_WIN
	});
	QCoreApplication::instance()->installEventFilter(filter);

	box->addButton(rpl::single(u"Olib tashlash"_q), [=] {
		auto &saved = Current();
		saved = Saved();
		(void)Register(0, 0);
		Save();
		box->closeBox();
		if (changed) {
			changed();
		}
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace Paradoks::Hotkey
