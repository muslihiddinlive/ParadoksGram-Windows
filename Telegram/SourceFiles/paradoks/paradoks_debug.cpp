/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_debug.h"

#include "settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <mutex>

namespace Paradoks::Debug {
namespace {

constexpr auto kMaxSize = qint64(400) * 1024;
constexpr auto kKeepSize = qint64(100) * 1024;

std::mutex Mutex;
bool Trimmed = false;

} // namespace

QString Path() {
	return cWorkingDir() + u"tdata/paradoks_debug.log"_q;
}

void Log(const QString &text) {
	const auto lock = std::lock_guard(Mutex);
	if (!Trimmed) {
		Trimmed = true;
		auto file = QFile(Path());
		if (file.exists() && file.size() > kMaxSize
			&& file.open(QIODevice::ReadOnly)) {
			file.seek(file.size() - kKeepSize);
			const auto rest = file.readAll();
			file.close();
			if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				file.write(rest);
			}
		}
	}
	auto file = QFile(Path());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		return;
	}
	file.write((QDateTime::currentDateTime().toString(
		u"MM-dd HH:mm:ss "_q) + text + '\n').toUtf8());
}

QString Tail(int maxBytes) {
	const auto lock = std::lock_guard(Mutex);
	auto file = QFile(Path());
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	if (file.size() > maxBytes) {
		file.seek(file.size() - maxBytes);
	}
	return QString::fromUtf8(file.readAll());
}

} // namespace Paradoks::Debug
