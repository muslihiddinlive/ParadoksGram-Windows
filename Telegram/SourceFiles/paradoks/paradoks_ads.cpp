/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ads.h"

#include "paradoks/paradoks_ads_server.h"
#include "paradoks/paradoks_config.h"
#include "apiwrap.h"

#include <QtCore/QDate>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace Paradoks::Ads {
namespace {

constexpr auto kIdPrefix = "PDXAD:";
constexpr auto kDefaultTtl = crl::time(30 * 60 * 1000);
constexpr auto kMaxBody = 256 * 1024;

struct Ad {
	int id = 0;
	QString title;
	QString text;
	QString button;
	QString url;
	bool channel = false;
	bool bot = false;
	int weight = 1;
	int dailyCap = 3;
};

std::vector<Ad> Ads;
bool CacheLoaded = false;
bool Fetching = false;
crl::time LastFetch = 0;
crl::time Ttl = kDefaultTtl;

[[nodiscard]] QString Server() {
	auto result = QString::fromUtf8(PARADOKS_AD_SERVER).trimmed();
	while (result.endsWith('/')) {
		result.chop(1);
	}
	return result;
}

[[nodiscard]] bool SafeUrl(const QString &url) {
	const auto lower = url.toLower();
	return lower.startsWith(u"https://"_q)
		|| lower.startsWith(u"http://"_q)
		|| lower.startsWith(u"tg://"_q);
}

[[nodiscard]] QNetworkAccessManager *Network() {
	static const auto manager = new QNetworkAccessManager();
	return manager;
}

[[nodiscard]] std::optional<std::vector<Ad>> Parse(const QByteArray &body) {
	const auto document = QJsonDocument::fromJson(body);
	if (!document.isObject()) {
		return std::nullopt;
	}
	const auto array = document.object().value(u"ads"_q).toArray();
	auto list = std::vector<Ad>();
	for (const auto &value : array) {
		const auto object = value.toObject();
		auto ad = Ad{
			.id = object.value(u"id"_q).toInt(),
			.title = object.value(u"title"_q).toString(),
			.text = object.value(u"text"_q).toString(),
			.button = object.value(u"button"_q).toString(u"Ochish"_q),
			.url = object.value(u"url"_q).toString(),
			.weight = std::max(1, object.value(u"weight"_q).toInt(1)),
			.dailyCap = std::max(1, object.value(u"daily_cap"_q).toInt(3)),
		};
		for (const auto &placement : object.value(u"placements"_q).toArray()) {
			const auto name = placement.toString();
			if (name == u"channel"_q) {
				ad.channel = true;
			} else if (name == u"bot"_q) {
				ad.bot = true;
			}
		}
		if (ad.title.isEmpty() || ad.text.isEmpty() || !SafeUrl(ad.url)) {
			continue;
		}
		list.push_back(std::move(ad));
	}
	return list;
}

void LoadCache() {
	if (std::exchange(CacheLoaded, true)) {
		return;
	}
	const auto cached = StringSetting(u"ads/cache"_q).toUtf8();
	if (const auto parsed = Parse(cached)) {
		Ads = *parsed;
	}
}

[[nodiscard]] QString Today() {
	return QDate::currentDate().toString(u"yyyyMMdd"_q);
}

[[nodiscard]] QString SeenKey(int id) {
	return u"ads/seen_%1_%2"_q.arg(id).arg(Today());
}

[[nodiscard]] QString InstallId() {
	auto id = StringSetting(u"ads/install_id"_q);
	if (id.isEmpty()) {
		id = QString::number(QRandomGenerator::global()->generate64(), 16);
		SetStringSetting(u"ads/install_id"_q, id);
	}
	return id;
}

[[nodiscard]] int IdOf(const QByteArray &randomId) {
	return QString::fromUtf8(randomId).mid(int(strlen(kIdPrefix))).toInt();
}

void SendEvent(int adId, const QString &kind) {
	const auto server = Server();
	if (server.isEmpty()) {
		return;
	}
	auto object = QJsonObject();
	object.insert(u"ad_id"_q, adId);
	object.insert(u"kind"_q, kind);
	object.insert(u"install_id"_q, InstallId());
	auto request = QNetworkRequest(QUrl(server + u"/event"_q));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	request.setTransferTimeout(8000);
	const auto reply = Network()->post(
		request,
		QJsonDocument(object).toJson(QJsonDocument::Compact));
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
	});
}

[[nodiscard]] const Ad *Pick(bool bot) {
	LoadCache();
	auto total = 0;
	auto candidates = std::vector<const Ad*>();
	for (const auto &ad : Ads) {
		if (bot ? !ad.bot : !ad.channel) {
			continue;
		} else if (IntSetting(SeenKey(ad.id), 0) >= ad.dailyCap) {
			continue;
		}
		candidates.push_back(&ad);
		total += ad.weight;
	}
	if (candidates.empty()) {
		return nullptr;
	}
	auto r = int(QRandomGenerator::global()->bounded(total));
	for (const auto ad : candidates) {
		r -= ad->weight;
		if (r < 0) {
			return ad;
		}
	}
	return candidates.front();
}

} // namespace

bool Active() {
	return Enabled(Flag::OwnAds) && !Server().isEmpty();
}

void Refresh() {
	if (!Active()) {
		return;
	}
	LoadCache();
	const auto now = crl::now();
	if (Fetching || (LastFetch && (now - LastFetch) < Ttl)) {
		return;
	}
	Fetching = true;
	auto request = QNetworkRequest(QUrl(Server() + u"/ads"_q));
	request.setRawHeader("Accept", "application/json");
	request.setTransferTimeout(8000);
	const auto reply = Network()->get(request);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		Fetching = false;
		LastFetch = crl::now();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (reply->error() == QNetworkReply::NoError
			&& status == 200
			&& reply->bytesAvailable() <= kMaxBody) {
			const auto body = reply->readAll();
			if (const auto parsed = Parse(body)) {
				Ads = *parsed;
				SetStringSetting(u"ads/cache"_q, QString::fromUtf8(body));
				const auto ttl = QJsonDocument::fromJson(body).object().value(
					u"ttl"_q).toInt(1800);
				Ttl = crl::time(std::max(60, ttl)) * 1000;
			}
		} else {
			// Try again in about five minutes.
			LastFetch = crl::now() - Ttl + 5 * 60 * 1000;
		}
		reply->deleteLater();
	});
}

MTPmessages_sponsoredMessages Build(bool bot) {
	if (!Active()) {
		return MTP_messages_sponsoredMessagesEmpty();
	}
	Refresh();
	const auto ad = Pick(bot);
	if (!ad) {
		return MTP_messages_sponsoredMessagesEmpty();
	}
	const auto randomId = QByteArray(kIdPrefix) + QByteArray::number(ad->id);
	auto messages = QVector<MTPSponsoredMessage>();
	messages.push_back(MTP_sponsoredMessage(
		MTP_flags(0),
		MTP_bytes(randomId),
		MTP_string(ad->url),
		MTP_string(ad->title),
		MTP_string(ad->text),
		MTPVector<MTPMessageEntity>(),
		MTPPhoto(),
		MTPMessageMedia(),
		MTPPeerColor(),
		MTP_string(ad->button),
		MTPstring(),
		MTPstring(),
		MTPint(),
		MTPint()));
	return MTP_messages_sponsoredMessages(
		MTP_flags(0),
		MTPint(),
		MTPint(),
		MTPint(),
		MTP_vector<MTPSponsoredMessage>(std::move(messages)),
		MTP_vector<MTPChat>(),
		MTP_vector<MTPUser>());
}

bool IsOurs(const QByteArray &randomId) {
	return randomId.startsWith(kIdPrefix)
		&& randomId.size() > int(strlen(kIdPrefix));
}

void OnView(const QByteArray &randomId) {
	const auto id = IdOf(randomId);
	SetIntSetting(SeenKey(id), IntSetting(SeenKey(id), 0) + 1);
	SendEvent(id, u"view"_q);
}

void OnClick(const QByteArray &randomId) {
	SendEvent(IdOf(randomId), u"click"_q);
}

} // namespace Paradoks::Ads
