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
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <deque>
#include <memory>

namespace Paradoks::WebCopy {
namespace {

constexpr auto kMaxFiles = 200;
constexpr auto kMaxFileSize = qint64(12 * 1024 * 1024);
constexpr auto kMaxTotalSize = qint64(60 * 1024 * 1024);

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
	std::deque<Item> queue;
	QHash<QString, QString> known; // url -> local path
	QStringList failed;
	qint64 total = 0;
	int saved = 0;
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
	return Relative(*job, fromLocal, i.value());
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

void Finish(const std::shared_ptr<Job> &job) {
	auto info = QStringList{
		u"ParadoksGram: mini-app sahifa nusxasi"_q,
		u"Manzil: "_q + job->root.adjusted(
			QUrl::RemoveQuery | QUrl::RemoveFragment).toString(),
		u"Vaqt: "_q + QDateTime::currentDateTime().toString(Qt::ISODate),
		u"Saqlangan fayllar: %1"_q.arg(job->saved + 1),
		u"Yuklanmagan: %1"_q.arg(job->failed.size()),
	};
	info += job->failed;
	info.push_back(QString());
	info.push_back(u"Eslatma: skriptlar ichidan keyin yuklanadigan "
		"fayllar va serverdagi (backend) kod saqlanmaydi."_q);
	WriteFile(job->dir + u"/_info.txt"_q, info.join('\n').toUtf8());
	job->progress(u"Tayyor: %1 ta fayl saqlandi."_q.arg(job->saved + 1));
	job->done(job->dir);
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
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto guard = std::unique_ptr<QNetworkReply, void(*)(QNetworkReply*)>(
			reply,
			[](QNetworkReply *r) { r->deleteLater(); });
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
		const auto rewritten = RewriteHtml(job, html);
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
