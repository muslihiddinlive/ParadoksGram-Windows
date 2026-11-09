# ParadoksGram Windows o'zgarishlar tarixi

Versiya formati Telegram kabi: `MAJOR.MINOR.PATCH`, qavs ichida build raqami (masalan `1.7.0 (V54)`).

- **MAJOR (X.0.0):** ilovaning yuzi yoki asosi o'zgardi.
- **MINOR (1.X.0):** yangi funksiya qo'shildi.
- **PATCH (1.1.X):** faqat xato tuzatildi.

Versiya raqami `VERSION` faylida turadi. Har versiya uchun foydalanuvchiga ko'rinadigan matn `changelog/<versiya>.md` fayliga yoziladi: CI shuni reliz matniga qo'yadi. Commit xabari reliz matniga tushmaydi.

## 1.43.3 (9-noyabr)
Inline tugmalar liquid glass, tugmalarda o'z rangimiz

## 1.43.2 (9-noyabr)
Ulanish hisobotlari: bir uzilish uchun bitta hisobot

## 1.43.1 (9-noyabr)
Liquid glass sinov maydoni foni: faqat sharpa logosi

## 1.43.0 (8-noyabr)
Liquid glass v2: rangsiz toza shisha, suv sinishi, tugma, o'chirilgan xabar

## 1.42.0 (8-noyabr)
Mini-app klonlash: o'z yoki soxta initData bilan

## 1.41.0 (8-noyabr)
Paradoks AI butunlay olib tashlandi; shisha sinov maydoniga logo qo'yildi

## 1.40.0 (8-noyabr)
Liquid glass: jonli sinov maydoni va yaxshilangan sinish effekti

## 1.39.2 (8-noyabr)
O'chirilgan media: baza nusxasidan yuklash eski xato holatga urilmaydi

## 1.39.1 (8-noyabr)
Media'ni oldindan saqlash endi standart o'chiq

## 1.39.0 (7-noyabr)
Asos Telegram Desktop 7.3.5 ga yangilandi
Yangi:
- Telegram Desktop 7.3.1 dan 7.3.5 gacha barcha yangiliklar va tuzatishlar qo'shildi, ParadoksGram funksiyalari saqlandi.
- Xabar pufagidagi vaqt soniyalar bilan ko'rinadi (masalan 11:52:37).
- "Tahrirlar tarixi" xabar menyusida har bir xabarda turadi, oynada "Hammasini nusxalash" tugmasi bor.
- Chatni klon qilish yopiq (havolasiz) guruh va kanallarda ham chiqadi.
Tuzatildi:
- Bir necha kompilyatsiya xatolari va Qt 6 bilan bog'lash xatosi.

## 1.38.0 (7-noyabr)
O'chirilgan media: baza nusxasi serverdan o'zi so'raladi

## 1.37.0 (7-noyabr)
O'chirilgan xabar mediasi yuklash tugmasi bilan bazadan olinadi

## 1.36.0 (7-noyabr)
Kelgan xabarlarga ham liquid glass, media oldindan saqlash hajmi

## 1.35.0 (7-noyabr)
Liquid glass: rangsiz, sozlanadigan

## 1.34.0 (7-noyabr)
Xabar orqa foni: liquid glass

## 1.33.0 (6-noyabr)
Mini-app menyusida "Mini-appni nusxalash" tugmasi

## 1.32.0 (6-noyabr)
Ghost mode: online holati qayta-qayta oflayn qilinadi

## 1.31.0 (6-noyabr)
Mini-appni bitta HTML qilib klonlash

## 1.30.0 (6-noyabr)
Yuklash rejimi: avto

## 1.29.0 (6-noyabr)
Yuborilgan rasmlardan metadata avtomatik tozalanadi

## 1.28.0 (5-noyabr)
Stiker saqlash: shu stiker yoki butun to'plam

## 1.27.0 (6-noyabr)
Metadata (EXIF) ko'rgich
Yangi:
- Rasm fayliga o'ng tugma bosib "Metadata (EXIF) ni ko'rish": qurilma modeli, sana, dastur va (o'chirilmagan bo'lsa) GPS joylashuv ko'rinadi. Telegram rasm sifatida yuborilganda metamalumotni o'chiradi, fayl sifatida yuborilganda qoladi.

## 1.26.0 (6-noyabr)
Nashriyotchi nomi
O'zgardi:
- Fayl va o'rnatuvchidagi nashriyotchi nomi "ParadoksGram Community" bo'ldi.

## 1.25.0 (6-noyabr)
Mini-app nusxasi Saved Messages ga
O'zgardi:
- Mini-app sahifa nusxasi endi ZIP bo'lib Saved Messages ga yuboriladi. Hajm limiti 1.5 GB.

## 1.24.0 (6-noyabr)
Mini-app sahifasini saqlash
Yangi:
- Mini-app dasturchi panelida yangi "Nusxa" bo'limi: ochiq mini-appning ochiq qismi (HTML, CSS, skriptlar, rasmlar) papkaga saqlanadi va offlayn ochiladi. Shaxsiy ma'lumot (initData, cookie) yuborilmaydi.

## 1.23.0 (6-noyabr)
Akkauntlar soni 16 taga yetdi
O'zgardi:
- Bir vaqtda 16 tagacha akkaunt qo'shish mumkin (avval 3 ta edi), Android bilan bir xil. Premium talab qilinmaydi.

## 1.22.0 (5-noyabr)
Xususiyatlari oynasi: stiker, GIF va custom emoji ID'lari

## 1.21.0 (5-noyabr)
Peer ID to'liq, yuklash rejimi
O'zgardi:
- Profildagi ID endi to'liq ko'rinadi: kanal va superguruhlar uchun -100 bilan boshlanadi (masalan -1002144293154), oddiy guruhlar uchun - bilan. Bosganda shu to'liq ID nusxalanadi.
Yangi:
- Yuklash rejimi (Sozlamalar → ParadoksGram → Yuklash tezligi): oddiy, tez va sekin internet uchun. Sekin internetda parallel oqimlar soni 32 gacha ko'payadi, har oqimga kamroq bo'lak beriladi.

## 1.20.0 (5-noyabr)
Tushunarli radar, yashirin chatlar oynasi, hato hisoboti, story'da musiqa
Yangi:
- Yashirin chatlar endi alohida oynada ochiladi: avatar, nom va oxirgi xabar bilan, "Chiqarish" tugmasi bor.
- Yashirin chatlar uchun birinchi marta himoya o'rnatganda PIN yoki chizma (nuqtalarni ulab chizish) tanlanadi. Keyin Sozlamalar → Maxfiylik → "Chat qulfi" dan o'zgartirish mumkin.
- Hato haqida xabar yozish oynasi: nima bo'lganini yozish, rasm, skrinshot yoki fayl biriktirish mumkin.
- ParadoksGram bo'limida qidiruv: funksiya nomi yoki tavsifi bo'yicha topadi.
- Story: rasm ustiga musiqa qo'yish (qaysi soniyadan boshlanishi, uzunligi va balandligi tanlanadi).
O'zgardi:
- O'chirilgan va tahrirlangan xabarlar oynalari qayta ishlandi: har xabar alohida kartochkada, kim yozgani, qachon yozilgani va qachon o'chirilgani aniq ko'rsatiladi. "Oldin" va "Hozir" matnlari ajratilgan.
- Ovoz effektini tanlash endi bosganda aylanmaydi, variantlari bor chiroyli oynada ochiladi.
- Mini-app proxy olib tashlandi.
Tuzatildi:
- Jonli efir boshlashda "STORY_LIVE_ALREADY" xatosi: endi oldingi efirni tugatish yoki uning kalitini olish taklif qilinadi.

## 1.19.0 (4-noyabr)
Mini-app dasturchilari uchun: bridge jurnali, initData, DevTools

## 1.18.0 (4-noyabr)
Mini-applar uchun proxy (IP almashtirish)
## 1.17.3 (4-noyabr)
Chatni klonlash tezlashtirildi

## 1.17.2 (4-noyabr)
"Chat hisoblagichlari" oynasi Android'dagidek qayta chizildi

## 1.17.1 (3-noyabr)
Jonli efir: CAPTION_NOT_EMPTY xatosi va bepul akkauntlarda Stars narxi tuzatildi

## 1.17.0 (4-noyabr)
Yon menyuda "Qo'llab-quvvatlash guruhi"

## 1.16.0 (3-noyabr)
Chat Settings ichida ikonka almashtirish va o'zbek tiliga o'tish

## 1.15.0 (3-noyabr)
ParadoksGram sozlamalari Telegram uslubida: logo, ikonkali qatorlar, bo'limlar

## 1.14.0 (3-noyabr)
Ilova ichida fayl ko'rgich (Word, Excel, PowerPoint, OpenDocument, matn)

## 1.13.0 (3-noyabr)
Jild menyusi (sukut, odatiy jild), hisoblagich ro'yxatlari, shaxsiy havola, reklama va qidiruv tumblerlari

## 1.12.1 (2-noyabr)
Kirish ekranida Telegram samolyoti o'rniga ParadoksGram arvohi

## 1.12.0 (1-noyabr)
Xabar menyusida "Xabar xususiyatlari"

## 1.11.2 (1-noyabr)
Video story yuklash xatosi tuzatildi

## 1.11.1 (1-noyabr)
Build raqami sozlamalar va yon paneldan olib tashlandi

## 1.11.0 (1-noyabr)
Wallet mini ilovasi, do'stlarni taklif qilish, onlayn kontaktlar soni

## 1.10.1 (1-noyabr)
Yon panelga Papkalar, Dizayn, Mavzular qo'shildi; panelda ParadoksGram versiyasi

## 1.10.0 (1-noyabr)
15 ta yangi ilova ikonkasi (jami 19)

## 1.9.0 (31-oktabr)
Ikonka tanlash oynasi Telegram uslubida (to'r, ✓ belgisi)

## 1.8.1 (31-oktabr)
ParadoksGram sozlamalari yon panelda va Sozlamalar ro'yxatida

## 1.8.0 (31-oktabr)
Yashirin chatlar uchun chizma bilan kirish (PIN bilan birga ikki usul)

## 1.7.0 (31-oktabr)
Android uslubidagi PIN ekrani (yashirin chatlar), chat hisoblagichlari, sozlamalarni zaxiralash, Ghost jadval vaqti, tezroq klon (foiz bilan)

## 1.6.0 (30-oktabr)
Chatni fonda klon qilish va "Klonlangan chatlar" ro'yxati, Broadcast (ko'p chatga xabar), Paradoks AI buyruqlari (kompyuterda amal), Live story (RTMP, Stars bilan)

## 1.5.0 (29-oktabr)
Yuklashlar: 1 MB bo'laklar va 32 so'rovgacha, uzilgan yuklashni davom ettirish, dastur yopilganda fonda tugatish; xatolik hisobotlari BuildNoticer'ga

## 1.4.0 (28-oktabr)
Paradoks AI oynasi (@ParadoksAiBot): sessiyalar, tarix, matn va fayl yuborish

## 1.3.0 (25–27-oktabr)
O'chirilgan va tahrirlangan xabarlar radari, so'z farqi, stiker ID'lari, o'chirilgan xabar bildirishnomalari, media oldindan saqlash va ma'lumotlar bazasi, jonli efirni yozib olish, Ghost online muzlatish, o'zbek tili, diagnostika jurnali, istalgan video/audio formatdagi story

## 1.2.0 (23–24-oktabr)
Fonda parallel yangilanish, yuklash tezligi hisoblagichi, 16 yuklash seansi, rasmli va videoli story qo'yish, o'rnatuvchi (installer) tuzatildi

## 1.1.0 (19–22-oktabr)
Yashirin chatlar va chat PIN qulfi, ovoz effektlari, ilova ikonkalari almashtirgichi, Ghost tez yoqish va shortcut, o'zining ma'lumotlar papkasi va ilova ID'si, avto-yangilanish

## 1.0.0 (13–18-oktabr)
Windows x64 build, Ghost rejimi, o'chirilgan xabarlarni saqlash, tahrirlar tarixi, sozlamalar oynasi
