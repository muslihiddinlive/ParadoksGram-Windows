/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_viewer.h"

#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/layers/show.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTextCodec>
#include <QtCore/QXmlStreamReader>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <map>

namespace Paradoks::Viewer {
namespace {

constexpr auto kMaxFileSize = qint64(40) * 1024 * 1024;
constexpr auto kMaxTextSize = 3 * 1000 * 1000;
constexpr auto kMaxLines = 40000;

[[nodiscard]] bool IsOffice(const QString &ext) {
	return (ext == u"docx"_q) || (ext == u"xlsx"_q) || (ext == u"pptx"_q)
		|| (ext == u"odt"_q) || (ext == u"ods"_q) || (ext == u"odp"_q);
}

[[nodiscard]] bool IsPlain(const QString &ext) {
	static const auto list = QStringList{
		u"txt"_q, u"md"_q, u"log"_q, u"json"_q, u"xml"_q, u"html"_q,
		u"htm"_q, u"csv"_q, u"tsv"_q, u"ini"_q, u"cfg"_q, u"yml"_q,
		u"yaml"_q, u"py"_q, u"js"_q, u"ts"_q, u"java"_q, u"kt"_q,
		u"cpp"_q, u"h"_q, u"c"_q, u"cs"_q, u"php"_q, u"sql"_q,
		u"bat"_q, u"sh"_q, u"css"_q, u"lua"_q, u"go"_q, u"rs"_q,
	};
	return list.contains(ext);
}

[[nodiscard]] QString ExtOf(const QString &name) {
	return QFileInfo(name).suffix().toLower();
}

// Members of a zip are read with the system tar.exe (Windows 10 and newer).
[[nodiscard]] QByteArray RunTar(const QStringList &arguments) {
	auto process = QProcess();
#ifdef Q_OS_WIN
	process.setCreateProcessArgumentsModifier([](
			QProcess::CreateProcessArguments *args) {
		args->flags |= 0x08000000; // CREATE_NO_WINDOW
	});
#endif // Q_OS_WIN
	process.start(u"tar"_q, arguments);
	if (!process.waitForStarted(5000)) {
		return QByteArray();
	}
	if (!process.waitForFinished(30000)) {
		process.kill();
		return QByteArray();
	}
	return process.readAllStandardOutput();
}

[[nodiscard]] QByteArray ReadMember(
		const QString &archive,
		const QString &member) {
	return RunTar({ u"-xOf"_q, archive, member });
}

[[nodiscard]] QStringList ListMembers(const QString &archive) {
	const auto output = QString::fromUtf8(RunTar({ u"-tf"_q, archive }));
	auto result = QStringList();
	for (const auto &line : output.split('\n')) {
		const auto trimmed = line.trimmed();
		if (!trimmed.isEmpty()) {
			result.push_back(trimmed);
		}
	}
	return result;
}

[[nodiscard]] int NumberIn(const QString &name) {
	static const auto expression = QRegularExpression(u"(\\d+)"_q);
	const auto match = expression.match(QFileInfo(name).baseName());
	return match.hasMatch() ? match.captured(1).toInt() : 0;
}

[[nodiscard]] QStringList MembersLike(
		const QStringList &all,
		const QString &prefix,
		const QString &suffix) {
	auto result = QStringList();
	for (const auto &name : all) {
		if (name.startsWith(prefix) && name.endsWith(suffix)) {
			result.push_back(name);
		}
	}
	std::sort(result.begin(), result.end(), [](
			const QString &a,
			const QString &b) {
		return NumberIn(a) < NumberIn(b);
	});
	return result;
}

// Word and PowerPoint: paragraphs <p>, runs <t>, tabs and breaks.
[[nodiscard]] QString ParagraphsText(const QByteArray &xml) {
	auto result = QString();
	auto reader = QXmlStreamReader(xml);
	while (!reader.atEnd()) {
		reader.readNext();
		if (reader.isStartElement()) {
			const auto name = reader.name();
			if (name == QLatin1String("t")) {
				result += reader.readElementText();
			} else if (name == QLatin1String("tab")) {
				result += '\t';
			} else if (name == QLatin1String("br")
				|| name == QLatin1String("cr")) {
				result += '\n';
			}
		} else if (reader.isEndElement()
			&& reader.name() == QLatin1String("p")) {
			result += '\n';
		}
		if (result.size() > kMaxTextSize) {
			break;
		}
	}
	return result;
}

[[nodiscard]] QString DocxText(const QString &path) {
	return ParagraphsText(ReadMember(path, u"word/document.xml"_q));
}

[[nodiscard]] QString PptxText(const QString &path) {
	auto result = QString();
	const auto slides = MembersLike(
		ListMembers(path),
		u"ppt/slides/slide"_q,
		u".xml"_q);
	auto index = 0;
	for (const auto &slide : slides) {
		result += u"\n—— Slayd %1 ——\n"_q.arg(++index);
		result += ParagraphsText(ReadMember(path, slide));
		if (result.size() > kMaxTextSize) {
			break;
		}
	}
	return result;
}

[[nodiscard]] int ColumnIndex(const QString &reference) {
	auto result = 0;
	auto letters = 0;
	for (const auto ch : reference) {
		if (ch >= 'A' && ch <= 'Z') {
			result = result * 26 + (ch.unicode() - 'A' + 1);
			++letters;
		} else if (ch >= 'a' && ch <= 'z') {
			result = result * 26 + (ch.unicode() - 'a' + 1);
			++letters;
		} else {
			break;
		}
	}
	return letters ? (result - 1) : -1;
}

[[nodiscard]] QStringList SharedStrings(const QByteArray &xml) {
	auto result = QStringList();
	auto reader = QXmlStreamReader(xml);
	auto current = QString();
	auto inItem = false;
	while (!reader.atEnd()) {
		reader.readNext();
		if (reader.isStartElement()) {
			const auto name = reader.name();
			if (name == QLatin1String("si")) {
				inItem = true;
				current.clear();
			} else if (inItem && name == QLatin1String("t")) {
				current += reader.readElementText();
			}
		} else if (reader.isEndElement()
			&& reader.name() == QLatin1String("si")) {
			inItem = false;
			result.push_back(current);
		}
	}
	return result;
}

[[nodiscard]] QString SheetText(
		const QByteArray &xml,
		const QStringList &shared,
		int &lines) {
	auto result = QString();
	auto reader = QXmlStreamReader(xml);
	auto row = QStringList();
	auto type = QString();
	auto value = QString();
	auto column = 0;
	auto inCell = false;
	while (!reader.atEnd() && lines < kMaxLines) {
		reader.readNext();
		if (reader.isStartElement()) {
			const auto name = reader.name();
			if (name == QLatin1String("row")) {
				row.clear();
			} else if (name == QLatin1String("c")) {
				const auto attributes = reader.attributes();
				type = attributes.value(QLatin1String("t")).toString();
				const auto index = ColumnIndex(
					attributes.value(QLatin1String("r")).toString());
				column = (index >= 0) ? index : int(row.size());
				value.clear();
				inCell = true;
			} else if (inCell
				&& (name == QLatin1String("v")
					|| name == QLatin1String("t"))) {
				value += reader.readElementText();
			}
		} else if (reader.isEndElement()) {
			const auto name = reader.name();
			if (name == QLatin1String("c") && inCell) {
				if (type == u"s"_q) {
					value = shared.value(value.toInt());
				}
				while (row.size() <= column && row.size() < 2000) {
					row.push_back(QString());
				}
				if (column < row.size()) {
					row[column] = value;
				}
				inCell = false;
			} else if (name == QLatin1String("row")) {
				while (!row.isEmpty() && row.back().isEmpty()) {
					row.pop_back();
				}
				result += row.join('\t') + '\n';
				++lines;
				row.clear();
			}
		}
		if (result.size() > kMaxTextSize) {
			break;
		}
	}
	return result;
}

[[nodiscard]] QString XlsxText(const QString &path) {
	const auto all = ListMembers(path);
	const auto shared = SharedStrings(
		ReadMember(path, u"xl/sharedStrings.xml"_q));
	auto names = QStringList();
	{
		auto reader = QXmlStreamReader(
			ReadMember(path, u"xl/workbook.xml"_q));
		while (!reader.atEnd()) {
			reader.readNext();
			if (reader.isStartElement()
				&& reader.name() == QLatin1String("sheet")) {
				names.push_back(reader.attributes().value(
					QLatin1String("name")).toString());
			}
		}
	}
	auto result = QString();
	auto lines = 0;
	auto index = 0;
	const auto sheets = MembersLike(
		all,
		u"xl/worksheets/sheet"_q,
		u".xml"_q);
	for (const auto &sheet : sheets) {
		result += u"\n—— Varaq: %1 ——\n"_q.arg(
			names.value(index, QString::number(index + 1)));
		++index;
		result += SheetText(ReadMember(path, sheet), shared, lines);
		if (lines >= kMaxLines || result.size() > kMaxTextSize) {
			result += u"\n... (davomi ko'rsatilmadi)\n"_q;
			break;
		}
	}
	return result;
}

// OpenDocument: text and tables in content.xml.
[[nodiscard]] QString OdfText(const QString &path) {
	auto result = QString();
	auto reader = QXmlStreamReader(ReadMember(path, u"content.xml"_q));
	auto cells = 0;
	while (!reader.atEnd()) {
		reader.readNext();
		if (reader.isCharacters() && !reader.isWhitespace()) {
			result += reader.text().toString();
		} else if (reader.isStartElement()) {
			const auto name = reader.name();
			if (name == QLatin1String("table-cell")) {
				++cells;
			} else if (name == QLatin1String("tab")) {
				result += '\t';
			} else if (name == QLatin1String("line-break")) {
				result += '\n';
			} else if (name == QLatin1String("s")) {
				result += ' ';
			}
		} else if (reader.isEndElement()) {
			const auto name = reader.name();
			if (name == QLatin1String("p") || name == QLatin1String("h")) {
				result += cells ? ' ' : '\n';
			} else if (name == QLatin1String("table-cell")) {
				--cells;
				result += '\t';
			} else if (name == QLatin1String("table-row")) {
				result += '\n';
			} else if (name == QLatin1String("page")) {
				result += u"\n————\n"_q;
			}
		}
		if (result.size() > kMaxTextSize) {
			break;
		}
	}
	return result;
}

[[nodiscard]] QString PlainText(const QString &path, const QString &ext) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	const auto data = file.read(kMaxTextSize);
	auto state = QTextCodec::ConverterState();
	const auto utf8 = QTextCodec::codecForName("UTF-8");
	auto text = utf8
		? utf8->toUnicode(data.constData(), int(data.size()), &state)
		: QString::fromUtf8(data);
	if (state.invalidChars > 0) {
		if (const auto cyrillic = QTextCodec::codecForName("Windows-1251")) {
			text = cyrillic->toUnicode(data);
		}
	}
	if (ext == u"json"_q) {
		auto error = QJsonParseError();
		const auto document = QJsonDocument::fromJson(data, &error);
		if (error.error == QJsonParseError::NoError) {
			return QString::fromUtf8(document.toJson(QJsonDocument::Indented));
		}
	}
	return text;
}

[[nodiscard]] QString Extract(const QString &path, const QString &ext) {
	if (ext == u"docx"_q) {
		return DocxText(path);
	} else if (ext == u"pptx"_q) {
		return PptxText(path);
	} else if (ext == u"xlsx"_q) {
		return XlsxText(path);
	} else if (IsOffice(ext)) {
		return OdfText(path);
	}
	return PlainText(path, ext);
}

void ShowWindow(const QString &title, const QString &text) {
	const auto window = new QWidget();
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->setWindowTitle(title + u" — ParadoksGram"_q);
	window->resize(900, 700);
	window->setStyleSheet(
		u"QWidget { background: #17212b; color: #e8eef5; }"
		"QPlainTextEdit { background: #0e1621; border: 1px solid #2b3a4a;"
		" selection-background-color: #2b5278; }"
		"QLineEdit { background: #0e1621; border: 1px solid #2b3a4a;"
		" padding: 4px; }"
		"QPushButton { background: #2b5278; border: none; padding: 5px 10px;"
		" border-radius: 4px; }"
		"QPushButton:hover { background: #3a6a98; }"_q);
	const auto layout = new QVBoxLayout(window);
	const auto bar = new QHBoxLayout();
	const auto search = new QLineEdit(window);
	search->setPlaceholderText(u"Qidirish..."_q);
	const auto previous = new QPushButton(u"▲"_q, window);
	const auto next = new QPushButton(u"▼"_q, window);
	const auto smaller = new QPushButton(u"A−"_q, window);
	const auto bigger = new QPushButton(u"A+"_q, window);
	const auto copy = new QPushButton(u"Nusxalash"_q, window);
	const auto words = text.split(
		QRegularExpression(u"\\s+"_q),
		Qt::SkipEmptyParts).size();
	const auto info = new QLabel(
		u"%1 so'z  •  ~%2 daqiqa"_q.arg(words).arg(
			std::max(1, int(words / 200))),
		window);
	bar->addWidget(search, 1);
	bar->addWidget(previous);
	bar->addWidget(next);
	bar->addWidget(smaller);
	bar->addWidget(bigger);
	bar->addWidget(copy);
	layout->addLayout(bar);
	const auto edit = new QPlainTextEdit(window);
	edit->setReadOnly(true);
	edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	edit->setPlainText(text);
	layout->addWidget(edit, 1);
	layout->addWidget(info);

	const auto find = [=](bool backward) {
		const auto query = search->text();
		if (query.isEmpty()) {
			return;
		}
		auto flags = QTextDocument::FindFlags();
		if (backward) {
			flags |= QTextDocument::FindBackward;
		}
		if (!edit->find(query, flags)) {
			auto cursor = edit->textCursor();
			cursor.movePosition(backward
				? QTextCursor::End
				: QTextCursor::Start);
			edit->setTextCursor(cursor);
			edit->find(query, flags);
		}
	};
	QObject::connect(search, &QLineEdit::returnPressed, window, [=] {
		find(false);
	});
	QObject::connect(next, &QPushButton::clicked, window, [=] {
		find(false);
	});
	QObject::connect(previous, &QPushButton::clicked, window, [=] {
		find(true);
	});
	QObject::connect(smaller, &QPushButton::clicked, window, [=] {
		edit->zoomOut(1);
	});
	QObject::connect(bigger, &QPushButton::clicked, window, [=] {
		edit->zoomIn(1);
	});
	QObject::connect(copy, &QPushButton::clicked, window, [=] {
		QGuiApplication::clipboard()->setText(edit->toPlainText());
	});
	window->show();
	window->raise();
	window->activateWindow();
}

} // namespace

void AddViewerAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document) {
		return;
	}
	const auto ext = ExtOf(document->filename());
	if (!IsOffice(ext) && !IsPlain(ext)) {
		return;
	}
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	menu->addAction(u"ParadoksGram ko'rgichida ochish"_q, [=] {
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
		if (QFileInfo(path).size() > kMaxFileSize) {
			show->showToast(u"Fayl juda katta (40 MB dan oshmasin)"_q);
			return;
		}
		const auto name = current->filename();
		const auto text = Extract(path, ExtOf(name));
		if (text.trimmed().isEmpty()) {
			show->showToast(u"Faylni o'qib bo'lmadi yoki bo'sh"_q);
			return;
		}
		ShowWindow(name, text);
	}, &st::menuIconShowAll);
}

} // namespace Paradoks::Viewer
