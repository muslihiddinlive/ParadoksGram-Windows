/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_icon.h"

#include "paradoks/paradoks_config.h"
#include "core/application.h"
#include "lang/lang_keys.h"
#include "settings.h"
#include "tray.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/labels.h"
#include "styles/style_basic.h"
#include "styles/style_layers.h"

#include <algorithm>
#include <array>

#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif // Q_OS_WIN

namespace Paradoks::AppIcon {
namespace {

constexpr auto kTileWidth = 84;
constexpr auto kTileHeight = 108;
constexpr auto kTileGap = 6;
constexpr auto kPictureSize = 60;

[[nodiscard]] QString Title(int variant) {
	switch (variant) {
	case 1: return u"Telegram"_q;
	case 2: return u"Neon"_q;
	case 3: return u"Matrix"_q;
	}
	return u"Ghost"_q;
}

[[nodiscard]] QImage Picture(int variant) {
	static auto cache = std::array<QImage, kVariants>();
	auto &image = cache[variant];
	if (image.isNull()) {
		image = QImage(ResourcePath(variant, true));
	}
	return image;
}

// A .ico with PNG frames; Windows (Vista+) reads these natively.
[[nodiscard]] QByteArray BuildIco(const QImage &source) {
	static const auto sizes = { 16, 24, 32, 48, 64, 128, 256 };
	auto frames = std::vector<QByteArray>();
	for (const auto size : sizes) {
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		source.scaled(
			size,
			size,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation).save(&buffer, "PNG");
		frames.push_back(std::move(bytes));
	}
	auto result = QByteArray();
	const auto put16 = [&](quint16 value) {
		result.append(char(value & 0xFF));
		result.append(char((value >> 8) & 0xFF));
	};
	const auto put32 = [&](quint32 value) {
		put16(quint16(value & 0xFFFF));
		put16(quint16((value >> 16) & 0xFFFF));
	};
	put16(0);
	put16(1);
	put16(quint16(frames.size()));
	auto offset = quint32(6 + 16 * frames.size());
	auto index = 0;
	for (const auto size : sizes) {
		const auto &frame = frames[index++];
		result.append(char(size >= 256 ? 0 : size));
		result.append(char(size >= 256 ? 0 : size));
		result.append(char(0));
		result.append(char(0));
		put16(1);
		put16(32);
		put32(quint32(frame.size()));
		put32(offset);
		offset += quint32(frame.size());
	}
	for (const auto &frame : frames) {
		result.append(frame);
	}
	return result;
}

[[nodiscard]] QString EnsureIcoFile(int variant) {
	const auto dir = cWorkingDir() + u"tdata/icons/"_q;
	QDir().mkpath(dir);
	const auto path = dir + u"app_%1.ico"_q.arg(variant);
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(BuildIco(QImage(ResourcePath(variant, false))));
		file.close();
		return path;
	}
	return QString();
}

#ifdef Q_OS_WIN

bool RetargetShortcut(
		const QString &lnkPath,
		const QString &icoPath,
		const QString &exePath) {
	auto link = (IShellLinkW*)nullptr;
	if (FAILED(CoCreateInstance(
			CLSID_ShellLink,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_IShellLinkW,
			reinterpret_cast<void**>(&link))) || !link) {
		return false;
	}
	auto changed = false;
	auto file = (IPersistFile*)nullptr;
	if (SUCCEEDED(link->QueryInterface(
			IID_IPersistFile,
			reinterpret_cast<void**>(&file))) && file) {
		const auto native = QDir::toNativeSeparators(lnkPath).toStdWString();
		if (SUCCEEDED(file->Load(native.c_str(), STGM_READWRITE))) {
			WCHAR target[MAX_PATH * 2] = {};
			link->GetPath(target, MAX_PATH * 2, nullptr, SLGP_RAWPATH);
			const auto mine = QDir::cleanPath(QString::fromWCharArray(target))
				.compare(
					QDir::cleanPath(exePath),
					Qt::CaseInsensitive) == 0;
			if (mine) {
				const auto ico = QDir::toNativeSeparators(icoPath)
					.toStdWString();
				if (SUCCEEDED(link->SetIconLocation(ico.c_str(), 0))
					&& SUCCEEDED(file->Save(native.c_str(), TRUE))) {
					changed = true;
				}
			}
		}
		file->Release();
	}
	link->Release();
	return changed;
}

void RetargetIn(
		const QString &folder,
		bool recursive,
		const QString &icoPath,
		const QString &exePath) {
	if (folder.isEmpty() || !QDir(folder).exists()) {
		return;
	}
	auto iterator = QDirIterator(
		folder,
		QStringList{ u"*.lnk"_q },
		QDir::Files,
		recursive
			? QDirIterator::Subdirectories
			: QDirIterator::NoIteratorFlags);
	while (iterator.hasNext()) {
		(void)RetargetShortcut(iterator.next(), icoPath, exePath);
	}
}

void UpdateShortcuts(int variant) {
	const auto ico = EnsureIcoFile(variant);
	if (ico.isEmpty()) {
		return;
	}
	const auto exe = QCoreApplication::applicationFilePath();
	for (const auto &folder : QStandardPaths::standardLocations(
			QStandardPaths::DesktopLocation)) {
		RetargetIn(folder, false, ico, exe);
	}
	for (const auto &folder : QStandardPaths::standardLocations(
			QStandardPaths::ApplicationsLocation)) {
		RetargetIn(folder, true, ico, exe);
	}
	WCHAR appData[MAX_PATH * 2] = {};
	if (GetEnvironmentVariable(L"APPDATA", appData, MAX_PATH * 2)) {
		RetargetIn(
			QString::fromWCharArray(appData)
				+ u"/Microsoft/Internet Explorer/Quick Launch/"
				"User Pinned/TaskBar"_q,
			false,
			ico,
			exe);
	}
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, 0, 0);
}

#else // Q_OS_WIN

void UpdateShortcuts(int variant) {
}

#endif // Q_OS_WIN

} // namespace

int Current() {
	return std::clamp(AppIconVariant(), 0, kVariants - 1);
}

QString ResourcePath(int variant, bool noMargin) {
	const auto suffix = noMargin ? u"_no_margin"_q : QString();
	switch (variant) {
	case 1: return u":/gui/art/logo_256_tg%1.png"_q.arg(suffix);
	case 2: return u":/gui/art/logo_256_neon%1.png"_q.arg(suffix);
	case 3: return u":/gui/art/logo_256_matrix%1.png"_q.arg(suffix);
	}
	return u":/gui/art/logo_256%1.png"_q.arg(suffix);
}

void Apply(int variant) {
	SetAppIconVariant(std::clamp(variant, 0, kVariants - 1));
	Core::App().refreshApplicationIcon();
	Core::App().tray().updateIconCounters();
	UpdateShortcuts(Current());
}

void PickerBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Ilova ikonkasi"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto widget = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		st::boxRowPadding);
	widget->resize(widget->width(), kTileHeight);
	widget->setMouseTracking(true);

	struct State {
		int selected = 0;
		int hovered = -1;
		int columns = 1;
	};
	const auto state = widget->lifetime().make_state<State>();
	state->selected = Current();

	// The grid wraps into rows, the box scrolls when there are many icons.
	const auto layout = [=](int width) {
		state->columns = std::max(
			1,
			(width + kTileGap) / (kTileWidth + kTileGap));
		const auto rows = (kVariants + state->columns - 1) / state->columns;
		widget->resize(
			width,
			rows * kTileHeight + (rows - 1) * kTileGap);
	};
	const auto tileRect = [=](int index) {
		const auto total = state->columns * kTileWidth
			+ (state->columns - 1) * kTileGap;
		const auto left = (widget->width() - total) / 2;
		const auto row = index / state->columns;
		const auto column = index % state->columns;
		return QRect(
			left + column * (kTileWidth + kTileGap),
			row * (kTileHeight + kTileGap),
			kTileWidth,
			kTileHeight);
	};
	const auto tileAt = [=](QPoint point) {
		for (auto i = 0; i != kVariants; ++i) {
			if (tileRect(i).contains(point)) {
				return i;
			}
		}
		return -1;
	};
	widget->widthValue(
	) | rpl::on_next([=](int width) {
		layout(width);
		widget->update();
	}, widget->lifetime());

	widget->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(widget);
		auto hq = PainterHighQualityEnabler(p);
		for (auto i = 0; i != kVariants; ++i) {
			const auto tile = tileRect(i);
			const auto selected = (state->selected == i);
			if (selected) {
				p.setPen(QPen(st::activeButtonBg, 2));
				p.setBrush(Qt::NoBrush);
				p.drawRoundedRect(tile.adjusted(1, 1, -1, -1), 12, 12);
			} else if (state->hovered == i) {
				p.setPen(QPen(st::windowSubTextFg, 1));
				p.setBrush(Qt::NoBrush);
				p.drawRoundedRect(tile.adjusted(1, 1, -1, -1), 12, 12);
			}
			const auto picture = QRect(
				tile.x() + (kTileWidth - kPictureSize) / 2,
				tile.y() + 12,
				kPictureSize,
				kPictureSize);
			p.drawImage(picture, Picture(i));
			if (selected) {
				// A check mark badge, like in the Telegram app.
				const auto badge = QRect(
					picture.right() - 12,
					picture.bottom() - 12,
					20,
					20);
				p.setPen(QPen(st::boxBg, 2));
				p.setBrush(st::activeButtonBg);
				p.drawEllipse(badge);
				p.setPen(QPen(
					st::activeButtonFg,
					2,
					Qt::SolidLine,
					Qt::RoundCap,
					Qt::RoundJoin));
				p.drawLine(
					badge.x() + 5,
					badge.y() + 10,
					badge.x() + 9,
					badge.y() + 14);
				p.drawLine(
					badge.x() + 9,
					badge.y() + 14,
					badge.x() + 15,
					badge.y() + 6);
			}
			p.setPen(selected ? st::activeButtonBg : st::windowFg);
			p.setFont(st::normalFont);
			p.drawText(
				QRect(
					tile.x() + 4,
					tile.y() + 12 + kPictureSize + 8,
					kTileWidth - 8,
					24),
				Qt::AlignHCenter | Qt::AlignTop,
				st::normalFont->elided(Title(i), kTileWidth - 8));
		}
	}, widget->lifetime());

	widget->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		switch (e->type()) {
		case QEvent::MouseMove: {
			const auto hovered = tileAt(
				static_cast<QMouseEvent*>(e.get())->pos());
			if (state->hovered != hovered) {
				state->hovered = hovered;
				widget->setCursor((hovered >= 0)
					? style::cur_pointer
					: style::cur_default);
				widget->update();
			}
		} break;
		case QEvent::Leave:
			state->hovered = -1;
			widget->update();
			break;
		case QEvent::MouseButtonPress: {
			const auto mouse = static_cast<QMouseEvent*>(e.get());
			const auto index = tileAt(mouse->pos());
			if (mouse->button() == Qt::LeftButton
				&& index >= 0
				&& index != state->selected) {
				state->selected = index;
				widget->update();
				Apply(index);
			}
		} break;
		default: break;
		}
	}, widget->lifetime());

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Ikonka oyna, taskbar, tray va yorliqlarda (Desktop, Start menyu, "
		"taskbar'ga qadalgan) almashadi. .exe faylning o'z ikonkasi "
		"o'zgarmaydi."_q,
		st::boxLabel));
}

} // namespace Paradoks::AppIcon
