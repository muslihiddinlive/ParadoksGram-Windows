/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_webdev.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_webcopy.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "core/application.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "base/options.h"
#include "webview/webview_embed.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QTextCursor>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <deque>
#include <memory>

namespace Paradoks::WebDev {
namespace {

constexpr auto kMaxEntries = 600;
constexpr auto kMaxPayload = 3000;

struct Entry {
	QDateTime time;
	bool incoming = false;
	QString name;
	QString payload;
};

std::deque<Entry> Log;
int LogVersion = 0;
QString CurrentUrl;
QPointer<QWidget> Panel;

void Push(bool incoming, const QString &name, QString payload) {
	if (payload.size() > kMaxPayload) {
		payload = payload.left(kMaxPayload) + u"…"_q;
	}
	Log.push_back({
		.time = QDateTime::currentDateTime(),
		.incoming = incoming,
		.name = name,
		.payload = std::move(payload),
	});
	while (Log.size() > kMaxEntries) {
		Log.pop_front();
	}
	++LogVersion;
}

[[nodiscard]] QString PrettyJson(const QString &text) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(text.toUtf8(), &error);
	if (error.error != QJsonParseError::NoError) {
		return text;
	}
	return QString::fromUtf8(document.toJson(QJsonDocument::Indented))
		.trimmed();
}

[[nodiscard]] QString LogText(const QString &filter) {
	auto result = QString();
	for (const auto &entry : Log) {
		auto line = u"%1  %2 %3"_q.arg(
			entry.time.toString(u"HH:mm:ss.zzz"_q),
			entry.incoming ? u"← app"_q : u"→ app"_q,
			entry.name);
		if (!entry.payload.isEmpty() && entry.payload != u"{}"_q) {
			line += u"  "_q + entry.payload;
		}
		if (filter.isEmpty()
			|| line.contains(filter, Qt::CaseInsensitive)) {
			result += line + '\n';
		}
	}
	return result.isEmpty()
		? u"Hali hech narsa yo'q. Mini-appni oching, hodisalar shu yerda "
			"paydo bo'ladi.\n← app: mini-app Telegram'ga yuborgan; "
			"→ app: Telegram mini-appga yuborgan."_q
		: result;
}

[[nodiscard]] QString InitDataText() {
	if (CurrentUrl.isEmpty()) {
		return u"Mini-app hali ochilmagan."_q;
	}
	const auto url = QUrl(CurrentUrl);
	const auto fragment = QUrlQuery(url.fragment());
	const auto raw = fragment.queryItemValue(
		u"tgWebAppData"_q,
		QUrl::FullyDecoded);
	auto result = u"Manzil: %1\n\n"_q.arg(
		url.adjusted(QUrl::RemoveFragment).toString());
	if (raw.isEmpty()) {
		return result + u"initData topilmadi. Bu mini-app manzili "
			"#tgWebAppData= qismisiz ochilgan bo'lishi mumkin."_q;
	}
	result += u"initData (raw):\n%1\n\n"_q.arg(raw);
	const auto query = QUrlQuery(raw);
	for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
		const auto &key = item.first;
		const auto &value = item.second;
		const auto pretty = (key == u"user"_q
			|| key == u"receiver"_q
			|| key == u"chat"_q)
			? PrettyJson(value)
			: value;
		result += u"%1 = %2\n"_q.arg(key, pretty);
	}
	const auto version = fragment.queryItemValue(
		u"tgWebAppVersion"_q,
		QUrl::FullyDecoded);
	if (!version.isEmpty()) {
		result += u"\ntgWebAppVersion = %1\n"_q.arg(version);
	}
	const auto theme = fragment.queryItemValue(
		u"tgWebAppThemeParams"_q,
		QUrl::FullyDecoded);
	if (!theme.isEmpty()) {
		result += u"\ntgWebAppThemeParams =\n%1\n"_q.arg(PrettyJson(theme));
	}
	return result;
}

void BuildWindow() {
	const auto window = new QWidget();
	Panel = window;
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->setWindowTitle(u"Mini-app dasturchi paneli — ParadoksGram"_q);
	window->resize(900, 640);
	window->setStyleSheet(
		u"QWidget { background: #17212b; color: #e8eef5; }"
		"QPlainTextEdit { background: #0e1621; border: 1px solid #2b3a4a;"
		" font-family: Consolas, monospace; selection-background-color:"
		" #2b5278; }"
		"QLineEdit { background: #0e1621; border: 1px solid #2b3a4a;"
		" padding: 4px; }"
		"QPushButton { background: #2b5278; border: none; padding: 5px 10px;"
		" border-radius: 4px; }"
		"QPushButton:hover { background: #3a6a98; }"
		"QTabBar::tab { background: #17212b; padding: 7px 14px; }"
		"QTabBar::tab:selected { background: #2b5278; }"_q);
	const auto layout = new QVBoxLayout(window);
	const auto tabs = new QTabWidget(window);
	layout->addWidget(tabs, 1);

	// Bridge log.
	const auto bridge = new QWidget(tabs);
	const auto bridgeLayout = new QVBoxLayout(bridge);
	const auto bar = new QHBoxLayout();
	const auto filter = new QLineEdit(bridge);
	filter->setPlaceholderText(u"Filtr (masalan: popup, MainButton)"_q);
	const auto clear = new QPushButton(u"Tozalash"_q, bridge);
	const auto copy = new QPushButton(u"Nusxalash"_q, bridge);
	bar->addWidget(filter, 1);
	bar->addWidget(clear);
	bar->addWidget(copy);
	bridgeLayout->addLayout(bar);
	const auto bridgeText = new QPlainTextEdit(bridge);
	bridgeText->setReadOnly(true);
	bridgeText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	bridgeLayout->addWidget(bridgeText, 1);
	tabs->addTab(bridge, u"Bridge jurnali"_q);

	// initData.
	const auto init = new QWidget(tabs);
	const auto initLayout = new QVBoxLayout(init);
	const auto initText = new QPlainTextEdit(init);
	initText->setReadOnly(true);
	initText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	const auto initCopy = new QPushButton(u"Nusxalash"_q, init);
	initLayout->addWidget(initText, 1);
	initLayout->addWidget(initCopy);
	tabs->addTab(init, u"initData"_q);

	// DevTools.
	const auto tools = new QWidget(tabs);
	const auto toolsLayout = new QVBoxLayout(tools);
	const auto devtools = new QCheckBox(
		u"WebView DevTools (konsol, tarmoq, elementlar)"_q,
		tools);
	devtools->setChecked(Enabled(Flag::WebDevTools));
	const auto note = new QLabel(
		u"Yoqilgach, YANGI ochilgan mini-appda o'ng tugma bosib "
		"\"Inspect\" (Tekshirish) ni tanlang yoki F12 bosing: Chrome "
		"DevTools ochiladi, undagi Console va Network bo'limlarida "
		"mini-appning konsoli va tarmoq so'rovlari ko'rinadi.\n\n"
		"Mini-appni yopib qayta oching, shunda sozlama qo'llanadi."_q,
		tools);
	note->setWordWrap(true);
	toolsLayout->addWidget(devtools);
	toolsLayout->addWidget(note);
	toolsLayout->addStretch(1);
	tabs->addTab(tools, u"DevTools"_q);

	// Copy of the page.
	const auto copyTab = new QWidget(tabs);
	const auto copyLayout = new QVBoxLayout(copyTab);
	const auto copyNote = new QLabel(
		u"Ochiq mini-appning ochiq qismini (HTML, CSS, skriptlar, rasmlar) "
		"ZIP qilib saqlaydi va Saved Messages ga yuboradi (limit 1.5 GB), "
		"havolalar offlayn ochiladigan qilib o'zgartiriladi. "
		"Hech qanday shaxsiy ma'lumot (initData, cookie) yuborilmaydi. "
		"Serverdagi (backend) kod saqlanmaydi. Faqat o'z mini-appingiz yoki "
		"o'rganish uchun foydalaning: boshqaning ishini o'zinikidek "
		"chiqarish mualliflik huquqini buzadi."_q,
		copyTab);
	copyNote->setWordWrap(true);
	const auto copyStart = new QPushButton(
		u"Sahifani saqlash va Saved Messages ga yuborish"_q,
		copyTab);
	const auto htmlStart = new QPushButton(
		u"Bitta HTML qilib saqlash (hammasi ichida) va yuborish"_q,
		copyTab);
	const auto copyOpen = new QPushButton(u"Fayl joylashgan papkani ochish"_q, copyTab);
	copyOpen->setEnabled(false);
	const auto copyLog = new QPlainTextEdit(copyTab);
	copyLog->setReadOnly(true);
	copyLayout->addWidget(copyNote);
	copyLayout->addWidget(copyStart);
	copyLayout->addWidget(htmlStart);
	copyLayout->addWidget(copyLog, 1);
	copyLayout->addWidget(copyOpen);
	tabs->addTab(copyTab, u"Nusxa"_q);
	const auto copyFolder = std::make_shared<QString>();

	const auto refresh = [=] {
		bridgeText->setPlainText(LogText(filter->text()));
		bridgeText->moveCursor(QTextCursor::End);
		initText->setPlainText(InitDataText());
	};
	const auto version = std::make_shared<int>(-1);
	const auto timer = new QTimer(window);
	QObject::connect(timer, &QTimer::timeout, window, [=] {
		if (*version != LogVersion) {
			*version = LogVersion;
			refresh();
		}
	});
	timer->start(700);
	refresh();
	QObject::connect(filter, &QLineEdit::textChanged, window, [=] {
		bridgeText->setPlainText(LogText(filter->text()));
		bridgeText->moveCursor(QTextCursor::End);
	});
	QObject::connect(clear, &QPushButton::clicked, window, [=] {
		Log.clear();
		++LogVersion;
	});
	QObject::connect(copy, &QPushButton::clicked, window, [=] {
		QGuiApplication::clipboard()->setText(bridgeText->toPlainText());
	});
	QObject::connect(initCopy, &QPushButton::clicked, window, [=] {
		QGuiApplication::clipboard()->setText(initText->toPlainText());
	});
	QObject::connect(devtools, &QCheckBox::toggled, window, [=](bool on) {
		SetDevTools(on);
	});
	const auto startCopy = [=](bool single) {
		if (CurrentUrl.isEmpty()) {
			copyLog->setPlainText(u"Mini-app ochilmagan."_q);
			return;
		}
		copyStart->setEnabled(false);
		htmlStart->setEnabled(false);
		copyOpen->setEnabled(false);
		copyLog->clear();
		const auto guard = QPointer<QWidget>(window);
		WebCopy::Start(
			CurrentUrl,
			single,
			[=](const QString &line) {
				if (guard) {
					copyLog->appendPlainText(line);
				}
			},
			[=](const QString &folder) {
				if (!guard) {
					return;
				}
				copyStart->setEnabled(true);
				htmlStart->setEnabled(true);
				*copyFolder = folder;
				copyOpen->setEnabled(!folder.isEmpty());
				if (folder.isEmpty()) {
					return;
				}
				copyLog->appendPlainText(u"Fayl: "_q
					+ QDir::toNativeSeparators(folder));
				const auto account
					= Core::App().domain().maybeLastOrSomeAuthedAccount();
				const auto session = account ? account->maybeSession() : nullptr;
				if (!session) {
					copyLog->appendPlainText(
						u"Saved Messages ga yuborib bo'lmadi (akkaunt yo'q)."_q);
					return;
				}
				auto files = Storage::PrepareMediaList(
					QStringList(folder),
					256,
					session->premium());
				if (files.error != Ui::PreparedList::Error::None
					|| files.files.empty()) {
					copyLog->appendPlainText(
						u"Faylni yuborib bo'lmadi (hajm katta bo'lishi mumkin)."_q);
					return;
				}
				files.files.front().caption = TextWithTags{
					u"Mini-app nusxasi: "_q + QFileInfo(folder).fileName(),
				};
				session->api().sendFiles(
					std::move(files),
					SendMediaType::File,
					nullptr,
					Api::SendAction(session->data().history(session->user())));
				copyLog->appendPlainText(
					u"Saved Messages ga yuborilmoqda."_q);
			});
	};
	QObject::connect(copyStart, &QPushButton::clicked, window, [=] {
		startCopy(false);
	});
	QObject::connect(htmlStart, &QPushButton::clicked, window, [=] {
		startCopy(true);
	});
	QObject::connect(copyOpen, &QPushButton::clicked, window, [=] {
		if (!copyFolder->isEmpty()) {
			QDesktopServices::openUrl(QUrl::fromLocalFile(
				QFileInfo(*copyFolder).absolutePath()));
		}
	});
	window->show();
}

} // namespace

void LogIncoming(const QString &command, const QJsonObject &arguments) {
	Push(
		true,
		command,
		QString::fromUtf8(QJsonDocument(arguments).toJson(
			QJsonDocument::Compact)));
}

void LogOutgoing(const QString &event, const QString &payload) {
	Push(false, event, payload);
}

void SetUrl(const QString &url) {
	CurrentUrl = url;
	++LogVersion;
}

void Open() {
	if (Panel) {
		Panel->show();
		Panel->raise();
		Panel->activateWindow();
		return;
	}
	BuildWindow();
}

void ApplyDevTools() {
	if (Enabled(Flag::WebDevTools)) {
		base::options::lookup<bool>(
			Webview::kOptionWebviewDebugEnabled).set(true);
	}
}

void SetDevTools(bool enabled) {
	SetEnabled(Flag::WebDevTools, enabled);
	base::options::lookup<bool>(
		Webview::kOptionWebviewDebugEnabled).set(enabled);
}

} // namespace Paradoks::WebDev
