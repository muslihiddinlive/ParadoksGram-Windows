/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_crash.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_report.h"
#include "paradoks/paradoks_update.h"
#include "base/call_delayed.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings.h"
#include "boxes/abstract_box.h" // Ui::show().
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"

#include <algorithm>
#include <utility>

#include <QtCore/QFile>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Paradoks::Crash {
namespace {

constexpr auto kMaxText = 6000;

[[nodiscard]] QString ReportPath() {
	return cWorkingDir() + u"tdata/paradoks_crash.txt"_q;
}

#ifdef Q_OS_WIN

wchar_t PathBuffer[1024];
int BuildNumber = 0;
volatile LONG Entered = 0;

struct Writer {
	char data[4096];
	int size = 0;

	void put(const char *text) {
		while (*text && size < int(sizeof(data)) - 1) {
			data[size++] = *text++;
		}
	}
	void hex(unsigned long long value) {
		static const char digits[] = "0123456789ABCDEF";
		char buffer[17];
		auto length = 0;
		do {
			buffer[length++] = digits[value & 0xF];
			value >>= 4;
		} while (value);
		put("0x");
		while (length) {
			if (size < int(sizeof(data)) - 1) {
				data[size++] = buffer[--length];
			} else {
				break;
			}
		}
	}
	void decimal(int value) {
		char buffer[16];
		auto length = 0;
		if (value <= 0) {
			put("0");
			return;
		}
		while (value) {
			buffer[length++] = char('0' + (value % 10));
			value /= 10;
		}
		while (length) {
			if (size < int(sizeof(data)) - 1) {
				data[size++] = buffer[--length];
			} else {
				break;
			}
		}
	}
	void address(const void *pointer) {
		HMODULE module = nullptr;
		if (GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
					| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)pointer,
				&module) && module) {
			char name[MAX_PATH] = { 0 };
			GetModuleFileNameA(module, name, MAX_PATH);
			auto base = name;
			for (auto p = name; *p; ++p) {
				if (*p == '\\' || *p == '/') {
					base = p + 1;
				}
			}
			put(base);
			put("+");
			hex((unsigned long long)((const char*)pointer - (const char*)module));
		} else {
			hex((unsigned long long)pointer);
		}
	}
};

LONG WINAPI Filter(EXCEPTION_POINTERS *info) {
	if (InterlockedExchange(&Entered, 1)) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	Writer w;
	w.put("ParadoksGram V");
	w.decimal(BuildNumber);
	w.put("\nException code: ");
	const auto record = info ? info->ExceptionRecord : nullptr;
	w.hex(record ? (unsigned long long)record->ExceptionCode : 0ULL);
	w.put("\nAddress: ");
	w.address(record ? record->ExceptionAddress : nullptr);
	if (record
		&& record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
		&& record->NumberParameters >= 2) {
		w.put(record->ExceptionInformation[0] == 0
			? "\nAccess violation: read of "
			: record->ExceptionInformation[0] == 1
			? "\nAccess violation: write to "
			: "\nAccess violation: execute at ");
		w.hex((unsigned long long)record->ExceptionInformation[1]);
	}
	SYSTEMTIME time;
	GetSystemTime(&time);
	w.put("\nTime (UTC): ");
	w.decimal(time.wYear);
	w.put("-");
	w.decimal(time.wMonth);
	w.put("-");
	w.decimal(time.wDay);
	w.put(" ");
	w.decimal(time.wHour);
	w.put(":");
	w.decimal(time.wMinute);
	w.put(":");
	w.decimal(time.wSecond);
	w.put("\nThread: ");
	w.decimal(int(GetCurrentThreadId()));
	w.put("\nStack:\n");
	void *frames[28];
	const auto count = int(CaptureStackBackTrace(0, 28, frames, nullptr));
	for (auto i = 0; i != count; ++i) {
		w.put("  ");
		w.address(frames[i]);
		w.put("\n");
	}
	const auto file = CreateFileW(
		PathBuffer,
		GENERIC_WRITE,
		0,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file != INVALID_HANDLE_VALUE) {
		auto written = DWORD(0);
		WriteFile(file, w.data, DWORD(w.size), &written, nullptr);
		CloseHandle(file);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

#endif // Q_OS_WIN

void CrashBox(not_null<Ui::GenericBox*> box, QString text) {
	box->setTitle(rpl::single(u"ParadoksGram: oxirgi xato"_q));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Oldingi ishga tushirishda ilova kutilmaganda yopilgan. "
		"Quyidagi matnni nusxalab dasturchiga yuboring:\n\n"_q + text,
		st::boxLabel));
	box->addButton(rpl::single(u"Nusxa olish"_q), [=] {
		QGuiApplication::clipboard()->setText(text);
		box->closeBox();
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void Install() {
#ifdef Q_OS_WIN
	BuildNumber = Update::CurrentBuild();
	const auto path = ReportPath().toStdWString();
	const auto length = std::min(
		int(path.size()),
		int(sizeof(PathBuffer) / sizeof(wchar_t)) - 1);
	for (auto i = 0; i != length; ++i) {
		PathBuffer[i] = path[i];
	}
	PathBuffer[length] = 0;
	SetUnhandledExceptionFilter(Filter);
#endif // Q_OS_WIN
}

void ShowLastIfAny(not_null<Main::Session*> session) {
	static auto shown = false;
	if (std::exchange(shown, true)) {
		return;
	}
	auto file = QFile(ReportPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	auto text = QString::fromUtf8(file.read(kMaxText * 2));
	file.close();
	QFile::remove(ReportPath());
	if (text.trimmed().isEmpty()) {
		return;
	} else if (text.size() > kMaxText) {
		text = text.left(kMaxText) + u"\n..."_q;
	}
	const auto weak = base::make_weak(session);
	base::call_delayed(3000, session, [=] {
		if (!Enabled(Flag::ErrorReports)) {
			Ui::show(Box(CrashBox, text));
			return;
		}
		// Sent by itself; the box is only a fallback if sending fails.
		Report::Send(weak.get(), u"xato (crash)"_q, text, [=](bool ok) {
			if (!ok) {
				Ui::show(Box(CrashBox, text));
			}
		});
	});
}

} // namespace Paradoks::Crash
