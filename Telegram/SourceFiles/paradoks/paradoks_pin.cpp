/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_pin.h"

#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"

#include <QtCore/QDateTime>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace Paradoks::Pin {
namespace {

constexpr auto kMinLength = 4;
constexpr auto kMaxLength = 12;
constexpr auto kMaxFails = 5;
constexpr auto kCooldown = qint64(30000);
constexpr auto kWidth = 340;
constexpr auto kHeight = 600;
constexpr auto kKeyDiameter = 68;
constexpr auto kPitchX = 96;
constexpr auto kPitchY = 80;
constexpr auto kPadTop = 250;
constexpr auto kBack = 9;
constexpr auto kOk = 11;

int Fails = 0;
qint64 BlockedUntil = 0;

struct State {
	QString entered;
	QString first;
	QString title;
	QString subtitle;
	QString initialTitle;
	QString initialSubtitle;
	int pressed = -1;
};

[[nodiscard]] QString KeyDigit(int index) {
	static const auto digits = std::array<QString, 12>{
		u"1"_q, u"2"_q, u"3"_q,
		u"4"_q, u"5"_q, u"6"_q,
		u"7"_q, u"8"_q, u"9"_q,
		QString(), u"0"_q, QString(),
	};
	return digits[index];
}

[[nodiscard]] QString KeyLetters(int index) {
	static const auto letters = std::array<QString, 12>{
		QString(), u"ABC"_q, u"DEF"_q,
		u"GHI"_q, u"JKL"_q, u"MNO"_q,
		u"PQRS"_q, u"TUV"_q, u"WXYZ"_q,
		QString(), QString(), QString(),
	};
	return letters[index];
}

[[nodiscard]] QPoint KeyCenter(int width, int index) {
	const auto row = index / 3;
	const auto column = index % 3;
	const auto left = (width - 3 * kPitchX) / 2 + kPitchX / 2;
	return QPoint(
		left + column * kPitchX,
		kPadTop + kKeyDiameter / 2 + row * kPitchY);
}

[[nodiscard]] int KeyAt(int width, QPoint point) {
	for (auto i = 0; i != 12; ++i) {
		const auto delta = point - KeyCenter(width, i);
		const auto radius = kKeyDiameter / 2;
		if (delta.x() * delta.x() + delta.y() * delta.y()
			<= radius * radius) {
			return i;
		}
	}
	return -1;
}

void SetPixelSize(QPainter &p, int size, bool bold) {
	auto font = p.font();
	font.setPixelSize(size);
	font.setBold(bold);
	p.setFont(font);
}

} // namespace

void BuildPinScreen(
		not_null<Ui::GenericBox*> box,
		const QString &title,
		const QString &subtitle,
		Fn<bool(const QString&)> validate,
		Fn<void(const QString&)> done) {
	box->setWidth(kWidth);
	box->setNoContentMargin(true);
	const auto widget = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		style::margins());
	widget->resize(kWidth, kHeight);

	const auto state = std::make_shared<State>();
	state->title = state->initialTitle = title;
	state->subtitle = state->initialSubtitle = subtitle;

	const auto finish = [=](const QString &pin) {
		auto callback = done;
		box->closeBox();
		if (callback) {
			callback(pin);
		}
	};
	const auto confirm = [=] {
		if (state->entered.size() < kMinLength) {
			return;
		}
		const auto pin = state->entered;
		if (!validate) {
			if (state->first.isEmpty()) {
				state->first = pin;
				state->entered.clear();
				state->title = u"PIN ni takrorlang"_q;
				state->subtitle
					= u"Xuddi shu PIN ni yana bir marta kiriting"_q;
			} else if (state->first == pin) {
				finish(pin);
				return;
			} else {
				state->first.clear();
				state->entered.clear();
				state->title = state->initialTitle;
				state->subtitle
					= u"PIN lar mos kelmadi, qaytadan kiriting"_q;
			}
			widget->update();
			return;
		}
		const auto now = QDateTime::currentMSecsSinceEpoch();
		if (now < BlockedUntil) {
			state->entered.clear();
			state->subtitle
				= u"Juda ko'p xato urinish. %1 soniyadan keyin urinib ko'ring"_q.arg(
					std::max<qint64>(1, (BlockedUntil - now + 999) / 1000));
			widget->update();
			return;
		}
		if (validate(pin)) {
			Fails = 0;
			finish(pin);
			return;
		}
		++Fails;
		state->entered.clear();
		if (Fails >= kMaxFails) {
			Fails = 0;
			BlockedUntil = now + kCooldown;
			state->subtitle
				= u"Juda ko'p xato urinish. 30 soniyadan keyin urinib ko'ring"_q;
		} else {
			state->subtitle
				= u"PIN noto'g'ri. Qolgan urinishlar: %1"_q.arg(
					kMaxFails - Fails);
		}
		widget->update();
	};
	const auto press = [=](int index) {
		if (index == kBack) {
			if (!state->entered.isEmpty()) {
				state->entered.chop(1);
			}
		} else if (index == kOk) {
			confirm();
			return;
		} else if (index >= 0
			&& state->entered.size() < kMaxLength) {
			state->entered += KeyDigit(index);
		}
		widget->update();
	};

	widget->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(widget);
		p.setRenderHint(QPainter::Antialiasing);
		const auto w = widget->width();
		auto gradient = QLinearGradient(0, 0, 0, kHeight);
		gradient.setColorAt(0., QColor(10, 16, 28));
		gradient.setColorAt(1., QColor(27, 38, 56));
		p.fillRect(widget->rect(), gradient);

		// Lock icon.
		const auto cx = w / 2;
		p.setPen(QPen(Qt::white, 4));
		p.setBrush(Qt::NoBrush);
		p.drawArc(QRect(cx - 11, 40, 22, 28), 0, 180 * 16);
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		p.drawRoundedRect(QRect(cx - 17, 56, 34, 28), 5, 5);
		p.setBrush(QColor(27, 38, 56));
		p.drawEllipse(QPoint(cx, 68), 4, 4);

		// Title and subtitle.
		p.setPen(Qt::white);
		SetPixelSize(p, 20, true);
		p.drawText(
			QRect(24, 112, w - 48, 30),
			Qt::AlignHCenter | Qt::AlignVCenter,
			state->title);
		p.setPen(QColor(255, 255, 255, 204));
		SetPixelSize(p, 14, false);
		p.drawText(
			QRect(24, 146, w - 48, 44),
			Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
			state->subtitle);

		// Dots.
		const auto count = std::max<int>(kMinLength, state->entered.size());
		const auto gap = 30;
		const auto first = cx - (count - 1) * gap / 2;
		for (auto i = 0; i != count; ++i) {
			const auto center = QPoint(first + i * gap, 214);
			if (i < state->entered.size()) {
				p.setPen(Qt::NoPen);
				p.setBrush(Qt::white);
			} else {
				p.setPen(QPen(QColor(255, 255, 255, 140), 1.5));
				p.setBrush(Qt::NoBrush);
			}
			p.drawEllipse(center, 6, 6);
		}

		// Number pad.
		for (auto i = 0; i != 12; ++i) {
			const auto center = KeyCenter(w, i);
			const auto circle = QRect(
				center.x() - kKeyDiameter / 2,
				center.y() - kKeyDiameter / 2,
				kKeyDiameter,
				kKeyDiameter);
			const auto accent = (i == kOk)
				&& (state->entered.size() >= kMinLength);
			p.setPen(QPen(QColor(255, 255, 255, 38), 1));
			p.setBrush(accent
				? QColor(70, 140, 255, state->pressed == i ? 230 : 190)
				: QColor(255, 255, 255, state->pressed == i ? 102 : 51));
			p.drawEllipse(circle);
			p.setPen(Qt::white);
			if (i == kBack) {
				p.setPen(QPen(Qt::white, 2.2, Qt::SolidLine, Qt::RoundCap));
				p.drawLine(center.x() + 12, center.y(), center.x() - 12, center.y());
				p.drawLine(center.x() - 12, center.y(), center.x() - 4, center.y() - 8);
				p.drawLine(center.x() - 12, center.y(), center.x() - 4, center.y() + 8);
			} else if (i == kOk) {
				p.setPen(QPen(
					accent ? QColor(Qt::white) : QColor(255, 255, 255, 90),
					2.6,
					Qt::SolidLine,
					Qt::RoundCap,
					Qt::RoundJoin));
				p.drawLine(center.x() - 10, center.y() + 1, center.x() - 3, center.y() + 8);
				p.drawLine(center.x() - 3, center.y() + 8, center.x() + 11, center.y() - 8);
			} else {
				const auto letters = KeyLetters(i);
				SetPixelSize(p, 28, false);
				p.drawText(
					QRect(circle.left(), circle.top() - (letters.isEmpty() ? 0 : 7), circle.width(), circle.height()),
					Qt::AlignCenter,
					KeyDigit(i));
				if (!letters.isEmpty()) {
					p.setPen(QColor(255, 255, 255, 153));
					SetPixelSize(p, 10, false);
					p.drawText(
						QRect(circle.left(), circle.top() + 44, circle.width(), 14),
						Qt::AlignCenter,
						letters);
				}
			}
		}
	}, widget->lifetime());

	widget->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::MouseButtonPress) {
			const auto event = static_cast<QMouseEvent*>(e.get());
			state->pressed = KeyAt(widget->width(), event->pos());
			widget->update();
		} else if (e->type() == QEvent::MouseButtonRelease) {
			const auto event = static_cast<QMouseEvent*>(e.get());
			const auto index = KeyAt(widget->width(), event->pos());
			const auto pressed = std::exchange(state->pressed, -1);
			widget->update();
			if (index >= 0 && index == pressed) {
				press(index);
			}
		}
	}, widget->lifetime());

	box->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() != QEvent::KeyPress) {
			return;
		}
		const auto key = static_cast<QKeyEvent*>(e.get());
		const auto text = key->text();
		if (key->key() == Qt::Key_Backspace) {
			press(kBack);
		} else if (key->key() == Qt::Key_Return
			|| key->key() == Qt::Key_Enter) {
			press(kOk);
		} else if (text.size() == 1
			&& text[0] >= QChar('0')
			&& text[0] <= QChar('9')) {
			const auto digit = text[0].unicode() - '0';
			press(digit == 0 ? 10 : digit - 1);
		} else if (text.size() == 1 && text[0].isLetterOrNumber()) {
			// PINs made before the number pad may contain letters.
			if (state->entered.size() < kMaxLength) {
				state->entered += text;
				widget->update();
			}
		}
	}, box->lifetime());
}

} // namespace Paradoks::Pin
