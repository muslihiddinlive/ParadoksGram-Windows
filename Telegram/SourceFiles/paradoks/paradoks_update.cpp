/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_update.h"

#include "core/application.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <algorithm>
#include <memory>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

#ifndef PARADOKS_BUILD_NUMBER
#define PARADOKS_BUILD_NUMBER 0
#endif // !PARADOKS_BUILD_NUMBER

namespace Paradoks::Update {
namespace {

constexpr auto kReleasesUrl = "https://api.github.com/repos/"
	"muslihiddinlive/ParadoksGram-Windows/releases?per_page=10";
constexpr auto kDownloadPrefix = "https://github.com/"
	"muslihiddinlive/ParadoksGram-Windows/releases/download/";
constexpr auto kAssetName = "ParadoksGram-Windows-x64.zip";
constexpr auto kMinExeSize = 5 * 1024 * 1024;

struct Info {
	int build = 0;
	QString url;
	QString sha256;
	qint64 size = 0;
};

struct State {
	Info available;
	rpl::variable<int> availableBuild = rpl::variable<int>(0);
	bool started = false;
};

[[nodiscard]] State &Data() {
	static auto state = State();
	return state;
}

[[nodiscard]] QNetworkAccessManager *Network() {
	// Intentionally never destroyed: it must outlive QApplication teardown.
	static const auto result = [] {
		const auto manager = new QNetworkAccessManager();
		manager->setRedirectPolicy(
			QNetworkRequest::NoLessSafeRedirectPolicy);
		return manager;
	}();
	return result;
}

[[nodiscard]] QNetworkRequest MakeRequest(const QUrl &url) {
	auto request = QNetworkRequest(url);
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setRawHeader("User-Agent", "ParadoksGram-Windows");
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	return request;
}

bool Parse(const QByteArray &data) {
	const auto document = QJsonDocument::fromJson(data);
	if (!document.isArray()) {
		return false;
	}
	static const auto tagRegex = QRegularExpression(u"^build-(\\d+)$"_q);
	auto best = Info();
	for (const auto &value : document.array()) {
		const auto release = value.toObject();
		if (release.value(u"draft"_q).toBool()) {
			continue;
		}
		const auto match = tagRegex.match(
			release.value(u"tag_name"_q).toString());
		if (!match.hasMatch()) {
			continue;
		}
		const auto build = match.captured(1).toInt();
		if (build <= best.build) {
			continue;
		}
		for (const auto &item : release.value(u"assets"_q).toArray()) {
			const auto asset = item.toObject();
			if (asset.value(u"name"_q).toString() != QLatin1String(kAssetName)) {
				continue;
			}
			const auto url = asset.value(u"browser_download_url"_q).toString();
			if (!url.startsWith(QLatin1String(kDownloadPrefix))) {
				continue;
			}
			auto digest = asset.value(u"digest"_q).toString();
			if (digest.startsWith(u"sha256:"_q)) {
				digest = digest.mid(7).toLower();
			} else {
				digest = QString();
			}
			best = Info{
				.build = build,
				.url = url,
				.sha256 = digest,
				.size = qint64(asset.value(u"size"_q).toDouble()),
			};
			break;
		}
	}
	auto &state = Data();
	state.available = (best.build > CurrentBuild()) ? best : Info();
	state.availableBuild = state.available.build;
	return true;
}

// A running download/install, shared with the box that shows its progress.
struct Job {
	QPointer<QNetworkReply> reply;
	QPointer<QProcess> process;
	std::unique_ptr<QFile> file;
	QCryptographicHash hash = QCryptographicHash(QCryptographicHash::Sha256);
	QString dir;
	bool cancelled = false;
	bool finished = false;
	Fn<void(const QString &)> status;
	Fn<void(const QString &)> fail;

	void cancel() {
		cancelled = true;
		if (reply) {
			reply->abort();
		}
		if (process) {
			process->kill();
		}
	}
};

[[nodiscard]] QString QuoteBatch(const QString &path) {
	return u"\""_q + QDir::toNativeSeparators(path) + u"\""_q;
}

bool LaunchApplyScript(
		const QString &script,
		const QString &source,
		const QString &target,
		const QString &log) {
#ifdef Q_OS_WIN
	const auto command = u"cmd.exe /d /c \"%1 %2 %3 %4 %5\""_q.arg(
		QuoteBatch(script),
		QString::number(QCoreApplication::applicationPid()),
		QuoteBatch(source),
		QuoteBatch(target),
		QuoteBatch(log));
	auto buffer = command.toStdWString();
	buffer.push_back(L'\0');
	auto startup = STARTUPINFOW();
	startup.cb = sizeof(startup);
	auto info = PROCESS_INFORMATION();
	const auto ok = CreateProcessW(
		nullptr,
		buffer.data(),
		nullptr,
		nullptr,
		FALSE,
		CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
		nullptr,
		nullptr,
		&startup,
		&info);
	if (!ok) {
		return false;
	}
	CloseHandle(info.hThread);
	CloseHandle(info.hProcess);
	return true;
#else // Q_OS_WIN
	Q_UNUSED(script);
	Q_UNUSED(source);
	Q_UNUSED(target);
	Q_UNUSED(log);
	return false;
#endif // Q_OS_WIN
}

void Apply(const std::shared_ptr<Job> &job) {
	const auto source = job->dir + u"/out/Telegram.exe"_q;
	auto exe = QFile(source);
	auto header = QByteArray();
	if (exe.open(QIODevice::ReadOnly)) {
		header = exe.read(2);
		exe.close();
	}
	if (!QFileInfo::exists(source)
		|| QFileInfo(source).size() < kMinExeSize
		|| header != "MZ") {
		job->fail(u"Arxiv ichida Telegram.exe topilmadi yoki buzilgan."_q);
		return;
	}

	const auto script = job->dir + u"/apply.cmd"_q;
	const auto log = job->dir + u"/apply.log"_q;
	auto file = QFile(script);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		job->fail(u"Yangilash skriptini yozib bo'lmadi."_q);
		return;
	}
	const auto lines = QStringList{
		u"@echo off"_q,
		u"set PID=%~1"_q,
		u"set SRC=%~2"_q,
		u"set DST=%~3"_q,
		u"set LOG=%~4"_q,
		u"set /a n=0"_q,
		u"set /a k=0"_q,
		u"echo start> \"%LOG%\""_q,
		u":wait"_q,
		u"tasklist /FI \"PID eq %PID%\" /NH 2>nul | find \"%PID%\" >nul"_q,
		u"if errorlevel 1 goto copy"_q,
		u"set /a n+=1"_q,
		u"if %n% GEQ 300 goto giveup"_q,
		u"ping -n 2 127.0.0.1 >nul"_q,
		u"goto wait"_q,
		u":copy"_q,
		u"copy /Y \"%SRC%\" \"%DST%\" >> \"%LOG%\" 2>&1"_q,
		u"if not errorlevel 1 goto done"_q,
		u"set /a k+=1"_q,
		u"if %k% GEQ 30 goto fail"_q,
		u"ping -n 2 127.0.0.1 >nul"_q,
		u"goto copy"_q,
		u":done"_q,
		u"echo done>> \"%LOG%\""_q,
		u"start \"\" \"%DST%\""_q,
		u"exit /b 0"_q,
		u":fail"_q,
		u"echo copy failed>> \"%LOG%\""_q,
		u"start \"\" \"%DST%\""_q,
		u"exit /b 1"_q,
		u":giveup"_q,
		u"echo old app still running>> \"%LOG%\""_q,
		u"exit /b 2"_q,
	};
	file.write(lines.join(u"\r\n"_q).toLocal8Bit() + "\r\n");
	file.close();

	const auto target = QCoreApplication::applicationFilePath();
	if (!LaunchApplyScript(script, source, target, log)) {
		job->fail(u"Yangilovchini ishga tushirib bo'lmadi."_q);
		return;
	}
	job->finished = true;
	job->status(u"Yangilanmoqda... ilova qayta ochiladi."_q);
	QTimer::singleShot(1500, [] {
		Core::Quit();
	});
}

void Extract(const std::shared_ptr<Job> &job, const QString &zip) {
	const auto out = job->dir + u"/out"_q;
	QDir(out).removeRecursively();
	QDir().mkpath(out);
	job->status(u"Fayllar ochilmoqda..."_q);

	const auto escape = [](QString path) {
		return QDir::toNativeSeparators(path).replace('\'', u"''"_q);
	};
	const auto process = new QProcess();
	job->process = process;
#ifdef Q_OS_WIN
	process->setCreateProcessArgumentsModifier([](
			QProcess::CreateProcessArguments *args) {
		args->flags |= CREATE_NO_WINDOW;
	});
#endif // Q_OS_WIN
	QObject::connect(
		process,
		QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		[=](int code, QProcess::ExitStatus status) {
			process->deleteLater();
			if (job->cancelled) {
				return;
			} else if (status != QProcess::NormalExit || code != 0) {
				job->fail(u"Arxivni ochib bo'lmadi."_q);
				return;
			}
			Apply(job);
		});
	QObject::connect(
		process,
		&QProcess::errorOccurred,
		[=](QProcess::ProcessError error) {
			if (error == QProcess::FailedToStart && !job->cancelled) {
				process->deleteLater();
				job->fail(u"PowerShell ishga tushmadi."_q);
			}
		});
	process->start(u"powershell.exe"_q, {
		u"-NoProfile"_q,
		u"-NonInteractive"_q,
		u"-ExecutionPolicy"_q,
		u"Bypass"_q,
		u"-Command"_q,
		u"Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force"_q
			.arg(escape(zip), escape(out)),
	});
}

void Download(const std::shared_ptr<Job> &job, const Info &info) {
	job->dir = QDir::tempPath() + u"/ParadoksGram-update"_q;
	QDir(job->dir).removeRecursively();
	QDir().mkpath(job->dir);
	const auto zip = job->dir + u"/update.zip"_q;
	job->file = std::make_unique<QFile>(zip);
	if (!job->file->open(QIODevice::WriteOnly)) {
		job->fail(u"Vaqtinchalik faylni ochib bo'lmadi."_q);
		return;
	}

	const auto reply = Network()->get(MakeRequest(QUrl(info.url)));
	job->reply = reply;
	job->status(u"Yuklanmoqda: 0%"_q);

	QObject::connect(reply, &QNetworkReply::readyRead, [=] {
		const auto data = reply->readAll();
		if (job->file && job->file->isOpen()) {
			job->file->write(data);
			job->hash.addData(data);
		}
	});
	QObject::connect(
		reply,
		&QNetworkReply::downloadProgress,
		[=](qint64 received, qint64 total) {
			if (total <= 0) {
				total = info.size;
			}
			if (total > 0 && !job->cancelled) {
				job->status(u"Yuklanmoqda: %1%"_q.arg(
					std::min<qint64>(100, received * 100 / total)));
			}
		});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (job->cancelled) {
			return;
		}
		const auto data = reply->readAll();
		if (!data.isEmpty() && job->file->isOpen()) {
			job->file->write(data);
			job->hash.addData(data);
		}
		const auto failed = (reply->error() != QNetworkReply::NoError);
		job->file->close();
		if (failed) {
			job->fail(u"Yuklab bo'lmadi: "_q + reply->errorString());
			return;
		}
		const auto written = QFileInfo(zip).size();
		if (info.size > 0 && written != info.size) {
			job->fail(u"Fayl to'liq yuklanmadi."_q);
			return;
		}
		if (!info.sha256.isEmpty()
			&& QString::fromLatin1(job->hash.result().toHex())
				!= info.sha256) {
			job->fail(u"Fayl nazorat summasi mos kelmadi."_q);
			return;
		}
		Extract(job, zip);
	});
}

void UpdateBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto info = Data().available;
	box->setTitle(rpl::single(u"Yangi versiya"_q));
	const auto label = QPointer<Ui::FlatLabel>(box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			u"ParadoksGram build-%1 chiqdi (sizda build-%2).\n"
			"Yangilasangiz, ilova yopilib, qayta ochiladi. "
			"Akkauntlar va sozlamalar saqlanadi."_q.arg(
				info.build).arg(CurrentBuild()),
			st::boxLabel)));

	const auto job = std::make_shared<Job>();
	const auto showClose = [=] {
		box->clearButtons();
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	};
	job->status = [=](const QString &text) {
		if (label) {
			label->setText(text);
		}
	};
	const auto raw = job.get();
	job->fail = [=](const QString &text) {
		raw->finished = true;
		if (label) {
			label->setText(text
				+ u"\nQo'lda yuklash: github.com/muslihiddinlive/"
				"ParadoksGram-Windows/releases"_q);
		}
		showClose();
	};
	box->lifetime().add([=] {
		if (!job->finished) {
			job->cancel();
		}
	});

	box->addButton(rpl::single(u"Yangilash"_q), [=] {
		box->clearButtons();
		box->addButton(rpl::single(u"Bekor qilish"_q), [=] {
			job->cancel();
			box->closeBox();
		});
		Download(job, info);
	});
	box->addButton(rpl::single(u"Keyinroq"_q), [=] { box->closeBox(); });
}

} // namespace

int CurrentBuild() {
	return PARADOKS_BUILD_NUMBER;
}

int AvailableBuild() {
	return Data().available.build;
}

rpl::producer<int> AvailableValue() {
	return Data().availableBuild.value();
}

void Check(Fn<void(Result)> done) {
	if (CurrentBuild() <= 0) {
		if (done) {
			done(Result::Latest);
		}
		return;
	}
	const auto reply = Network()->get(MakeRequest(QUrl(
		QString::fromLatin1(kReleasesUrl))));
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto ok = (reply->error() == QNetworkReply::NoError)
			&& Parse(reply->readAll());
		if (done) {
			done(!ok
				? Result::Failed
				: AvailableBuild() > 0
				? Result::Newer
				: Result::Latest);
		}
	});
}

void ShowBox(not_null<Window::SessionController*> controller) {
	if (AvailableBuild() > 0) {
		controller->show(Box(UpdateBox, controller));
	}
}

void CheckOnStart() {
	auto &state = Data();
	if (state.started || CurrentBuild() <= 0) {
		return;
	}
	state.started = true;
	// The "new version" button at the bottom of the chats list reacts to
	// the result; check shortly after start and then every few hours.
	QTimer::singleShot(15 * 1000, [] {
		Check(nullptr);
	});
	const auto timer = new QTimer();
	timer->setInterval(4 * 60 * 60 * 1000);
	QObject::connect(timer, &QTimer::timeout, [] {
		Check(nullptr);
	});
	timer->start();
}

} // namespace Paradoks::Update
