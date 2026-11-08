/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_webcopy.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include "crl/crl.h"

#include <QtCore/QMimeDatabase>

#include <deque>
#include <memory>

namespace Paradoks::WebCopy {
namespace {

constexpr auto kMaxFiles = 2000;
constexpr auto kMaxFileSize = qint64(100 * 1024 * 1024);
constexpr auto kMaxTotalSize = qint64(1536) * 1024 * 1024; // 1.5 GB

struct Item {
	QUrl url;
	QString local;
	bool css = false;
};

struct Job {
	QNetworkAccessManager network;
	QUrl root;
	QString dir;
	QString html;
	QString initData; // See the doc comment of Start() in the header.
	std::deque<Item> queue;
	QHash<QString, QString> known; // url -> local path
	QStringList failed;
	qint64 total = 0;
	int saved = 0;
	bool single = false;
	QStringList placeholderLocals;
	QHash<QString, int> placeholderIndex;
	QHash<QString, bool> savedLocals; // local -> is css
	QHash<QString, QString> originalUrls; // local -> absolute url
	Fn<void(const QString &)> progress;
	Fn<void(const QString &)> done;
};

[[nodiscard]] QString LocalPathFor(const Job &job, const QUrl &url) {
	auto path = QDir::cleanPath(url.path());
	if (path.isEmpty() || path == u"/"_q || url.path().endsWith('/')) {
		path += u"/index"_q;
	}
	auto parts = path.split('/', Qt::SkipEmptyParts);
	parts.removeAll(u".."_q);
	parts.removeAll(u"."_q);
	if (parts.isEmpty()) {
		parts.push_back(u"file"_q);
	}
	if (url.hasQuery()) {
		auto &name = parts.back();
		const auto dot = name.lastIndexOf('.');
		const auto tag = u"~"_q + QString::number(
			qHash(url.query()),
			16);
		if (dot > 0) {
			name.insert(dot, tag);
		} else {
			name += tag;
		}
	}
	auto result = parts.join('/');
	if (url.host() != job.root.host()) {
		result = u"_ext/"_q + url.host() + '/' + result;
	}
	static const auto bad = QRegularExpression(u"[<>:\"|?*\\\\]"_q);
	return result.replace(bad, u"_"_q);
}

[[nodiscard]] QString Relative(
		const Job &job,
		const QString &from,
		const QString &to) {
	const auto base = QFileInfo(job.dir + '/' + from).path();
	return QDir(base).relativeFilePath(job.dir + '/' + to);
}

// Registers a link found in a file: queues it and returns the local
// replacement, or an empty string to leave the link untouched.
[[nodiscard]] QString Register(
		const std::shared_ptr<Job> &job,
		const QUrl &base,
		const QString &fromLocal,
		const QString &raw,
		bool css) {
	const auto text = raw.trimmed();
	if (text.isEmpty()
		|| text.startsWith('#')
		|| text.startsWith(u"data:"_q, Qt::CaseInsensitive)
		|| text.startsWith(u"javascript:"_q, Qt::CaseInsensitive)
		|| text.startsWith(u"mailto:"_q, Qt::CaseInsensitive)
		|| text.startsWith(u"blob:"_q, Qt::CaseInsensitive)) {
		return QString();
	}
	auto url = base.resolved(QUrl(text));
	url.setFragment(QString());
	if (!url.isValid()
		|| (url.scheme() != u"http"_q && url.scheme() != u"https"_q)) {
		return QString();
	}
	const auto key = url.toString();
	auto i = job->known.find(key);
	if (i == job->known.end()) {
		if (job->known.size() >= kMaxFiles) {
			return QString();
		}
		const auto local = LocalPathFor(*job, url);
		i = job->known.insert(key, local);
		const auto lower = url.path().toLower();
		job->queue.push_back({
			.url = url,
			.local = local,
			.css = css || lower.endsWith(u".css"_q),
		});
	}
	if (job->single) {
		const auto &local = i.value();
		job->originalUrls.insert(local, key);
		auto index = job->placeholderIndex.value(local, -1);
		if (index < 0) {
			index = job->placeholderLocals.size();
			job->placeholderLocals.push_back(local);
			job->placeholderIndex.insert(local, index);
		}
		return QString(QChar(0xE000))
			+ QString::number(index)
			+ QChar(0xE001);
	}
	return Relative(*job, fromLocal, i.value());
}

[[nodiscard]] QString InjectInitData(
		QString html,
		const QString &initData) {
	if (initData.isEmpty()) {
		return html;
	}
	// Runs before anything else on the page (including the mini-app SDK
	// script), so Telegram.WebApp.initData / initDataUnsafe read the value
	// we set here as if the page had really been opened with it.
	static const auto headRe = QRegularExpression(
		u"<head[^>]*>"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto script = u"<script>(function(){try{"
		"var h=(location.hash&&location.hash.length>1)?location.hash.slice(1):'';"
		"var p=h?h.split('&').filter(function(x){"
			"return x.indexOf('tgWebAppData=')!==0;"
		"}):[];"
		"p.unshift('tgWebAppData=%1');"
		"if(h.indexOf('tgWebAppVersion=')===-1){p.push('tgWebAppVersion=7.10');}"
		"if(h.indexOf('tgWebAppPlatform=')===-1){"
			"p.push('tgWebAppPlatform=tdesktop');"
		"}"
		"location.hash='#'+p.join('&');"
		"}catch(e){}})();</script>"_q.arg(initData);
	const auto match = headRe.match(html);
	if (match.hasMatch()) {
		const auto at = match.capturedEnd();
		return html.left(at) + script + html.mid(at);
	}
	return script + html;
}

[[nodiscard]] QString RewriteHtml(
		const std::shared_ptr<Job> &job,
		const QString &html) {
	static const auto tagRe = QRegularExpression(
		u"<(script|link|img|source|video|audio)\\b[^>]*>"_q,
		QRegularExpression::CaseInsensitiveOption);
	static const auto attrRe = QRegularExpression(
		u"\\b(src|href|poster)\\s*=\\s*([\"'])(.*?)\\2"_q,
		QRegularExpression::CaseInsensitiveOption
			| QRegularExpression::DotMatchesEverythingOption);
	static const auto relRe = QRegularExpression(
		u"\\brel\\s*=\\s*[\"']([^\"']*)[\"']"_q,
		QRegularExpression::CaseInsensitiveOption);
	auto result = QString();
	auto last = 0;
	auto it = tagRe.globalMatch(html);
	while (it.hasNext()) {
		const auto match = it.next();
		result += html.mid(last, match.capturedStart() - last);
		last = match.capturedEnd();
		auto tag = match.captured();
		const auto name = match.captured(1).toLower();
		if (name == u"link"_q) {
			const auto rel = relRe.match(tag).captured(1).toLower();
			if (!rel.contains(u"stylesheet"_q)
				&& !rel.contains(u"icon"_q)
				&& !rel.contains(u"preload"_q)
				&& !rel.contains(u"manifest"_q)) {
				result += tag;
				continue;
			}
		}
		auto out = QString();
		auto tagLast = 0;
		auto attrs = attrRe.globalMatch(tag);
		const auto isStyle = (name == u"link"_q)
			&& relRe.match(tag).captured(1).toLower().contains(
				u"stylesheet"_q);
		while (attrs.hasNext()) {
			const auto attr = attrs.next();
			out += tag.mid(tagLast, attr.capturedStart(3) - tagLast);
			tagLast = attr.capturedEnd(3);
			const auto local = Register(
				job,
				job->root,
				u"index.html"_q,
				attr.captured(3),
				isStyle);
			out += local.isEmpty() ? attr.captured(3) : local;
		}
		out += tag.mid(tagLast);
		result += out;
	}
	result += html.mid(last);
	return result;
}

[[nodiscard]] QString RewriteCss(
		const std::shared_ptr<Job> &job,
		const QUrl &base,
		const QString &fromLocal,
		const QString &css) {
	static const auto urlRe = QRegularExpression(
		u"url\\(\\s*([\"']?)([^\"')]+)\\1\\s*\\)|@import\\s+([\"'])([^\"']+)\\3"_q);
	auto result = QString();
	auto last = 0;
	auto it = urlRe.globalMatch(css);
	while (it.hasNext()) {
		const auto match = it.next();
		const auto group = match.captured(2).isEmpty() ? 4 : 2;
		result += css.mid(last, match.capturedStart(group) - last);
		last = match.capturedEnd(group);
		const auto local = Register(
			job,
			base,
			fromLocal,
			match.captured(group),
			true);
		result += local.isEmpty() ? match.captured(group) : local;
	}
	result += css.mid(last);
	return result;
}

bool WriteFile(const QString &path, const QByteArray &data) {
	QDir().mkpath(QFileInfo(path).path());
	auto file = QFile(path);
	return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

struct Assembly {
	QString dir;
	QString html;
	QString out;
	QStringList locals;
	QHash<QString, bool> css;
	QHash<QString, QString> original;
	QStringList failed;
	int saved = 0;
	bool limited = false;
};

[[nodiscard]] QString MimeFor(const QString &local) {
	const auto lower = local.toLower();
	auto ext = lower.mid(lower.lastIndexOf('.') + 1);
	const auto tilde = ext.indexOf('~');
	if (tilde >= 0) {
		ext = ext.left(tilde);
	}
	if (ext == u"css"_q) return u"text/css"_q;
	if (ext == u"js"_q || ext == u"mjs"_q) return u"text/javascript"_q;
	if (ext == u"json"_q) return u"application/json"_q;
	if (ext == u"svg"_q) return u"image/svg+xml"_q;
	if (ext == u"woff"_q) return u"font/woff"_q;
	if (ext == u"woff2"_q) return u"font/woff2"_q;
	if (ext == u"ttf"_q) return u"font/ttf"_q;
	if (ext == u"otf"_q) return u"font/otf"_q;
	const auto name = QMimeDatabase().mimeTypeForFile(
		u"x."_q + ext,
		QMimeDatabase::MatchExtension).name();
	return name.isEmpty() ? u"application/octet-stream"_q : name;
}

[[nodiscard]] QString ExpandCss(
		const Assembly &data,
		const QString &local,
		int depth) {
	constexpr auto kInlineInCssMax = qint64(8) * 1024 * 1024;
	auto file = QFile(data.dir + '/' + local);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	const auto css = QString::fromUtf8(file.readAll());
	file.close();
	static const auto re = QRegularExpression(u"\\x{E000}(\\d+)\\x{E001}"_q);
	auto result = QString();
	auto last = 0;
	auto it = re.globalMatch(css);
	while (it.hasNext()) {
		const auto match = it.next();
		result += css.mid(last, match.capturedStart() - last);
		last = match.capturedEnd();
		const auto inner = data.locals[match.captured(1).toInt()];
		const auto isCss = data.css.value(inner, false);
		const auto info = QFileInfo(data.dir + '/' + inner);
		const auto original = data.original.value(inner);
		if (!data.css.contains(inner)) {
			result += original;
		} else if (isCss) {
			result += (depth >= 4)
				? original
				: (u"data:text/css;base64,"_q + QString::fromLatin1(
					ExpandCss(data, inner, depth + 1).toUtf8().toBase64()));
		} else if (info.size() > kInlineInCssMax) {
			result += original;
		} else {
			auto asset = QFile(info.filePath());
			if (asset.open(QIODevice::ReadOnly)) {
				result += u"data:"_q + MimeFor(inner) + u";base64,"_q
					+ QString::fromLatin1(asset.readAll().toBase64());
			} else {
				result += original;
			}
		}
	}
	result += css.mid(last);
	return result;
}

[[nodiscard]] bool AssembleSingle(const Assembly &data) {
	QDir().mkpath(QFileInfo(data.out).path());
	auto out = QFile(data.out);
	if (!out.open(QIODevice::WriteOnly)) {
		return false;
	}
	static const auto re = QRegularExpression(u"\\x{E000}(\\d+)\\x{E001}"_q);
	auto last = 0;
	auto it = re.globalMatch(data.html);
	while (it.hasNext()) {
		const auto match = it.next();
		out.write(data.html.mid(last, match.capturedStart() - last).toUtf8());
		last = match.capturedEnd();
		const auto local = data.locals[match.captured(1).toInt()];
		if (!data.css.contains(local)) {
			out.write(data.original.value(local).toUtf8());
		} else if (data.css.value(local)) {
			out.write(u"data:text/css;base64,"_q.toUtf8());
			out.write(ExpandCss(data, local, 0).toUtf8().toBase64());
		} else {
			auto asset = QFile(data.dir + '/' + local);
			if (!asset.open(QIODevice::ReadOnly)) {
				out.write(data.original.value(local).toUtf8());
				continue;
			}
			out.write((u"data:"_q + MimeFor(local) + u";base64,"_q).toUtf8());
			constexpr auto kChunk = 3 * 65536;
			while (!asset.atEnd()) {
				out.write(asset.read(kChunk).toBase64());
			}
		}
	}
	out.write(data.html.mid(last).toUtf8());
	auto info = QStringList{
		u"\n<!-- ParadoksGram: mini-app nusxasi."_q,
		u"Fayllar: %1, yuklanmagan: %2"_q.arg(data.saved).arg(data.failed.size()),
	};
	for (auto line : data.failed) {
		info.push_back(line.replace(u"--"_q, u"- -"_q));
	}
	if (data.limited) {
		info.push_back(u"1.5 GB limitga yetildi, qolgan fayllar saqlanmadi."_q);
	}
	info.push_back(u"Skriptlar ichidan keyin yuklanadigan fayllar va backend kod saqlanmaydi. -->\n"_q);
	out.write(info.join('\n').toUtf8());
	return out.error() == QFileDevice::NoError;
}

void FinishSingle(const std::shared_ptr<Job> &job) {
	job->progress(u"Bitta HTML fayl yig'ilmoqda..."_q);
	auto data = std::make_shared<Assembly>();
	data->dir = job->dir;
	data->html = job->html;
	data->out = job->dir + u".html"_q;
	data->locals = job->placeholderLocals;
	data->css = job->savedLocals;
	data->original = job->originalUrls;
	data->failed = job->failed;
	data->saved = job->saved + 1;
	data->limited = !job->queue.empty();
	const auto progress = job->progress;
	const auto done = job->done;
	crl::async([=] {
		const auto ok = AssembleSingle(*data);
		QDir(data->dir).removeRecursively();
		crl::on_main([=] {
			if (!ok) {
				progress(u"HTML faylni yig'ib bo'lmadi."_q);
				done(QString());
				return;
			}
			progress(u"Tayyor: %1 ta fayl bitta HTML ichiga joylandi."_q.arg(
				data->saved));
			done(data->out);
		});
	});
}

void Finish(const std::shared_ptr<Job> &job) {
	if (job->single) {
		FinishSingle(job);
		return;
	}
	auto info = QStringList{
		u"ParadoksGram: mini-app sahifa nusxasi"_q,
		u"Manzil: "_q + job->root.adjusted(
			QUrl::RemoveQuery | QUrl::RemoveFragment).toString(),
		u"Vaqt: "_q + QDateTime::currentDateTime().toString(Qt::ISODate),
		u"Saqlangan fayllar: %1"_q.arg(job->saved + 1),
		u"Yuklanmagan: %1"_q.arg(job->failed.size()),
	};
	info += job->failed;
	if (!job->queue.empty()) {
		info.push_back(u"1.5 GB limitga yetildi, qolgan fayllar saqlanmadi."_q);
	}
	info.push_back(QString());
	info.push_back(u"Eslatma: skriptlar ichidan keyin yuklanadigan "
		"fayllar va serverdagi (backend) kod saqlanmaydi."_q);
	WriteFile(job->dir + u"/_info.txt"_q, info.join('\n').toUtf8());
	job->progress(u"ZIP yaratilmoqda..."_q);
	const auto zip = job->dir + u".zip"_q;
	QFile::remove(zip);
	const auto process = new QProcess();
	process->setProgram(u"tar"_q);
	process->setArguments({
		u"-a"_q, u"-c"_q, u"-f"_q, zip, u"-C"_q, job->dir, u"."_q,
	});
	QObject::connect(
		process,
		&QProcess::errorOccurred,
		process,
		[=](QProcess::ProcessError error) {
			if (error != QProcess::FailedToStart) {
				return;
			}
			process->deleteLater();
			job->progress(u"ZIP yaratib bo'lmadi (tar topilmadi)."_q);
			job->done(QString());
		});
	QObject::connect(
		process,
		QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		process,
		[=](int code, QProcess::ExitStatus) {
			process->deleteLater();
			if (code != 0 || !QFileInfo::exists(zip)) {
				job->progress(u"ZIP yaratib bo'lmadi."_q);
				job->done(QString());
				return;
			}
			QDir(job->dir).removeRecursively();
			job->progress(u"Tayyor: %1 ta fayl, ZIP yaratildi."_q.arg(
				job->saved + 1));
			job->done(zip);
		});
	process->start();
}

void Next(const std::shared_ptr<Job> &job) {
	if (job->queue.empty() || job->total > kMaxTotalSize) {
		Finish(job);
		return;
	}
	const auto item = job->queue.front();
	job->queue.pop_front();
	job->progress(u"Yuklanmoqda: "_q + item.url.toString());
	auto request = QNetworkRequest(item.url);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(20000);
	request.setHeader(
		QNetworkRequest::UserAgentHeader,
		u"Mozilla/5.0 (Windows NT 10.0; Win64; x64) ParadoksGram-Copy"_q);
	const auto reply = job->network.get(request);
	// Everything except CSS is streamed straight to the disk, so big
	// files (video) do not have to fit into memory.
	const auto streamed = !item.css;
	const auto file = std::make_shared<QFile>();
	const auto written = std::make_shared<qint64>(0);
	if (streamed) {
		const auto path = job->dir + '/' + item.local;
		QDir().mkpath(QFileInfo(path).path());
		file->setFileName(path);
		if (!file->open(QIODevice::WriteOnly)) {
			job->failed.push_back(item.url.toString()
				+ u" (saqlab bo'lmadi)"_q);
			reply->abort();
		}
		QObject::connect(reply, &QNetworkReply::readyRead, reply, [=] {
			const auto chunk = reply->readAll();
			if (!file->isOpen()) {
				return;
			}
			*written += chunk.size();
			if (job->total + *written > kMaxTotalSize) {
				reply->abort();
				return;
			}
			file->write(chunk);
		});
	}
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto guard = std::unique_ptr<QNetworkReply, void(*)(QNetworkReply*)>(
			reply,
			[](QNetworkReply *r) { r->deleteLater(); });
		if (streamed) {
			const auto opened = file->isOpen();
			file->close();
			if (reply->error() != QNetworkReply::NoError) {
				QFile::remove(file->fileName());
				if (opened) {
					job->failed.push_back(item.url.toString() + u" ("_q
						+ reply->errorString() + ')');
				}
			} else if (opened) {
				job->total += *written;
				++job->saved;
				job->savedLocals.insert(item.local, false);
			}
			Next(job);
			return;
		}
		if (reply->error() != QNetworkReply::NoError) {
			job->failed.push_back(item.url.toString() + u" ("_q
				+ reply->errorString() + ')');
		} else {
			auto data = reply->readAll();
			if (data.size() > kMaxFileSize) {
				job->failed.push_back(item.url.toString()
					+ u" (juda katta)"_q);
			} else {
				if (item.css) {
					const auto rewritten = RewriteCss(
						job,
						reply->url(),
						item.local,
						QString::fromUtf8(data));
					data = rewritten.toUtf8();
				}
				job->total += data.size();
				if (WriteFile(job->dir + '/' + item.local, data)) {
					++job->saved;
					job->savedLocals.insert(item.local, item.css);
				} else {
					job->failed.push_back(item.url.toString()
						+ u" (saqlab bo'lmadi)"_q);
				}
			}
		}
		Next(job);
	});
}

} // namespace

void Start(
		const QString &url,
		bool single,
		const QString &initData,
		Fn<void(const QString &)> progress,
		Fn<void(const QString &)> done) {
	auto root = QUrl(url.trimmed());
	root.setFragment(QString());
	if (!root.isValid()
		|| (root.scheme() != u"http"_q && root.scheme() != u"https"_q)) {
		done(QString());
		return;
	}
	const auto job = std::make_shared<Job>();
	job->root = root;
	job->single = single;
	job->initData = initData;
	job->progress = std::move(progress);
	job->done = std::move(done);
	const auto documents = QStandardPaths::writableLocation(
		QStandardPaths::DocumentsLocation);
	job->dir = documents
		+ u"/ParadoksGram/MiniAppCopies/"_q
		+ root.host()
		+ '_'
		+ QDateTime::currentDateTime().toString(u"yyyyMMdd_HHmmss"_q);
	QDir().mkpath(job->dir);

	job->progress(u"Sahifa yuklanmoqda..."_q);
	auto request = QNetworkRequest(root);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(20000);
	request.setHeader(
		QNetworkRequest::UserAgentHeader,
		u"Mozilla/5.0 (Windows NT 10.0; Win64; x64) ParadoksGram-Copy"_q);
	const auto reply = job->network.get(request);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			job->progress(u"Sahifani yuklab bo'lmadi: "_q
				+ reply->errorString());
			job->done(QString());
			return;
		}
		job->root = reply->url();
		job->root.setFragment(QString());
		const auto html = QString::fromUtf8(reply->readAll());
		const auto rewritten = InjectInitData(
			RewriteHtml(job, html),
			job->initData);
		if (single) {
			job->html = rewritten;
			Next(job);
			return;
		}
		if (!WriteFile(
				job->dir + u"/index.html"_q,
				rewritten.toUtf8())) {
			job->progress(u"index.html ni saqlab bo'lmadi"_q);
			job->done(QString());
			return;
		}
		Next(job);
	});
}

} // namespace Paradoks::WebCopy
