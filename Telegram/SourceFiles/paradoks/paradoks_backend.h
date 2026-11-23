/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Paradoks::Backend {

// "ParadoksGram Media Backend" (backend/ papkasi, Android repo'sida)
// deploy qilingan server manzili, masalan "https://sizning-app.onrender.com".
// Foydalanuvchi sozlamalardan bir marta kiritadi, keyin shu yerda saqlanadi.
[[nodiscard]] QString Url();
void SetUrl(const QString &url);

// Shu session uchun serverda forward ishga tushirilganmi (oxirgi marta
// muvaffaqiyatli ulanilganidan beri) - faqat lokal belgi, serverdan qayta
// so'ramaydi (buni "Holatni tekshirish" tugmasi qiladi).
[[nodiscard]] bool Connected(not_null<Main::Session*> session);
void CheckStatus(not_null<Main::Session*> session, Fn<void(bool)> done);

// Serverga "media bazasi"ni ulash: QR-login tokenini olib, O'ZIMIZNING
// (allaqachon login qilingan) sessiyamiz orqali auth.acceptLoginToken bilan
// tasdiqlaydi (rasmiy "boshqa qurilmani ulash" mexanizmi - foydalanuvchining
// jonli sessiyasi hech qachon serverga yuborilmaydi), so'ng backend
// tasdiqlaganini bir necha soniyada poll qilib kutadi. Bu ulanish akkountga,
// qaysi qurilma yoki ilovadan (Android, Windows) chaqirilganidan qat'i
// nazar bir xil ishlaydi.
void Connect(
	not_null<Main::Session*> session,
	PeerId targetChatId,
	Fn<void(QString)> progress,
	Fn<void()> done,
	Fn<void()> passwordRequired,
	Fn<void(QString)> fail);

// `passwordRequired` chaqirilgandan keyin bulut parolini yuborish uchun.
void SubmitPassword(
	not_null<Main::Session*> session,
	const QString &password,
	Fn<void()> done,
	Fn<void(QString)> fail);

// Nishon chatni o'zgartiradi (server allaqachon ulangan bo'lsa).
void SetTarget(
	not_null<Main::Session*> session,
	PeerId targetChatId,
	Fn<void(bool)> done);

void Disconnect(not_null<Main::Session*> session, Fn<void(bool)> done);

// O'chirilgan (yoki hali yuklab olinmagan) xabarning backup nusxasini
// SERVERDAN so'raydi - lokal DbMap'da yozuv topilmaganda ishlatiladi
// (masalan forward boshqa qurilmada yoki qurilma o'chiq paytida bo'lgan
// bo'lsa). Topilsa `found(targetChatId, copyMsgId)`, aks holda `notFound()`.
// `sourceChatId` - o'chirilgan xabarning ASL chati (PeerId, Windows ichidagi
// odatdagi shakli); funksiya buni serverning (Telethon) "belgilangan id"
// shakliga o'zi o'giradi, chaqiruvchi tomonda hech narsa o'girish shart emas.
void LookupCopy(
	not_null<Main::Session*> session,
	PeerId sourceChatId,
	int64 msgId,
	Fn<void(PeerId targetChatId, int64 copyMsgId)> found,
	Fn<void()> notFound);

} // namespace Paradoks::Backend
