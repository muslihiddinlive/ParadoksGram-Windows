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
#include "window/window_controller.h"
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
#include <QtCore/QRandomGenerator>
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
#include <QtWidgets/QMessageBox>
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

bool CloneBusy = false;

enum class CloneData {
	None, // Page only, no initData (as before).
	Real, // The initData this mini-app was really opened with.
	Fake, // Same shape, but id/username/first_name are replaced, and
	      // hash/signature/query_id/auth_date no longer validate.
};

// The literal value of tgWebAppData=... in CurrentUrl's fragment, taken
// as-is (still percent-encoded exactly like Telegram produced it), so
// re-embedding it reproduces a fully working, still-valid initData.
[[nodiscard]] QString RawInitDataFragmentValue() {
	const auto hash = CurrentUrl.indexOf('#');
	if (hash < 0) {
		return QString();
	}
	const auto fragment = CurrentUrl.mid(hash + 1);
	for (const auto &pair : fragment.split('&', Qt::SkipEmptyParts)) {
		const auto eq = pair.indexOf('=');
		if (eq > 0 && pair.left(eq) == u"tgWebAppData"_q) {
			return pair.mid(eq + 1);
		}
	}
	return QString();
}

// tgWebAppData, decoded exactly one level (same as the "raw" value shown
// in the initData tab): a plain "key=value&key=value..." string where the
// user/receiver values are themselves still percent-encoded JSON.
[[nodiscard]] QString DecodedInitDataQuery() {
	if (CurrentUrl.isEmpty()) {
		return QString();
	}
	const auto url = QUrl(CurrentUrl);
	const auto fragment = QUrlQuery(url.fragment());
	return fragment.queryItemValue(u"tgWebAppData"_q, QUrl::FullyDecoded);
}

[[nodiscard]] qint64 FakeUserId() {
	return 100000000LL
		+ qint64(QRandomGenerator::global()->bounded(1900000000));
}

[[nodiscard]] QString RandomToken(int length) {
	static const auto chars = u"abcdefghijklmnopqrstuvwxyz0123456789"_q;
	auto result = QString();
	result.reserve(length);
	for (auto i = 0; i != length; ++i) {
		result += chars[QRandomGenerator::global()->bounded(chars.size())];
	}
	return result;
}

// Keeps the same JSON shape (so the mini-app's own code does not break on
// a missing field), but the id, username and name no longer point to the
// real account.
[[nodiscard]] QString FakeIdentityJson(const QString &json) {
	const auto doc = QJsonDocument::fromJson(json.toUtf8());
	if (!doc.isObject()) {
		return json;
	}
	auto object = doc.object();
	if (object.contains(u"id"_q)) {
		object[u"id"_q] = FakeUserId();
	}
	if (object.contains(u"username"_q)) {
		object[u"username"_q] = u"user_"_q + RandomToken(7);
	}
	if (object.contains(u"first_name"_q)) {
		object[u"first_name"_q] = u"Foydalanuvchi"_q;
	}
	if (object.contains(u"last_name"_q)) {
		object.remove(u"last_name"_q);
	}
	if (object.contains(u"photo_url"_q)) {
		object.remove(u"photo_url"_q);
	}
	return QString::fromUtf8(
		QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// A value with the same shape as RawInitDataFragmentValue(), but with the
// identifying fields replaced. The hash/signature no longer validate
// against the bot's secret, so a backend that checks them will reject it:
// this is for trying out the mini-app's own screens under a different
// identity, not for impersonating anyone with the real app's server.
[[nodiscard]] QString FakeInitDataFragmentValue() {
	const auto raw = DecodedInitDataQuery();
	if (raw.isEmpty()) {
		return QString();
	}
	const auto query = QUrlQuery(raw);
	auto rebuilt = QUrlQuery();
	for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
		const auto &key = item.first;
		const auto &value = item.second;
		if (key == u"user"_q || key == u"receiver"_q) {
			rebuilt.addQueryItem(key, FakeIdentityJson(value));
		} else if (key == u"hash"_q || key == u"signature"_q) {
			rebuilt.addQueryItem(key, RandomToken(64));
		} else if (key == u"query_id"_q) {
			rebuilt.addQueryItem(key, u"AAFAKE"_q + RandomToken(16));
		} else if (key == u"chat_instance"_q) {
			rebuilt.addQueryItem(key, RandomToken(16));
		} else if (key == u"auth_date"_q) {
			rebuilt.addQueryItem(
				key,
				QString::number(QDateTime::currentSecsSinceEpoch()));
		} else {
			rebuilt.addQueryItem(key, value);
		}
	}
	return QString::fromUtf8(
		QUrl::toPercentEncoding(rebuilt.toString(QUrl::FullyEncoded)));
}

void SendCloneToSavedMessages(
		const QString &path,
		const Fn<void(const QString &)> &toast) {
	const auto account
		= Core::App().domain().maybeLastOrSomeAuthedAccount();
	const auto session = account ? account->maybeSession() : nullptr;
	if (!session) {
		toast(u"Saved Messages ga yuborib bo'lmadi (akkaunt yo'q)."_q);
		return;
	}
	auto files = Storage::PrepareMediaList(
		QStringList(path),
		256,
		session->premium());
	if (files.error != Ui::PreparedList::Error::None
		|| files.files.empty()) {
		toast(u"Faylni yuborib bo'lmadi (hajm katta bo'lishi mumkin)."_q);
		return;
	}
	files.files.front().caption = TextWithTags{
		u"Mini-app nusxasi: "_q + QFileInfo(path).fileName(),
	};
	session->api().sendFiles(
		std::move(files),
		SendMediaType::File,
		nullptr,
		Api::SendAction(session->data().history(session->user())));
	toast(u"Saved Messages ga yuborilmoqda."_q);
}

void StartClone(bool single, CloneData mode) {
	const auto toast = [](const QString &text) {
		if (const auto window = Core::App().activePrimaryWindow()) {
			window->showToast(text);
		}
	};
	if (CloneBusy) {
		toast(u"Nusxalash davom etyapti..."_q);
		return;
	}
	auto initData = QString();
	if (mode == CloneData::Real) {
		initData = RawInitDataFragmentValue();
		if (initData.isEmpty()) {
			toast(u"initData topilmadi, bu mini-app shunsiz ochilgan."_q);
			return;
		}
	} else if (mode == CloneData::Fake) {
		initData = FakeInitDataFragmentValue();
		if (initData.isEmpty()) {
			toast(u"initData topilmadi, bu mini-app shunsiz ochilgan."_q);
			return;
		}
	}
	CloneBusy = true;
	toast(u"Mini-app nusxalanmoqda..."_q);
	WebCopy::Start(
		CurrentUrl,
		single,
		initData,
		[](const QString &) {},
		[=](const QString &path) {
			CloneBusy = false;
			if (path.isEmpty()) {
				toast(u"Mini-appni nusxalab bo'lmadi."_q);
				return;
			}
			SendCloneToSavedMessages(path, toast);
		});
}

// Second step of the clone flow: after the ZIP/single-HTML choice, ask
// whether to bake initData into the copy, and which kind.
void AskCloneDataMode(QWidget *parent, Fn<void(CloneData)> chosen) {
	const auto box = new QMessageBox(
		QMessageBox::Question,
		u"Mini-appni klonlash"_q,
		u"initData bilanmi?"_q,
		QMessageBox::NoButton,
		parent);
	box->setAttribute(Qt::WA_DeleteOnClose);
	const auto none = box->addButton(
		u"Yo'q (faqat sahifa)"_q,
		QMessageBox::AcceptRole);
	const auto real = box->addButton(
		u"Mening initData'm bilan"_q,
		QMessageBox::AcceptRole);
	const auto fake = box->addButton(
		u"Soxta initData bilan (ID, username almashtiriladi)"_q,
		QMessageBox::AcceptRole);
	box->addButton(u"Bekor qilish"_q, QMessageBox::RejectRole);
	QObject::connect(box, &QMessageBox::buttonClicked, box, [=](
			QAbstractButton *button) {
		if (button == none) {
			chosen(CloneData::None);
		} else if (button == real) {
			chosen(CloneData::Real);
		} else if (button == fake) {
			chosen(CloneData::Fake);
		}
	});
	box->show();
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
		"Hech qanday shaxsiy ma'lumot tarmoqqa yuborilmaydi. Xohlasangiz, "
		"keyingi oynada o'zingizning yoki soxtalashtirilgan (ID, username "
		"almashtirilgan) initData ham nusxaning ichiga qo'shiladi — bu "
		"hech qayerga yuborilmaydi, faqat saqlangan nusxa ochilganda "
		"ishlatiladi. Serverdagi (backend) kod saqlanmaydi. Faqat o'z "
		"mini-appingiz yoki "
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
	const auto startCopy = [=](bool single, CloneData mode) {
		if (CurrentUrl.isEmpty()) {
			copyLog->setPlainText(u"Mini-app ochilmagan."_q);
			return;
		}
		auto initData = QString();
		if (mode == CloneData::Real) {
			initData = RawInitDataFragmentValue();
		} else if (mode == CloneData::Fake) {
			initData = FakeInitDataFragmentValue();
		}
		if (mode != CloneData::None && initData.isEmpty()) {
			copyLog->setPlainText(
				u"initData topilmadi, bu mini-app shunsiz ochilgan."_q);
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
			initData,
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
		AskCloneDataMode(window, [=](CloneData mode) {
			startCopy(false, mode);
		});
	});
	QObject::connect(htmlStart, &QPushButton::clicked, window, [=] {
		AskCloneDataMode(window, [=](CloneData mode) {
			startCopy(true, mode);
		});
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

void CloneCurrent() {
	if (CurrentUrl.isEmpty()) {
		if (const auto window = Core::App().activePrimaryWindow()) {
			window->showToast(u"Mini-app ochilmagan."_q);
		}
		return;
	}
	const auto box = new QMessageBox(
		QMessageBox::Question,
		u"Mini-appni klonlash"_q,
		u"Qaysi ko'rinishda saqlaymiz?"_q,
		QMessageBox::NoButton);
	box->setAttribute(Qt::WA_DeleteOnClose);
	const auto zip = box->addButton(
		u"ZIP (papka tuzilmasi bilan)"_q,
		QMessageBox::AcceptRole);
	const auto html = box->addButton(
		u"Bitta HTML (hammasi ichida)"_q,
		QMessageBox::AcceptRole);
	box->addButton(u"Bekor qilish"_q, QMessageBox::RejectRole);
	QObject::connect(box, &QMessageBox::buttonClicked, box, [=](
			QAbstractButton *button) {
		if (button == zip) {
			AskCloneDataMode(nullptr, [](CloneData mode) {
				StartClone(false, mode);
			});
		} else if (button == html) {
			AskCloneDataMode(nullptr, [](CloneData mode) {
				StartClone(true, mode);
			});
		}
	});
	box->show();
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
