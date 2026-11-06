/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_report.h"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "core/version.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"
#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "paradoks/paradoks_update.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeDatabase>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QSysInfo>
#include <QtCore/QTimeZone>
#include <QtNetwork/QHttpMultiPart>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <memory>

namespace Paradoks::Report {
namespace {

constexpr auto kEndpoint = "https://buildnoticer.soglomhayotila.workers.dev/report";
constexpr auto kStuckFor = crl::time(45'000);
constexpr auto kCooldown = crl::time(10 * 60'000);
constexpr auto kPoll = crl::time(5'000);
constexpr auto kMaxSummary = 2500;
constexpr auto kMaxComment = 3000;
constexpr auto kMaxFiles = 5;
constexpr auto kMaxFileSize = 8 * 1024 * 1024;

[[nodiscard]] QNetworkAccessManager *Network() {
	// Intentionally never destroyed: it must outlive QApplication teardown.
	static const auto result = new QNetworkAccessManager();
	return result;
}

// Removes bot tokens and long digit strings (phone numbers, ids).
[[nodiscard]] QString Scrub(QString text) {
	text.replace(
		QRegularExpression(u"\\d{6,}:[A-Za-z0-9_-]{30,}"_q),
		u"<token>"_q);
	text.replace(
		QRegularExpression(u"\\+?\\d{9,15}"_q),
		u"<son>"_q);
	return text;
}

[[nodiscard]] QString Where(const QString &details) {
	auto head = QString();
	auto frame = QString();
	for (const auto &raw : details.split(QChar('\n'))) {
		const auto line = raw.trimmed();
		if (head.isEmpty()
			&& (line.contains(u"Exception"_q)
				|| line.contains(u"Error"_q)
				|| line.contains(u"Access violation"_q))) {
			head = line;
		}
		if (frame.isEmpty() && line.contains(u".exe+0x"_q)) {
			frame = line;
		}
		if (!head.isEmpty() && !frame.isEmpty()) {
			break;
		}
	}
	if (head.isEmpty() && frame.isEmpty()) {
		return u"ulanish holati (asosiy oyna)"_q;
	}
	return head + (frame.isEmpty() ? QString() : u"  ->  "_q + frame);
}

[[nodiscard]] QString Causes(
		const QString &kind,
		const QString &details,
		bool proxy) {
	auto list = QStringList();
	if (proxy) {
		list << u"Proxy yoqilgan: eskirgan yoki ishlamaydigan proxy ulanishni buzishi mumkin"_q;
	}
	if (kind.contains(u"ulanish"_q)) {
		list << u"Provayder Telegram IP manzillarini sekinlatgan yoki bloklagan bo'lishi mumkin (boshqa tarmoq bilan sinab ko'ring)"_q;
		list << u"VPN yoqilgan bo'lsa, u yo'lni buzishi mumkin"_q;
		list << u"Telegram serverlarida vaqtincha nosozlik bo'lishi mumkin"_q;
		list << u"Juda ko'p parallel yuklash: server vaqtincha cheklagan bo'lishi mumkin"_q;
	}
	if (details.contains(u"0xC0000005"_q)) {
		list << u"Dasturdagi xato: noto'g'ri xotiraga murojaat (kod xatosi)"_q;
	}
	if (details.contains(u"0xE06D7363"_q)) {
		list << u"Ushlanmagan C++ istisno (kod xatosi)"_q;
	}
	if (details.contains(u"0xC00000FD"_q)) {
		list << u"Stek to'ldi (cheksiz rekursiya)"_q;
	}
	if (details.contains(u"0xC0000017"_q)
		|| details.contains(u"0xC0000409"_q)) {
		list << u"Xotira yetishmadi yoki xotira buzildi"_q;
	}
	if (list.isEmpty()) {
		list << u"Aniq sabab topilmadi, logni ko'ring"_q;
	}
	return u"- "_q + list.join(u"\n- "_q);
}

[[nodiscard]] QString Summary(
		const QString &kind,
		const QString &details) {
	const auto proxy = Core::App().settings().proxy().isEnabled();
	auto result = QString();
	result += u"Tur: "_q + kind + u"\n"_q;
	result += u"Qurilma: "_q + QSysInfo::prettyProductName()
		+ u" ("_q + QSysInfo::currentCpuArchitecture() + u")\n"_q;
	result += u"Ilova: ParadoksGram Windows %1 (V%2, tdesktop %3)\n"_q.arg(
		Update::CurrentVersion()).arg(Update::CurrentBuild()).arg(QString::fromLatin1(AppVersionStr));
	result += u"Vaqt: "_q
		+ QDateTime::currentDateTime().toString(u"yyyy-MM-dd HH:mm:ss t"_q)
		+ u" ["_q + QString::fromUtf8(QTimeZone::systemTimeZoneId()) + u"]\n"_q;
	result += u"Qayerda: "_q + Where(details) + u"\n"_q;
	result += u"Hudud (taxminiy): til="_q + QLocale::system().name()
		+ u", mintaqa="_q + QString::fromUtf8(QTimeZone::systemTimeZoneId())
		+ u"\n"_q;
	result += u"Tarmoq: "_q + (proxy ? u"proxy yoqilgan"_q : u"proxy yo'q"_q)
		+ u"\n"_q;
	result += u"\nTaxminiy sabablar:\n"_q + Causes(kind, details, proxy);
	return result;
}

[[nodiscard]] QString AccountText(Main::Session *session) {
	if (!session) {
		return QString();
	}
	const auto user = session->user();
	const auto username = user->username();
	const auto phone = user->phone();
	return u"ID: "_q + QString::number(session->userId().bare)
		+ u"\nIsm: "_q + user->name()
		+ u"\nUsername: "_q
		+ (username.isEmpty() ? u"-"_q : u"@"_q + username)
		+ u"\nTelefon: "_q
		+ (phone.isEmpty() ? u"-"_q : u"+"_q + phone);
}

void AddText(
		not_null<QHttpMultiPart*> multi,
		const QString &name,
		const QString &value) {
	auto part = QHttpPart();
	part.setHeader(
		QNetworkRequest::ContentDispositionHeader,
		u"form-data; name=\"%1\""_q.arg(name));
	part.setBody(value.toUtf8());
	multi->append(part);
}

} // namespace

void Send(
		Main::Session *session,
		const QString &kind,
		const QString &details,
		Fn<void(bool)> done) {
	SendWithUser(session, kind, details, QString(), {}, std::move(done));
}

void SendWithUser(
		Main::Session *session,
		const QString &kind,
		const QString &details,
		const QString &comment,
		const QStringList &files,
		Fn<void(bool)> done) {
	auto summary = Summary(kind, details).left(kMaxSummary);
	if (!comment.trimmed().isEmpty()) {
		summary += u"\n\nFoydalanuvchi izohi:\n"_q
			+ comment.trimmed().left(kMaxComment);
	}
	const auto log = Scrub(
		details + u"\n\n===== PARADOKS DEBUG LOG =====\n"_q + Debug::Tail());
	const auto account = AccountText(session);

	const auto multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
	AddText(multi, u"name"_q, u"ParadoksGram Windows"_q);
	AddText(
		multi,
		u"contact"_q,
		QDateTime::currentDateTime().toString(u"yyyy-MM-dd HH:mm:ss"_q));
	AddText(multi, u"message"_q, summary);
	if (!account.isEmpty()) {
		AddText(multi, u"user"_q, account);
	}
	auto file = QHttpPart();
	file.setHeader(
		QNetworkRequest::ContentDispositionHeader,
		u"form-data; name=\"files\"; filename=\"paradoks-log.txt\""_q);
	file.setHeader(QNetworkRequest::ContentTypeHeader, u"text/plain"_q);
	file.setBody(log.toUtf8());
	multi->append(file);
	auto count = 0;
	for (const auto &path : files) {
		if (++count > kMaxFiles) {
			break;
		}
		auto source = QFile(path);
		if (QFileInfo(path).size() > kMaxFileSize
			|| !source.open(QIODevice::ReadOnly)) {
			continue;
		}
		auto part = QHttpPart();
		part.setHeader(
			QNetworkRequest::ContentDispositionHeader,
			u"form-data; name=\"files\"; filename=\"%1\""_q.arg(
				QFileInfo(path).fileName().replace('"', '_')));
		part.setHeader(
			QNetworkRequest::ContentTypeHeader,
			QMimeDatabase().mimeTypeForFile(path).name());
		part.setBody(source.readAll());
		multi->append(part);
	}

	auto request = QNetworkRequest(QUrl(QString::fromLatin1(kEndpoint)));
	request.setRawHeader("User-Agent", "ParadoksGram-Windows");
	request.setTransferTimeout(30'000);
	const auto reply = Network()->post(request, multi);
	multi->setParent(reply);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto ok = (reply->error() == QNetworkReply::NoError)
			&& status >= 200
			&& status < 300;
		Debug::Log(u"report sent: ok=%1 status=%2"_q.arg(ok).arg(status));
		reply->deleteLater();
		if (done) {
			done(ok);
		}
	});
}

void ComposeBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Hato haqida xabar"_q));
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(u"Nima bo'lganini o'z so'zlaringiz bilan yozing va "
				"kerak bo'lsa rasm, skrinshot yoki fayl biriktiring. "
				"Qurilma ma'lumoti va jurnal avtomatik qo'shiladi."_q),
			st::boxLabel),
		st::boxRowPadding);
	const auto input = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Nima bo'ldi? (izoh)"_q)),
		st::boxRowPadding);
	input->setMaxHeight(160);

	struct State {
		QStringList files;
		rpl::variable<QString> info = u"Biriktirilgan fayl yo'q"_q;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto refresh = [=] {
		auto names = QStringList();
		for (const auto &path : state->files) {
			names.push_back(QFileInfo(path).fileName());
		}
		state->info = names.isEmpty()
			? u"Biriktirilgan fayl yo'q"_q
			: u"Biriktirilgan (%1): %2"_q.arg(
				names.size()).arg(names.join(u", "_q));
	};
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->info.value(),
			st::boxLabel),
		st::boxRowPadding);
	const auto attach = box->addRow(
		object_ptr<Ui::LinkButton>(
			box,
			u"Rasm yoki fayl biriktirish"_q),
		st::boxRowPadding);
	attach->setClickedCallback([=] {
		if (state->files.size() >= kMaxFiles) {
			controller->showToast(u"Eng ko'pi bilan %1 ta fayl"_q.arg(
				kMaxFiles));
			return;
		}
		FileDialog::GetOpenPaths(
			QPointer<QWidget>(box.get()),
			u"Faylni tanlang"_q,
			FileDialog::AllFilesFilter(),
			crl::guard(box, [=](const FileDialog::OpenResult &result) {
				for (const auto &path : result.paths) {
					if (QFileInfo(path).size() > kMaxFileSize) {
						controller->showToast(
							u"Fayl 8 MB dan katta: "_q
							+ QFileInfo(path).fileName());
					} else if (!state->files.contains(path)
						&& state->files.size() < kMaxFiles) {
						state->files.push_back(path);
					}
				}
				refresh();
			}));
	});
	const auto clear = box->addRow(
		object_ptr<Ui::LinkButton>(box, u"Biriktirilganlarni tozalash"_q),
		st::boxRowPadding);
	clear->setClickedCallback([=] {
		state->files.clear();
		refresh();
	});

	const auto sending = box->lifetime().make_state<bool>(false);
	box->addButton(rpl::single(u"Yuborish"_q), [=] {
		if (*sending) {
			return;
		}
		const auto comment = input->getLastText().trimmed();
		if (comment.isEmpty() && state->files.isEmpty()) {
			input->showError();
			return;
		}
		*sending = true;
		const auto weak = base::make_weak(controller);
		SendWithUser(
			&controller->session(),
			u"qo'lda yuborildi"_q,
			u"Foydalanuvchi sozlamalardan yubordi."_q,
			comment,
			state->files,
			[=](bool ok) {
				if (const auto strong = weak.get()) {
					strong->showToast(ok
						? u"Hato hisoboti yuborildi. Rahmat!"_q
						: u"Hisobotni yuborib bo'lmadi (internet?)"_q);
				}
			});
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void WatchConnection(not_null<Main::Session*> session) {
	struct State {
		base::Timer timer;
		crl::time badSince = 0;
		crl::time lastSent = 0;
	};
	const auto state = session->lifetime().make_state<State>();
	state->timer.setCallback([=] {
		if (!Enabled(Flag::ErrorReports)) {
			state->badSince = 0;
			return;
		}
		const auto code = session->mtp().dcstate();
		const auto bad = (code == MTP::ConnectingState)
			|| (code == MTP::DisconnectedState)
			|| (code < 0);
		const auto now = crl::now();
		if (!bad) {
			state->badSince = 0;
			return;
		} else if (!state->badSince) {
			state->badSince = now;
			return;
		} else if (now - state->badSince < kStuckFor
			|| (state->lastSent && now - state->lastSent < kCooldown)) {
			return;
		}
		state->lastSent = now;
		Send(
			session,
			u"ulanish muammosi"_q,
			u"Ulanish holati: %1 (1=ulanmoqda, 0=uzilgan, <0=qayta urinish)\n"
			"Shu holatda turgan vaqt: %2 soniya"_q.arg(code).arg(
				(now - state->badSince) / 1000));
	});
	state->timer.callEach(kPoll);
}

} // namespace Paradoks::Report
