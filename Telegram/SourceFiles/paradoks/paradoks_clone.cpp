/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_clone.h"

#include "api/api_updates.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "settings.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Paradoks::Clone {
namespace {

constexpr auto kPageLimit = 100;
constexpr auto kForwardChunk = 100;
constexpr auto kChunkPause = crl::time(500);
constexpr auto kHistoryPause = crl::time(120);

struct Record {
	qint64 id = 0; // Creation time in ms, also the key.
	uint64 target = 0;
	QString source;
	QString title;
	QString status; // running, done, stopped, error
	QString note;
	int forwarded = 0;
	int failed = 0;
	int total = 0;
};

[[nodiscard]] QString RecordsPath() {
	return cWorkingDir() + u"tdata/paradoks_clones.json"_q;
}

[[nodiscard]] std::vector<Record> LoadRecords() {
	auto result = std::vector<Record>();
	auto file = QFile(RecordsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
		const auto o = value.toObject();
		result.push_back({
			.id = qint64(o.value(u"id"_q).toString().toLongLong()),
			.target = o.value(u"target"_q).toString().toULongLong(),
			.source = o.value(u"source"_q).toString(),
			.title = o.value(u"title"_q).toString(),
			.status = o.value(u"status"_q).toString(),
			.note = o.value(u"note"_q).toString(),
			.forwarded = o.value(u"forwarded"_q).toInt(),
			.failed = o.value(u"failed"_q).toInt(),
			.total = o.value(u"total"_q).toInt(),
		});
	}
	return result;
}

void SaveRecords(const std::vector<Record> &list) {
	auto array = QJsonArray();
	for (const auto &r : list) {
		array.push_back(QJsonObject{
			{ u"id"_q, QString::number(r.id) },
			{ u"target"_q, QString::number(r.target) },
			{ u"source"_q, r.source },
			{ u"title"_q, r.title },
			{ u"status"_q, r.status },
			{ u"note"_q, r.note },
			{ u"forwarded"_q, r.forwarded },
			{ u"failed"_q, r.failed },
			{ u"total"_q, r.total },
		});
	}
	auto file = QFile(RecordsPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
	}
}

void UpdateRecord(const Record &record) {
	auto list = LoadRecords();
	for (auto &existing : list) {
		if (existing.id == record.id) {
			existing = record;
			SaveRecords(list);
			return;
		}
	}
	list.insert(list.begin(), record);
	if (list.size() > 50) {
		list.resize(50);
	}
	SaveRecords(list);
}

[[nodiscard]] QString StatusText(const Record &r) {
	const auto done = r.forwarded + r.failed;
	const auto counts = u"%1 / %2%3"_q.arg(done).arg(r.total).arg(
		r.total > 0
			? (u" ("_q + QString::number(done * 100 / r.total) + u"%)"_q)
			: QString());
	if (r.status == u"running"_q) {
		return u"davom etmoqda: "_q + counts;
	} else if (r.status == u"done"_q) {
		return u"tayyor: %1 ta xabar"_q.arg(r.forwarded);
	} else if (r.status == u"error"_q) {
		return u"xato: "_q + r.note + u" ("_q + counts + u")"_q;
	}
	return u"to'xtatilgan: "_q + counts;
}

struct Item {
	int id = 0;
	uint64 grouped = 0;
};

struct Job {
	Job(not_null<Main::Session*> session, not_null<PeerData*> source)
	: session(session)
	, source(source) {
	}

	not_null<Main::Session*> session;
	not_null<PeerData*> source;
	QString sourceTitle;
	QString newTitle;
	std::vector<Item> items;
	ChannelData *target = nullptr;
	int forwarded = 0;
	int failed = 0;
	bool cancelled = false;
	Record record;
	rpl::variable<QString> text = QString();
	Fn<void(const QString &)> finished;
	Fn<void(const QString &)> milestone;
	int lastMilestone = 0;
};

using JobPtr = std::shared_ptr<Job>;

// The clone that is running right now (one at a time).
JobPtr Active;

void Save(const JobPtr &job, const QString &status, const QString &note) {
	job->record.status = status;
	job->record.note = note;
	job->record.forwarded = job->forwarded;
	job->record.failed = job->failed;
	job->record.total = int(job->items.size());
	if (job->target) {
		job->record.target = job->target->id.value;
	}
	UpdateRecord(job->record);
}

[[nodiscard]] QString CloneTitle(QString name) {
	const auto suffix = u"_clon"_q;
	if (name.size() + suffix.size() > 128) {
		name = name.left(128 - suffix.size());
	}
	return name + suffix;
}

[[nodiscard]] const QVector<MTPMessage> *MessagesOf(
		const MTPmessages_Messages &result,
		int &count) {
	switch (result.type()) {
	case mtpc_messages_messages: {
		const auto &data = result.c_messages_messages();
		count = data.vmessages().v.size();
		return &data.vmessages().v;
	}
	case mtpc_messages_messagesSlice: {
		const auto &data = result.c_messages_messagesSlice();
		count = data.vcount().v;
		return &data.vmessages().v;
	}
	case mtpc_messages_channelMessages: {
		const auto &data = result.c_messages_channelMessages();
		count = data.vcount().v;
		return &data.vmessages().v;
	}
	}
	return nullptr;
}

void Finish(
		const JobPtr &job,
		const QString &text,
		const QString &status,
		const QString &note = QString()) {
	Save(job, status, note);
	if (Active == job) {
		Active = nullptr;
	}
	if (job->finished) {
		job->finished(text);
	}
}

void Forward(const JobPtr &job, int index);

void Collect(const JobPtr &job, int offsetId, int seen) {
	if (job->cancelled) {
		Finish(job, u"Klon qilish to'xtatildi"_q, u"stopped"_q);
		return;
	}
	job->session->api().request(MTPmessages_GetHistory(
		job->source->input(),
		MTP_int(offsetId),
		MTP_int(0),
		MTP_int(0),
		MTP_int(kPageLimit),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		auto count = 0;
		const auto list = MessagesOf(result, count);
		const auto createGroup = [=] {
			if (job->cancelled) {
				Finish(job, u"Klon qilish to'xtatildi"_q, u"stopped"_q);
				return;
			}
			std::sort(
				job->items.begin(),
				job->items.end(),
				[](const Item &a, const Item &b) { return a.id < b.id; });
			if (job->items.empty()) {
				Finish(job, u"Forward qilinadigan xabar topilmadi"_q, u"error"_q, u"xabar yo'q"_q);
				return;
			}
			job->text = u"Yangi guruh yaratilmoqda..."_q;
			job->session->api().request(MTPchannels_CreateChannel(
				MTP_flags(MTPchannels_CreateChannel::Flag::f_megagroup),
				MTP_string(job->newTitle),
				MTP_string((u"Klon: "_q + job->sourceTitle).left(255)),
				MTPInputGeoPoint(),
				MTPstring(),
				MTP_int(0)
			)).done([=](const MTPUpdates &updates) {
				job->session->api().applyUpdates(updates);
				const auto chats = updates.match([](
						const MTPDupdates &data) {
					return data.vchats().v;
				}, [](const MTPDupdatesCombined &data) {
					return data.vchats().v;
				}, [](const auto &) {
					return QVector<MTPChat>();
				});
				for (const auto &chat : chats) {
					if (chat.type() == mtpc_channel) {
						job->target = job->session->data().channel(
							chat.c_channel().vid().v);
						break;
					}
				}
				if (!job->target) {
					Finish(job, u"Yangi guruh ma'lumoti olinmadi"_q, u"error"_q, u"guruh olinmadi"_q);
					return;
				}
				Save(job, u"running"_q, QString());
				Forward(job, 0);
			}).fail([=](const MTP::Error &error) {
				Finish(job, u"Guruh yaratilmadi: "_q + error.type(), u"error"_q, error.type());
			}).send();
		};
		if (!list || list->isEmpty()) {
			createGroup();
			return;
		}
		auto minId = std::numeric_limits<int>::max();
		for (const auto &message : *list) {
			message.match([&](const MTPDmessage &data) {
				const auto id = data.vid().v;
				minId = std::min(minId, id);
				if (!data.is_noforwards()) {
					job->items.push_back({
						.id = id,
						.grouped = uint64(
							data.vgrouped_id().value_or_empty()),
					});
				}
			}, [&](const MTPDmessageService &data) {
				minId = std::min(minId, data.vid().v);
			}, [&](const MTPDmessageEmpty &data) {
				minId = std::min(minId, data.vid().v);
			});
		}
		const auto total = seen + int(list->size());
		if (minId == std::numeric_limits<int>::max()
			|| (offsetId && minId >= offsetId)) {
			createGroup();
			return;
		}
		job->text = u"Xabarlar o'qilmoqda: "_q + QString::number(total)
			+ (count > total
				? (u" / "_q + QString::number(count))
				: QString());
		base::call_delayed(kHistoryPause, job->session, [=] {
			Collect(job, minId, total);
		});
	}).fail([=](const MTP::Error &error) {
		Finish(job, u"Xabarlarni o'qib bo'lmadi: "_q + error.type(), u"error"_q, error.type());
	}).send();
}

void Forward(const JobPtr &job, int index) {
	const auto total = int(job->items.size());
	if (job->cancelled) {
		Finish(job, u"Klon to'xtatildi: %1 / %2 forward qilindi"_q.arg(
			job->forwarded).arg(total), u"stopped"_q);
		return;
	} else if (index >= total) {
		Finish(job, u"Klon tayyor: %1 ta xabar%2"_q.arg(
			job->forwarded).arg(job->failed
				? (u", %1 tasi o'tkazib yuborildi"_q.arg(job->failed))
				: QString()), u"done"_q);
		return;
	}
	// Chunks of up to 100 messages, never splitting an album.
	auto end = std::min(index + kForwardChunk, total);
	while (end < total
		&& end - 1 > index
		&& job->items[end - 1].grouped != 0
		&& job->items[end - 1].grouped == job->items[end].grouped) {
		--end;
	}
	auto ids = QVector<MTPint>();
	auto randoms = QVector<MTPlong>();
	for (auto i = index; i != end; ++i) {
		ids.push_back(MTP_int(job->items[i].id));
		randoms.push_back(MTP_long(base::RandomValue<uint64>()));
	}
	const auto count = end - index;
	const auto progress = [=] {
		const auto done = job->forwarded + job->failed;
		const auto percent = (total > 0) ? (done * 100 / total) : 0;
		job->text = u"Forward qilinmoqda: %1 / %2 (%3%)"_q.arg(
			done).arg(total).arg(percent);
		if (percent / 10 > job->lastMilestone / 10 && percent < 100) {
			job->lastMilestone = percent;
			if (job->milestone) {
				job->milestone(u"Klon: %1% tayyor (%2 / %3)"_q.arg(
					percent / 10 * 10).arg(done).arg(total));
			}
		}
	};
	job->session->api().request(MTPmessages_ForwardMessages(
		MTP_flags(0),
		job->source->input(),
		MTP_vector<MTPint>(ids),
		MTP_vector<MTPlong>(randoms),
		job->target->input(),
		MTP_int(0),
		MTPInputReplyTo(),
		MTP_int(0),
		MTP_int(0),
		MTP_inputPeerEmpty(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTPint(),
		MTP_long(0),
		MTPSuggestedPost()
	)).done([=](const MTPUpdates &result) {
		job->session->api().applyUpdates(result);
		job->forwarded += count;
		progress();
		Save(job, u"running"_q, QString());
		base::call_delayed(kChunkPause, job->session, [=] {
			Forward(job, end);
		});
	}).fail([=](const MTP::Error &error) {
		const auto type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			const auto seconds = std::max(type.mid(11).toInt(), 1);
			job->text = u"Telegram %1 soniya kutishni so'radi..."_q.arg(
				seconds);
			base::call_delayed(
				crl::time(seconds) * 1000 + 500,
				job->session,
				[=] { Forward(job, index); });
		} else if (type.contains(u"MESSAGE_ID_INVALID"_q)
			|| type.contains(u"MESSAGE_IDS_EMPTY"_q)) {
			job->failed += count;
			progress();
			base::call_delayed(kChunkPause, job->session, [=] {
				Forward(job, end);
			});
		} else {
			Finish(job, u"Forward xatosi: "_q + type, u"error"_q, type);
		}
	}).send();
}

void ListBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Klonlangan chatlar"_q));
	auto records = LoadRecords();
	for (auto &record : records) {
		// A clone that was running when the app closed cannot be running now.
		if (record.status == u"running"_q
			&& !(Active && Active->record.id == record.id)) {
			record.status = u"stopped"_q;
		}
	}
	if (records.empty()) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(u"Hali chat klon qilinmagan."_q),
				st::boxLabel),
			st::boxRowPadding);
	}
	for (const auto &record : records) {
		const auto live = Active && Active->record.id == record.id;
		const auto line = record.source + u"  \u2192  "_q + record.title;
		box->addRow(
			object_ptr<Ui::FlatLabel>(box, rpl::single(line), st::boxLabel),
			st::boxRowPadding);
		auto status = live
			? rpl::producer<QString>(Active->text.value())
			: rpl::producer<QString>(rpl::single(StatusText(record)));
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				std::move(status),
				st::settingsButtonNoIcon),
			style::margins());
		const auto target = record.target;
		button->setClickedCallback([=] {
			const auto peer = controller->session().data().peerLoaded(
				PeerId(target));
			if (peer) {
				controller->showPeerHistory(peer);
				box->closeBox();
			} else {
				controller->showToast(u"Guruh topilmadi (o'chirilgan?)"_q);
			}
		});
	}
	if (Active) {
		box->addLeftButton(rpl::single(u"To'xtatish"_q), [=] {
			if (Active) {
				Active->cancelled = true;
				Active->text = u"To'xtatilmoqda..."_q;
			}
		});
	} else if (!records.empty()) {
		box->addLeftButton(rpl::single(u"Ro'yxatni tozalash"_q), [=] {
			SaveRecords({});
			box->closeBox();
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

bool CanClone(not_null<PeerData*> peer) {
	if (const auto channel = peer->asChannel()) {
		return channel->isPublic()
			&& !channel->isMonoforum()
			&& peer->allowsForwarding();
	} else if (const auto user = peer->asUser()) {
		return !user->isInaccessible() && peer->allowsForwarding();
	}
	return false;
}

void ShowList(not_null<Window::SessionController*> controller) {
	controller->show(Box(ListBox, controller));
}

void Start(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	if (Active) {
		controller->showToast(u"Klon qilish allaqachon davom etmoqda"_q);
		return;
	} else if (!CanClone(peer)) {
		controller->showToast(
			u"Bu chatni klon qilib bo'lmaydi (yopiq yoki saqlash cheklangan)"_q);
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto begin = [=] {
		const auto strong = weak.get();
		if (!strong || Active) {
			return;
		}
		const auto job = std::make_shared<Job>(
			&peer->session(),
			peer);
		job->sourceTitle = peer->name();
		job->newTitle = CloneTitle(job->sourceTitle);
		job->text = u"Boshlanmoqda..."_q;
		job->record = {
			.id = QDateTime::currentMSecsSinceEpoch(),
			.source = job->sourceTitle,
			.title = job->newTitle,
			.status = u"running"_q,
		};
		job->milestone = [=](const QString &text) {
			if (const auto again = weak.get()) {
				again->showToast(text);
			}
		};
		job->finished = [=](const QString &result) {
			if (const auto again = weak.get()) {
				again->showToast(result);
			}
		};
		Active = job;
		UpdateRecord(job->record);
		// No window to keep open: the clone goes on in the background.
		strong->showToast(
			u"Klon qilinmoqda. Fonda davom etadi, holatini Sozlamalar "
			"\u2192 Klonlangan chatlar da ko'rasiz."_q);
		Collect(job, 0, 0);
	};
	controller->show(Ui::MakeConfirmBox({
		.text = u"\"%1\" chatidagi barcha xabarlar (media va fayllar bilan) "
			"yangi yopiq guruhga forward qilinadi. Guruh nomi: %2.\n\n"
			"Katta chatlar uchun uzoq vaqt ketishi mumkin. Jarayon fonda "
			"ketadi: boshqa ishlar bilan shug'ullanishingiz mumkin, lekin "
			"dasturni yopmang."_q.arg(
				peer->name(),
				CloneTitle(peer->name())),
		.confirmed = [=](Fn<void()> &&close) {
			close();
			begin();
		},
		.confirmText = rpl::single(u"Boshlash"_q),
	}));
}

} // namespace Paradoks::Clone
