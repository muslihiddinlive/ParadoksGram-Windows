/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_update.h"

#include "core/application.h"
#include "paradoks/paradoks_config.h"

#include <algorithm>
#include <memory>
#include <vector>

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
#include <QtCore/QtGlobal>
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
constexpr auto kSevenAssetName = "ParadoksGram-Windows-x64.7z";
constexpr auto kMinExeSize = 5 * 1024 * 1024;
constexpr auto kParallelMinSize = qint64(2 * 1024 * 1024);
constexpr auto kParallelParts = 6;
constexpr auto kChunkTries = 5;

struct Source {
	QString url;
	QString sha256;
	qint64 size = 0;
	bool sevenZip = false;
};

struct Info {
	int build = 0;
	Source zip;
	Source sevenZip;
};

struct Chunk {
	qint64 begin = 0;
	qint64 end = 0; // Inclusive.
	qint64 received = 0;
	int tries = 0;
	bool done = false;
	QPointer<QNetworkReply> reply;
};

// A running background download/install.
struct Job {
	QPointer<QNetworkReply> reply; // Single-stream mode.
	QPointer<QProcess> process;
	std::unique_ptr<QFile> file;
	std::vector<Chunk> chunks;
	qint64 received = 0;
	int percent = -1;
	int generation = 0;
	QString dir;
	QString archive;
	Info info;
	Source source;
	bool cancelled = false;
	bool finished = false;
	Fn<void(const QString &)> status;
	Fn<void(const QString &)> fail;

	void abortReplies() {
		if (reply) {
			reply->abort();
		}
		for (auto &chunk : chunks) {
			if (chunk.reply) {
				chunk.reply->abort();
			}
		}
	}
	void cancel() {
		cancelled = true;
		abortReplies();
		if (process) {
			process->kill();
		}
	}
};

struct State {
	Info available;
	rpl::variable<int> availableBuild = rpl::variable<int>(0);
	// -1 idle, 0..100 downloading, 101 installing.
	rpl::variable<int> progress = rpl::variable<int>(-1);
	rpl::event_stream<QString> failed;
	std::shared_ptr<Job> job;
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

[[nodiscard]] QNetworkRequest MakeRequest(
		const QUrl &url,
		bool binary = false) {
	auto request = QNetworkRequest(url);
	request.setRawHeader(
		"Accept",
		binary ? "application/octet-stream" : "application/vnd.github+json");
	request.setRawHeader("User-Agent", "ParadoksGram-Windows");
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	return request;
}

[[nodiscard]] Source ReadAsset(const QJsonObject &asset, bool sevenZip) {
	const auto url = asset.value(u"browser_download_url"_q).toString();
	if (!url.startsWith(QLatin1String(kDownloadPrefix))) {
		return {};
	}
	auto digest = asset.value(u"digest"_q).toString();
	if (digest.startsWith(u"sha256:"_q)) {
		digest = digest.mid(7).toLower();
	} else {
		digest = QString();
	}
	return {
		.url = url,
		.sha256 = digest,
		.size = qint64(asset.value(u"size"_q).toDouble()),
		.sevenZip = sevenZip,
	};
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
		auto found = Info();
		for (const auto &item : release.value(u"assets"_q).toArray()) {
			const auto asset = item.toObject();
			const auto name = asset.value(u"name"_q).toString();
			if (name == QLatin1String(kAssetName)) {
				found.zip = ReadAsset(asset, false);
			} else if (name == QLatin1String(kSevenAssetName)) {
				found.sevenZip = ReadAsset(asset, true);
			}
		}
		if (!found.zip.url.isEmpty()) {
			found.build = build;
			best = found;
		}
	}
	auto &state = Data();
	state.available = (best.build > CurrentBuild()) ? best : Info();
	state.availableBuild = state.available.build;
	return true;
}

void SetProgress(int percent) {
	Data().progress = percent;
}

[[nodiscard]] QString QuoteArg(const QString &text) {
	return u"\""_q + QDir::toNativeSeparators(text) + u"\""_q;
}

// Hidden PowerShell (CREATE_NO_WINDOW, but not DETACHED_PROCESS, so no
// console windows pop up) waits for this process and swaps the exe.
bool LaunchApplyScript(
		const QString &script,
		const QString &source,
		const QString &target,
		const QString &log) {
#ifdef Q_OS_WIN
	const auto command = u"powershell.exe -NoProfile -NonInteractive "
		"-ExecutionPolicy Bypass -WindowStyle Hidden -File %1 "
		"-ProcId %2 -Src %3 -Dst %4 -Log %5"_q.arg(
			QuoteArg(script),
			QString::number(QCoreApplication::applicationPid()),
			QuoteArg(source),
			QuoteArg(target),
			QuoteArg(log));
	auto buffer = command.toStdWString();
	buffer.push_back(L'\0');
	auto startup = STARTUPINFOW();
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;
	auto info = PROCESS_INFORMATION();
	const auto ok = CreateProcessW(
		nullptr,
		buffer.data(),
		nullptr,
		nullptr,
		FALSE,
		CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
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

	const auto script = job->dir + u"/apply.ps1"_q;
	const auto log = job->dir + u"/apply.log"_q;
	auto file = QFile(script);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		job->fail(u"Yangilash skriptini yozib bo'lmadi."_q);
		return;
	}
	const auto lines = QStringList{
		u"param([int]$ProcId, [string]$Src, [string]$Dst, [string]$Log)"_q,
		u"function L([string]$m) {"_q,
		u"  try { Add-Content -LiteralPath $Log -Value ((Get-Date -Format s) + ' ' + $m) } catch {}"_q,
		u"}"_q,
		u"L 'start'"_q,
		u"try { Wait-Process -Id $ProcId -Timeout 300 -ErrorAction SilentlyContinue } catch { L ('wait: ' + $_.Exception.Message) }"_q,
		u"Start-Sleep -Milliseconds 500"_q,
		u"$ok = $false"_q,
		u"for ($i = 0; $i -lt 60 -and -not $ok; $i++) {"_q,
		u"  try {"_q,
		u"    Copy-Item -LiteralPath $Src -Destination $Dst -Force -ErrorAction Stop"_q,
		u"    $ok = $true"_q,
		u"  } catch {"_q,
		u"    L ('copy failed: ' + $_.Exception.Message)"_q,
		u"    Start-Sleep -Milliseconds 750"_q,
		u"  }"_q,
		u"}"_q,
		u"if ($ok) { L 'done' } else { L 'gave up' }"_q,
		u"try { Start-Process -FilePath $Dst } catch { L ('start failed: ' + $_.Exception.Message) }"_q,
	};
	file.write(lines.join(u"\r\n"_q).toUtf8() + "\r\n");
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

void Download(const std::shared_ptr<Job> &job, const Source &source);

[[nodiscard]] bool FileHashMatches(const QString &path, const QString &sha) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	if (!hash.addData(&file)) {
		return false;
	}
	return QString::fromLatin1(hash.result().toHex()) == sha;
}

[[nodiscard]] QString ReadyMarkerPath(const QString &dir) {
	return dir + u"/ready.txt"_q;
}

// The extracted exe stays in the temp folder, so if installing fails the
// next attempt does not download everything again.
void WriteReady(const std::shared_ptr<Job> &job) {
	auto file = QFile(ReadyMarkerPath(job->dir));
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QByteArray::number(job->info.build));
	}
}

[[nodiscard]] int ReadyBuild(const QString &dir) {
	auto file = QFile(ReadyMarkerPath(dir));
	if (!file.open(QIODevice::ReadOnly)) {
		return 0;
	}
	return file.readAll().trimmed().toInt();
}

[[nodiscard]] bool PreparedReady(const QString &dir, int build) {
	return (build > 0)
		&& (ReadyBuild(dir) == build)
		&& (QFileInfo(dir + u"/out/Telegram.exe"_q).size() >= kMinExeSize);
}

[[nodiscard]] QString UpdateDir() {
	return QDir::tempPath() + u"/ParadoksGram-update"_q;
}

void Extract(const std::shared_ptr<Job> &job) {
	const auto out = job->dir + u"/out"_q;
	QDir(out).removeRecursively();
	QDir().mkpath(out);
	job->status(QString());

	const auto archive = job->archive;
	const auto sevenZip = job->source.sevenZip;
	// 7z archives are opened with the system tar.exe; if that is not
	// possible on this Windows, remember it and use the zip instead.
	const auto retryWithZip = [=] {
		if (!sevenZip) {
			return false;
		}
		SetSevenZipBroken();
		if (job->info.zip.url.isEmpty()) {
			return false;
		}
		Download(job, job->info.zip);
		return true;
	};
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
			}
			const auto ok = (status == QProcess::NormalExit)
				&& (code == 0)
				&& QFileInfo::exists(out + u"/Telegram.exe"_q);
			if (!ok) {
				if (!retryWithZip()) {
					job->fail(u"Arxivni ochib bo'lmadi."_q);
				}
				return;
			}
			WriteReady(job);
			Apply(job);
		});
	QObject::connect(
		process,
		&QProcess::errorOccurred,
		[=](QProcess::ProcessError error) {
			if (error == QProcess::FailedToStart && !job->cancelled) {
				process->deleteLater();
				if (!retryWithZip()) {
					job->fail(u"Arxivni ochuvchi ishga tushmadi."_q);
				}
			}
		});
	if (sevenZip) {
		const auto root = qEnvironmentVariable("SystemRoot", u"C:/Windows"_q);
		process->start(
			QDir::toNativeSeparators(root + u"/System32/tar.exe"_q),
			{ u"-xf"_q, QDir::toNativeSeparators(archive),
				u"-C"_q, QDir::toNativeSeparators(out) });
	} else {
		process->start(u"powershell.exe"_q, {
			u"-NoProfile"_q,
			u"-NonInteractive"_q,
			u"-ExecutionPolicy"_q,
			u"Bypass"_q,
			u"-Command"_q,
			u"Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force"_q
				.arg(escape(archive), escape(out)),
		});
	}
}

void Finish(const std::shared_ptr<Job> &job) {
	if (job->file) {
		job->file->flush();
		job->file->close();
	}
	const auto &source = job->source;
	const auto written = QFileInfo(job->archive).size();
	if (source.size > 0 && written != source.size) {
		job->fail(u"Fayl to'liq yuklanmadi."_q);
		return;
	}
	if (!source.sha256.isEmpty()
		&& !FileHashMatches(job->archive, source.sha256)) {
		job->fail(u"Fayl nazorat summasi mos kelmadi."_q);
		return;
	}
	Extract(job);
}

void ReportProgress(const std::shared_ptr<Job> &job, qint64 total) {
	const auto percent = (total > 0)
		? int(std::min<qint64>(99, job->received * 100 / total))
		: 0;
	if (percent != job->percent) {
		job->percent = percent;
		SetProgress(percent);
	}
}

void StartSingle(const std::shared_ptr<Job> &job) {
	const auto generation = job->generation;
	job->file->resize(0);
	job->file->seek(0);
	job->received = 0;

	const auto reply = Network()->get(
		MakeRequest(QUrl(job->source.url), true));
	job->reply = reply;

	const auto drain = [=] {
		const auto data = reply->readAll();
		if (!data.isEmpty() && job->file && job->file->isOpen()) {
			job->file->write(data);
			job->received += data.size();
			ReportProgress(job, job->source.size);
		}
	};
	QObject::connect(reply, &QNetworkReply::readyRead, [=] {
		if (!job->cancelled && generation == job->generation) {
			drain();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (job->cancelled || generation != job->generation) {
			return;
		}
		drain();
		if (reply->error() != QNetworkReply::NoError) {
			job->file->close();
			job->fail(u"Yuklab bo'lmadi: "_q + reply->errorString());
			return;
		}
		Finish(job);
	});
}

void StartChunk(const std::shared_ptr<Job> &job, int index);

// The server did not honor Range: restart as one plain download.
void FallbackToSingle(const std::shared_ptr<Job> &job, int generation) {
	if (job->cancelled || generation != job->generation) {
		return;
	}
	++job->generation;
	job->abortReplies();
	job->chunks.clear();
	StartSingle(job);
}

void ConsumeChunk(
		const std::shared_ptr<Job> &job,
		int index,
		QNetworkReply *reply) {
	auto &chunk = job->chunks[index];
	auto data = reply->readAll();
	const auto code = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (data.isEmpty() || code != 206) {
		return;
	}
	const auto left = chunk.end + 1 - (chunk.begin + chunk.received);
	if (data.size() > left) {
		data.truncate(int(std::max<qint64>(0, left)));
	}
	if (!data.isEmpty()
		&& job->file->seek(chunk.begin + chunk.received)
		&& job->file->write(data) == data.size()) {
		chunk.received += data.size();
		job->received += data.size();
		ReportProgress(job, job->source.size);
	}
}

void StartChunk(const std::shared_ptr<Job> &job, int index) {
	const auto generation = job->generation;
	auto &chunk = job->chunks[index];
	auto request = MakeRequest(QUrl(job->source.url), true);
	request.setRawHeader("Range", "bytes="
		+ QByteArray::number(chunk.begin + chunk.received)
		+ "-"
		+ QByteArray::number(chunk.end));
	const auto reply = Network()->get(request);
	chunk.reply = reply;

	QObject::connect(reply, &QNetworkReply::readyRead, [=] {
		if (job->cancelled || generation != job->generation) {
			return;
		}
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (code != 206) {
			QTimer::singleShot(0, [=] {
				FallbackToSingle(job, generation);
			});
			return;
		}
		ConsumeChunk(job, index, reply);
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (job->cancelled || generation != job->generation) {
			return;
		}
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (code == 200) {
			FallbackToSingle(job, generation);
			return;
		}
		ConsumeChunk(job, index, reply);
		auto &chunk = job->chunks[index];
		const auto complete = (chunk.begin + chunk.received > chunk.end);
		if (reply->error() != QNetworkReply::NoError || !complete) {
			if (++chunk.tries > kChunkTries) {
				job->file->close();
				job->fail(u"Yuklab bo'lmadi: "_q + reply->errorString());
				return;
			}
			QTimer::singleShot(300 * chunk.tries, [=] {
				if (!job->cancelled && generation == job->generation) {
					StartChunk(job, index);
				}
			});
			return;
		}
		chunk.done = true;
		const auto all = std::all_of(
			job->chunks.begin(),
			job->chunks.end(),
			[](const Chunk &c) { return c.done; });
		if (all) {
			Finish(job);
		}
	});
}

void StartParallel(const std::shared_ptr<Job> &job) {
	const auto total = job->source.size;
	job->file->resize(total);
	job->received = 0;
	const auto step = (total + kParallelParts - 1) / kParallelParts;
	for (auto begin = qint64(0); begin < total; begin += step) {
		auto chunk = Chunk();
		chunk.begin = begin;
		chunk.end = std::min(total, begin + step) - 1;
		job->chunks.push_back(std::move(chunk));
	}
	for (auto i = 0; i != int(job->chunks.size()); ++i) {
		StartChunk(job, i);
	}
}

void Download(const std::shared_ptr<Job> &job, const Source &source) {
	++job->generation;
	job->abortReplies();
	job->chunks.clear();
	if (job->file) {
		job->file->close();
		job->file = nullptr;
	}
	job->source = source;
	job->dir = UpdateDir();
	QDir(job->dir).removeRecursively();
	QDir().mkpath(job->dir);
	job->archive = job->dir
		+ (source.sevenZip ? u"/update.7z"_q : u"/update.zip"_q);
	job->file = std::make_unique<QFile>(job->archive);
	if (!job->file->open(QIODevice::ReadWrite | QIODevice::Truncate)) {
		job->fail(u"Vaqtinchalik faylni ochib bo'lmadi."_q);
		return;
	}
	job->percent = -1;
	SetProgress(0);
	if (source.size >= kParallelMinSize) {
		StartParallel(job);
	} else {
		StartSingle(job);
	}
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

int Progress() {
	return Data().progress.current();
}

rpl::producer<int> ProgressValue() {
	return Data().progress.value();
}

rpl::producer<QString> Failures() {
	return Data().failed.events();
}

void Start() {
	auto &state = Data();
	if (state.available.build <= 0) {
		return;
	} else if (state.job && !state.job->finished) {
		return;
	}
	const auto job = std::make_shared<Job>();
	state.job = job;
	job->info = state.available;
	job->status = [](const QString &) {
		SetProgress(101);
	};
	const auto raw = job.get();
	job->fail = [=](const QString &text) {
		raw->finished = true;
		SetProgress(-1);
		Data().failed.fire_copy(text
			+ u"\nQo'lda yuklash: github.com/muslihiddinlive/"
			"ParadoksGram-Windows/releases"_q);
	};
	job->dir = UpdateDir();
	if (PreparedReady(job->dir, job->info.build)) {
		SetProgress(101);
		Apply(job);
		return;
	}
	const auto useSevenZip = !job->info.sevenZip.url.isEmpty()
		&& !SevenZipBroken();
	Download(job, useSevenZip ? job->info.sevenZip : job->info.zip);
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
	Q_UNUSED(controller);
	Start();
}

void CheckOnStart() {
	auto &state = Data();
	if (state.started || CurrentBuild() <= 0) {
		return;
	}
	state.started = true;
	// Leftovers of an update that was installed successfully.
	const auto leftover = UpdateDir();
	const auto readyBuild = ReadyBuild(leftover);
	if (readyBuild > 0 && readyBuild <= CurrentBuild()) {
		QDir(leftover).removeRecursively();
	}
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
