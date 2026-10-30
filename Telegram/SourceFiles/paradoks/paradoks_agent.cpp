/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_agent.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/timer.h"
#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "boxes/abstract_box.h" // Ui::show().
#include "ui/boxes/confirm_box.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>
#include <memory>

namespace Paradoks::Agent {
namespace {

constexpr auto kPollInterval = crl::time(4000);
constexpr auto kErrorBackoff = crl::time(15000);
constexpr auto kNextJobDelay = crl::time(400);
constexpr auto kConfirmTimeout = crl::time(60000);
constexpr auto kFileWait = crl::time(45000);

struct State {
	explicit State(not_null<Main::Session*> session) : session(session) {
	}

	not_null<Main::Session*> session;
	base::Timer timer;
	bool pairSent = false;
	bool dead = false;
};

using StatePtr = std::shared_ptr<State>;
using Done = Fn<void(QJsonObject)>;

[[nodiscard]] QNetworkAccessManager *Network() {
	// Intentionally never destroyed: it must outlive QApplication teardown.
	static const auto result = new QNetworkAccessManager();
	return result;
}

[[nodiscard]] QString DeviceToken() {
	auto token = StringSetting(u"ai/device_token"_q);
	if (token.size() < 32) {
		auto bytes = QByteArray(24, Qt::Uninitialized);
		for (auto &byte : bytes) {
			byte = char(QRandomGenerator::securelySeeded().bounded(256));
		}
		token = QString::fromLatin1(bytes.toHex());
		SetStringSetting(u"ai/device_token"_q, token);
	}
	return token;
}

[[nodiscard]] QString BotUsername() {
	const auto value = StringSetting(u"ai/bot"_q);
	return value.isEmpty() ? u"ParadoksAiBot"_q : value;
}

[[nodiscard]] QJsonObject Error(const QString &text) {
	return QJsonObject{ { u"error"_q, text } };
}

[[nodiscard]] QString NameOf(not_null<PeerData*> peer) {
	return peer->isSelf() ? u"Saqlangan xabarlar"_q : peer->name();
}

[[nodiscard]] QNetworkRequest MakeRequest(const QString &path) {
	auto request = QNetworkRequest(QUrl(ServerUrl() + path));
	request.setRawHeader("X-Device-Token", DeviceToken().toLatin1());
	request.setRawHeader("User-Agent", "ParadoksGram-Windows");
	request.setTransferTimeout(20'000);
	return request;
}

[[nodiscard]] bool Active(const StatePtr &state) {
	return !state->dead
		&& Enabled(Flag::DeviceCommands)
		&& !ServerUrl().isEmpty()
		&& (&Core::App().activeAccount() == &state->session->account())
		&& state->session->user()->id.value != 0;
}

// ---- peers ----

[[nodiscard]] PeerData *Resolve(
		not_null<Main::Session*> session,
		const QString &query,
		bool usersOnly) {
	auto needle = query.trimmed().toLower();
	if (needle.startsWith('@')) {
		needle = needle.mid(1);
	}
	if (needle.isEmpty()) {
		return nullptr;
	}
	auto best = (PeerData*)nullptr;
	auto bestScore = -1;
	for (const auto &row : session->data().chatsList()->indexed()->all()) {
		const auto history = row->history();
		if (!history) {
			continue;
		}
		const auto peer = history->peer;
		const auto user = peer->asUser();
		if (usersOnly && (!user || user->isBot())) {
			continue;
		}
		const auto title = NameOf(peer).toLower();
		const auto username = peer->username().toLower();
		auto score = -1;
		if (!username.isEmpty() && username == needle) {
			score = 100;
		} else if (title == needle) {
			score = 90;
		} else if (title.startsWith(needle)) {
			score = 70;
		} else if (title.contains(needle)) {
			score = 50;
		}
		if (score > bestScore) {
			bestScore = score;
			best = peer;
		}
	}
	return best;
}

[[nodiscard]] PeerData *ByRef(
		not_null<Main::Session*> session,
		const QString &ref) {
	auto ok = false;
	const auto value = ref.toULongLong(&ok);
	return ok ? session->data().peerLoaded(PeerId(value)) : nullptr;
}

// Goes through the messages of the history that are loaded right now.
template <typename Callback>
void ForEachLoadedItem(not_null<History*> history, Callback &&callback) {
	for (const auto &block : history->blocks) {
		for (const auto &element : block->messages) {
			callback(element->data());
		}
	}
}

// ---- actions ----

QJsonObject DoSendMessage(
		not_null<Main::Session*> session,
		const QString &target,
		const QString &text) {
	if (target.trimmed().isEmpty() || text.trimmed().isEmpty()) {
		return Error(u"Kimga va nima yozish kerakligi aniq emas."_q);
	}
	const auto peer = Resolve(session, target, false);
	if (!peer) {
		return Error(u"\"%1\" nomli chat yoki odam topilmadi. Aniqroq nom "
			"yoki @username bilan qayta urining."_q.arg(target));
	}
	auto message = Api::MessageToSend(
		Api::SendAction(session->data().history(peer)));
	message.textWithTags = TextWithTags{ text };
	session->api().sendMessage(std::move(message));
	return QJsonObject{
		{ u"ok"_q, true },
		{ u"chat_title"_q, NameOf(peer) },
	};
}

QJsonObject DoListUnread(not_null<Main::Session*> session) {
	auto chats = QJsonArray();
	for (const auto &row : session->data().chatsList()->indexed()->all()) {
		const auto history = row->history();
		if (!history || history->unreadCount() <= 0) {
			continue;
		}
		const auto peer = history->peer;
		chats.push_back(QJsonObject{
			{ u"title"_q, NameOf(peer) },
			{ u"unread"_q, history->unreadCount() },
			{ u"type"_q, peer->isUser()
				? u"shaxsiy"_q
				: peer->isBroadcast()
				? u"kanal"_q
				: u"guruh"_q },
		});
	}
	return QJsonObject{
		{ u"chats"_q, chats },
		{ u"note"_q,
			u"Bu ro'yxatga qarash xabarlarni o'qilgan deb belgilamaydi."_q },
	};
}

[[nodiscard]] bool IsApk(not_null<DocumentData*> document) {
	return document->mimeString()
			== u"application/vnd.android.package-archive"_q
		|| document->filename().endsWith(u".apk"_q, Qt::CaseInsensitive);
}

[[nodiscard]] HistoryItem *FindApk(not_null<History*> history) {
	auto found = (HistoryItem*)nullptr;
	ForEachLoadedItem(history, [&](not_null<HistoryItem*> item) {
		const auto media = item->media();
		const auto document = media ? media->document() : nullptr;
		if (!found && !item->out() && document && IsApk(document)) {
			found = item;
		}
	});
	return found;
}

// Windows can only report facts about the file (it cannot open an APK).
void DoFindApk(
		const StatePtr &state,
		const QString &hint,
		Done done) {
	const auto session = state->session;
	const auto needle = hint.trimmed().toLower();
	auto candidate = (UserData*)nullptr;
	for (const auto &row : session->data().chatsList()->indexed()->all()) {
		const auto history = row->history();
		const auto user = history ? history->peer->asUser() : nullptr;
		if (!user || user->isSelf()) {
			continue;
		}
		const auto deletedMatch = user->isInaccessible()
			&& (needle.isEmpty()
				|| needle.contains(u"o'chir"_q)
				|| needle.contains(u"ochir"_q)
				|| needle.contains(u"delete"_q));
		const auto nameMatch = !needle.isEmpty()
			&& user->name().toLower().contains(needle);
		if (deletedMatch || nameMatch) {
			candidate = user;
			if (nameMatch) {
				break;
			}
		}
	}
	if (!candidate) {
		done(Error(u"\"%1\" ga mos kontakt topilmadi (o'chirilgan hisob "
			"yoki shunday nomli odam yo'q)."_q.arg(hint)));
		return;
	}
	const auto history = session->data().history(candidate);
	const auto item = FindApk(history);
	auto result = QJsonObject{
		{ u"chat_ref"_q, QString::number(candidate->id.value) },
		{ u"chat_title"_q, NameOf(candidate) },
	};
	if (!item) {
		result.insert(
			u"error"_q,
			u"\"%1\" dan .apk fayl topilmadi (yuklangan tarixda)."_q.arg(
				NameOf(candidate)));
		done(result);
		return;
	}
	const auto document = item->media()->document();
	result.insert(u"file_name"_q, document->filename());
	result.insert(u"size_bytes"_q, double(document->size));
	result.insert(
		u"note"_q,
		u"Windows ilovasi APK ichini (ruxsatlar, imzo) tahlil qila olmaydi: "
		"faqat fayl nomi, hajmi va SHA-256 berildi."_q);
	const auto path = document->filepath(true);
	if (path.isEmpty() || !QFile::exists(path)) {
		// Start the download and wait for it, like the Android agent does.
		document->save(
			Data::FileOriginMessage(item->fullId()),
			QString());
	}
	struct Wait {
		base::Timer timer;
		crl::time started = crl::now();
	};
	const auto wait = std::make_shared<Wait>();
	const auto weak = std::weak_ptr<State>(state);
	const auto check = [=]() mutable {
		const auto strong = weak.lock();
		if (!strong || strong->dead) {
			wait->timer.cancel();
			return;
		}
		const auto file = document->filepath(true);
		if (!file.isEmpty() && QFile::exists(file) && !document->loading()) {
			wait->timer.cancel();
			auto reply = result;
			auto source = QFile(file);
			if (source.open(QIODevice::ReadOnly)) {
				auto hash = QCryptographicHash(QCryptographicHash::Sha256);
				hash.addData(&source);
				reply.insert(
					u"sha256"_q,
					QString::fromLatin1(hash.result().toHex()));
			} else {
				reply.insert(u"error"_q, u"Faylni o'qib bo'lmadi."_q);
			}
			done(reply);
		} else if (crl::now() - wait->started > kFileWait) {
			wait->timer.cancel();
			auto reply = result;
			reply.insert(
				u"error"_q,
				u"APK fayli hali yuklanmagan (ulanish sekin bo'lishi "
				"mumkin). Birozdan keyin qayta so'rang."_q);
			done(reply);
		}
	};
	wait->timer.setCallback(check);
	wait->timer.callEach(500);
}

void DoDeleteChat(
		const StatePtr &state,
		const QString &ref,
		bool confirm,
		Done done) {
	const auto session = state->session;
	const auto peer = ByRef(session, ref);
	if (!peer) {
		done(Error(u"Chat topilmadi."_q));
		return;
	}
	const auto title = NameOf(peer);
	if (!confirm) {
		done(QJsonObject{
			{ u"needs_confirmation"_q, true },
			{ u"chat_title"_q, title },
			{ u"chat_ref"_q, ref },
		});
		return;
	}
	// A second, local confirmation: the chat is only deleted if the person
	// at this computer agrees (nothing happens if nobody answers).
	const auto answered = std::make_shared<bool>(false);
	const auto answer = [=](bool yes) {
		if (std::exchange(*answered, true)) {
			return;
		}
		if (yes) {
			session->api().deleteConversation(peer, false);
		}
		done(QJsonObject{
			{ u"deleted"_q, yes },
			{ u"chat_title"_q, title },
			{ u"declined"_q, !yes },
		});
	};
	Ui::show(Ui::MakeConfirmBox({
		.text = u"Paradoks AI \"%1\" chatini o'chirishni so'rayapti "
			"(faqat sizdan, boshqa tomonda qoladi). Ruxsat berasizmi?"_q
			.arg(title),
		.confirmed = [=](Fn<void()> &&close) {
			close();
			answer(true);
		},
		.cancelled = [=](Fn<void()> &&close) {
			close();
			answer(false);
		},
		.confirmText = rpl::single(u"O'chirish"_q),
	}));
	base::call_delayed(kConfirmTimeout, session, [=] { answer(false); });
}

void Execute(
		const StatePtr &state,
		const QString &action,
		const QJsonObject &params,
		Done done) {
	const auto session = state->session;
	if (action == u"send_message"_q) {
		done(DoSendMessage(
			session,
			params.value(u"target"_q).toString(),
			params.value(u"text"_q).toString()));
	} else if (action == u"list_unread"_q) {
		done(DoListUnread(session));
	} else if (action == u"find_apk_from_contact"_q) {
		DoFindApk(state, params.value(u"contact_hint"_q).toString(), done);
	} else if (action == u"delete_chat"_q) {
		DoDeleteChat(
			state,
			params.value(u"chat_ref"_q).toString(),
			params.value(u"confirm"_q).toBool(false),
			done);
	} else if (action == u"post_last_video_story"_q) {
		done(Error(u"Windows ilovasida bu amal hali qo'llab-quvvatlanmaydi."_q));
	} else {
		done(Error(u"Noma'lum amal: "_q + action));
	}
}

// ---- server loop ----

void Schedule(const StatePtr &state, crl::time delay);

void PostResult(
		const StatePtr &state,
		const QString &jobId,
		const QJsonObject &result) {
	auto request = MakeRequest(u"/device/result"_q);
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json; charset=utf-8"_q);
	const auto body = QJsonDocument(QJsonObject{
		{ u"job_id"_q, jobId },
		{ u"result"_q, result },
	}).toJson(QJsonDocument::Compact);
	const auto reply = Network()->post(request, body);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		Schedule(state, kNextJobDelay);
	});
}

void Poll(const StatePtr &state) {
	const auto reply = Network()->get(MakeRequest(u"/device/poll"_q));
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto data = reply->readAll();
		const auto failed = (reply->error() != QNetworkReply::NoError);
		reply->deleteLater();
		if (state->dead) {
			return;
		} else if (status == 204) {
			Schedule(state, kPollInterval);
			return;
		} else if (failed || status != 200) {
			Schedule(state, kErrorBackoff);
			return;
		}
		const auto job = QJsonDocument::fromJson(data).object();
		const auto jobId = job.value(u"job_id"_q).toString();
		const auto action = job.value(u"action"_q).toString();
		if (jobId.isEmpty() || action.isEmpty()) {
			Schedule(state, kErrorBackoff);
			return;
		}
		Debug::Log(u"agent: job %1"_q.arg(action));
		Execute(state, action, job.value(u"params"_q).toObject(), [=](
				QJsonObject result) {
			if (state->dead) {
				return;
			}
			PostResult(state, jobId, result);
		});
	});
}

// ---- pairing ----

void SendPairMessage(const StatePtr &state, not_null<UserData*> bot) {
	const auto session = state->session;
	const auto text = QString(QChar(0x27E6))
		+ u"device-pair:"_q
		+ DeviceToken()
		+ QString(QChar(0x27E7));
	const auto history = session->data().history(bot);
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = TextWithTags{ text };
	session->api().sendMessage(std::move(message));
	SetIntSetting(u"ai/device_paired"_q, 1);
	// The message is only a handshake: remove it so it never stays in the
	// chat.
	base::call_delayed(5000, session, [=] {
		auto ids = MessageIdsList();
		ForEachLoadedItem(history, [&](not_null<HistoryItem*> item) {
			if (item->out() && item->originalText().text == text) {
				ids.push_back(item->fullId());
			}
		});
		if (!ids.empty()) {
			session->data().histories().deleteMessages(ids, true);
		}
	});
}

void Pair(const StatePtr &state) {
	state->pairSent = true;
	const auto session = state->session;
	const auto send = [=](PeerData *peer) {
		const auto bot = peer ? peer->asUser() : nullptr;
		if (!bot || !bot->isBot() || state->dead) {
			state->pairSent = false; // Try again later.
			return;
		}
		SendPairMessage(state, bot);
	};
	if (const auto peer = session->data().peerByUsername(BotUsername())) {
		send(peer);
		return;
	}
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(BotUsername()),
		MTPstring()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		send(session->data().peerLoaded(peerFromMTP(data.vpeer())));
	}).fail([=] {
		state->pairSent = false;
	}).send();
}

void Tick(const StatePtr &state) {
	if (!Active(state)) {
		state->pairSent = false;
		Schedule(state, kPollInterval);
		return;
	}
	if (!state->pairSent && !Paired()) {
		Pair(state);
	}
	Poll(state);
}

void Schedule(const StatePtr &state, crl::time delay) {
	if (!state->dead) {
		state->timer.callOnce(delay);
	}
}

} // namespace

QString ServerUrl() {
	auto url = StringSetting(u"ai/server"_q).trimmed();
	while (url.endsWith('/')) {
		url.chop(1);
	}
	// Only encrypted addresses: the device token travels in a header.
	return url.startsWith(u"https://"_q) ? url : QString();
}

void SetServerUrl(const QString &url) {
	SetStringSetting(u"ai/server"_q, url.trimmed());
	SetIntSetting(u"ai/device_paired"_q, 0);
}

bool Paired() {
	return IntSetting(u"ai/device_paired"_q, 0) != 0;
}

void StartForSession(not_null<Main::Session*> session) {
	const auto state = std::make_shared<State>(session);
	session->lifetime().add([=] {
		state->dead = true;
		state->timer.cancel();
	});
	state->timer.setCallback([=] { Tick(state); });
	state->timer.callOnce(3000);
}

} // namespace Paradoks::Agent
