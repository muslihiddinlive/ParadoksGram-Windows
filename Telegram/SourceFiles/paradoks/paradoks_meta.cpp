/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_meta.h"

#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/layers/show.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_menu_icons.h"

#include <cstring>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeDatabase>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QImageReader>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <optional>

namespace Paradoks::Meta {
namespace {

constexpr auto kMaxRead = 4 * 1024 * 1024;

struct Exif {
	QStringList lines;
	std::optional<double> latitude;
	std::optional<double> longitude;
	bool found = false;
};

class Reader {
public:
	Reader(const QByteArray &data, int tiff)
	: _data(data)
	, _tiff(tiff) {
		_little = (data.mid(tiff, 2) == "II");
	}

	[[nodiscard]] bool valid() const {
		return _tiff + 8 <= _data.size()
			&& (_data.mid(_tiff, 2) == "II" || _data.mid(_tiff, 2) == "MM")
			&& u16(_tiff + 2) == 42;
	}
	[[nodiscard]] quint32 firstIfd() const {
		return u32(_tiff + 4);
	}

	struct Entry {
		quint16 tag = 0;
		quint16 type = 0;
		quint32 count = 0;
		int valueAt = 0; // absolute offset of the data
	};

	[[nodiscard]] std::vector<Entry> entries(quint32 ifd) const {
		auto result = std::vector<Entry>();
		const auto base = _tiff + int(ifd);
		if (base < 0 || base + 2 > _data.size()) {
			return result;
		}
		const auto count = int(u16(base));
		for (auto i = 0; i != count && i < 400; ++i) {
			const auto at = base + 2 + i * 12;
			if (at + 12 > _data.size()) {
				break;
			}
			auto entry = Entry{
				.tag = u16(at),
				.type = u16(at + 2),
				.count = u32(at + 4),
			};
			const auto size = TypeSize(entry.type) * qint64(entry.count);
			entry.valueAt = (size <= 4)
				? (at + 8)
				: (_tiff + int(u32(at + 8)));
			if (entry.valueAt < 0
				|| entry.valueAt + size > _data.size()
				|| size <= 0) {
				continue;
			}
			result.push_back(entry);
		}
		return result;
	}

	[[nodiscard]] QString text(const Entry &e) const {
		if (e.type != 2) {
			return QString();
		}
		auto bytes = _data.mid(e.valueAt, int(e.count));
		const auto zero = bytes.indexOf('\0');
		if (zero >= 0) {
			bytes.truncate(zero);
		}
		return QString::fromUtf8(bytes).trimmed();
	}
	[[nodiscard]] double number(const Entry &e, int index = 0) const {
		const auto size = TypeSize(e.type);
		const auto at = e.valueAt + index * size;
		switch (e.type) {
		case 1: case 7: return double(quint8(_data[at]));
		case 3: return double(u16(at));
		case 4: return double(u32(at));
		case 5: {
			const auto d = u32(at + 4);
			return d ? (double(u32(at)) / double(d)) : 0.;
		}
		case 10: {
			const auto d = qint32(u32(at + 4));
			return d ? (double(qint32(u32(at))) / double(d)) : 0.;
		}
		}
		return 0.;
	}

private:
	[[nodiscard]] static int TypeSize(quint16 type) {
		switch (type) {
		case 1: case 2: case 7: return 1;
		case 3: return 2;
		case 4: return 4;
		case 5: case 10: return 8;
		}
		return 0;
	}
	[[nodiscard]] quint16 u16(int at) const {
		if (at < 0 || at + 2 > _data.size()) {
			return 0;
		}
		const auto a = quint8(_data[at]);
		const auto b = quint8(_data[at + 1]);
		return _little ? quint16(a | (b << 8)) : quint16((a << 8) | b);
	}
	[[nodiscard]] quint32 u32(int at) const {
		if (at < 0 || at + 4 > _data.size()) {
			return 0;
		}
		const auto a = quint32(quint8(_data[at]));
		const auto b = quint32(quint8(_data[at + 1]));
		const auto c = quint32(quint8(_data[at + 2]));
		const auto d = quint32(quint8(_data[at + 3]));
		return _little
			? (a | (b << 8) | (c << 16) | (d << 24))
			: ((a << 24) | (b << 16) | (c << 8) | d);
	}

	const QByteArray &_data;
	int _tiff = 0;
	bool _little = true;

};

[[nodiscard]] double Degrees(const Reader &r, const Reader::Entry &e) {
	if (e.count < 3) {
		return 0.;
	}
	return r.number(e, 0) + r.number(e, 1) / 60. + r.number(e, 2) / 3600.;
}

[[nodiscard]] Exif ParseExif(const QByteArray &data) {
	auto result = Exif();
	if (data.size() < 4
		|| quint8(data[0]) != 0xFF
		|| quint8(data[1]) != 0xD8) {
		return result;
	}
	auto at = 2;
	auto tiff = -1;
	while (at + 4 < data.size()) {
		if (quint8(data[at]) != 0xFF) {
			break;
		}
		const auto marker = quint8(data[at + 1]);
		if (marker == 0xDA || marker == 0xD9) {
			break;
		}
		const auto length = (quint8(data[at + 2]) << 8)
			| quint8(data[at + 3]);
		if (marker == 0xE1
			&& data.mid(at + 4, 6) == QByteArray("Exif\0\0", 6)) {
			tiff = at + 10;
			break;
		}
		at += 2 + length;
	}
	if (tiff < 0) {
		return result;
	}
	const auto reader = Reader(data, tiff);
	if (!reader.valid()) {
		return result;
	}
	result.found = true;
	auto exifIfd = quint32(0);
	auto gpsIfd = quint32(0);
	auto add = [&](const QString &label, const QString &value) {
		if (!value.isEmpty()) {
			result.lines.push_back(label + u": "_q + value);
		}
	};
	for (const auto &e : reader.entries(reader.firstIfd())) {
		switch (e.tag) {
		case 0x010F: add(u"Ishlab chiqaruvchi"_q, reader.text(e)); break;
		case 0x0110: add(u"Qurilma modeli"_q, reader.text(e)); break;
		case 0x0131: add(u"Dastur (Software)"_q, reader.text(e)); break;
		case 0x0132: add(u"Sana (o'zgartirilgan)"_q, reader.text(e)); break;
		case 0x013B: add(u"Muallif (Artist)"_q, reader.text(e)); break;
		case 0x8298: add(u"Mualliflik (Copyright)"_q, reader.text(e)); break;
		case 0x010E: add(u"Tavsif"_q, reader.text(e)); break;
		case 0x8769: exifIfd = quint32(reader.number(e)); break;
		case 0x8825: gpsIfd = quint32(reader.number(e)); break;
		}
	}
	if (exifIfd) {
		for (const auto &e : reader.entries(exifIfd)) {
			switch (e.tag) {
			case 0x9003: add(u"Surat olingan vaqt"_q, reader.text(e)); break;
			case 0x9004: add(u"Raqamlashtirilgan vaqt"_q, reader.text(e)); break;
			case 0xA434: add(u"Obyektiv"_q, reader.text(e)); break;
			case 0xA430: add(u"Kamera egasi"_q, reader.text(e)); break;
			case 0xA431: add(u"Kamera seriya raqami"_q, reader.text(e)); break;
			case 0x8827:
				add(u"ISO"_q, QString::number(int(reader.number(e))));
				break;
			case 0x829D:
				add(u"Diafragma"_q, u"f/%1"_q.arg(reader.number(e), 0, 'f', 1));
				break;
			case 0x829A: {
				const auto value = reader.number(e);
				add(u"Ekspozitsiya"_q, value > 0 && value < 1
					? u"1/%1 s"_q.arg(int(1. / value + 0.5))
					: u"%1 s"_q.arg(value));
			} break;
			case 0x920A:
				add(u"Fokus masofasi"_q, u"%1 mm"_q.arg(reader.number(e), 0, 'f', 1));
				break;
			}
		}
	}
	if (gpsIfd) {
		auto latRef = QString();
		auto lonRef = QString();
		auto lat = std::optional<double>();
		auto lon = std::optional<double>();
		for (const auto &e : reader.entries(gpsIfd)) {
			switch (e.tag) {
			case 0x0001: latRef = reader.text(e); break;
			case 0x0002: lat = Degrees(reader, e); break;
			case 0x0003: lonRef = reader.text(e); break;
			case 0x0004: lon = Degrees(reader, e); break;
			case 0x0006:
				add(u"Balandlik"_q, u"%1 m"_q.arg(reader.number(e), 0, 'f', 1));
				break;
			case 0x001D: add(u"GPS sanasi"_q, reader.text(e)); break;
			}
		}
		if (lat && lon) {
			result.latitude = (latRef == u"S"_q) ? -*lat : *lat;
			result.longitude = (lonRef == u"W"_q) ? -*lon : *lon;
		}
	}
	return result;
}

[[nodiscard]] QString BuildReport(
		const QString &path,
		const QString &name,
		Exif &exif) {
	auto lines = QStringList();
	const auto info = QFileInfo(path);
	lines.push_back(u"Fayl: "_q + name);
	lines.push_back(u"Hajmi: %1 bayt"_q.arg(info.size()));
	lines.push_back(u"Turi: "_q
		+ QMimeDatabase().mimeTypeForFile(path).name());
	lines.push_back(u"Kompyuterdagi o'zgargan vaqti: "_q
		+ info.lastModified().toString(Qt::ISODate));
	auto reader = QImageReader(path);
	if (reader.canRead()) {
		const auto size = reader.size();
		lines.push_back(u"Format: "_q + QString::fromLatin1(reader.format()));
		if (size.isValid()) {
			lines.push_back(u"O'lchami: %1 x %2"_q.arg(size.width()).arg(size.height()));
		}
		for (const auto &key : reader.textKeys()) {
			lines.push_back(key + u": "_q + reader.text(key));
		}
	}
	lines.push_back(QString());
	auto file = QFile(path);
	if (file.open(QIODevice::ReadOnly)) {
		exif = ParseExif(file.read(kMaxRead));
	}
	if (exif.found && !exif.lines.isEmpty()) {
		lines.push_back(u"EXIF metama'lumotlar:"_q);
		lines += exif.lines;
	} else {
		lines.push_back(u"EXIF topilmadi (metama'lumot yo'q yoki "
			"o'chirilgan; masalan Telegram rasm sifatida yuborilganda "
			"uni o'chiradi)."_q);
	}
	if (exif.latitude && exif.longitude) {
		lines.push_back(QString());
		lines.push_back(u"JOYLASHUV (GPS): %1, %2"_q.arg(
			*exif.latitude,
			0,
			'f',
			6).arg(*exif.longitude, 0, 'f', 6));
		lines.push_back(u"Diqqat: bu fayl yuborilgan joyning koordinatasi "
			"saqlangan. O'z suratlaringizni fayl sifatida yuborishdan "
			"oldin metama'lumotni tozalang."_q);
	}
	return lines.join('\n');
}

void ShowWindow(const QString &title, const QString &report, const Exif &exif) {
	const auto window = new QWidget();
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->setWindowTitle(u"Metadata: "_q + title);
	window->resize(560, 560);
	const auto layout = new QVBoxLayout(window);
	const auto text = new QPlainTextEdit(window);
	text->setReadOnly(true);
	text->setPlainText(report);
	layout->addWidget(text, 1);
	const auto buttons = new QHBoxLayout();
	const auto copy = new QPushButton(u"Nusxalash"_q, window);
	buttons->addWidget(copy);
	if (exif.latitude && exif.longitude) {
		const auto map = new QPushButton(u"Xaritada ochish"_q, window);
		buttons->addWidget(map);
		const auto lat = *exif.latitude;
		const auto lon = *exif.longitude;
		QObject::connect(map, &QPushButton::clicked, window, [=] {
			QDesktopServices::openUrl(QUrl(
				u"https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=16/%1/%2"_q
					.arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6)));
		});
	}
	buttons->addStretch(1);
	layout->addLayout(buttons);
	QObject::connect(copy, &QPushButton::clicked, window, [=] {
		QGuiApplication::clipboard()->setText(report);
	});
	window->show();
}

[[nodiscard]] bool IsImageName(const QString &name) {
	const auto dot = name.lastIndexOf('.');
	if (dot < 0) {
		return false;
	}
	const auto ext = name.mid(dot + 1).toLower();
	return ext == u"jpg"_q || ext == u"jpeg"_q || ext == u"png"_q
		|| ext == u"tif"_q || ext == u"tiff"_q || ext == u"webp"_q
		|| ext == u"gif"_q || ext == u"bmp"_q;
}

} // namespace

void AddMetaAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || !IsImageName(document->filename())) {
		return;
	}
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	menu->addAction(u"Metadata (EXIF) ni ko'rish"_q, [=] {
		const auto again = session->data().message(id);
		const auto current = (again && again->media())
			? again->media()->document()
			: nullptr;
		if (!current) {
			return;
		}
		const auto path = current->filepath(true);
		if (path.isEmpty() || !QFileInfo::exists(path)) {
			show->showToast(u"Avval faylni yuklab oling"_q);
			return;
		}
		auto exif = Exif();
		const auto report = BuildReport(path, current->filename(), exif);
		ShowWindow(current->filename(), report, exif);
	}, &st::menuIconInfo);
}

namespace {

[[nodiscard]] int ExifOrientation(const uchar *d, int size) {
	// d points to "Exif\0\0" + TIFF header.
	if (size < 20 || memcmp(d, "Exif\0\0", 6) != 0) {
		return 1;
	}
	const auto t = d + 6;
	const auto n = size - 6;
	const auto big = (t[0] == 'M');
	if (!big && t[0] != 'I') {
		return 1;
	}
	const auto r16 = [&](int o) -> int {
		return (o + 2 > n) ? 0 : big ? ((t[o] << 8) | t[o + 1]) : ((t[o + 1] << 8) | t[o]);
	};
	const auto r32 = [&](int o) -> int {
		if (o + 4 > n) {
			return n;
		}
		return big
			? int((uint(t[o]) << 24) | (uint(t[o + 1]) << 16) | (uint(t[o + 2]) << 8) | uint(t[o + 3]))
			: int((uint(t[o + 3]) << 24) | (uint(t[o + 2]) << 16) | (uint(t[o + 1]) << 8) | uint(t[o]));
	};
	const auto ifd = r32(4);
	if (ifd < 8 || ifd + 2 > n) {
		return 1;
	}
	const auto count = r16(ifd);
	for (auto i = 0; i != count; ++i) {
		const auto e = ifd + 2 + i * 12;
		if (e + 12 > n) {
			break;
		}
		if (r16(e) == 0x0112) {
			const auto v = r16(e + 8);
			return (v >= 1 && v <= 8) ? v : 1;
		}
	}
	return 1;
}

[[nodiscard]] QByteArray OrientationOnlyExif(int orientation) {
	auto result = QByteArray("Exif\0\0", 6);
	result.append("MM\0*\0\0\0\10", 8);
	result.append("\0\1", 2);
	result.append("\x01\x12\0\3\0\0\0\1", 8);
	result.append(char(0));
	result.append(char(orientation));
	result.append("\0\0", 2);
	result.append("\0\0\0\0", 4);
	return result;
}

[[nodiscard]] QByteArray StripJpeg(const QByteArray &data) {
	const auto d = reinterpret_cast<const uchar*>(data.constData());
	const auto size = int(data.size());
	if (size < 4 || d[0] != 0xFF || d[1] != 0xD8) {
		return {};
	}
	auto result = QByteArray();
	result.reserve(size);
	result.append(data.constData(), 2);
	auto changed = false;
	auto pos = 2;
	while (pos + 4 <= size) {
		if (d[pos] != 0xFF) {
			return {};
		}
		const auto marker = d[pos + 1];
		if (marker == 0xFF) {
			++pos;
			continue;
		}
		if (marker == 0xDA || marker == 0xD9) {
			break;
		}
		if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
			result.append(data.constData() + pos, 2);
			pos += 2;
			continue;
		}
		const auto length = (int(d[pos + 2]) << 8) | d[pos + 3];
		if (length < 2 || pos + 2 + length > size) {
			return {};
		}
		const auto seg = data.constData() + pos;
		const auto total = 2 + length;
		auto drop = false;
		if (marker == 0xE1) {
			// EXIF or XMP.
			const auto orientation = ExifOrientation(d + pos + 4, length - 2);
			drop = true;
			changed = true;
			if (orientation != 1
				&& length >= 8
				&& memcmp(d + pos + 4, "Exif\0\0", 6) == 0) {
				const auto body = OrientationOnlyExif(orientation);
				const auto len = int(body.size()) + 2;
				result.append(char(0xFF));
				result.append(char(0xE1));
				result.append(char(len >> 8));
				result.append(char(len & 0xFF));
				result.append(body);
			}
		} else if (marker == 0xED // IPTC / Photoshop
			|| marker == 0xFE // Comment
			|| marker == 0xEB // APP11 (JUMBF)
			|| marker == 0xEC // APP12
			|| marker == 0xEE) { // APP14 Adobe: keep colour only if needed
			if (marker != 0xEE) {
				drop = true;
				changed = true;
			}
		} else if (marker == 0xE2
			&& length > 14
			&& memcmp(d + pos + 4, "MPF\0", 4) == 0) {
			drop = true; // Multi-picture offsets, may hold extra images.
			changed = true;
		}
		if (!drop) {
			result.append(seg, total);
		}
		pos += total;
	}
	if (!changed) {
		return {};
	}
	result.append(data.constData() + pos, size - pos);
	return result;
}

[[nodiscard]] QByteArray StripPng(const QByteArray &data) {
	static const auto kSignature = QByteArray("\x89PNG\r\n\x1a\n", 8);
	if (data.size() < 20 || !data.startsWith(kSignature)) {
		return {};
	}
	const auto d = reinterpret_cast<const uchar*>(data.constData());
	const auto size = int(data.size());
	auto result = QByteArray();
	result.reserve(size);
	result.append(kSignature);
	auto changed = false;
	auto pos = 8;
	while (pos + 12 <= size) {
		const auto length = (qint64(d[pos]) << 24)
			| (qint64(d[pos + 1]) << 16)
			| (qint64(d[pos + 2]) << 8)
			| qint64(d[pos + 3]);
		const auto total = length + 12;
		if (pos + total > size) {
			return {};
		}
		const auto type = QByteArray(data.constData() + pos + 4, 4);
		const auto drop = (type == "tEXt")
			|| (type == "zTXt")
			|| (type == "iTXt")
			|| (type == "eXIf")
			|| (type == "tIME");
		if (drop) {
			changed = true;
		} else {
			result.append(data.constData() + pos, int(total));
		}
		pos += int(total);
		if (type == "IEND") {
			break;
		}
	}
	return changed ? result : QByteArray();
}

} // namespace

QByteArray StripImageMetadata(const QByteArray &data) {
	if (data.size() < 12) {
		return {};
	}
	if (auto jpeg = StripJpeg(data); !jpeg.isEmpty()) {
		return jpeg;
	}
	return StripPng(data);
}

} // namespace Paradoks::Meta
