/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ai.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "core/file_utilities.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/abstract_button.h"
#include "ui/chat/attach/attach_extensions.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace Paradoks::Ai {
namespace {

constexpr auto kWidth = 640;
constexpr auto kHeight = 560;
constexpr auto kHeaderHeight = 44;
constexpr auto kSide = 20;
constexpr auto kContentWidth = kWidth - 2 * kSide;
constexpr auto kDrawerWidth = 270;
constexpr auto kHistoryLimit = 60;
constexpr auto kTopicsLimit = 60;
constexpr auto kWaitTimeout = crl::time(120000);
constexpr auto kRefreshDelay = crl::time(120);

// The palette of the Android AI window (Claude-like).
struct Palette {
	QColor bg;
	QColor surface;
	QColor user;
	QColor text;
	QColor muted;
	QColor border;
	QColor accent;
	QColor sendOff;
	bool dark = true;
};

[[nodiscard]] Palette MakePalette() {
	const auto dark = (st::windowBg->c.lightness() < 128);
	auto result = Palette();
	result.dark = dark;
	if (dark) {
		result.bg = QColor(0x26, 0x26, 0x24);
		result.surface = QColor(0x30, 0x30, 0x2E);
		result.user = QColor(0x3B, 0x3A, 0x37);
		result.text = QColor(0xF0, 0xEE, 0xE6);
		result.muted = QColor(0xA6, 0xA5, 0x9E);
		result.border = QColor(255, 255, 255, 0x26);
		result.sendOff = QColor(0x4A, 0x4A, 0x47);
	} else {
		result.bg = QColor(0xFA, 0xF9, 0xF5);
		result.surface = QColor(0xFF, 0xFF, 0xFF);
		result.user = QColor(0xF0, 0xEE, 0xE6);
		result.text = QColor(0x1F, 0x1E, 0x1D);
		result.muted = QColor(0x73, 0x72, 0x6C);
		result.border = QColor(0, 0, 0, 0x1F);
		result.sendOff = QColor(0xD9, 0xD6, 0xCB);
	}
	result.accent = QColor(0xD9, 0x77, 0x57);
	return result;
}

// Logo: four layers (shell, visor, trim, eyes), tinted with the palette.
[[nodiscard]] QImage Tinted(const QImage &source, QColor color) {
	auto result = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	auto p = QPainter(&result);
	p.setCompositionMode(QPainter::CompositionMode_SourceIn);
	p.fillRect(result.rect(), color);
	p.end();
	return result;
}

[[nodiscard]] QImage Darkened(const QImage &source, int factor) {
	auto result = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	for (auto y = 0; y != result.height(); ++y) {
		const auto line = reinterpret_cast<QRgb*>(result.scanLine(y));
		for (auto x = 0; x != result.width(); ++x) {
			const auto px = line[x];
			line[x] = qRgba(
				qRed(px) * factor / 255,
				qGreen(px) * factor / 255,
				qBlue(px) * factor / 255,
				qAlpha(px));
		}
	}
	return result;
}

[[nodiscard]] QImage MakeLogo(const Palette &pal) {
	const auto shell = QImage(u":/gui/art/paradoks_ai_shell.png"_q);
	const auto visor = QImage(u":/gui/art/paradoks_ai_visor.png"_q);
	const auto trim = QImage(u":/gui/art/paradoks_ai_trim.png"_q);
	const auto eyes = QImage(u":/gui/art/paradoks_ai_eyes.png"_q);
	if (shell.isNull()) {
		return QImage();
	}
	auto result = QImage(
		shell.size(),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	p.drawImage(0, 0, pal.dark ? shell : Darkened(shell, 0x9E));
	if (!visor.isNull()) {
		p.drawImage(0, 0, Tinted(visor, pal.bg));
	}
	if (!trim.isNull()) {
		p.drawImage(0, 0, Tinted(trim, pal.dark
			? QColor(0xFF, 0xFF, 0xFF, 0xE6)
			: QColor(0x3C, 0x46, 0x55, 0x80)));
	}
	if (!eyes.isNull()) {
		p.drawImage(0, 0, Tinted(eyes, pal.text));
	}
	p.end();
	return result;
}

[[nodiscard]] QImage ScaledLogo(const QImage &logo, int size) {
	if (logo.isNull()) {
		return QImage();
	}
	const auto ratio = style::DevicePixelRatio();
	auto result = logo.scaled(
		QSize(size, size) * ratio,
		Qt::KeepAspectRatio,
		Qt::SmoothTransformation);
	result.setDevicePixelRatio(ratio);
	return result;
}

struct Topic {
	MsgId id = 0;
	QString title;
	int top = 0;
};

struct State {
	explicit State(const Palette &palette)
	: pal(palette)
	, textColor(palette.text)
	, mutedColor(palette.muted)
	, surfaceColor(palette.surface) {
		userLabel = st::defaultFlatLabel;
		userLabel.textFg = textColor.color();
		userLabel.style.font = st::boxTextFont;
		botLabel = st::defaultFlatLabel;
		botLabel.textFg = textColor.color();
		botLabel.style.font = st::boxTextFont;
		botLabel.style.lineHeight = 22;
		fieldSt = st::historyComposeField;
		fieldSt.textBg = surfaceColor.color();
		fieldSt.textFg = textColor.color();
		fieldSt.placeholderFg = mutedColor.color();
		fieldSt.placeholderFgActive = mutedColor.color();
		fieldSt.placeholderFgError = mutedColor.color();
	}

	Palette pal;
	style::owned_color textColor;
	style::owned_color mutedColor;
	style::owned_color surfaceColor;
	style::FlatLabel userLabel = st::defaultFlatLabel;
	style::FlatLabel botLabel = st::defaultFlatLabel;
	style::InputField fieldSt = st::historyComposeField;
	QImage logoFull;
	QImage logoBig;
	QImage logoSmall;

	Window::SessionController *controller = nullptr;
	Main::Session *session = nullptr;
	UserData *bot = nullptr;
	// Root of the shown topic, zero for an ordinary (not forum) bot.
	MsgId root = 0;
	// The topic is just reserved, the server creates it with the first message.
	bool creating = false;
	std::vector<MsgId> ids;
	std::vector<Topic> topics;
	// Messages up to this id are hidden ("clear window").
	int64 hiddenBefore = 0;
	QString attachment;
	bool waiting = false;
	int token = 0;
	base::Timer refreshTimer;
	base::Timer waitTimer;
	Fn<void()> close;
	Fn<void()> refreshMessages;
	Fn<void()> refreshSessions;
	Fn<void()> reloadTopics;
	Fn<void()> loadMessages;
	Fn<void()> updateAttachment;
	Fn<void(QString)> send;
	Fn<void(MsgId)> openTopic;
	Fn<void()> newChat;
	Fn<void()> clearWindow;
	Fn<void(bool)> toggleDrawer;
	Fn<void()> updateHeader;
};

// A widget which computes its height from the width through a callback.
class FlowWidget final : public Ui::RpWidget {
public:
	explicit FlowWidget(QWidget *parent) : Ui::RpWidget(parent) {
	}

	void setRelayout(Fn<int(int)> relayout) {
		_relayout = std::move(relayout);
	}

private:
	int resizeGetHeight(int newWidth) override {
		return _relayout ? _relayout(newWidth) : height();
	}

	Fn<int(int)> _relayout;

};

[[nodiscard]] QString BotUsername() {
	const auto value = StringSetting(u"ai/bot"_q);
	return value.isEmpty() ? u"ParadoksAiBot"_q : value;
}

[[nodiscard]] Data::Thread *CurrentThread(not_null<State*> state) {
	const auto history = state->session->data().history(state->bot->id);
	if (!state->root.bare) {
		return history;
	}
	if (const auto forum = state->bot->forum()) {
		if (const auto topic = forum->enforceTopicFor(state->root)) {
			return topic;
		}
	}
	return history;
}

[[nodiscard]] bool InThread(
		not_null<State*> state,
		not_null<HistoryItem*> item) {
	if (item->history()->peer->id != state->bot->id) {
		return false;
	}
	return !state->root.bare || item->inThread(state->root);
}

[[nodiscard]] bool Visible(not_null<HistoryItem*> item) {
	if (item->isService()) {
		return false;
	}
	return !item->originalText().text.isEmpty() || (item->media() != nullptr);
}

void AddId(not_null<State*> state, MsgId id) {
	auto &ids = state->ids;
	if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
		ids.push_back(id);
		std::sort(ids.begin(), ids.end());
	}
}

void ScheduleRefresh(not_null<State*> state) {
	state->refreshTimer.callOnce(kRefreshDelay);
}

void GotReply(not_null<State*> state) {
	state->waiting = false;
	state->waitTimer.cancel();
}

// Takes the messages of a getReplies / getHistory answer.
void ProcessMessages(
		not_null<State*> state,
		const MTPmessages_Messages &result) {
	auto &owner = state->session->data();
	const auto list = result.match([&](
			const MTPDmessages_messagesNotModified &) {
		return QVector<MTPMessage>();
	}, [&](const auto &data) {
		owner.processUsers(data.vusers());
		owner.processChats(data.vchats());
		return data.vmessages().v;
	});
	for (const auto &message : list) {
		const auto item = owner.addNewMessage(
			message,
			MessageFlags(),
			NewMessageType::Existing);
		if (item && InThread(state, item) && Visible(item)) {
			AddId(state, item->id);
		}
	}
}

[[nodiscard]] QColor WithAlpha(QColor color, int alpha) {
	color.setAlpha(alpha);
	return color;
}

// Widgets with custom painting.

[[nodiscard]] Ui::RpWidget *MakeLogo(
		not_null<State*> state,
		QWidget *parent,
		int size) {
	const auto widget = Ui::CreateChild<Ui::RpWidget>(parent);
	widget->resize(size, size);
	widget->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(widget);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		const auto &image = (size > 40) ? state->logoBig : state->logoSmall;
		if (!image.isNull()) {
			p.drawImage(QRect(0, 0, size, size), image);
		}
	}, widget->lifetime());
	return widget;
}

[[nodiscard]] Ui::AbstractButton *MakePill(
		not_null<State*> state,
		QWidget *parent,
		const QString &text,
		int radius,
		bool center,
		Fn<void()> callback) {
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent);
	const auto over = button->lifetime().make_state<bool>(false);
	button->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Enter) {
			*over = true;
			button->update();
		} else if (e->type() == QEvent::Leave) {
			*over = false;
			button->update();
		}
	}, button->lifetime());
	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		p.setRenderHint(QPainter::Antialiasing);
		const auto &pal = state->pal;
		p.setPen(QPen(pal.border, 1.));
		p.setBrush(*over ? pal.user : pal.surface);
		p.drawRoundedRect(
			QRectF(button->rect()).adjusted(0.5, 0.5, -0.5, -0.5),
			radius,
			radius);
		p.setPen(pal.text);
		p.setFont(st::normalFont);
		const auto inner = button->rect().marginsRemoved(
			QMargins(16, 0, 16, 0));
		p.drawText(
			inner,
			Qt::AlignVCenter | (center ? Qt::AlignHCenter : Qt::AlignLeft),
			st::normalFont->elided(text, inner.width()));
	}, button->lifetime());
	button->setClickedCallback(std::move(callback));
	return button;
}

[[nodiscard]] Ui::AbstractButton *MakeIconButton(
		QWidget *parent,
		int size,
		Fn<void(QPainter&, QRect, bool)> paint,
		Fn<void()> callback) {
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent);
	button->resize(size, size);
	const auto over = button->lifetime().make_state<bool>(false);
	button->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Enter) {
			*over = true;
			button->update();
		} else if (e->type() == QEvent::Leave) {
			*over = false;
			button->update();
		}
	}, button->lifetime());
	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		p.setRenderHint(QPainter::Antialiasing);
		paint(p, button->rect(), *over);
	}, button->lifetime());
	button->setClickedCallback(std::move(callback));
	return button;
}

void PaintPlus(QPainter &p, QPointF center, double half, QColor color) {
	auto pen = QPen(color, 2.);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.drawLine(
		QPointF(center.x() - half, center.y()),
		QPointF(center.x() + half, center.y()));
	p.drawLine(
		QPointF(center.x(), center.y() - half),
		QPointF(center.x(), center.y() + half));
}

[[nodiscard]] Ui::AbstractButton *MakeCard(
		not_null<State*> state,
		QWidget *parent,
		const QString &extension,
		const QString &title,
		const QString &subtitle,
		Fn<void()> callback) {
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent);
	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		p.setRenderHint(QPainter::Antialiasing);
		const auto &pal = state->pal;
		p.setPen(QPen(pal.border, 1.));
		p.setBrush(pal.surface);
		p.drawRoundedRect(
			QRectF(button->rect()).adjusted(0.5, 0.5, -0.5, -0.5),
			14,
			14);
		const auto badge = QRect(10, 10, 40, 40);
		p.setPen(Qt::NoPen);
		p.setBrush(pal.accent);
		p.drawRoundedRect(badge, 10, 10);
		p.setPen(Qt::white);
		p.setFont(st::semiboldFont);
		p.drawText(badge, Qt::AlignCenter, extension);
		const auto left = badge.right() + 12;
		const auto available = button->width() - left - 12;
		p.setPen(pal.text);
		p.drawText(
			QRect(left, 10, available, 20),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::semiboldFont->elided(title, available));
		p.setPen(pal.muted);
		p.setFont(st::normalFont);
		p.drawText(
			QRect(left, 30, available, 20),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::normalFont->elided(subtitle, available));
	}, button->lifetime());
	button->setClickedCallback(std::move(callback));
	return button;
}

struct MediaInfo {
	QString extension;
	QString title;
	QString subtitle;
};

[[nodiscard]] std::optional<MediaInfo> MediaOf(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return std::nullopt;
	}
	if (media->photo()) {
		return MediaInfo{
			u"IMG"_q,
			u"Rasm"_q,
			u"Chatda ochish uchun bosing"_q,
		};
	}
	if (const auto document = media->document()) {
		const auto name = document->filename().isEmpty()
			? u"Fayl"_q
			: document->filename();
		auto extension = QFileInfo(name).suffix().toUpper();
		if (extension.isEmpty()) {
			extension = u"FAYL"_q;
		} else if (extension.size() > 4) {
			extension = extension.left(4);
		}
		return MediaInfo{
			extension,
			name,
			Ui::FormatSizeText(document->size),
		};
	}
	return MediaInfo{
		u"MEDIA"_q,
		u"Media"_q,
		u"Chatda ochish uchun bosing"_q,
	};
}

void AddMessage(
		not_null<State*> state,
		not_null<Ui::VerticalLayout*> list,
		not_null<HistoryItem*> item,
		bool showLogo) {
	const auto out = item->out();
	const auto row = list->add(
		object_ptr<FlowWidget>(list),
		style::margins(kSide, out ? 8 : 6, kSide, out ? 8 : 6));
	const auto &text = item->originalText();
	const auto hasText = !text.text.isEmpty();

	Ui::FlatLabel *label = nullptr;
	if (hasText) {
		label = Ui::CreateChild<Ui::FlatLabel>(
			row,
			rpl::single(text),
			out ? state->userLabel : state->botLabel);
		label->setSelectable(true);
	}
	Ui::AbstractButton *card = nullptr;
	if (const auto info = MediaOf(item)) {
		const auto peerId = item->history()->peer->id;
		const auto msgId = item->id;
		const auto controller = state->controller;
		const auto close = state->close;
		card = MakeCard(
			state,
			row,
			info->extension,
			info->title,
			info->subtitle,
			[=] {
				controller->showPeerHistory(
					peerId,
					Window::SectionShow::Way::Forward,
					msgId);
				close();
			});
	}

	if (out) {
		// Right-aligned rounded bubble, up to 82% of the width.
		const auto rect = row->lifetime().make_state<QRect>();
		row->paintRequest(
		) | rpl::on_next([=] {
			if (rect->isEmpty() || !label) {
				return;
			}
			auto p = QPainter(row);
			p.setRenderHint(QPainter::Antialiasing);
			p.setPen(Qt::NoPen);
			p.setBrush(state->pal.user);
			p.drawRoundedRect(QRectF(*rect), 20., 20.);
		}, row->lifetime());
		row->setRelayout([=](int width) {
			auto y = 0;
			if (label) {
				const auto padX = 16;
				const auto padY = 10;
				const auto maxBubble = width * 82 / 100;
				label->resizeToNaturalWidth(maxBubble - 2 * padX);
				const auto bubbleWidth = label->width() + 2 * padX;
				const auto bubbleHeight = label->height() + 2 * padY;
				*rect = QRect(
					width - bubbleWidth,
					0,
					bubbleWidth,
					bubbleHeight);
				label->moveToLeft(rect->x() + padX, padY);
				y = bubbleHeight;
			} else {
				*rect = QRect();
			}
			if (card) {
				const auto cardWidth = std::min(width, 340);
				card->setGeometry(
					width - cardWidth,
					y + (label ? 6 : 0),
					cardWidth,
					60);
				y += (label ? 6 : 0) + 60;
			}
			return y;
		});
	} else {
		Ui::RpWidget *logo = showLogo ? MakeLogo(state, row, 26) : nullptr;
		Ui::AbstractButton *copy = nullptr;
		if (hasText) {
			const auto plain = text.text;
			copy = MakeIconButton(
				row,
				30,
				[=](QPainter &p, QRect rect, bool over) {
					const auto color = over
						? state->pal.text
						: state->pal.muted;
					p.setPen(QPen(color, 1.6));
					p.setBrush(Qt::NoBrush);
					const auto c = rect.center();
					p.drawRoundedRect(
						QRectF(c.x() - 7, c.y() - 3, 10, 11),
						2.5,
						2.5);
					p.drawRoundedRect(
						QRectF(c.x() - 3, c.y() - 7, 10, 11),
						2.5,
						2.5);
				},
				[=] {
					QGuiApplication::clipboard()->setText(plain);
					state->controller->showToast(u"Nusxalandi"_q);
				});
		}
		row->setRelayout([=](int width) {
			auto y = 0;
			if (logo) {
				logo->move(0, 0);
				y += 26 + 8;
			}
			if (label) {
				label->resizeToWidth(width);
				label->move(0, y);
				y += label->height();
			}
			if (card) {
				const auto cardWidth = std::min(width, 340);
				card->setGeometry(0, y + 6, cardWidth, 60);
				y += 6 + 60;
			}
			if (copy) {
				copy->move(-6, y + 2);
				y += 2 + copy->height();
			}
			return y;
		});
	}
	row->resizeToWidth(kContentWidth);
}

void AddTyping(
		not_null<State*> state,
		not_null<Ui::VerticalLayout*> list,
		bool showLogo) {
	const auto row = list->add(
		object_ptr<FlowWidget>(list),
		style::margins(kSide, 6, kSide, 6));
	const auto logo = showLogo ? MakeLogo(state, row, 26) : nullptr;
	const auto dots = Ui::CreateChild<Ui::RpWidget>(row);
	dots->resize(48, 24);
	const auto timer = dots->lifetime().make_state<base::Timer>([=] {
		dots->update();
	});
	timer->callEach(50);
	dots->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(dots);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		const auto now = double(crl::now());
		for (auto i = 0; i != 3; ++i) {
			const auto wave = 0.5 + 0.5 * std::sin(now / 180. - i * 0.9);
			p.setBrush(WithAlpha(
				state->pal.muted,
				int(std::lround(90 + 165 * wave))));
			p.drawEllipse(QPointF(8 + i * 14, 12), 3.5, 3.5);
		}
	}, dots->lifetime());
	row->setRelayout([=](int) {
		auto y = 0;
		if (logo) {
			logo->move(0, 0);
			y += 26 + 4;
		}
		dots->move(0, y);
		return y + dots->height();
	});
	row->resizeToWidth(kContentWidth);
}

void AddEmpty(
		not_null<State*> state,
		not_null<Ui::VerticalLayout*> list) {
	struct Geo {
		int greetingY = 0;
		int titleY = 0;
		int headerY = -1;
	};
	const auto row = list->add(
		object_ptr<FlowWidget>(list),
		style::margins(kSide, 0, kSide, 0));
	const auto geo = row->lifetime().make_state<Geo>();
	const auto logo = MakeLogo(state, row, 104);

	const auto texts = std::vector<QString>{
		u"Bugun nimalar qila olasan?"_q,
		u"Menga qisqa reja tuzib ber"_q,
		u"Matnni o'zbekchaga tarjima qil"_q,
		u"Kod yozishda yordam ber"_q,
	};
	auto chips = std::vector<Ui::AbstractButton*>();
	for (const auto &text : texts) {
		chips.push_back(MakePill(state, row, text, 20, true, [=] {
			state->send(text);
		}));
	}
	auto recent = std::vector<Ui::AbstractButton*>();
	const auto count = std::min(int(state->topics.size()), 3);
	for (auto i = 0; i != count; ++i) {
		const auto id = state->topics[i].id;
		recent.push_back(MakePill(
			state,
			row,
			state->topics[i].title,
			14,
			false,
			[=] { state->openTopic(id); }));
	}

	row->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(row);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);
		const auto width = row->width();
		p.setPen(state->pal.accent);
		p.setFont(st::semiboldFont);
		p.drawText(
			QRect(0, geo->greetingY, width, 22),
			Qt::AlignHCenter | Qt::AlignVCenter,
			u"Salom, men Paradoks AI"_q);
		auto serif = QFont(u"Georgia"_q);
		serif.setStyleHint(QFont::Serif);
		serif.setPixelSize(28);
		p.setFont(serif);
		p.setPen(state->pal.text);
		p.drawText(
			QRect(0, geo->titleY, width, 80),
			Qt::AlignHCenter | Qt::AlignTop,
			u"Bugun nima haqida\ngaplashamiz?"_q);
		if (geo->headerY >= 0) {
			p.setFont(st::semiboldFont);
			p.setPen(state->pal.muted);
			const auto left = (width - std::min(width, 440)) / 2;
			p.drawText(
				QRect(left, geo->headerY, 300, 24),
				Qt::AlignVCenter | Qt::AlignLeft,
				u"So'nggi suhbatlar"_q);
		}
	}, row->lifetime());

	row->setRelayout([=](int width) {
		auto y = 28;
		logo->move((width - logo->width()) / 2, y);
		y += logo->height() + 12;
		geo->greetingY = y;
		y += 26;
		geo->titleY = y;
		y += 76 + 20;

		// Wrapped, centered rows of suggestions.
		const auto gap = 8;
		const auto chipHeight = 40;
		const auto maxRow = std::min(width, 540);
		auto widths = std::vector<int>();
		for (auto i = 0; i != int(chips.size()); ++i) {
			widths.push_back(
				std::min(st::normalFont->width(texts[i]) + 36, maxRow));
		}
		auto index = 0;
		while (index < int(chips.size())) {
			auto end = index;
			auto total = 0;
			while (end < int(chips.size())) {
				const auto next = total + (end > index ? gap : 0) + widths[end];
				if (next > maxRow && end > index) {
					break;
				}
				total = next;
				++end;
			}
			auto x = (width - total) / 2;
			for (auto i = index; i != end; ++i) {
				chips[i]->setGeometry(x, y, widths[i], chipHeight);
				x += widths[i] + gap;
			}
			y += chipHeight + gap;
			index = end;
		}
		if (!recent.empty()) {
			y += 14;
			geo->headerY = y;
			y += 30;
			const auto rowWidth = std::min(width, 440);
			const auto left = (width - rowWidth) / 2;
			for (const auto button : recent) {
				button->setGeometry(left, y, rowWidth, 46);
				y += 46 + gap;
			}
		} else {
			geo->headerY = -1;
		}
		return y + 24;
	});
	row->resizeToWidth(kContentWidth);
}

void AddDrawerRow(
		not_null<State*> state,
		not_null<Ui::VerticalLayout*> list,
		const QString &text,
		bool selected,
		bool accent,
		Fn<void()> callback) {
	const auto row = list->add(
		object_ptr<Ui::AbstractButton>(list),
		style::margins(8, 1, 8, 1));
	row->resize(row->width(), 40);
	const auto over = row->lifetime().make_state<bool>(false);
	row->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Enter) {
			*over = true;
			row->update();
		} else if (e->type() == QEvent::Leave) {
			*over = false;
			row->update();
		}
	}, row->lifetime());
	row->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(row);
		p.setRenderHint(QPainter::Antialiasing);
		const auto &pal = state->pal;
		if (selected || *over) {
			p.setPen(Qt::NoPen);
			p.setBrush(selected ? pal.user : WithAlpha(pal.text, 18));
			p.drawRoundedRect(QRectF(row->rect()), 10., 10.);
		}
		p.setPen(accent ? pal.accent : pal.text);
		p.setFont(st::normalFont);
		const auto inner = row->rect().marginsRemoved(QMargins(14, 0, 14, 0));
		p.drawText(
			inner,
			Qt::AlignVCenter | Qt::AlignLeft,
			st::normalFont->elided(text, inner.width()));
	}, row->lifetime());
	row->setClickedCallback(std::move(callback));
}

void AiBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> bot) {
	box->setTitle(rpl::single(u"Paradoks AI"_q));
	box->setWidth(kWidth);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto session = &controller->session();
	const auto state = box->lifetime().make_state<State>(MakePalette());
	state->controller = controller;
	state->session = session;
	state->bot = bot;
	state->close = [=] { box->closeBox(); };
	state->logoFull = MakeLogo(state->pal);
	state->logoBig = ScaledLogo(state->logoFull, 104);
	state->logoSmall = ScaledLogo(state->logoFull, 26);

	const auto root = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		style::margins());
	root->resize(kWidth, kHeight);
	root->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(root);
		p.fillRect(clip, state->pal.bg);
	}, root->lifetime());

	const auto messagesScroll = Ui::CreateChild<Ui::ScrollArea>(root);
	Ui::VerticalLayout *messages = messagesScroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(messagesScroll));

	// Header: sessions drawer, title of the session, new chat.
	const auto header = Ui::CreateChild<Ui::RpWidget>(root);
	const auto menuButton = MakeIconButton(
		header,
		36,
		[=](QPainter &p, QRect rect, bool over) {
			const auto c = rect.center();
			if (over) {
				p.setPen(Qt::NoPen);
				p.setBrush(WithAlpha(state->pal.text, 18));
				p.drawEllipse(QRectF(rect));
			}
			auto pen = QPen(state->pal.text, 2.);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			for (const auto dy : { -6, 0, 6 }) {
				p.drawLine(c.x() - 9, c.y() + dy, c.x() + 9, c.y() + dy);
			}
		},
		[=] { state->toggleDrawer(true); });
	const auto plusButton = MakeIconButton(
		header,
		36,
		[=](QPainter &p, QRect rect, bool over) {
			if (over) {
				p.setPen(Qt::NoPen);
				p.setBrush(WithAlpha(state->pal.text, 18));
				p.drawEllipse(QRectF(rect));
			}
			PaintPlus(p, rect.center(), 8., state->pal.text);
		},
		[=] { state->newChat(); });
	header->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(header);
		p.setRenderHint(QPainter::TextAntialiasing);
		p.setPen(state->pal.border);
		p.drawLine(0, header->height() - 1, header->width(), header->height() - 1);
		auto title = u"Paradoks AI"_q;
		if (state->creating) {
			title = u"Yangi suhbat"_q;
		} else {
			for (const auto &topic : state->topics) {
				if (topic.id == state->root) {
					title = topic.title;
					break;
				}
			}
		}
		p.setPen(state->pal.text);
		p.setFont(st::semiboldFont);
		const auto available = header->width() - 2 * 56;
		p.drawText(
			QRect(56, 0, available, header->height()),
			Qt::AlignCenter,
			st::semiboldFont->elided(title, available));
	}, header->lifetime());
	state->updateHeader = [=] {
		header->update();
	};

	// Input card.
	const auto card = Ui::CreateChild<Ui::RpWidget>(root);
	card->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(card);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(state->pal.border, 1.));
		p.setBrush(state->pal.surface);
		p.drawRoundedRect(
			QRectF(card->rect()).adjusted(0.5, 0.5, -0.5, -0.5),
			26.,
			26.);
	}, card->lifetime());
	const auto input = Ui::CreateChild<Ui::InputField>(
		card,
		state->fieldSt,
		Ui::InputField::Mode::MultiLine,
		rpl::single(u"Paradoks AI bilan gaplashing"_q));
	input->setMaxHeight(110);
	input->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);

	const auto chip = Ui::CreateChild<Ui::AbstractButton>(card);
	chip->hide();
	chip->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(chip);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(state->pal.user);
		p.drawRoundedRect(QRectF(chip->rect()), 12., 12.);
		p.setPen(state->pal.text);
		p.setFont(st::normalFont);
		const auto inner = chip->rect().marginsRemoved(QMargins(12, 0, 30, 0));
		p.drawText(
			inner,
			Qt::AlignVCenter | Qt::AlignLeft,
			st::normalFont->elided(
				QFileInfo(state->attachment).fileName(),
				inner.width()));
		p.setPen(state->pal.muted);
		p.drawText(
			QRect(chip->width() - 30, 0, 24, chip->height()),
			Qt::AlignCenter,
			QString(QChar(0x2715)));
	}, chip->lifetime());
	chip->setClickedCallback([=] {
		state->attachment.clear();
		state->updateAttachment();
	});

	const auto attachButton = MakeIconButton(
		card,
		36,
		[=](QPainter &p, QRect rect, bool over) {
			p.setPen(QPen(over ? state->pal.text : state->pal.muted, 1.4));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(QRectF(rect).adjusted(1, 1, -1, -1));
			PaintPlus(
				p,
				rect.center(),
				7.,
				over ? state->pal.text : state->pal.muted);
		},
		[=] {
			FileDialog::GetOpenPath(
				root,
				u"Faylni tanlang"_q,
				FileDialog::AllFilesFilter(),
				crl::guard(root, [=](const FileDialog::OpenResult &result) {
					if (result.paths.isEmpty()) {
						return;
					}
					state->attachment = result.paths.front();
					state->updateAttachment();
				}));
		});
	const auto canSend = [=] {
		return !state->attachment.isEmpty()
			|| !input->getLastText().trimmed().isEmpty();
	};
	const auto sendButton = MakeIconButton(
		card,
		36,
		[=](QPainter &p, QRect rect, bool) {
			p.setPen(Qt::NoPen);
			p.setBrush(canSend() ? state->pal.accent : state->pal.sendOff);
			p.drawEllipse(QRectF(rect));
			auto pen = QPen(Qt::white, 2.2);
			pen.setCapStyle(Qt::RoundCap);
			pen.setJoinStyle(Qt::RoundJoin);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			const auto c = rect.center();
			p.drawLine(QPointF(c.x(), c.y() + 7), QPointF(c.x(), c.y() - 7));
			p.drawLine(QPointF(c.x() - 6, c.y() - 1), QPointF(c.x(), c.y() - 7));
			p.drawLine(QPointF(c.x() + 6, c.y() - 1), QPointF(c.x(), c.y() - 7));
		},
		[=] { state->send(input->getLastText()); });
	input->changes(
	) | rpl::on_next([=] {
		sendButton->update();
	}, input->lifetime());

	// Sessions drawer over the whole window.
	const auto scrim = Ui::CreateChild<Ui::RpWidget>(root);
	scrim->hide();
	scrim->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(scrim);
		p.fillRect(clip, QColor(0, 0, 0, 100));
	}, scrim->lifetime());
	scrim->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::MouseButtonPress) {
			state->toggleDrawer(false);
		}
	}, scrim->lifetime());
	const auto drawer = Ui::CreateChild<Ui::RpWidget>(root);
	drawer->hide();
	drawer->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(drawer);
		p.fillRect(drawer->rect(), state->pal.surface);
		p.setPen(state->pal.border);
		p.drawLine(
			drawer->width() - 1,
			0,
			drawer->width() - 1,
			drawer->height());
	}, drawer->lifetime());
	const auto drawerScroll = Ui::CreateChild<Ui::ScrollArea>(drawer);
	Ui::VerticalLayout *drawerList = drawerScroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(drawerScroll));

	state->toggleDrawer = [=](bool shown) {
		scrim->setVisible(shown);
		drawer->setVisible(shown);
		if (shown) {
			scrim->raise();
			drawer->raise();
		}
	};

	const auto layout = [=] {
		const auto width = root->width();
		const auto height = root->height();
		header->setGeometry(0, 0, width, kHeaderHeight);
		menuButton->moveToLeft(10, (kHeaderHeight - menuButton->height()) / 2);
		plusButton->moveToRight(10, (kHeaderHeight - plusButton->height()) / 2);

		const auto margin = 14;
		const auto cardWidth = width - 2 * margin;
		const auto fieldWidth = cardWidth - 36;
		input->resizeToWidth(fieldWidth);
		const auto chipHeight = state->attachment.isEmpty() ? 0 : 34;
		const auto cardHeight = 10
			+ chipHeight
			+ input->height()
			+ 4
			+ 36
			+ 10;
		card->setGeometry(
			margin,
			height - margin - cardHeight,
			cardWidth,
			cardHeight);
		if (chipHeight) {
			chip->setGeometry(18, 10, std::min(fieldWidth, 360), 28);
			chip->show();
		} else {
			chip->hide();
		}
		input->moveToLeft(18, 10 + chipHeight);
		attachButton->moveToLeft(10, cardHeight - 10 - 36);
		sendButton->moveToLeft(cardWidth - 10 - 36, cardHeight - 10 - 36);

		const auto top = kHeaderHeight;
		messagesScroll->setGeometry(
			0,
			top,
			width,
			std::max(card->y() - 8 - top, 60));
		messages->resizeToWidth(width);

		scrim->setGeometry(0, 0, width, height);
		drawer->setGeometry(0, 0, kDrawerWidth, height);
		drawerScroll->setGeometry(0, 8, kDrawerWidth - 1, height - 16);
		drawerList->resizeToWidth(kDrawerWidth - 1);
	};
	rpl::combine(
		root->sizeValue(),
		input->heightValue()
	) | rpl::on_next([=](QSize, int) {
		layout();
	}, root->lifetime());

	// Always show the end of the conversation.
	messages->heightValue(
	) | rpl::on_next([=](int) {
		messagesScroll->scrollToY(messagesScroll->scrollTopMax());
	}, messages->lifetime());

	state->updateAttachment = [=] {
		layout();
		chip->update();
		sendButton->update();
	};

	state->send = [=](QString text) {
		text = text.trimmed();
		const auto attachment = state->attachment;
		if (text.isEmpty() && attachment.isEmpty()) {
			return;
		}
		const auto thread = CurrentThread(state);
		if (!thread) {
			return;
		}
		if (!attachment.isEmpty()) {
			auto files = Storage::PrepareMediaList(
				QStringList(attachment),
				256,
				session->premium());
			if (files.error != Ui::PreparedList::Error::None
				|| files.files.empty()) {
				controller->showToast(u"Faylni yuborib bo'lmadi."_q);
				return;
			}
			files.files.front().caption = TextWithTags{ text };
			auto isImage = false;
			for (const auto &extension : Ui::ImageExtensions()) {
				if (attachment.endsWith(extension, Qt::CaseInsensitive)) {
					isImage = true;
					break;
				}
			}
			session->api().sendFiles(
				std::move(files),
				isImage ? SendMediaType::Photo : SendMediaType::File,
				nullptr,
				Api::SendAction(thread));
			state->attachment.clear();
			state->updateAttachment();
		} else {
			auto message = Api::MessageToSend(Api::SendAction(thread));
			message.textWithTags = TextWithTags{ text };
			session->api().sendMessage(std::move(message));
		}
		input->setText(QString());
		state->waiting = true;
		state->waitTimer.callOnce(kWaitTimeout);
		Debug::Log(u"ai window: message sent"_q);
		ScheduleRefresh(state);
		state->refreshMessages();
	};
	input->submits(
	) | rpl::to_empty | rpl::on_next([=] {
		state->send(input->getLastText());
	}, input->lifetime());

	state->refreshTimer.setCallback([=] {
		state->refreshMessages();
	});
	state->waitTimer.setCallback([=] {
		state->waiting = false;
		state->refreshMessages();
	});

	state->refreshMessages = [=] {
		messages->clear();
		auto shown = 0;
		auto previousOut = true;
		for (const auto id : state->ids) {
			if (id.bare <= state->hiddenBefore) {
				continue;
			}
			const auto item = session->data().message(
				FullMsgId(bot->id, id));
			if (!item || !Visible(item)) {
				continue;
			}
			++shown;
			AddMessage(state, messages, item, previousOut);
			previousOut = item->out();
		}
		if (state->waiting) {
			AddTyping(state, messages, previousOut);
		} else if (!shown) {
			AddEmpty(state, messages);
		}
		messages->resizeToWidth(messagesScroll->width());
	};

	state->openTopic = [=](MsgId id) {
		state->toggleDrawer(false);
		if (!state->creating && state->root == id) {
			return;
		}
		state->root = id;
		state->creating = false;
		state->ids.clear();
		state->hiddenBefore = 0;
		state->waiting = false;
		state->updateHeader();
		state->refreshSessions();
		state->loadMessages();
	};

	state->newChat = [=] {
		state->toggleDrawer(false);
		const auto forum = bot->forum();
		if (!forum) {
			controller->showToast(
				u"Bu bot alohida suhbatlarni qo'llamaydi."_q);
			return;
		}
		const auto topic = forum->reserveNewBotTopic();
		if (!topic) {
			return;
		}
		state->root = topic->rootId();
		state->creating = true;
		state->ids.clear();
		state->hiddenBefore = 0;
		state->waiting = false;
		++state->token;
		state->updateHeader();
		state->refreshMessages();
		state->refreshSessions();
		input->setFocus();
	};

	state->clearWindow = [=] {
		state->toggleDrawer(false);
		state->hiddenBefore = state->ids.empty()
			? 0
			: state->ids.back().bare;
		state->waiting = false;
		state->refreshMessages();
	};

	state->refreshSessions = [=] {
		drawerList->clear();
		AddDrawerRow(
			state,
			drawerList,
			u"+  Yangi suhbat"_q,
			false,
			true,
			[=] { state->newChat(); });
		AddDrawerRow(
			state,
			drawerList,
			u"Suhbatlar"_q,
			false,
			false,
			[] {});
		if (state->creating) {
			AddDrawerRow(
				state,
				drawerList,
				u"Yangi suhbat"_q,
				true,
				false,
				[] {});
		}
		for (const auto &topic : state->topics) {
			const auto selected = !state->creating
				&& (topic.id == state->root);
			const auto id = topic.id;
			AddDrawerRow(
				state,
				drawerList,
				topic.title,
				selected,
				false,
				[=] { state->openTopic(id); });
		}
		AddDrawerRow(
			state,
			drawerList,
			u"Bot chatini ochish"_q,
			false,
			false,
			[=] {
				controller->showPeerHistory(
					bot->id,
					Window::SectionShow::Way::Forward);
				box->closeBox();
			});
		AddDrawerRow(
			state,
			drawerList,
			u"Oynani tozalash"_q,
			false,
			false,
			[=] { state->clearWindow(); });
		drawerList->resizeToWidth(kDrawerWidth - 1);
		state->updateHeader();
	};

	state->loadMessages = [=] {
		const auto token = ++state->token;
		if (state->creating) {
			state->ids.clear();
			state->refreshMessages();
			return;
		}
		const auto done = [=](const MTPmessages_Messages &result) {
			if (token != state->token) {
				return;
			}
			ProcessMessages(state, result);
			state->refreshMessages();
		};
		const auto fail = [=] {
			if (token == state->token) {
				state->refreshMessages();
			}
		};
		if (state->root.bare) {
			session->api().request(MTPmessages_GetReplies(
				bot->input(),
				MTP_int(state->root.bare),
				MTP_int(0), // offset_id
				MTP_int(0), // offset_date
				MTP_int(0), // add_offset
				MTP_int(kHistoryLimit),
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0) // hash
			)).done(done).fail(fail).send();
		} else {
			session->api().request(MTPmessages_GetHistory(
				bot->input(),
				MTP_int(0), // offset_id
				MTP_int(0), // offset_date
				MTP_int(0), // add_offset
				MTP_int(kHistoryLimit),
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0) // hash
			)).done(done).fail(fail).send();
		}
	};

	state->reloadTopics = [=] {
		if (!bot->isForum()) {
			state->refreshSessions();
			return;
		}
		session->api().request(MTPmessages_GetForumTopics(
			MTP_flags(0),
			bot->input(),
			MTPstring(), // q
			MTP_int(0), // offset_date
			MTP_int(0), // offset_id
			MTP_int(0), // offset_topic
			MTP_int(kTopicsLimit)
		)).done([=](const MTPmessages_ForumTopics &result) {
			const auto &data = result.data();
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			auto list = std::vector<Topic>();
			for (const auto &topic : data.vtopics().v) {
				topic.match([&](const MTPDforumTopic &fields) {
					list.push_back({
						.id = MsgId(fields.vid().v),
						.title = fields.vtitle().v,
						.top = fields.vtop_message().v,
					});
				}, [](const auto &) {
				});
			}
			std::sort(list.begin(), list.end(), [](
					const Topic &a,
					const Topic &b) {
				return a.top > b.top;
			});
			state->topics = std::move(list);
			if (!state->root.bare
				&& !state->creating
				&& !state->topics.empty()) {
				state->root = state->topics.front().id;
				state->loadMessages();
			}
			state->refreshSessions();
		}).fail([=] {
			state->refreshSessions();
		}).send();
	};

	// Live updates.
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (item->history()->peer->id != bot->id) {
			return;
		}
		if (state->creating) {
			const auto forum = bot->forum();
			const auto top = item->topicRootId();
			if (forum
				&& top.bare
				&& top != state->root
				&& !forum->creating(top)) {
				// The topic was created by the server.
				state->root = top;
				state->creating = false;
				state->ids.clear();
				state->reloadTopics();
				state->loadMessages();
				return;
			}
		}
		if (!InThread(state, item) || !Visible(item)) {
			return;
		}
		AddId(state, item->id);
		if (!item->out()) {
			GotReply(state);
		}
		ScheduleRefresh(state);
	}, box->lifetime());

	session->data().itemIdChanged(
	) | rpl::on_next([=](Data::Session::IdChange change) {
		if (change.newId.peer != bot->id) {
			return;
		}
		auto &ids = state->ids;
		const auto i = std::find(ids.begin(), ids.end(), change.oldId);
		if (i != ids.end()) {
			*i = change.newId.msg;
			std::sort(ids.begin(), ids.end());
			ScheduleRefresh(state);
		}
	}, box->lifetime());

	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		const auto item = update.item;
		if (item->history()->peer->id != bot->id || !InThread(state, item)) {
			return;
		}
		if (!Visible(item)) {
			return;
		}
		AddId(state, item->id);
		if (!item->out()) {
			GotReply(state);
		}
		ScheduleRefresh(state);
	}, box->lifetime());

	layout();
	state->refreshMessages();
	state->refreshSessions();
	if (bot->isForum()) {
		if (const auto forum = bot->forum()) {
			forum->requestTopics();
		}
		state->reloadTopics();
	} else {
		state->loadMessages();
	}
	input->setFocusFast();
}

} // namespace

void Open(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	const auto name = BotUsername();
	const auto show = [=](not_null<PeerData*> peer) {
		const auto bot = peer->asUser();
		if (!bot || !bot->isBot()) {
			controller->showToast(u"Paradoks AI boti topilmadi."_q);
			return;
		}
		controller->show(Box(AiBox, controller, not_null<UserData*>(bot)));
	};
	if (const auto peer = session->data().peerByUsername(name)) {
		show(peer);
		return;
	}
	controller->showToast(u"Paradoks AI ulanmoqda..."_q);
	const auto weak = base::make_weak(controller);
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(name),
		MTPstring()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(data.vpeer()));
		if (!weak.get()) {
			return;
		}
		if (!peer) {
			weak.get()->showToast(u"Paradoks AI boti topilmadi."_q);
			return;
		}
		show(peer);
	}).fail([=] {
		if (const auto strong = weak.get()) {
			strong->showToast(u"Paradoks AI'ga ulanib bo'lmadi."_q);
		}
	}).send();
}

} // namespace Paradoks::Ai
