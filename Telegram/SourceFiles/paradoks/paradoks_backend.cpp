/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_backend.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_peer_id.h"
#include "main/main_session.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <memory>

namespace Paradoks::Backend {
namespace {

// Taxminan backend'dagi _PENDING_TTL_SECONDS (180s) ga mos, bir oz zaxira
// bilan: 1.5s * 130 ~= 195s.
constexpr auto kPollInterval = crl::time(1500);
constexpr auto kMaxPolls = 130;

[[nodiscard]] QNetworkAccessManager *Network() {
	// Hech qachon yo'q qilinmaydi: QApplication tugashidan ham uzoq yashashi kerak.
	static const auto result = new QNetworkAccessManager();
	return result;
}

[[nodiscard]] QString UserKey(not_null<Main::Session*> session) {
	return QString::number(session->userId().bare);
}

[[nodiscard]] QString ConnectedKey(not_null<Main::Session*> session) {
	return u"backend/connected_"_q + UserKey(session);
}

// PeerId (Windows ichidagi shaklda) <-> Telethon/Bot API "belgilangan" chat_id
// (foydalanuvchi: musbat; oddiy guruh: manfiy; kanal/supergroup: -100...).
// Backend Telethon orqali ishlaydi va aynan shu konvensiyadan foydalanadi -
// shuning uchun server bilan gaplashganda har doim shu ikki funksiya orqali
// o'giramiz, PeerId.value'ni hech qachon to'g'ridan-to'g'ri yubormaymiz.
[[nodiscard]] int64 MarkedChatId(PeerId id) {
	if (peerIsUser(id)) {
		return int64(peerToUser(id).bare);
	} else if (peerIsChat(id)) {
		return -int64(peerToChat(id).bare);
	} else if (peerIsChannel(id)) {
		return -1000000000000LL - int64(peerToChannel(id).bare);
	}
	return 0;
}

[[nodiscard]] PeerId PeerFromMarkedChatId(int64 markedId) {
	if (markedId > 0) {
		return peerFromUser(UserId(BareId(markedId)));
	} else if (markedId > -1000000000000LL) {
		return peerFromChat(ChatId(BareId(-markedId)));
	} else {
		return peerFromChannel(ChannelId(BareId(-1000000000000LL - markedId)));
	}
}

[[nodiscard]] QUrl Endpoint(const QString &path) {
	auto base = Url();
	while (base.endsWith('/')) {
		base.chop(1);
	}
	return QUrl(base + path);
}

void PostJson(
		const QString &path,
		const QJsonObject &body,
		Fn<void(QJsonObject, int)> done) {
	auto request = QNetworkRequest(Endpoint(path));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	request.setTransferTimeout(20'000);
	const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
	const auto reply = Network()->post(request, payload);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto data = reply->readAll();
		reply->deleteLater();
		const auto document = QJsonDocument::fromJson(data);
		Debug::Log(u"backend POST %1: status=%2"_q.arg(path).arg(status));
		if (done) {
			done(document.isObject() ? document.object() : QJsonObject(), status);
		}
	});
}

void GetJson(const QString &path, Fn<void(QJsonObject, int)> done) {
	auto request = QNetworkRequest(Endpoint(path));
	request.setTransferTimeout(20'000);
	const auto reply = Network()->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto data = reply->readAll();
		reply->deleteLater();
		const auto document = QJsonDocument::fromJson(data);
		if (done) {
			done(document.isObject() ? document.object() : QJsonObject(), status);
		}
	});
}

struct ConnectFlow {
	base::weak_ptr<Main::Session> session;
	Fn<void(QString)> progress;
	Fn<void()> done;
	Fn<void()> passwordRequired;
	Fn<void(QString)> fail;
	base::Timer pollTimer;
	int pollsLeft = kMaxPolls;
};

std::unique_ptr<ConnectFlow> ActiveFlow;

void StopFlow() {
	if (ActiveFlow) {
		ActiveFlow->pollTimer.cancel();
		ActiveFlow = nullptr;
	}
}

void PollOnce() {
	if (!ActiveFlow) {
		return;
	}
	const auto session = ActiveFlow->session.get();
	if (!session) {
		StopFlow();
		return;
	}
	auto body = QJsonObject();
	body.insert(u"user_key"_q, UserKey(session));
	PostJson(u"/sessions/qr/poll"_q, body, [=](QJsonObject result, int status) {
		if (!ActiveFlow || ActiveFlow->session.get() != session) {
			return;
		}
		const auto state = result.value(u"status"_q).toString();
		if (state == u"connected"_q) {
			SetStringSetting(ConnectedKey(session), u"1"_q);
			const auto done = ActiveFlow->done;
			StopFlow();
			if (done) {
				done();
			}
		} else if (state == u"password_required"_q) {
			// Parolni SubmitPassword orqali kutamiz - shu oraliqda pollni
			// to'xtatamiz (backend ham parol kelguncha "waiting" qaytaraveradi).
			ActiveFlow->pollTimer.cancel();
			const auto passwordRequired = ActiveFlow->passwordRequired;
			if (passwordRequired) {
				passwordRequired();
			}
		} else if (state == u"error"_q) {
			const auto message = result.value(u"message"_q).toString();
			const auto fail = ActiveFlow->fail;
			StopFlow();
			if (fail) {
				fail(message.isEmpty() ? u"Noma'lum xatolik"_q : message);
			}
		} else {
			if (--ActiveFlow->pollsLeft <= 0) {
				const auto fail = ActiveFlow->fail;
				StopFlow();
				if (fail) {
					fail(u"Tasdiqlash vaqti tugadi. Qaytadan urinib ko'ring."_q);
				}
				return;
			}
			ActiveFlow->pollTimer.callOnce(kPollInterval);
		}
	});
}

} // namespace

QString Url() {
	return StringSetting(u"backend/url"_q);
}

void SetUrl(const QString &url) {
	SetStringSetting(u"backend/url"_q, url.trimmed());
}

bool Connected(not_null<Main::Session*> session) {
	return StringSetting(ConnectedKey(session)) == u"1"_q;
}

void CheckStatus(not_null<Main::Session*> session, Fn<void(bool)> done) {
	if (Url().isEmpty()) {
		if (done) {
			done(false);
		}
		return;
	}
	GetJson(
		u"/sessions/%1/status"_q.arg(UserKey(session)),
		[=](QJsonObject result, int status) {
			const auto connected = (status >= 200)
				&& (status < 300)
				&& result.value(u"connected"_q).toBool();
			SetStringSetting(
				ConnectedKey(session),
				connected ? u"1"_q : QString());
			if (done) {
				done(connected);
			}
		});
}

void Connect(
		not_null<Main::Session*> session,
		PeerId targetChatId,
		Fn<void(QString)> progress,
		Fn<void()> done,
		Fn<void()> passwordRequired,
		Fn<void(QString)> fail) {
	if (Url().isEmpty()) {
		if (fail) {
			fail(u"Avval sozlamalarda server manzilini kiriting."_q);
		}
		return;
	}
	StopFlow();
	// Diqqat: ConnectFlow ichida base::Timer bor (ko'chirib bo'lmaydigan),
	// shuning uchun uni aggregate sifatida make_unique(ConnectFlow{...}) deb
	// bitta argument bilan (keyin ko'chirish kerak bo'ladigan) yarata
	// olmaymiz - avval bo'sh yaratib, keyin maydonlarga alohida yozamiz.
	ActiveFlow = std::make_unique<ConnectFlow>();
	ActiveFlow->session = base::make_weak(session.get());
	ActiveFlow->progress = progress;
	ActiveFlow->done = done;
	ActiveFlow->passwordRequired = passwordRequired;
	ActiveFlow->fail = fail;
	if (progress) {
		progress(u"Serverga so'rov yuborilmoqda..."_q);
	}
	const auto weakSession = base::make_weak(session.get());
	auto body = QJsonObject();
	body.insert(u"user_key"_q, UserKey(session));
	if (targetChatId) {
		body.insert(
			u"target_chat_id"_q,
			double(MarkedChatId(targetChatId)));
	}
	PostJson(u"/sessions/qr/start"_q, body, [=](QJsonObject result, int status) {
		const auto strong = weakSession.get();
		if (!strong || !ActiveFlow || ActiveFlow->session.get() != strong) {
			return;
		}
		if (status < 200 || status >= 300
			|| result.value(u"status"_q).toString() == u"error"_q) {
			const auto message = result.value(u"message"_q).toString();
			const auto fail = ActiveFlow->fail;
			StopFlow();
			if (fail) {
				fail(message.isEmpty() ? u"Serverga ulanib bo'lmadi"_q : message);
			}
			return;
		}
		const auto tokenB64 = result.value(u"token"_q).toString();
		const auto token = QByteArray::fromBase64(
			tokenB64.toUtf8(),
			QByteArray::Base64UrlEncoding);
		if (token.isEmpty()) {
			const auto fail = ActiveFlow->fail;
			StopFlow();
			if (fail) {
				fail(u"Server noto'g'ri javob qaytardi (token)"_q);
			}
			return;
		}
		if (ActiveFlow->progress) {
			ActiveFlow->progress(
				u"O'zingizning sessiyangiz orqali tasdiqlanmoqda..."_q);
		}
		strong->api().request(MTPauth_AcceptLoginToken(
			MTP_bytes(token)
		)).done([=](const MTPAuthorization &) {
			if (!ActiveFlow || ActiveFlow->session.get() != strong) {
				return;
			}
			if (ActiveFlow->progress) {
				ActiveFlow->progress(
					u"Tasdiqlandi, server ulanishini kutyapmiz..."_q);
			}
			ActiveFlow->pollTimer.setCallback([] { PollOnce(); });
			PollOnce();
		}).fail([=](const MTP::Error &error) {
			if (!ActiveFlow || ActiveFlow->session.get() != strong) {
				return;
			}
			const auto fail = ActiveFlow->fail;
			StopFlow();
			if (fail) {
				fail(u"Tasdiqlab bo'lmadi: "_q + error.type());
			}
		}).send();
	});
}

void SubmitPassword(
		not_null<Main::Session*> session,
		const QString &password,
		Fn<void()> done,
		Fn<void(QString)> fail) {
	if (!ActiveFlow || ActiveFlow->session.get() != session.get()) {
		if (fail) {
			fail(u"Ulanish seansi tugagan, qaytadan urining."_q);
		}
		return;
	}
	const auto weakSession = base::make_weak(session.get());
	auto body = QJsonObject();
	body.insert(u"user_key"_q, UserKey(session));
	body.insert(u"password"_q, password);
	PostJson(u"/sessions/qr/password"_q, body, [=](QJsonObject result, int status) {
		const auto strong = weakSession.get();
		if (!strong || !ActiveFlow || ActiveFlow->session.get() != strong) {
			return;
		}
		if (status < 200 || status >= 300
			|| result.value(u"status"_q).toString() != u"ok"_q) {
			const auto message = result.value(u"message"_q).toString();
			if (fail) {
				fail(message.isEmpty() ? u"Parol noto'g'ri"_q : message);
			}
			return;
		}
		ActiveFlow->done = done;
		ActiveFlow->pollTimer.setCallback([] { PollOnce(); });
		PollOnce();
	});
}

void SetTarget(
		not_null<Main::Session*> session,
		PeerId targetChatId,
		Fn<void(bool)> done) {
	if (Url().isEmpty()) {
		if (done) {
			done(false);
		}
		return;
	}
	auto body = QJsonObject();
	body.insert(u"target_chat_id"_q, double(MarkedChatId(targetChatId)));
	PostJson(
		u"/sessions/%1/target"_q.arg(UserKey(session)),
		body,
		[=](QJsonObject result, int status) {
			if (done) {
				done(status >= 200 && status < 300);
			}
		});
}

void Disconnect(not_null<Main::Session*> session, Fn<void(bool)> done) {
	StopFlow();
	if (Url().isEmpty()) {
		SetStringSetting(ConnectedKey(session), QString());
		if (done) {
			done(true);
		}
		return;
	}
	PostJson(
		u"/sessions/%1/disconnect"_q.arg(UserKey(session)),
		QJsonObject(),
		[=](QJsonObject result, int status) {
			SetStringSetting(ConnectedKey(session), QString());
			if (done) {
				done(status >= 200 && status < 300);
			}
		});
}

void LookupCopy(
		not_null<Main::Session*> session,
		PeerId sourceChatId,
		int64 msgId,
		Fn<void(PeerId, int64)> found,
		Fn<void()> notFound) {
	if (Url().isEmpty()) {
		if (notFound) {
			notFound();
		}
		return;
	}
	const auto path = u"/sessions/%1/copy?chat_id=%2&msg_id=%3"_q
		.arg(UserKey(session))
		.arg(MarkedChatId(sourceChatId))
		.arg(msgId);
	GetJson(path, [=](QJsonObject result, int status) {
		if (status < 200
			|| status >= 300
			|| !result.value(u"found"_q).toBool()) {
			if (notFound) {
				notFound();
			}
			return;
		}
		const auto targetMarked = int64(
			result.value(u"target_chat_id"_q).toDouble());
		const auto copyMsgId = int64(
			result.value(u"copy_msg_id"_q).toDouble());
		if (found) {
			found(PeerFromMarkedChatId(targetMarked), copyMsgId);
		}
	});
}

} // namespace Paradoks::Backend
