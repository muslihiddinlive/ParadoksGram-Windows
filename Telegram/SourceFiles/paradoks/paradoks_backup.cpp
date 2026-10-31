/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_backup.h"

#include "paradoks/paradoks_agent.h"
#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_update.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "mainwindow.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_session_controller.h"

#include <algorithm>

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Paradoks::Backup {
namespace {

constexpr auto kKind = "paradoksgram-settings-backup";
constexpr auto kApp = "paradoksgram-windows";
constexpr auto kVersion = 1;

// Integer settings that belong to the backup (same on both platforms are
// only the flags; numbers are restored only from a Windows file).
struct IntKey {
	const char *key;
	int byDefault;
};
constexpr IntKey kInts[] = {
	{ "ghost_sched_start", 22 * 60 },
	{ "ghost_sched_end", 8 * 60 },
	{ "voice_effect", 0 },
	{ "app_icon", 0 },
	{ "media_days", 0 },
	{ "media_max_mb", 0 },
};

[[nodiscard]] QJsonObject Build() {
	auto prefs = QJsonObject();
	for (auto i = 0; i != int(Flag::kCount); ++i) {
		const auto flag = Flag(i);
		prefs.insert(FlagKey(flag), Enabled(flag));
	}
	for (const auto &item : kInts) {
		const auto key = QString::fromLatin1(item.key);
		prefs.insert(key, IntSetting(key, item.byDefault));
	}
	prefs.insert(u"ai_server"_q, Agent::ServerUrl());
	return QJsonObject{
		{ u"app"_q, QString::fromLatin1(kApp) },
		{ u"kind"_q, QString::fromLatin1(kKind) },
		{ u"version"_q, kVersion },
		{ u"build"_q, Update::CurrentBuild() },
		{ u"exported_at"_q,
			QDateTime::currentDateTime().toString(Qt::ISODate) },
		{ u"prefs"_q, prefs },
	};
}

// Returns how many settings were applied, -1 if the file is not a backup.
[[nodiscard]] int Apply(const QByteArray &data, int &skipped) {
	auto error = QJsonParseError();
	const auto root = QJsonDocument::fromJson(data, &error).object();
	if (error.error != QJsonParseError::NoError
		|| root.value(u"kind"_q).toString() != QLatin1String(kKind)
		|| !root.value(u"prefs"_q).isObject()) {
		return -1;
	}
	const auto prefs = root.value(u"prefs"_q).toObject();
	const auto fromWindows = (root.value(u"app"_q).toString()
		== QLatin1String(kApp));
	auto applied = 0;
	// Flags: only the keys of this build, and only real booleans.
	for (auto i = 0; i != int(Flag::kCount); ++i) {
		const auto flag = Flag(i);
		const auto value = prefs.value(FlagKey(flag));
		if (value.isBool()) {
			SetEnabled(flag, value.toBool());
			++applied;
		}
	}
	if (fromWindows) {
		for (const auto &item : kInts) {
			const auto key = QString::fromLatin1(item.key);
			const auto value = prefs.value(key);
			if (value.isDouble()) {
				SetIntSetting(key, value.toInt());
				++applied;
			}
		}
		const auto server = prefs.value(u"ai_server"_q);
		if (server.isString()
			&& (server.toString().isEmpty()
				|| server.toString().startsWith(u"https://"_q))) {
			Agent::SetServerUrl(server.toString());
			++applied;
		}
	}
	skipped = std::max(0, prefs.size() - applied);
	return applied;
}

} // namespace

void Export(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	const auto name = u"paradoksgram-sozlamalar-%1.json"_q.arg(
		QDate::currentDate().toString(u"yyyy-MM-dd"_q));
	FileDialog::GetWritePath(
		QPointer<QWidget>(controller->widget().get()),
		u"Sozlamalarni zaxiralash"_q,
		u"JSON (*.json)"_q,
		name,
		crl::guard(controller, [=](QString &&path) {
			auto file = QFile(path);
			const auto ok = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
				&& file.write(QJsonDocument(Build()).toJson(
					QJsonDocument::Indented)) > 0;
			if (const auto strong = weak.get()) {
				strong->showToast(ok
					? u"Sozlamalar saqlandi"_q
					: u"Faylga yozib bo'lmadi"_q);
			}
		}));
}

void Import(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	FileDialog::GetOpenPath(
		QPointer<QWidget>(controller->widget().get()),
		u"Sozlamalar faylini tanlang"_q,
		u"JSON (*.json)"_q,
		crl::guard(controller, [=](FileDialog::OpenResult &&result) {
			const auto strong = weak.get();
			if (!strong || result.paths.isEmpty()) {
				return;
			}
			auto file = QFile(result.paths.front());
			if (!file.open(QIODevice::ReadOnly)) {
				strong->showToast(u"Faylni o'qib bo'lmadi"_q);
				return;
			}
			const auto data = file.read(2 * 1024 * 1024);
			strong->show(Ui::MakeConfirmBox({
				.text = u"Joriy sozlamalar fayldagilar bilan almashtiriladi. "
					"Davom etamizmi?"_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					auto skipped = 0;
					const auto applied = Apply(data, skipped);
					if (const auto again = weak.get()) {
						again->showToast(applied < 0
							? u"Bu ParadoksGram sozlamalar fayli emas"_q
							: u"%1 ta sozlama qo'llandi%2"_q.arg(applied).arg(
								skipped
									? u", %1 tasi o'tkazib yuborildi"_q.arg(
										skipped)
									: QString()));
					}
				},
				.confirmText = rpl::single(u"Tiklash"_q),
			}));
		}));
}

} // namespace Paradoks::Backup
