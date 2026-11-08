/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_ui.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"
#include "paradoks/paradoks_hotkey.h"
#include "paradoks/paradoks_icon.h"
#include "paradoks/paradoks_privacy.h"
#include "paradoks/paradoks_store.h"
#include "paradoks/paradoks_agent.h"
#include "paradoks/paradoks_backup.h"
#include "paradoks/paradoks_clone.h"
#include "paradoks/paradoks_report.h"
#include "paradoks/paradoks_update.h"
#include "paradoks/paradoks_voice.h"
#include "paradoks/paradoks_webdev.h"
#include "mainwindow.h"
#include "window/window_session_controller.h"
#include "base/unixtime.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/stickers/data_stickers_set.h"
#include "data/data_channel.h"
#include "data/data_user.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_media_types.h"
#include "data/stickers/data_custom_emoji.h"
#include "paradoks/paradoks_media.h"
#include "base/timer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/text/text_entity.h"
#include "window/section_widget.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_cloud_manager.h"
#include "lang/lang_keys.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "settings/settings_common.h"
#include "ui/painter.h"
#include "ui/chat/message_bubble.h"
#include "ui/vertical_list.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/rp_widget.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/animations.h"
#include "core/file_utilities.h"
#include "styles/style_boxes.h"
#include "styles/style_widgets.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <algorithm>
#include <array>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtGui/QClipboard>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QGuiApplication>

namespace Paradoks {
namespace {

constexpr auto kRadarLimit = 100;
constexpr auto kPreviewHeight = 180;

[[nodiscard]] QString FormatDate(TimeId date) {
	return base::unixtime::parse(date).toString(u"dd.MM.yyyy HH:mm"_q);
}

bool LastWasDivider = true;

using Wrap = Ui::SlideWrap<Ui::VerticalLayout>;

// Everything on the settings page is wrapped so the search can hide it.
struct SearchRow {
	not_null<Wrap*> wrap;
	std::shared_ptr<QString> title;
	QString description;
};
struct SearchRegistry {
	std::vector<SearchRow> rows;
	std::vector<not_null<Wrap*>> chrome;
	std::vector<size_t> pending;
};
SearchRegistry *CurrentSearch = nullptr;

[[nodiscard]] not_null<Wrap*> AddWrap(
		not_null<Ui::VerticalLayout*> container) {
	return container->add(object_ptr<Wrap>(
		container,
		object_ptr<Ui::VerticalLayout>(container)));
}

// A chrome block (title, divider, hint) that hides while searching.
[[nodiscard]] not_null<Ui::VerticalLayout*> AddChrome(
		not_null<Ui::VerticalLayout*> container) {
	const auto wrap = AddWrap(container);
	if (CurrentSearch) {
		CurrentSearch->chrome.push_back(wrap);
	}
	return wrap->entity();
}

void AddGap(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(AddChrome(container));
}

// ---- Liquid glass settings: preview and inline controls ----

constexpr auto kGlassHeader = 54;
constexpr auto kGlassInput = 44;
constexpr auto kGlassPad = 12;
constexpr auto kSegmentHeight = 42;
constexpr auto kSwatchHeight = 56;

struct GlassMessage {
	bool out = false;
	QString name;
	QString text;
	QString time;
};

[[nodiscard]] const std::vector<GlassMessage> &GlassMessages() {
	static const auto result = std::vector<GlassMessage>{
		{ false, u"Aziz"_q, u"ParadoksGram'da yangi liquid glass chiqibdi, ko'rdingmi?"_q, u"12:40"_q },
		{ true, QString(), u"Ha! Xabarlar haqiqiy shishaga o'xshab qoldi"_q, u"12:41"_q },
		{ true, QString(), u"Chekkadagi yorug'lik va sinish juda zo'r chiqdi"_q, u"12:41"_q },
		{ false, u"Aziz"_q, u"Fon rang-barang bo'lsa yanayam chiroyli ko'rinadi"_q, u"12:42"_q },
	};
	return result;
}

struct GlassLayout {
	std::vector<QRect> bubbles;
	QRect input;
	int height = 0;
};

[[nodiscard]] GlassLayout LayoutGlassPreview(int width) {
	auto result = GlassLayout();
	const auto metrics = QFontMetrics(st::normalFont->f);
	const auto boldMetrics = QFontMetrics(st::semiboldFont->f);
	const auto maxText = std::max(60, int(width * 0.7) - 28);
	auto y = kGlassHeader + 14;
	for (const auto &message : GlassMessages()) {
		const auto text = metrics.boundingRect(
			QRect(0, 0, maxText, 10000),
			Qt::TextWordWrap,
			message.text);
		const auto nameWidth = message.name.isEmpty()
			? 0
			: boldMetrics.horizontalAdvance(message.name);
		const auto nameHeight = message.name.isEmpty() ? 0 : 17;
		const auto bubbleWidth = std::max({ text.width(), nameWidth, 44 }) + 30;
		const auto bubbleHeight = 9 + nameHeight + text.height() + 15 + 6;
		const auto x = message.out
			? (width - kGlassPad - bubbleWidth)
			: kGlassPad;
		result.bubbles.push_back(QRect(x, y, bubbleWidth, bubbleHeight));
		y += bubbleHeight + 8;
	}
	y += 6;
	result.input = QRect(
		kGlassPad,
		y,
		std::max(0, width - 2 * kGlassPad),
		kGlassInput);
	result.height = y + kGlassInput + kGlassPad;
	return result;
}

// Our logo as a backdrop: the black plate is turned transparent, so only
// the ghost itself stays.
[[nodiscard]] const QImage &GlassGhost() {
	static const auto result = [] {
		auto image = QImage(u":/gui/art/logo_256_no_margin.png"_q)
			.convertToFormat(QImage::Format_ARGB32);
		for (auto y = 0; y != image.height(); ++y) {
			auto line = reinterpret_cast<QRgb*>(image.scanLine(y));
			for (auto x = 0; x != image.width(); ++x) {
				const auto pixel = line[x];
				const auto peak = std::max({
					qRed(pixel),
					qGreen(pixel),
					qBlue(pixel) });
				const auto alpha = std::clamp((peak - 14) * 255 / 56, 0, 255);
				line[x] = qRgba(
					qRed(pixel),
					qGreen(pixel),
					qBlue(pixel),
					std::min(alpha, qAlpha(pixel)));
			}
		}
		return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}();
	return result;
}

[[nodiscard]] const QImage &GlassLogo() {
	static const auto result = QImage(u":/gui/art/logo_256_no_margin.png"_q);
	return result;
}

// Deep gradient with soft glows and the ParadoksGram ghost in the middle.
void PaintGlassWall(QPainter &p, QSize size) {
	auto hq = PainterHighQualityEnabler(p);
	const auto w = double(size.width());
	const auto h = double(size.height());
	auto base = QLinearGradient(QPointF(0, 0), QPointF(w, h));
	base.setColorAt(0., QColor(9, 13, 32));
	base.setColorAt(0.55, QColor(24, 18, 58));
	base.setColorAt(1., QColor(46, 20, 78));
	p.fillRect(QRectF(0, 0, w, h), base);
	struct Glow {
		double x, y, r;
		QColor color;
	};
	const Glow glows[] = {
		{ 0.12, 0.10, 0.55, QColor(40, 190, 255, 150) },
		{ 0.92, 0.32, 0.60, QColor(150, 90, 255, 150) },
		{ 0.18, 0.88, 0.50, QColor(255, 70, 170, 130) },
		{ 0.88, 0.96, 0.38, QColor(255, 170, 60, 100) },
	};
	p.setPen(Qt::NoPen);
	for (const auto &glow : glows) {
		const auto center = QPointF(glow.x * w, glow.y * h);
		const auto radius = glow.r * std::max(w, h);
		auto gradient = QRadialGradient(center, radius);
		auto outer = glow.color;
		outer.setAlpha(0);
		gradient.setColorAt(0., glow.color);
		gradient.setColorAt(1., outer);
		p.setBrush(gradient);
		p.drawEllipse(center, radius, radius);
	}
	const auto &ghost = GlassGhost();
	if (!ghost.isNull()) {
		const auto side = std::min(w * 0.8, h * 0.7);
		const auto center = QPointF(w / 2., h * 0.5);
		auto aura = QRadialGradient(center, side * 0.75);
		aura.setColorAt(0., QColor(190, 200, 255, 70));
		aura.setColorAt(1., QColor(190, 200, 255, 0));
		p.setBrush(aura);
		p.drawEllipse(center, side * 0.75, side * 0.75);
		p.setOpacity(0.9);
		p.drawImage(
			QRectF(
				center.x() - side / 2.,
				center.y() - side / 2.,
				side,
				side),
			ghost);
		p.setOpacity(1.);
	}
}

// The whole mock chat, drawn once per settings/size change and cached.
[[nodiscard]] QImage RenderGlassPreview(QSize size, qreal ratio) {
	auto canvas = QImage(
		QSize(
			int(std::ceil(size.width() * ratio)),
			int(std::ceil(size.height() * ratio))),
		QImage::Format_ARGB32_Premultiplied);
	canvas.setDevicePixelRatio(ratio);
	canvas.fill(Qt::transparent);
	if (size.width() < 120 || size.height() < 120) {
		return canvas;
	}
	auto wall = QImage(size, QImage::Format_RGB32);
	{
		auto wallPainter = QPainter(&wall);
		PaintGlassWall(wallPainter, size);
	}

	auto p = QPainter(&canvas);
	auto hq = PainterHighQualityEnabler(p);
	const auto width = size.width();
	auto frame = QPainterPath();
	frame.addRoundedRect(QRectF(QPointF(), QSizeF(size)), 18., 18.);
	p.setClipPath(frame);
	PaintGlassWall(p, size);

	// Header.
	p.fillRect(QRect(0, 0, width, kGlassHeader), QColor(8, 11, 26, 150));
	p.fillRect(QRect(0, kGlassHeader - 1, width, 1), QColor(255, 255, 255, 28));
	{
		const auto avatar = QRect(14, 9, 36, 36);
		auto circle = QPainterPath();
		circle.addEllipse(avatar);
		p.save();
		p.setClipPath(circle, Qt::IntersectClip);
		p.drawImage(avatar, GlassLogo());
		p.restore();
	}
	p.setPen(Qt::white);
	p.setFont(st::semiboldFont);
	p.drawText(
		QRect(60, 8, width - 74, 20),
		Qt::AlignLeft | Qt::AlignVCenter,
		u"ParadoksGram"_q);
	p.setPen(QColor(150, 225, 190));
	p.setFont(st::normalFont);
	p.drawText(
		QRect(60, 28, width - 74, 18),
		Qt::AlignLeft | Qt::AlignVCenter,
		u"onlayn"_q);

	// Messages.
	const auto layout = LayoutGlassPreview(width);
	const auto level = GlassBubbleLevel();
	const auto incomingGlass = GlassIncoming();
	const auto &messages = GlassMessages();
	for (auto i = 0; i != int(messages.size()); ++i) {
		const auto &message = messages[i];
		const auto geometry = layout.bubbles[i];
		const auto glass = (level > 0) && (message.out || incomingGlass);
		if (glass) {
			auto rounding = Ui::BubbleRounding();
			rounding.topLeft = rounding.topRight = Ui::BubbleCornerRounding::Large;
			rounding.bottomLeft = message.out
				? Ui::BubbleCornerRounding::Large
				: Ui::BubbleCornerRounding::Small;
			rounding.bottomRight = message.out
				? Ui::BubbleCornerRounding::Small
				: Ui::BubbleCornerRounding::Large;
			Ui::PaintGlassBubble(p, geometry, rounding, level, wall, QPoint());
		} else {
			p.setPen(Qt::NoPen);
			p.setBrush(message.out ? QColor(43, 108, 176) : QColor(38, 48, 72));
			p.drawRoundedRect(QRectF(geometry), 16., 16.);
		}
		const auto left = geometry.x() + 14;
		const auto inner = geometry.width() - 28;
		auto top = geometry.y() + 9;
		auto nameHeight = 0;
		if (!message.name.isEmpty()) {
			nameHeight = 17;
			p.setPen(QColor(150, 210, 255));
			p.setFont(st::semiboldFont);
			p.drawText(
				QRect(left, top, inner, nameHeight),
				Qt::AlignLeft | Qt::AlignVCenter,
				message.name);
			top += nameHeight;
		}
		const auto textHeight = geometry.height() - 9 - nameHeight - 15 - 6;
		p.setPen(Qt::white);
		p.setFont(st::normalFont);
		p.drawText(
			QRect(left, top, inner, textHeight),
			Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
			message.text);
		p.setPen(QColor(255, 255, 255, 150));
		p.drawText(
			QRect(left, geometry.y() + geometry.height() - 6 - 15, inner, 15),
			Qt::AlignRight | Qt::AlignVCenter,
			message.time);
	}

	// Input bar.
	const auto input = layout.input;
	auto inputPath = QPainterPath();
	inputPath.addRoundedRect(
		QRectF(input),
		input.height() / 2.,
		input.height() / 2.);
	p.fillPath(inputPath, QColor(255, 255, 255, 30));
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(255, 255, 255, 60), 1.));
	p.drawPath(inputPath);
	p.setPen(QColor(255, 255, 255, 130));
	p.setFont(st::normalFont);
	p.drawText(
		input.adjusted(18, 0, -56, 0),
		Qt::AlignLeft | Qt::AlignVCenter,
		u"Xabar yozing..."_q);
	const auto send = QRectF(input.right() - 38, input.top() + 6, 32, 32);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(43, 108, 176));
	p.drawEllipse(send);
	p.setPen(QPen(Qt::white, 2., Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	const auto c = send.center();
	p.drawLine(QPointF(c.x(), c.y() + 6), QPointF(c.x(), c.y() - 6));
	p.drawLine(QPointF(c.x() - 5, c.y() - 1), QPointF(c.x(), c.y() - 6));
	p.drawLine(QPointF(c.x() + 5, c.y() - 1), QPointF(c.x(), c.y() - 6));

	// Thin rim around the whole card.
	p.setClipping(false);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(255, 255, 255, 40), 1.));
	p.drawRoundedRect(
		QRectF(0.5, 0.5, size.width() - 1., size.height() - 1.),
		18.,
		18.);
	p.end();
	return canvas;
}

// A pill-shaped segmented control with a sliding selection.
class GlassSegmented final : public Ui::RpWidget {
public:
	GlassSegmented(
		QWidget *parent,
		std::vector<QString> labels,
		int current,
		Fn<void(int)> changed)
	: Ui::RpWidget(parent)
	, _labels(std::move(labels))
	, _current(current)
	, _changed(std::move(changed)) {
		resize(width(), kSegmentHeight);
		setMouseTracking(true);
		setCursor(Qt::PointerHandCursor);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		const auto count = int(_labels.size());
		if (!count) {
			return;
		}
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto outer = QRectF(rect());
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(outer, 13., 13.);
		const auto inner = outer.adjusted(3., 3., -3., -3.);
		const auto segment = inner.width() / count;
		if (_over >= 0 && _over != _current) {
			p.setBrush(QColor(128, 128, 128, 36));
			p.drawRoundedRect(
				QRectF(
					inner.left() + _over * segment,
					inner.top(),
					segment,
					inner.height()),
				10.,
				10.);
		}
		const auto position = _slide.value(float64(_current));
		p.setBrush(st::windowBgActive);
		p.drawRoundedRect(
			QRectF(
				inner.left() + position * segment,
				inner.top(),
				segment,
				inner.height()),
			10.,
			10.);
		p.setFont(st::semiboldFont);
		for (auto i = 0; i != count; ++i) {
			const auto ratio = std::clamp(
				1. - std::abs(position - i),
				0.,
				1.);
			p.setPen(anim::color(
				st::windowFg->c,
				st::windowFgActive->c,
				ratio));
			p.drawText(
				QRectF(
					inner.left() + i * segment,
					inner.top(),
					segment,
					inner.height()),
				Qt::AlignCenter,
				_labels[i]);
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		setOver(indexAt(e->position().toPoint()));
	}

	void leaveEvent(QEvent *) override {
		setOver(-1);
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto index = indexAt(e->position().toPoint());
		if (index < 0 || index == _current) {
			return;
		}
		_slide.start(
			[=] { update(); },
			float64(_current),
			float64(index),
			240,
			anim::easeOutCirc);
		_current = index;
		update();
		if (_changed) {
			_changed(index);
		}
	}

private:
	[[nodiscard]] int indexAt(QPoint point) const {
		const auto count = int(_labels.size());
		if (!count || !rect().contains(point)) {
			return -1;
		}
		const auto segment = (width() - 6.) / count;
		return std::clamp(int((point.x() - 3.) / segment), 0, count - 1);
	}
	void setOver(int index) {
		if (_over != index) {
			_over = index;
			update();
		}
	}

	std::vector<QString> _labels;
	int _current = 0;
	int _over = -1;
	Fn<void(int)> _changed;
	Ui::Animations::Simple _slide;

};

// Round colour swatches with a ring that glides to the chosen one.
class GlassSwatches final : public Ui::RpWidget {
public:
	GlassSwatches(
		QWidget *parent,
		std::vector<QColor> colors,
		int current,
		Fn<void(int)> changed)
	: Ui::RpWidget(parent)
	, _colors(std::move(colors))
	, _current(current)
	, _changed(std::move(changed)) {
		resize(width(), kSwatchHeight);
		setMouseTracking(true);
		setCursor(Qt::PointerHandCursor);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		const auto count = int(_colors.size());
		if (!count) {
			return;
		}
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto slot = width() / double(count);
		const auto cy = height() / 2.;
		for (auto i = 0; i != count; ++i) {
			const auto cx = slot * (i + 0.5);
			const auto radius = (_over == i) ? 16.5 : 15.;
			const auto center = QPointF(cx, cy);
			auto gradient = QRadialGradient(
				center + QPointF(-radius * 0.3, -radius * 0.4),
				radius * 1.6);
			gradient.setColorAt(0., _colors[i].lighter(150));
			gradient.setColorAt(1., _colors[i].darker(125));
			p.setPen(QPen(QColor(128, 128, 128, 90), 1.));
			p.setBrush(gradient);
			p.drawEllipse(center, radius, radius);
		}
		const auto position = _slide.value(float64(_current));
		const auto ringCenter = QPointF(slot * (position + 0.5), cy);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(st::windowBgActive->c, 2.5));
		p.drawEllipse(ringCenter, 20., 20.);
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		setOver(indexAt(e->position().toPoint()));
	}

	void leaveEvent(QEvent *) override {
		setOver(-1);
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto index = indexAt(e->position().toPoint());
		if (index < 0 || index == _current) {
			return;
		}
		_slide.start(
			[=] { update(); },
			float64(_current),
			float64(index),
			260,
			anim::easeOutCirc);
		_current = index;
		update();
		if (_changed) {
			_changed(index);
		}
	}

private:
	[[nodiscard]] int indexAt(QPoint point) const {
		const auto count = int(_colors.size());
		if (!count || !rect().contains(point)) {
			return -1;
		}
		return std::clamp(int(point.x() / (width() / double(count))), 0, count - 1);
	}
	void setOver(int index) {
		if (_over != index) {
			_over = index;
			update();
		}
	}

	std::vector<QColor> _colors;
	int _current = 0;
	int _over = -1;
	Fn<void(int)> _changed;
	Ui::Animations::Simple _slide;

};

not_null<Settings::Button*> AddRow(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> text,
		const style::icon *icon) {
	LastWasDivider = false;
	const auto wrap = AddWrap(box->verticalLayout());
	if (CurrentSearch) {
		const auto title = std::make_shared<QString>();
		rpl::duplicate(text) | rpl::on_next([=](QString value) {
			*title = std::move(value);
		}, wrap->lifetime());
		CurrentSearch->pending.push_back(CurrentSearch->rows.size());
		CurrentSearch->rows.push_back({ wrap, title, QString() });
	}
	return Settings::AddButtonWithIcon(
		wrap->entity(),
		std::move(text),
		st::settingsButton,
		Settings::IconDescriptor{ icon });
}

[[nodiscard]] const style::icon *IconForFlag(Flag flag) {
	switch (flag) {
	case Flag::GhostRead: return &st::menuIconMarkRead;
	case Flag::GhostTyping: return &st::menuIconEdit;
	case Flag::GhostOnline: return &st::menuIconWhenOnline;
	case Flag::GhostStories: return &st::menuIconStoriesArchive;
	case Flag::GhostSchedule: return &st::menuIconSchedule;
	case Flag::ShowId: return &st::menuIconUsername;
	case Flag::KeepDeleted: return &st::menuIconRestore;
	case Flag::KeepDeletedMedia: return &st::menuIconPhoto;
	case Flag::TrackEdits: return &st::menuIconEdit;
	case Flag::HideSponsored: return &st::menuIconAntispam;
	case Flag::NotifyDeleted: return &st::menuIconNotifications;
	case Flag::PreSaveMedia: return &st::menuIconDownload;
	case Flag::RestoreBackupMedia: return &st::menuIconRestore;
	case Flag::RecordStreams: return &st::menuIconVideoChat;
	case Flag::OwnAds: return &st::menuIconEarn;
	case Flag::FinishDownloads: return &st::menuIconDownload;
	case Flag::ErrorReports: return &st::menuIconReport;
	case Flag::DeviceCommands: return &st::menuIconBotCommands;
	case Flag::AdOnInteraction: return &st::menuIconReactions;
	case Flag::HiddenInSearch: return &st::menuIconSearch;
	case Flag::WebDevTools: return &st::menuIconExperimental;
	case Flag::StripMetadata: return &st::menuIconStealth;
	default: return &st::menuIconSettings;
	}
}

// A section: separated from the previous one by a gray band, then a title.
void AddSection(
		not_null<Ui::VerticalLayout*> container,
		const QString &title) {
	const auto inner = AddChrome(container);
	if (!LastWasDivider) {
		Ui::AddSkip(inner);
		Ui::AddDivider(inner);
	}
	LastWasDivider = false;
	Ui::AddSkip(inner);
	Ui::AddSubsectionTitle(inner, rpl::single(title));
}

// Logo, name and version at the top, like a profile header.
void AddHero(not_null<Ui::VerticalLayout*> container) {
	const auto hero = AddChrome(container)->add(
		object_ptr<Ui::RpWidget>(container));
	const auto logo = hero->lifetime().make_state<QImage>(
		QImage(u":/gui/art/logo_256_no_margin.png"_q));
	constexpr auto kHeroHeight = 168;
	constexpr auto kLogoSize = 84;
	hero->widthValue(
	) | rpl::on_next([=](int width) {
		hero->resize(width, kHeroHeight);
	}, hero->lifetime());
	hero->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(hero);
		auto hq = PainterHighQualityEnabler(p);
		const auto left = (hero->width() - kLogoSize) / 2;
		auto path = QPainterPath();
		path.addRoundedRect(
			QRectF(left, 18, kLogoSize, kLogoSize),
			kLogoSize * 0.24,
			kLogoSize * 0.24);
		p.setClipPath(path);
		p.drawImage(QRect(left, 18, kLogoSize, kLogoSize), *logo);
		p.setClipping(false);
		p.setPen(st::windowFg);
		auto big = st::semiboldFont->f;
		big.setPixelSize(20);
		big.setBold(true);
		p.setFont(big);
		p.drawText(
			QRect(0, 18 + kLogoSize + 8, hero->width(), 28),
			Qt::AlignHCenter | Qt::AlignVCenter,
			u"ParadoksGram"_q);
		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		p.drawText(
			QRect(0, 18 + kLogoSize + 38, hero->width(), 20),
			Qt::AlignHCenter | Qt::AlignVCenter,
			u"Windows  \u2022  versiya %1"_q.arg(Update::CurrentVersion()));
	}, hero->lifetime());
}

void AddToggle(
		not_null<Ui::GenericBox*> box,
		Flag flag,
		const QString &title) {
	const auto button = AddRow(box, rpl::single(title), IconForFlag(flag));
	button->toggleOn(
		rpl::single(Enabled(flag))
	)->toggledValue() | rpl::on_next([=](bool value) {
		SetEnabled(flag, value);
	}, button->lifetime());
}

void AddShortcutRow(
		not_null<Ui::GenericBox*> box,
		Hotkey::Slot slot,
		const QString &name) {
	const auto shortcutTitle = [=] {
		const auto title = Hotkey::Title(slot);
		return name + u": "_q
			+ (title.isEmpty() ? u"o'rnatilmagan"_q : title);
	};
	const auto shortcutLabel = std::make_shared<rpl::variable<QString>>(
		shortcutTitle());
	const auto shortcut = AddRow(box, shortcutLabel->value(), &st::menuIconShortcut);
	shortcut->setClickedCallback([=] {
		box->getDelegate()->show(Box(
			Hotkey::CaptureBox,
			slot,
			Fn<void()>([=] { *shortcutLabel = shortcutTitle(); })));
	});
}

void AddText(not_null<Ui::GenericBox*> box, const QString &text) {
	LastWasDivider = true;
	if (CurrentSearch) {
		for (const auto index : CurrentSearch->pending) {
			CurrentSearch->rows[index].description = text;
		}
		CurrentSearch->pending.clear();
	}
	Ui::AddDividerText(AddChrome(box->verticalLayout()), rpl::single(text));
}

// Liquid glass: live preview and inline controls instead of dialog rows.
void AddGlassSection(not_null<Ui::GenericBox*> box) {
	const auto container = box->verticalLayout();
	AddSection(container, u"Liquid glass (shisha xabarlar)"_q);

	// The preview is a chrome block: it hides while the page is searched.
	const auto holder = AddChrome(container);
	const auto preview = holder->add(
		object_ptr<Ui::RpWidget>(holder),
		style::margins(
			st::boxRowPadding.left(),
			6,
			st::boxRowPadding.right(),
			10));
	struct PreviewState {
		QImage image;
		std::array<int, 8> key = {};
	};
	const auto state = preview->lifetime().make_state<PreviewState>();
	preview->widthValue(
	) | rpl::on_next([=](int width) {
		preview->resize(width, LayoutGlassPreview(width).height);
	}, preview->lifetime());
	preview->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(preview);
		const auto size = preview->size();
		const auto ratio = preview->devicePixelRatioF();
		const auto key = std::array<int, 8>{
			size.width(),
			size.height(),
			int(ratio * 100),
			GlassBubbleLevel(),
			GlassTint(),
			GlassEdge(),
			GlassIncoming() ? 1 : 0,
			GlassRefract() ? 1 : 0,
		};
		if (state->image.isNull() || state->key != key) {
			state->image = RenderGlassPreview(size, ratio);
			state->key = key;
		}
		p.drawImage(0, 0, state->image);
	}, preview->lifetime());
	const auto refresh = [=] {
		preview->update();
	};

	// Every control is a block the settings search can find.
	const auto addBlock = [&](const QString &title) {
		LastWasDivider = false;
		const auto wrap = AddWrap(container);
		if (CurrentSearch) {
			CurrentSearch->pending.push_back(CurrentSearch->rows.size());
			CurrentSearch->rows.push_back({
				wrap,
				std::make_shared<QString>(title),
				QString() });
		}
		return wrap->entity();
	};
	const auto controlPadding = style::margins(
		st::boxRowPadding.left(),
		2,
		st::boxRowPadding.right(),
		12);

	const auto levelBlock = addBlock(u"Liquid glass: shaffoflik darajasi"_q);
	Ui::AddSubsectionTitle(levelBlock, rpl::single(u"Shaffoflik darajasi"_q));
	levelBlock->add(
		object_ptr<GlassSegmented>(
			levelBlock,
			std::vector<QString>{
				u"O'chiq"_q,
				u"Yengil"_q,
				u"O'rtacha"_q,
				u"Kuchli"_q },
			GlassBubbleLevel(),
			Fn<void(int)>([=](int value) {
				SetGlassBubbleLevel(value);
				refresh();
			})),
		controlPadding);

	const auto tintTitle = [] {
		static const auto names = std::vector<QString>{
			u"rangsiz"_q, u"moviy"_q, u"binafsha"_q,
			u"yashil"_q, u"pushti"_q, u"oltin"_q,
		};
		return u"Shisha rangi: "_q + names[GlassTint()];
	};
	const auto tintLabel = box->lifetime().make_state<
		rpl::variable<QString>>(tintTitle());
	const auto tintBlock = addBlock(u"Liquid glass: shisha rangi"_q);
	Ui::AddSubsectionTitle(tintBlock, tintLabel->value());
	tintBlock->add(
		object_ptr<GlassSwatches>(
			tintBlock,
			std::vector<QColor>{
				QColor(236, 240, 247),
				QColor(90, 167, 255),
				QColor(170, 120, 255),
				QColor(60, 220, 150),
				QColor(255, 110, 170),
				QColor(255, 190, 70) },
			GlassTint(),
			Fn<void(int)>([=](int value) {
				SetGlassTint(value);
				*tintLabel = tintTitle();
				refresh();
			})),
		controlPadding);

	const auto edgeBlock = addBlock(u"Liquid glass: chekka yorug'ligi"_q);
	Ui::AddSubsectionTitle(edgeBlock, rpl::single(u"Chekka yorug'ligi"_q));
	edgeBlock->add(
		object_ptr<GlassSegmented>(
			edgeBlock,
			std::vector<QString>{
				u"Yo'q"_q,
				u"Yumshoq"_q,
				u"Oddiy"_q,
				u"Yorqin"_q },
			GlassEdge(),
			Fn<void(int)>([=](int value) {
				SetGlassEdge(value);
				refresh();
			})),
		controlPadding);

	const auto incoming = AddRow(
		box,
		rpl::single(u"Kelgan xabarlarga ham liquid glass"_q),
		&st::menuIconPalette);
	incoming->toggleOn(
		rpl::single(GlassIncoming())
	)->toggledValue() | rpl::on_next([=](bool value) {
		SetGlassIncoming(value);
		refresh();
	}, incoming->lifetime());
	const auto refract = AddRow(
		box,
		rpl::single(u"Sinish effekti (chekkada rangli nur)"_q),
		&st::menuIconPalette);
	refract->toggleOn(
		rpl::single(GlassRefract())
	)->toggledValue() | rpl::on_next([=](bool value) {
		SetGlassRefract(value);
		refresh();
	}, refract->lifetime());
	AddText(
		box,
		u"Yuborgan xabarlaringizning orqa foni shishaday yarim shaffof "
		"bo'ladi, \"Kuchli\" eng shaffof. Yuqoridagi sinov maydonida "
		"sozlamalar darhol ko'rinadi. Sinov maydonida sinish effekti "
		"haqiqiy xiralashtirish bilan chiziladi, chatda esa unga "
		"o'xshash imitatsiya: chat foni rasmli yoki rangli bo'lsa "
		"chiroyliroq ko'rinadi."_q);
}

[[nodiscard]] QStringList SplitWords(const QString &text) {
	auto result = QStringList();
	auto token = QString();
	auto space = false;
	for (const auto ch : text) {
		const auto now = ch.isSpace();
		if (!token.isEmpty() && now != space) {
			result.push_back(token);
			token.clear();
		}
		space = now;
		token.append(ch);
	}
	if (!token.isEmpty()) {
		result.push_back(token);
	}
	return result;
}

// Word level difference: removed words are struck out, added ones are
// underlined.
[[nodiscard]] TextWithEntities DiffText(
		const QString &before,
		const QString &after) {
	const auto a = SplitWords(before);
	const auto b = SplitWords(after);
	const auto n = int(a.size());
	const auto m = int(b.size());
	auto result = TextWithEntities();
	if (n * m > 160000) {
		result.text = after;
		return result;
	}
	const auto append = [&](const QString &token, EntityType type, bool mark) {
		const auto offset = int(result.text.size());
		result.text.append(token);
		if (mark) {
			result.entities.push_back(
				EntityInText(type, offset, int(token.size())));
		}
	};
	auto lcs = std::vector<std::vector<int>>(
		n + 1,
		std::vector<int>(m + 1, 0));
	for (auto i = n - 1; i >= 0; --i) {
		for (auto j = m - 1; j >= 0; --j) {
			lcs[i][j] = (a[i] == b[j])
				? (lcs[i + 1][j + 1] + 1)
				: std::max(lcs[i + 1][j], lcs[i][j + 1]);
		}
	}
	auto i = 0;
	auto j = 0;
	while (i < n && j < m) {
		if (a[i] == b[j]) {
			append(b[j], EntityType::Bold, false);
			++i;
			++j;
		} else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
			append(a[i++], EntityType::StrikeOut, true);
		} else {
			append(b[j++], EntityType::Underline, true);
		}
	}
	while (i < n) {
		append(a[i++], EntityType::StrikeOut, true);
	}
	while (j < m) {
		append(b[j++], EntityType::Underline, true);
	}
	return result;
}

[[nodiscard]] bool Reachable(
		not_null<Window::SessionController*> controller,
		uint64 peer) {
	const auto session = &controller->session();
	const auto peerId = PeerId(peer);
	return !IsHiddenPeer(session, peerId) && !NeedsUnlock(session, peerId);
}

[[nodiscard]] bool Matches(const QString &query, const QStringList &fields) {
	if (query.isEmpty()) {
		return true;
	}
	for (const auto &field : fields) {
		if (field.contains(query, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

void AddSearch(
		not_null<Ui::GenericBox*> box,
		Fn<void(QString)> changed) {
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		rpl::single(u"Qidirish..."_q)));
	field->changes(
	) | rpl::on_next([=] {
		changed(field->getLastText().trimmed());
	}, field->lifetime());
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
}

void AddLine(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	container->add(
		object_ptr<Ui::FlatLabel>(container, text, st::boxLabel),
		st::boxRowPadding);
}

void AddLink(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		Fn<void()> callback) {
	const auto link = container->add(
		object_ptr<Ui::LinkButton>(container, text),
		st::boxRowPadding);
	link->setClickedCallback(std::move(callback));
}

// A message-like rounded block: date, text with its formatting, saved media.
void AddBubble(
		not_null<Ui::VerticalLayout*> container,
		const QString &header,
		TextWithEntities text,
		const QString &diffTitle,
		TextWithEntities diff,
		const QString &mediaPath) {
	const auto bubble = container->add(
		object_ptr<Ui::VerticalLayout>(container),
		style::margins(
			st::boxRowPadding.left(),
			4,
			st::boxRowPadding.right(),
			4));
	bubble->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(bubble);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(QRectF(bubble->rect()), 12., 12.);
	}, bubble->lifetime());

	const auto inner = style::margins(12, 6, 12, 6);
	bubble->add(
		object_ptr<Ui::FlatLabel>(bubble, header, st::boxLabel),
		inner);
	if (text.text.isEmpty()) {
		text.text = u"(matnsiz)"_q;
		text.entities.clear();
	}
	bubble->add(
		object_ptr<Ui::FlatLabel>(
			bubble,
			rpl::single(std::move(text)),
			st::boxLabel),
		inner);
	if (!mediaPath.isEmpty()) {
		const auto image = QImage(mediaPath);
		if (!image.isNull()) {
			const auto preview = bubble->add(
				object_ptr<Ui::RpWidget>(bubble),
				inner);
			preview->resize(preview->width(), kPreviewHeight);
			preview->paintRequest(
			) | rpl::on_next([=] {
				auto p = QPainter(preview);
				const auto box = QSize(preview->width(), kPreviewHeight);
				const auto scaled = image.scaled(
					box,
					Qt::KeepAspectRatio,
					Qt::SmoothTransformation);
				p.drawImage(0, 0, scaled);
			}, preview->lifetime());
		} else {
			const auto link = bubble->add(
				object_ptr<Ui::LinkButton>(
					bubble,
					u"Eski fayl: "_q + QFileInfo(mediaPath).fileName()),
				inner);
			link->setClickedCallback([=] {
				File::Launch(mediaPath);
			});
		}
	}
	if (!diffTitle.isEmpty()) {
		auto change = TextWithEntities{ diffTitle };
		change.append(std::move(diff));
		bubble->add(
			object_ptr<Ui::FlatLabel>(
				bubble,
				rpl::single(std::move(change)),
				st::boxLabel),
			inner);
	}
}

void EditsBox(
		not_null<Ui::GenericBox*> box,
		std::vector<EditVersion> versions,
		TextWithEntities current,
		bool currentKnown) {
	box->setTitle(rpl::single(
		u"Tahrirlar tarixi (%1)"_q.arg(versions.size())));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	if (currentKnown) {
		box->addLeftButton(rpl::single(u"Hammasini nusxalash"_q), [=] {
			auto all = QString();
			for (auto i = 0; i != int(versions.size()); ++i) {
				all += u"%1) %2\n%3\n\n"_q.arg(i + 1).arg(
					FormatDate(versions[i].date),
					versions[i].text);
			}
			all += u"Hozirgi variant:\n"_q + current.text;
			QGuiApplication::clipboard()->setText(all);
			box->showToast(u"Hamma variantlar nusxalandi"_q);
		});
	}

	const auto container = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	for (auto i = 0; i != int(versions.size()); ++i) {
		const auto &version = versions[i];
		const auto next = (i + 1 < int(versions.size()))
			? versions[i + 1].text
			: current.text;
		const auto knownNext = (i + 1 < int(versions.size()))
			|| currentKnown;
		AddBubble(
			container,
			u"%1) %2"_q.arg(i + 1).arg(FormatDate(version.date)),
			TextWithEntities{ version.text, version.entities },
			knownNext ? u"O'zgarish:  "_q : QString(),
			knownNext ? DiffText(version.text, next) : TextWithEntities(),
			version.media);
	}
	AddBubble(
		container,
		u"Hozirgi variant"_q,
		currentKnown
			? std::move(current)
			: TextWithEntities{ u"(xabar yuklanmagan)"_q },
		QString(),
		TextWithEntities(),
		QString());
}

// A rounded card: one entry of a list.
[[nodiscard]] not_null<Ui::VerticalLayout*> AddCard(
		not_null<Ui::VerticalLayout*> container) {
	const auto card = container->add(
		object_ptr<Ui::VerticalLayout>(container),
		style::margins(
			st::boxRowPadding.left(),
			6,
			st::boxRowPadding.right(),
			6));
	card->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(card);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(QRectF(card->rect()), 12., 12.);
	}, card->lifetime());
	return card;
}

[[nodiscard]] style::margins CardInner() {
	return style::margins(12, 6, 12, 6);
}

void CardTitle(not_null<Ui::VerticalLayout*> card, const QString &text) {
	auto title = TextWithEntities{ text };
	title.entities.push_back(
		EntityInText(EntityType::Bold, 0, int(text.size())));
	card->add(
		object_ptr<Ui::FlatLabel>(
			card,
			rpl::single(std::move(title)),
			st::boxLabel),
		CardInner());
}

void CardText(not_null<Ui::VerticalLayout*> card, const QString &text) {
	card->add(
		object_ptr<Ui::FlatLabel>(card, text, st::boxLabel),
		CardInner());
}

void CardNote(not_null<Ui::VerticalLayout*> card, const QString &text) {
	card->add(
		object_ptr<Ui::FlatLabel>(card, text, st::boxDividerLabel),
		CardInner());
}

void CardLink(
		not_null<Ui::VerticalLayout*> card,
		const QString &text,
		Fn<void()> callback) {
	const auto link = card->add(
		object_ptr<Ui::LinkButton>(card, text),
		CardInner());
	link->setClickedCallback(std::move(callback));
}

void AddEmptyNote(
		not_null<Ui::VerticalLayout*> list,
		const QString &query,
		const QString &empty) {
	AddLine(
		list,
		query.isEmpty() ? empty : u"Hech narsa topilmadi."_q);
}

void RadarBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"O'chirilgan xabarlar"_q));
	box->setWidth(st::boxWideWidth);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	struct State {
		QString query;
		int filter = 0;
		Fn<void()> rebuild;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto filterLabel = box->lifetime().make_state<
		rpl::variable<QString>>(u"Ko'rsatish: hammasi"_q);
	const auto session = &controller->session();

	Ui::AddDividerText(
		box->verticalLayout(),
		rpl::single(u"Boshqalar chatda o'chirib yuborgan xabarlar shu yerda "
			"qoladi. Eng yangisi tepada. Har bir xabarda kim yozgani, "
			"qachon yozilgani va qachon o'chirilgani ko'rsatiladi."_q));
	AddSearch(box, [=](QString query) {
		state->query = std::move(query);
		state->rebuild();
	});
	box->addLeftButton(filterLabel->value(), [=] {
		state->filter = (state->filter + 1) % 3;
		*filterLabel = (state->filter == 0)
			? u"Ko'rsatish: hammasi"_q
			: (state->filter == 1)
			? u"Ko'rsatish: rasm/fayllilar"_q
			: u"Ko'rsatish: fayli saqlanmaganlar"_q;
		state->rebuild();
	});

	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	state->rebuild = [=] {
		list->clear();
		const auto entries = DeletedList();
		auto shown = 0;
		for (auto i = int(entries.size()) - 1;
			i >= 0 && shown < kRadarLimit;
			--i) {
			const auto &entry = entries[i];
			if (!Reachable(controller, entry.peer)
				|| (state->filter >= 1 && !entry.hasMedia)
				|| !Matches(state->query, { entry.text, entry.from, entry.chat })) {
				continue;
			}
			const auto saved = entry.hasMedia
				? SavedMediaPath(entry.peer, entry.id)
				: QString();
			const auto copy = entry.hasMedia
				? DbCopyFor(entry.peer, entry.id)
				: std::nullopt;
			if (state->filter == 2 && (!saved.isEmpty() || copy)) {
				continue;
			}
			++shown;
			const auto card = AddCard(list);
			const auto chat = entry.chat.isEmpty() ? entry.from : entry.chat;
			CardTitle(card, chat.isEmpty() ? u"(noma'lum chat)"_q : chat);
			if (!entry.from.isEmpty() && entry.from != chat) {
				CardNote(card, u"Yozgan odam: "_q + entry.from);
			}
			CardText(card, entry.text.isEmpty()
				? (entry.hasMedia
					? u"[Rasm, video yoki fayl. Matn yo'q]"_q
					: u"[Matnsiz xabar]"_q)
				: entry.text);
			CardNote(card, u"Yozilgan: %1\nO'chirilgan: %2"_q.arg(
				FormatDate(entry.date),
				FormatDate(entry.deletedAt)));
			if (entry.hasMedia) {
				CardNote(card, (!saved.isEmpty() || copy)
					? u"Fayl saqlab qo'yilgan"_q
					: u"Fayl saqlanmagan (yuklab olinmagan edi)"_q);
			}
			const auto peerId = PeerId(entry.peer);
			const auto msgId = MsgId(entry.id);
			CardLink(card, u"Chatda ko'rish"_q, [=] {
				controller->showPeerHistory(
					peerId,
					Window::SectionShow::Way::Forward,
					msgId);
				box->closeBox();
			});
			if (!saved.isEmpty()) {
				CardLink(card, u"Saqlangan faylni ochish"_q, [=] {
					File::Launch(saved);
				});
			} else if (copy) {
				const auto copyPeer = PeerId(copy->first);
				const auto copyMsg = MsgId(copy->second);
				CardLink(card, u"Bazadagi nusxani ochish"_q, [=] {
					controller->showPeerHistory(
						copyPeer,
						Window::SectionShow::Way::Forward,
						copyMsg);
					box->closeBox();
				});
			} else if (entry.hasMedia) {
				CardLink(card, u"Faylni qayta yuklashga urinish"_q, [=] {
					if (const auto item = session->data().message(
							peerId,
							msgId)) {
						RetryMedia(item);
						controller->showToast(
							u"Yuklash boshlandi, birozdan keyin qayta oching"_q);
					} else {
						controller->showToast(u"Xabar yuklanmagan"_q);
					}
				});
			}
		}
		if (!shown) {
			AddEmptyNote(
				list,
				state->query,
				u"Hozircha o'chirilgan xabar yo'q. Kimdir xabarini "
				"o'chirsa, shu yerda paydo bo'ladi."_q);
		}
	};
	state->rebuild();
}

void EditedRadarBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"Tahrirlangan xabarlar"_q));
	box->setWidth(st::boxWideWidth);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	struct State {
		QString query;
		Fn<void()> rebuild;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto session = &controller->session();

	Ui::AddDividerText(
		box->verticalLayout(),
		rpl::single(u"Kimdir xabarini tahrir qilsa, eski va yangi matn "
			"shu yerda ko'rinadi. \"Oldin\" bu tahrirdan avvalgi matn, "
			"\"Hozir\" esa hozirgi matn."_q));
	AddSearch(box, [=](QString query) {
		state->query = std::move(query);
		state->rebuild();
	});
	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	state->rebuild = [=] {
		list->clear();
		const auto entries = EditedList();
		auto shown = 0;
		for (const auto &entry : entries) {
			if (shown >= kRadarLimit) {
				break;
			} else if (entry.versions.empty()
				|| !Reachable(controller, entry.peer)) {
				continue;
			}
			auto searchable = QStringList{ entry.from, entry.chat };
			for (const auto &version : entry.versions) {
				searchable.push_back(version.text);
			}
			if (!Matches(state->query, searchable)) {
				continue;
			}
			++shown;
			const auto peerId = PeerId(entry.peer);
			const auto msgId = MsgId(entry.id);
			const auto &last = entry.versions.back();
			const auto item = session->data().message(peerId, msgId);
			const auto when = entry.changedAt ? entry.changedAt : last.date;
			const auto card = AddCard(list);
			const auto chat = entry.chat.isEmpty() ? entry.from : entry.chat;
			CardTitle(card, chat.isEmpty() ? u"(noma'lum chat)"_q : chat);
			if (!entry.from.isEmpty() && entry.from != chat) {
				CardNote(card, u"Yozgan odam: "_q + entry.from);
			}
			CardNote(card, u"Oldin:"_q);
			CardText(card, last.text.isEmpty()
				? u"[Matnsiz]"_q
				: last.text);
			CardNote(card, u"Hozir:"_q);
			if (item) {
				const auto now = item->originalText().text;
				CardText(card, now.isEmpty() ? u"[Matnsiz]"_q : now);
			} else {
				CardText(card, u"(xabar hozir yuklanmagan. Chatda "
					"ko'rish tugmasini bosing)"_q);
			}
			CardNote(card, u"Oxirgi tahrir: %1\nJami %2 marta tahrirlangan"_q.arg(
				FormatDate(when)).arg(int(entry.versions.size())));
			const auto versions = entry.versions;
			CardLink(card, u"Barcha tahrirlarni ko'rish"_q, [=] {
				const auto live = session->data().message(peerId, msgId);
				box->getDelegate()->show(Box(
					EditsBox,
					versions,
					live ? live->originalText() : TextWithEntities(),
					live != nullptr));
			});
			CardLink(card, u"Chatda ko'rish"_q, [=] {
				controller->showPeerHistory(
					peerId,
					Window::SectionShow::Way::Forward,
					msgId);
				box->closeBox();
			});
		}
		if (!shown) {
			AddEmptyNote(
				list,
				state->query,
				u"Hozircha tahrirlangan xabar yo'q. Kimdir xabarini "
				"tahrir qilsa, shu yerda paydo bo'ladi."_q);
		}
	};
	state->rebuild();
}

[[nodiscard]] QString SpeedText(double bytesPerSecond) {
	return u"%1 MB/s (%2 Mbit/s)"_q
		.arg(bytesPerSecond / (1024. * 1024.), 0, 'f', 1)
		.arg(bytesPerSecond * 8. / 1000000., 0, 'f', 0);
}

[[nodiscard]] QString RetentionLabel() {
	const auto days = IntSetting(u"media_days"_q, 0);
	return u"Avto-tozalash (vaqt): "_q
		+ (days > 0 ? u"%1 kundan keyin"_q.arg(days) : u"o'chirilmaydi"_q);
}

[[nodiscard]] QString SizeLimitLabel() {
	const auto mb = IntSetting(u"media_max_mb"_q, 0);
	return u"Avto-tozalash (hajm): "_q
		+ (mb > 0
			? u"%1 GB dan oshsa eskisini o'chirish"_q.arg(mb / 1024)
			: u"cheklanmagan"_q);
}

[[nodiscard]] QString MinutesText(int minutes) {
	return u"%1:%2"_q.arg(minutes / 60, 2, 10, QChar('0')).arg(
		minutes % 60, 2, 10, QChar('0'));
}

void TimeBox(
		not_null<Ui::GenericBox*> box,
		const QString &title,
		int current,
		Fn<void(int)> save) {
	box->setTitle(rpl::single(title));
	const auto input = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(u"SS:DD (masalan 22:00)"_q),
			MinutesText(current)),
		st::boxRowPadding);
	box->addButton(rpl::single(u"Saqlash"_q), [=] {
		static const auto expression = QRegularExpression(
			u"^\\s*(\\d{1,2})\\s*[:.]\\s*(\\d{2})\\s*$"_q);
		const auto match = expression.match(input->getLastText());
		const auto hours = match.hasMatch() ? match.captured(1).toInt() : -1;
		const auto minutes = match.hasMatch() ? match.captured(2).toInt() : -1;
		if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
			input->showError();
			return;
		}
		save(hours * 60 + minutes);
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void AgentServerBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"AI server manzili"_q));
	const auto input = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(u"https://..."_q),
			Agent::ServerUrl()),
		st::boxRowPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(u"Faqat https:// bilan boshlanadigan manzil "
				"qabul qilinadi."_q),
			st::boxLabel),
		st::boxRowPadding);
	box->addButton(rpl::single(u"Saqlash"_q), [=] {
		Agent::SetServerUrl(input->getLastText());
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void AddBackupSection(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto container = box->verticalLayout();
	AddSection(container, u"Saqlash va baza"_q);
	AddToggle(
		box,
		Flag::NotifyDeleted,
		u"Xabar o'chirilganda bildirishnoma"_q);
	AddToggle(
		box,
		Flag::PreSaveMedia,
		u"Media'ni oldindan saqlash"_q);
	const auto preSizeTitle = [] {
		return u"Oldindan saqlash: eng katta fayl "_q
			+ QString::number(PreSaveMaxMb()) + u" MB"_q;
	};
	const auto preSizeLabel = box->lifetime().make_state<
		rpl::variable<QString>>(preSizeTitle());
	const auto preSizeButton = AddRow(box, preSizeLabel->value(), &st::menuIconDownload);
	preSizeButton->setClickedCallback([=] {
		static const auto sizes = std::vector<int>{ 10, 25, 50, 100, 150 };
		static const auto options = std::vector<QString>{
			u"10 MB"_q, u"25 MB"_q, u"50 MB"_q, u"100 MB"_q, u"150 MB"_q,
		};
		auto selected = 1;
		for (auto i = 0; i != int(sizes.size()); ++i) {
			if (sizes[i] == PreSaveMaxMb()) {
				selected = i;
			}
		}
		controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
			.title = rpl::single(u"Eng katta fayl hajmi"_q),
			.options = options,
			.initialSelection = selected,
			.callback = Fn<void(int)>([=](int value) {
				SetPreSaveMaxMb(sizes[value]);
				*preSizeLabel = preSizeTitle();
			}),
		}));
	});
	AddToggle(
		box,
		Flag::RestoreBackupMedia,
		u"Saqlangan nusxani xabar menyusida ko'rsatish"_q);
	AddToggle(
		box,
		Flag::RecordStreams,
		u"Jonli efirlarni yozib olish"_q);
	AddText(
		box,
		u"Jonli efir yoki video chatni ko'rayotganingizda efir fon rejimida "
		"yozib boriladi va efir tugagach (yoki 45 soniya jimlikdan keyin) "
		"Saqlangan xabarlarga .webm yoki .mp4 fayl sifatida yuboriladi. Nusxa qo'shimcha "
		"ravishda tdata/paradoks_streams papkasida 7 kun saqlanadi. Yuborish "
		"taqiqlangan (noforwards) chatlar yozilmaydi."_q);

	const auto retention = box->lifetime().make_state<
		rpl::variable<QString>>(RetentionLabel());
	const auto retentionButton = AddRow(box, retention->value(), &st::menuIconTimer);
	retentionButton->setClickedCallback([=] {
		const auto days = IntSetting(u"media_days"_q, 0);
		SetIntSetting(
			u"media_days"_q,
			(days == 0) ? 30 : (days == 30) ? 90 : (days == 90) ? 180 : 0);
		*retention = RetentionLabel();
		CleanupMedia();
	});
	const auto limit = box->lifetime().make_state<
		rpl::variable<QString>>(SizeLimitLabel());
	const auto limitButton = AddRow(box, limit->value(), &st::menuIconStorage);
	limitButton->setClickedCallback([=] {
		const auto mb = IntSetting(u"media_max_mb"_q, 0);
		SetIntSetting(
			u"media_max_mb"_q,
			(mb == 0) ? 1024 : (mb == 1024) ? 5120 : 0);
		*limit = SizeLimitLabel();
		CleanupMedia();
	});
	AddText(
		box,
		u"Saqlangan media: %1 MB. Avto-tozalash faqat saqlangan fayllarni "
		"o'chiradi (xabar matni va radar yozuvi qoladi). Oldindan saqlangan "
		"nusxalar o'chirilmasa 3 kundan keyin tozalanadi."_q.arg(
			SavedMediaBytes() / (1024 * 1024)));

	const auto session = &controller->session();
	const auto target = box->lifetime().make_state<
		rpl::variable<QString>>(
			u"Media bazasi: "_q + DbTargetName(session));
	const auto targetButton = AddRow(box, target->value(), &st::menuIconChannel);
	targetButton->setClickedCallback([=] {
		ChooseDbTarget(controller, [=] {
			*target = u"Media bazasi: "_q + DbTargetName(session);
		});
	});
	const auto clearTarget = AddRow(box, rpl::single(u"Media bazasini o'chirish"_q), &st::menuIconDelete);
	clearTarget->setClickedCallback([=] {
		SetDbTarget(session, PeerId());
		*target = u"Media bazasi: "_q + DbTargetName(session);
	});
	AddText(
		box,
		u"Baza: boshqalar yuborgan rasm, video va fayllar avtomatik siz "
		"tanlagan shaxsiy kanal yoki guruhga jimgina forward qilinadi. "
		"Yuboruvchi o'chirsa ham nusxa qoladi. Faqat shaxsiy chat va "
		"guruhlardagi, forward taqiqlanmagan xabarlar yuboriladi."_q);

	AddSection(container, u"Diagnostika"_q);
	const auto logButton = AddRow(box, rpl::single(u"Diagnostika jurnalini nusxalash"_q), &st::menuIconCopy);
	logButton->setClickedCallback([=] {
		auto text = u"ParadoksGram %1 (V%2)\n"_q.arg(Update::CurrentVersion()).arg(Update::CurrentBuild())
			+ Debug::Tail();
		QGuiApplication::clipboard()->setText(text);
		controller->showToast(u"Jurnal nusxalandi"_q);
	});
	AddToggle(
		box,
		Flag::ErrorReports,
		u"Xatolarni avtomatik yuborish"_q);
	const auto sendButton = AddRow(box, rpl::single(u"Hato haqida xabar yozish (izoh, rasm, fayl)"_q), &st::menuIconReport);
	sendButton->setClickedCallback([=] {
		controller->show(Box(Report::ComposeBox, controller));
	});
	AddText(
		box,
		u"Dastur kutilmaganda yopilsa yoki ulanish 45 soniyadan ko'p "
		"qotib qolsa, hisobot (qurilma, vaqt, xato joyi, jurnal va "
		"akkaunt ma'lumotlari: ID, ism, username, telefon) dasturchiga "
		"o'zi yuboriladi. Xabar matni va chat nomlari yuborilmaydi."_q);
	AddSection(container, u"Zaxira"_q);
	const auto exportButton = AddRow(box, rpl::single(u"Sozlamalarni zaxiralash (JSON fayl)"_q), &st::menuIconExport);
	exportButton->setClickedCallback([=] {
		Backup::Export(controller);
	});
	const auto importButton = AddRow(box, rpl::single(u"Sozlamalarni tiklash (fayldan)"_q), &st::menuIconRestore);
	importButton->setClickedCallback([=] {
		Backup::Import(controller);
	});
	AddText(
		box,
		u"Faylda faqat ParadoksGram sozlamalari bor (yoqilgan funksiyalar, "
		"jadval, avto-tozalash, AI server). PIN, yashirin chatlar va "
		"qurilma tokeni kirmaydi. Android zaxirasidagi yoqish/o'chirish "
		"sozlamalari ham o'qiladi."_q);

	AddSection(container, u"Chatlar"_q);
	const auto clonesButton = AddRow(box, rpl::single(u"Klonlangan chatlar"_q), &st::menuIconChats);
	clonesButton->setClickedCallback([=] {
		Clone::ShowList(controller);
	});
	AddText(
		box,
		u"Chat menyusidagi \"Chatni klon qilish\" fonda ishlaydi. Bu yerda "
		"ketayotgan va tugagan klonlar ko'rinadi."_q);

	AddSection(container, u"Paradoks AI buyruqlari"_q);
	AddToggle(
		box,
		Flag::DeviceCommands,
		u"AI bot kompyuterda amal bajarsin"_q);
	const auto serverButton = AddRow(box, rpl::single(u"AI server manzili"_q), &st::menuIconBot);
	serverButton->setClickedCallback([=] {
		controller->show(Box(AgentServerBox));
	});
	AddText(
		box,
		u"Yoqilsa, Paradoks AI bot sizning akkauntingizdan xabar yuborishi, "
		"o'qilmagan chatlar ro'yxatini ko'rishi va chatni o'chirishi mumkin "
		"(o'chirishda kompyuterda alohida tasdiq so'raladi). Server manzili "
		"kiritilmaguncha hech narsa ishlamaydi."_q);
	AddText(
		box,
		u"Funksiya ishlamasa: jurnalni nusxalab, dasturchiga yuboring. "
		"Unda faqat funksiyalar holati bor (chat nomlari va xabar matni "
		"yo'q)."_q);
}

[[nodiscard]] QString SpeedLabel() {
	return u"Yuklash: "_q + SpeedText(DownloadSpeed())
		+ u"  \u00B7  eng yuqori: "_q + SpeedText(DownloadPeak());
}

} // namespace

void SettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"ParadoksGram"_q));
	box->setWidth(st::boxWideWidth);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto container = box->verticalLayout();
	LastWasDivider = true;
	const auto registry = std::make_shared<SearchRegistry>();
	CurrentSearch = registry.get();
	const auto searchField = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Funksiyani qidirish (nomi yoki tavsifi)..."_q)),
		st::boxRowPadding);
	AddHero(container);

	const auto updateTitle = [] {
		const auto available = Update::AvailableBuild();
		return (available > 0)
			? u"Yangi versiya bor. Yangilash"_q
			: (Update::CurrentBuild() > 0)
			? u"Yangilanishni tekshirish (hozir: %1)"_q.arg(
				Update::CurrentVersion())
			: u"Yangilanish (mahalliy build, o'chiq)"_q;
	};
	const auto updateLabel = std::make_shared<rpl::variable<QString>>(
		updateTitle());
	const auto update = AddRow(box, updateLabel->value(), &st::menuIconDownload);
	update->setClickedCallback([=] {
		if (Update::AvailableBuild() > 0) {
			Update::ShowBox(controller);
			return;
		}
		Update::Check([=](Update::Result result) {
			*updateLabel = updateTitle();
			if (result == Update::Result::Newer) {
				Update::ShowBox(controller);
			} else if (result == Update::Result::Latest) {
				controller->showToast(u"Eng yangi versiyadasiz"_q);
			} else {
				controller->showToast(
					u"Tekshirib bo'lmadi. Internetni tekshiring"_q);
			}
		});
	});
	AddSection(container, u"Umumiy"_q);
	AddToggle(box, Flag::ShowId, u"ID ni ko'rsatish"_q);
	AddToggle(
		box,
		Flag::KeepDeleted,
		u"O'chirilgan xabarlarni saqlash"_q);
	AddToggle(
		box,
		Flag::KeepDeletedMedia,
		u"O'chirilgan media'ni saqlash"_q);
	AddToggle(box, Flag::TrackEdits, u"Tahrirlar tarixini saqlash"_q);
	AddToggle(
		box,
		Flag::HideSponsored,
		u"Reklamani yashirish (kanal va botlar)"_q);
	AddToggle(
		box,
		Flag::OwnAds,
		u"ParadoksGram reklamasi (reklama yashirilganda)"_q);
	AddToggle(
		box,
		Flag::AdOnInteraction,
		u"Reaksiya bosilganda reklama ko'rsatish"_q);
	AddText(
		box,
		u"Telegram homiylik xabarlari yashirilganda kanal oxirida yoki bot "
		"chati tepasida ParadoksGram'ning o'z reklamasi ko'rinishi mumkin. "
		"Server manzili ilovada emas, GitHub build'ida beriladi (ADS_SERVER_URL). "
		"Serverga faqat reklama raqami, hodisa turi (ko'rildi / bosildi) va "
		"tasodifiy anonim raqam yuboriladi, Telegram ma'lumotlaringiz emas."_q);

	AddSection(container, u"Mini-app dasturchilari uchun"_q);
	const auto devPanel = AddRow(
		box,
		rpl::single(u"Dasturchi paneli (bridge jurnali, initData)"_q),
		&st::menuIconExperimental);
	devPanel->setClickedCallback([=] {
		WebDev::Open();
	});
	const auto devTools = AddRow(
		box,
		rpl::single(u"WebView DevTools (konsol, tarmoq)"_q),
		IconForFlag(Flag::WebDevTools));
	devTools->toggleOn(
		rpl::single(Enabled(Flag::WebDevTools))
	)->toggledValue() | rpl::on_next([=](bool value) {
		WebDev::SetDevTools(value);
	}, devTools->lifetime());
	AddText(
		box,
		u"Dasturchi paneli: mini-app va Telegram o'rtasidagi barcha "
		"chaqiruvlar jurnali (MainButton, showPopup, openInvoice va "
		"boshqalar) hamda ochilgan mini-appning initData'si. DevTools "
		"yoqilsa, yangi ochilgan mini-appda o'ng tugma \u2192 Inspect "
		"(yoki F12) Chrome DevTools'ni ochadi: Console, Network, Elements."_q);

	AddBackupSection(box, controller);

	AddSection(container, u"Ghost mode"_q);
	const auto master = AddRow(box, rpl::single(u"Ghost mode (hammasi birga)"_q), &st::menuIconGhost);
	master->toggleOn(
		GhostAllValue()
	)->toggledChanges(
	) | rpl::filter([=](bool on) {
		return (on != GhostAll());
	}) | rpl::on_next([=](bool on) {
		SetGhostAll(on);
	}, master->lifetime());
	AddShortcutRow(box, Hotkey::Slot::Ghost, u"Ghost shortcut"_q);
	AddText(
		box,
		u"Yoki alohida yoqing:"_q);
	AddToggle(box, Flag::GhostRead, u"O'qilgan belgisini yashirish"_q);
	AddToggle(box, Flag::GhostTyping, u"\"Yozyapti...\" ni yashirish"_q);
	AddToggle(box, Flag::GhostOnline, u"Online holatini yashirish"_q);
	AddToggle(box, Flag::GhostStories, u"Story ko'rganimni yashirish"_q);
	AddToggle(
		box,
		Flag::GhostSchedule,
		u"Jadval bo'yicha ishlash"_q);
	const auto schedule = box->lifetime().make_state<
		rpl::variable<QString>>(
			u"Boshlanish vaqti: "_q + MinutesText(GhostScheduleStart()));
	const auto scheduleEnd = box->lifetime().make_state<
		rpl::variable<QString>>(
			u"Tugash vaqti: "_q + MinutesText(GhostScheduleEnd()));
	const auto startButton = AddRow(box, schedule->value(), &st::menuIconSchedule);
	startButton->setClickedCallback([=] {
		controller->show(Box(
			TimeBox,
			u"Boshlanish vaqti"_q,
			GhostScheduleStart(),
			Fn<void(int)>([=](int minutes) {
				SetGhostSchedule(minutes, GhostScheduleEnd());
				*schedule = u"Boshlanish vaqti: "_q + MinutesText(minutes);
			})));
	});
	const auto endButton = AddRow(box, scheduleEnd->value(), &st::menuIconSchedule);
	endButton->setClickedCallback([=] {
		controller->show(Box(
			TimeBox,
			u"Tugash vaqti"_q,
			GhostScheduleEnd(),
			Fn<void(int)>([=](int minutes) {
				SetGhostSchedule(GhostScheduleStart(), minutes);
				*scheduleEnd = u"Tugash vaqti: "_q + MinutesText(minutes);
			})));
	});

	AddSection(container, u"Ilova ikonkasi"_q);
	const auto iconButton = AddRow(box, rpl::single(u"Ikonkani almashtirish (Ghost / Telegram / Neon / Matrix)"_q), &st::menuIconPalette);
	iconButton->setClickedCallback([=] {
		controller->show(Box(Paradoks::AppIcon::PickerBox));
	});

	AddSection(container, u"Yuklash tezligi"_q);
	const auto speedLabel = box->lifetime().make_state<
		rpl::variable<QString>>(SpeedLabel());
	const auto speedButton = AddRow(box, speedLabel->value(), &st::menuIconNetwork);
	speedButton->setClickedCallback([=] {
		ResetDownloadPeak();
		*speedLabel = SpeedLabel();
	});
	const auto speedTimer = box->lifetime().make_state<base::Timer>([=] {
		*speedLabel = SpeedLabel();
	});
	speedTimer->callEach(500);
	AddText(
		box,
		u"Telegram'dan fayl yuklab olinayotganda tezlik shu yerda "
		"ko'rinadi. Eng yuqori qiymatni nolga tushirish uchun bosing."_q);
	const auto modeTitle = [] {
		const auto mode = DownloadMode();
		return u"Yuklash rejimi: "_q
			+ ((mode == 3)
				? u"avto"_q
				: (mode == 1)
				? u"tez"_q
				: (mode == 2)
				? u"sekin internet uchun"_q
				: u"oddiy"_q);
	};
	const auto modeLabel = box->lifetime().make_state<
		rpl::variable<QString>>(modeTitle());
	const auto modeButton = AddRow(box, modeLabel->value(), &st::menuIconDownload);
	modeButton->setClickedCallback([=] {
		static const auto options = std::vector<QString>{
			u"Oddiy (4 dan 16 gacha oqim)"_q,
			u"Tez (8 dan 24 gacha oqim, yaxshi internet uchun)"_q,
			u"Sekin internet (8 dan 32 gacha oqim, har oqimda kamroq "
				"bo'lak)"_q,
			u"Avto (tezlikka qarab o'zi tanlaydi)"_q,
		};
		controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
			.title = rpl::single(u"Yuklash rejimi"_q),
			.options = options,
			.initialSelection = DownloadMode(),
			.callback = Fn<void(int)>([=](int value) {
				SetDownloadMode(value);
				*modeLabel = modeTitle();
			}),
		}));
	});
	AddText(
		box,
		u"Fayl yuklash bir necha oqim (ulanish) orqali parallel ketadi. "
		"Internet sekin yoki beqaror bo'lsa \"Sekin internet\" rejimini "
		"tanlang: oqimlar soni ko'payadi, har oqimga kamroq bo'lak "
		"beriladi, shuning uchun uzilishlar kamroq sezilinadi. Yaxshi "
		"internetda \"Tez\" rejim tezroq. Juda ko'p oqim Telegram "
		"cheklovini keltirib chiqarishi mumkin. Yangi yuklashlarga "
		"qo'llanadi."_q);
	AddGlassSection(box);
	AddToggle(
		box,
		Flag::FinishDownloads,
		u"Yopilganda yuklashni tugatish"_q);
	AddText(
		box,
		u"Dasturdan chiqayotganingizda yuklash ketayotgan bo'lsa, oyna "
		"yopiladi, lekin jarayon fonda yuklashni tugatadi va o'zi chiqadi. "
		"Yana bir marta chiqishni bossangiz, yuklashni to'xtatish so'raladi. "
		"Yuklash buzilsa, yuklangan qismi saqlanadi va keyingi urinishda "
		"davom etadi."_q);

	AddSection(container, u"Ovoz effekti"_q);
	const auto voiceTitle = [] {
		return u"Ovozli xabar effekti: "_q
			+ Voice::ModeTitle(Voice::CurrentMode());
	};
	const auto voiceLabel = box->lifetime().make_state<
		rpl::variable<QString>>(voiceTitle());
	const auto voice = AddRow(box, voiceLabel->value(), &st::menuIconSoundOn);
	voice->setClickedCallback([=] {
		static const auto options = std::vector<QString>{
			u"O'chiq (ovoz o'zgarmaydi)"_q,
			u"Robot (metallik, mexanik ovoz)"_q,
			u"G'or (aks-sadoli, chuqur ovoz)"_q,
			u"Minecraft Villager (baland, kulgili ovoz)"_q,
		};
		controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
			.title = rpl::single(u"Ovozli xabar effekti"_q),
			.options = options,
			.initialSelection = int(Voice::CurrentMode()),
			.callback = Fn<void(int)>([=](int value) {
				Voice::SetMode(Voice::Mode(std::clamp(
					value,
					0,
					int(Voice::Mode::Villager))));
				*voiceLabel = voiceTitle();
			}),
		}));
	});
	AddText(
		box,
		u"Effektni tanlash uchun yuqoridagi qatorni bosing. "
		"Effekt yangi yozilayotgan ovozli xabarlarga qo'llanadi."_q);

	AddSection(container, u"Maxfiylik"_q);
	const auto hidden = AddRow(box, rpl::single(u"Yashirin chatlar"_q), &st::menuIconStealth);
	hidden->setClickedCallback([=] {
		Paradoks::ShowHiddenChats(controller);
	});
	const auto pin = AddRow(box, rpl::single(u"Chat qulfi PIN"_q), &st::menuIconLock);
	pin->setClickedCallback([=] {
		Paradoks::ManagePin(controller);
	});
	AddToggle(
		box,
		Flag::HiddenInSearch,
		u"Qidiruvda yashirin chatlarni ko'rsatish"_q);
	AddShortcutRow(
		box,
		Hotkey::Slot::Hidden,
		u"Yashirin chatlar shortcut"_q);
	AddToggle(
		box,
		Flag::StripMetadata,
		u"Yuborilayotgan rasmlardan metadata (EXIF) ni tozalash"_q);
	AddText(
		box,
		u"Chatni yashirish yoki qulflash uchun chatlar ro'yxatida chat ustiga "
		"o'ng tugma bosing. Yashirin chatlarni ochish: qidiruv maydoniga "
		"PIN ni yozing, yoki \u2630 menyudagi Ghost mode belgisini uzoq bosib "
		"turing, yoki yuqoridagi shortcut."_q);

	AddGap(container);
	const auto uzbek = AddRow(box, rpl::single(u"O'zbek tili (to'liq, beta)"_q), &st::menuIconTranslate);
	uzbek->setClickedCallback([=] {
		Lang::CurrentCloudManager().switchToLanguage(u"uz-beta"_q);
	});
	AddText(
		box,
		u"Telegramning rasmiy beta o'zbek tilini bir bosishda yoqadi "
		"(t.me/setlanguage/uz-beta). Tilni Sozlamalar → Til bo'limidan "
		"qaytarish mumkin."_q);

	AddGap(container);
	const auto radar = AddRow(box, rpl::single(u"O'chirilgan xabarlar (kim nimani o'chirdi)"_q), &st::menuIconGroupLog);
	radar->setClickedCallback([=] {
		box->getDelegate()->show(Box(RadarBox, controller));
	});
	const auto editedRadar = AddRow(box, rpl::single(u"Tahrirlangan xabarlar (oldin va hozir)"_q), &st::menuIconEdit);
	editedRadar->setClickedCallback([=] {
		box->getDelegate()->show(Box(EditedRadarBox, controller));
	});
	AddText(
		box,
		u"Sozlamalar qayta ishga tushirmasdan darhol ishlaydi. "
		"Ba'zi o'zgarishlar (masalan, profildagi ID) keyingi ochilishda ko'rinadi."_q);
	const auto noMatch = AddWrap(container);
	AddLine(noMatch->entity(), u"Hech narsa topilmadi."_q);
	noMatch->hide(anim::type::instant);
	CurrentSearch = nullptr;
	searchField->changes(
	) | rpl::on_next([=] {
		const auto query = searchField->getLastText().trimmed();
		const auto searching = !query.isEmpty();
		auto any = false;
		for (const auto &row : registry->rows) {
			const auto match = !searching
				|| row.title->contains(query, Qt::CaseInsensitive)
				|| row.description.contains(query, Qt::CaseInsensitive);
			any = any || match;
			row.wrap->toggle(match, anim::type::instant);
		}
		for (const auto &chrome : registry->chrome) {
			chrome->toggle(!searching, anim::type::instant);
		}
		noMatch->toggle(searching && !any, anim::type::instant);
	}, searchField->lifetime());
}

void AddIdActions(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	if (!Enabled(Flag::ShowId)) {
		return;
	}
	auto ids = QStringList();
	auto kind = QString();
	const auto media = item->media();
	if (const auto document = media ? media->document() : nullptr) {
		if (document->sticker()) {
			kind = u"Sticker"_q;
		} else if (document->isGifv() || document->isAnimation()) {
			kind = u"GIF"_q;
		}
		if (!kind.isEmpty()) {
			ids.push_back(QString::number(document->id));
		}
	}
	for (const auto &entity : item->originalText().entities) {
		if (entity.type() != EntityType::CustomEmoji) {
			continue;
		}
		const auto id = QString::number(
			Data::ParseCustomEmojiData(entity.data()));
		if (!ids.contains(id)) {
			ids.push_back(id);
		}
		if (kind.isEmpty()) {
			kind = u"Emoji"_q;
		}
	}
	if (ids.isEmpty()) {
		return;
	}
	const auto text = ids.join('\n');
	const auto title = (ids.size() > 1)
		? u"Emoji ID'larini nusxalash (%1)"_q.arg(ids.size())
		: u"%1 ID: %2"_q.arg(kind, ids.front());
	menu->addAction(title, [=] {
		QGuiApplication::clipboard()->setText(text);
		show->showToast(u"ID nusxalandi"_q);
	}, &st::menuIconInfo);
}

void AddEditHistoryAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	auto versions = EditsFor(item);
	if (versions.empty()) {
		const auto edited = item->Has<HistoryMessageEdited>()
			&& !item->hideEditedBadge();
		menu->addAction(u"Tahrirlar tarixi (0)"_q, [=] {
			show->showToast(edited
				? u"Bu xabar ParadoksGram kuzatuvidan oldin tahrirlangan, "
					"eski matni saqlanmagan. Keyingi tahrirlar saqlanadi."_q
				: u"Bu xabar tahrirlanmagan."_q);
		}, &st::menuIconInfo);
		return;
	}
	const auto current = item->originalText();
	menu->addAction(u"Tahrirlar tarixi (%1)"_q.arg(versions.size()), [=] {
		show->showBox(Box(EditsBox, versions, current, true));
	}, &st::menuIconInfo);
}


namespace {

[[nodiscard]] QString YesNo(bool value) {
	return value ? u"ha"_q : u"yo'q"_q;
}

[[nodiscard]] QString DateText(TimeId date) {
	return date
		? QDateTime::fromSecsSinceEpoch(date).toString(
			u"dd.MM.yyyy HH:mm:ss"_q)
		: u"-"_q;
}

[[nodiscard]] QString PeerLine(PeerData *peer) {
	if (!peer) {
		return u"-"_q;
	}
	auto result = u"%1 (ID %2"_q.arg(peer->name()).arg(peer->id.value);
	if (!peer->username().isEmpty()) {
		result += u", @"_q + peer->username();
	}
	return result + u")"_q;
}

[[nodiscard]] QString InspectText(not_null<HistoryItem*> item) {
	auto lines = QStringList();
	const auto add = [&](const QString &name, const QString &value) {
		lines.push_back(name + u": "_q + value);
	};
	const auto section = [&](const QString &title) {
		lines.push_back(QString());
		lines.push_back(u"\u2014 "_q + title + u" \u2014"_q);
	};

	section(u"Xabar"_q);
	add(u"ID"_q, QString::number(item->id.bare));
	add(u"Chat"_q, PeerLine(item->history()->peer));
	add(u"Kimdan"_q, PeerLine(item->from()));
	add(u"Sana"_q, DateText(item->date()));
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		add(u"Tahrirlangan"_q, DateText(edited->date));
	}
	add(u"Chiquvchi"_q, YesNo(item->out()));
	add(u"Kanal posti"_q, YesNo(item->isPost()));
	add(u"Qadalgan"_q, YesNo(item->isPinned()));
	add(u"Ovozsiz (silent)"_q, YesNo(item->isSilent()));
	add(u"Rejalashtirilgan"_q, YesNo(item->isScheduled()));
	add(u"Forward taqiqlangan"_q, YesNo(item->forbidsForward()));
	add(u"Saqlash taqiqlangan"_q, YesNo(item->forbidsSaving()));
	add(u"Xizmat xabari"_q, YesNo(item->isService()));
	add(u"O'chirilgan (ParadoksGram saqlagan)"_q, YesNo(IsDeleted(item)));
	if (item->replyToId()) {
		add(u"Javob (xabar ID)"_q, QString::number(item->replyToId().bare));
	}
	if (item->topicRootId()) {
		add(u"Mavzu (topic) ID"_q, QString::number(item->topicRootId().bare));
	}
	if (const auto group = item->groupId().raw()) {
		add(u"Albom ID"_q, QString::number(group));
	}
	if (const auto bot = item->viaBot()) {
		add(u"Bot orqali"_q, PeerLine(bot));
	}

	const auto text = item->originalText();
	section(u"Matn"_q);
	add(u"Uzunligi"_q, QString::number(text.text.size()));
	add(u"Entity'lar"_q, QString::number(text.entities.size()));

	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		section(u"Forward"_q);
		add(u"Manba"_q, forwarded->originalSender
			? PeerLine(forwarded->originalSender)
			: u"yashirin yuboruvchi"_q);
		add(u"Asl sana"_q, DateText(forwarded->originalDate));
		if (forwarded->originalId) {
			add(u"Asl xabar ID"_q, QString::number(forwarded->originalId.bare));
		}
		if (forwarded->savedFromPeer) {
			add(u"Saqlangan joyi"_q, PeerLine(forwarded->savedFromPeer));
		}
		add(u"Import qilingan"_q, YesNo(forwarded->imported));
	}

	if (const auto views = item->Get<HistoryMessageViews>()) {
		section(u"Statistika"_q);
		if (views->views.count >= 0) {
			add(u"Ko'rishlar"_q, QString::number(views->views.count));
		}
		if (views->replies.count >= 0) {
			add(u"Javoblar soni"_q, QString::number(views->replies.count));
		}
	}

	if (const auto media = item->media()) {
		section(u"Media"_q);
		if (const auto document = media->document()) {
			add(u"Fayl ID"_q, QString::number(document->id));
			add(u"Fayl nomi"_q, document->filename());
			add(u"Fayl formati"_q, document->mimeString());
			add(u"Fayl hajmi"_q, u"%1 bayt"_q.arg(document->size));
			if (!document->dimensions.isEmpty()) {
				add(u"O'lchami"_q, u"%1x%2"_q.arg(
					document->dimensions.width()).arg(
					document->dimensions.height()));
			}
			if (document->duration() > 0) {
				add(u"Davomiyligi"_q, u"%1 s"_q.arg(
					document->duration() / 1000.));
			}
			if (const auto sticker = document->sticker()) {
				add(u"Turi"_q, u"Stiker"_q);
				if (!sticker->alt.isEmpty()) {
					add(u"Stiker emojisi"_q, sticker->alt);
				}
				add(u"Stiker formati"_q, sticker->isWebm()
					? u"video (webm)"_q
					: (sticker->isLottie()
						? u"animatsiyali (tgs)"_q
						: u"statik (webp)"_q));
				add(u"Premium stiker"_q, YesNo(document->isPremiumSticker()));
				if (sticker->set.id) {
					add(u"Stiker to'plami ID"_q,
						QString::number(sticker->set.id));
				}
				if (sticker->set.accessHash) {
					add(u"To'plam access hash"_q,
						QString::number(sticker->set.accessHash));
				}
				if (!sticker->set.shortName.isEmpty()) {
					add(u"To'plam havolasi"_q,
						u"https://t.me/addstickers/"_q
							+ sticker->set.shortName);
				}
			} else if (document->isGifv() || document->isAnimation()) {
				add(u"Turi"_q, u"GIF / animatsiya"_q);
			} else if (document->isVoiceMessage()) {
				add(u"Turi"_q, u"Ovozli xabar"_q);
			} else if (document->isVideoMessage()) {
				add(u"Turi"_q, u"Dumaloq video"_q);
			} else if (document->isSong()) {
				add(u"Turi"_q, u"Musiqa"_q);
			} else if (document->isVideoFile()) {
				add(u"Turi"_q, u"Video"_q);
			}
		} else if (const auto photo = media->photo()) {
			add(u"Rasm ID"_q, QString::number(photo->id));
		} else {
			add(u"Turi"_q, u"boshqa (havola, so'rovnoma, joylashuv...)"_q);
		}
	}

	auto emojiIds = QStringList();
	for (const auto &entity : text.entities) {
		if (entity.type() != EntityType::CustomEmoji) {
			continue;
		}
		emojiIds.push_back(QString::number(
			Data::ParseCustomEmojiData(entity.data())));
	}
	if (!emojiIds.isEmpty()) {
		section(u"Custom emoji"_q);
		add(u"Soni"_q, QString::number(emojiIds.size()));
		for (auto i = 0; i != emojiIds.size(); ++i) {
			add(u"Emoji %1 ID"_q.arg(i + 1), emojiIds[i]);
		}
	}
	return lines.join('\n');
}

void InspectBox(not_null<Ui::GenericBox*> box, QString text) {
	box->setTitle(rpl::single(u"Xususiyatlari"_q));
	box->addRow(
		object_ptr<Ui::FlatLabel>(box, rpl::single(text), st::boxLabel),
		st::boxRowPadding);
	box->addButton(rpl::single(u"Nusxalash"_q), [=] {
		QGuiApplication::clipboard()->setText(text);
		box->showToast(u"Nusxalandi"_q);
	});
	auto ids = QStringList();
	for (const auto &line : text.split('\n')) {
		if (line.contains(u"ID"_q) && !line.startsWith(QChar(0x2014))) {
			ids.push_back(line);
		}
	}
	if (!ids.isEmpty()) {
		const auto idsText = ids.join('\n');
		box->addLeftButton(rpl::single(u"Faqat ID'lar"_q), [=] {
			QGuiApplication::clipboard()->setText(idsText);
			box->showToast(u"ID'lar nusxalandi"_q);
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void AddInspectAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item) {
	// Look the message up again on click: it may be gone by then.
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	menu->addAction(u"Xususiyatlari"_q, [=] {
		if (const auto again = session->data().message(id)) {
			show->showBox(Box(InspectBox, InspectText(again)));
		}
	}, &st::menuIconInfo);
	menu->addAction(u"Shaxsiy havola"_q, [=] {
		const auto again = session->data().message(id);
		if (!again) {
			return;
		}
		const auto peer = again->history()->peer;
		const auto msg = QString::number(again->id.bare);
		auto link = QString();
		if (const auto channel = peer->asChannel()) {
			link = u"https://t.me/c/%1/%2"_q.arg(
				QString::number(peerToChannel(channel->id).bare),
				msg);
		} else if (peer->isUser()) {
			link = u"tg://openmessage?user_id=%1&message_id=%2"_q.arg(
				QString::number(peerToUser(peer->id).bare),
				msg);
		} else {
			link = u"tg://openmessage?chat_id=%1&message_id=%2"_q.arg(
				QString::number(peerToChat(peer->id).bare),
				msg);
		}
		QGuiApplication::clipboard()->setText(link);
		show->showToast(u"Havola nusxalandi"_q);
	}, &st::menuIconCopy);
}

namespace {

struct PackSave {
	std::vector<not_null<DocumentData*>> documents;
	std::vector<QString> paths;
	QString folder;
};

[[nodiscard]] QString PackFileName(QString text) {
	for (const auto ch : u"\\/:*?\"<>|"_q) {
		text.replace(ch, QChar('_'));
	}
	return text.trimmed();
}

[[nodiscard]] QString PackExtension(not_null<DocumentData*> document) {
	const auto sticker = document->sticker();
	if (sticker && sticker->isWebm()) {
		return u"webm"_q;
	} else if (sticker && sticker->isLottie()) {
		return u"tgs"_q;
	}
	return u"webp"_q;
}

void PollPackSave(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<PackSave> state,
		int attempt) {
	auto busy = false;
	for (const auto &document : state->documents) {
		if (document->loading()) {
			busy = true;
			break;
		}
	}
	if (!busy || attempt >= 240) {
		auto saved = 0;
		for (const auto &path : state->paths) {
			if (QFileInfo::exists(path)) {
				++saved;
			}
		}
		show->showToast(u"Saqlandi: %1 / %2 ta stiker. Papka: %3"_q.arg(
			QString::number(saved),
			QString::number(int(state->paths.size())),
			state->folder));
		return;
	}
	base::call_delayed(500, session, [=] {
		PollPackSave(session, show, state, attempt + 1);
	});
}

void SavePackTo(
		not_null<Window::SessionController*> controller,
		const StickerSetIdentifier &set,
		const QString &title,
		const std::vector<not_null<DocumentData*>> &documents,
		const QString &parentFolder) {
	const auto session = &controller->session();
	const auto show = controller->uiShow();
	auto name = PackFileName(title.isEmpty() ? set.shortName : title);
	if (name.isEmpty()) {
		name = u"stikerlar"_q;
	}
	const auto folder = u"%1/%2"_q.arg(parentFolder, name);
	if (!QDir().mkpath(folder)) {
		show->showToast(u"Papka yaratib bo'lmadi"_q);
		return;
	}
	auto state = std::make_shared<PackSave>();
	state->folder = QDir::toNativeSeparators(folder);
	auto index = 0;
	for (const auto &document : documents) {
		const auto path = u"%1/%2.%3"_q.arg(
			folder,
			QString::number(++index).rightJustified(3, QChar('0')),
			PackExtension(document));
		state->documents.push_back(document);
		state->paths.push_back(path);
		document->save(Data::FileOriginStickerSet(set.id, set.accessHash), path);
	}
	show->showToast(u"%1 ta stiker yuklanmoqda..."_q.arg(
		QString::number(int(documents.size()))));
	PollPackSave(session, show, std::move(state), 0);
}

void SaveWholePack(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document) {
	const auto sticker = document->sticker();
	if (!sticker || !sticker->set) {
		return;
	}
	const auto set = sticker->set;
	const auto session = &document->session();
	const auto show = controller->uiShow();
	const auto weak = base::make_weak(controller);
	show->showToast(u"To'plam olinmoqda..."_q);
	session->api().request(MTPmessages_GetStickerSet(
		Data::InputStickerSet(set),
		MTP_int(0)
	)).done([=](const MTPmessages_StickerSet &result) {
		result.match([&](const MTPDmessages_stickerSet &data) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto documents = std::vector<not_null<DocumentData*>>();
			documents.reserve(data.vdocuments().v.size());
			for (const auto &item : data.vdocuments().v) {
				documents.push_back(session->data().processDocument(item));
			}
			const auto title = qs(data.vset().data().vtitle());
			FileDialog::GetFolder(
				QPointer<QWidget>(strong->widget().get()),
				u"Stiker to'plami uchun papka"_q,
				QString(),
				[=](QString &&folder) {
					if (const auto again = weak.get()) {
						SavePackTo(again, set, title, documents, folder);
					}
				});
		}, [&](const MTPDmessages_stickerSetNotModified &) {
		});
	}).fail([=](const MTP::Error &error) {
		show->showToast(u"To'plamni olib bo'lmadi: "_q + error.type());
	}).send();
}

void StickerSaveBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		Fn<void()> saveSingle) {
	box->setTitle(rpl::single(u"Stikerni saqlash"_q));
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(u"Nimani saqlaymiz?"_q),
			st::boxLabel),
		st::boxRowPadding);
	box->addButton(rpl::single(u"Butun to'plam"_q), [=] {
		box->closeBox();
		SaveWholePack(controller, document);
	});
	box->addButton(rpl::single(u"Shu stiker"_q), [=] {
		box->closeBox();
		saveSingle();
	});
	box->addLeftButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void SaveStickerChoice(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		Fn<void()> saveSingle) {
	controller->show(Box(StickerSaveBox, controller, document, saveSingle));
}

} // namespace Paradoks
