/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_story.h"

#include "api/api_updates.h"
#include "apiwrap.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "editor/photo_editor_layer_widget.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwidget.h"
#include "mtproto/mtproto_response.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>

namespace Paradoks::Story {
namespace {

constexpr auto kMaxWidth = 1080;
constexpr auto kMaxHeight = 1920;
constexpr auto kPreviewHeight = 240;

struct Options {
	QString caption;
	int privacy = 0; // 0 everyone, 1 contacts, 2 close friends.
	int period = 86400; // Seconds.
	bool noForwards = false;
	bool pinned = false;
};

[[nodiscard]] QImage Prepared(QImage image) {
	if (image.width() > kMaxWidth || image.height() > kMaxHeight) {
		image = image.scaled(
			kMaxWidth,
			kMaxHeight,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	return image.convertToFormat(QImage::Format_RGB32);
}

// Same kind of prepared file as a profile photo upload uses.
[[nodiscard]] std::shared_ptr<FilePrepareResult> PreparePhoto(
		MTP::DcId dcId,
		PeerId peerId,
		QImage image) {
	PreparedPhotoThumbs photoThumbs;
	QVector<MTPPhotoSize> photoSizes;

	QByteArray jpeg;
	QBuffer jpegBuffer(&jpeg);
	image.save(&jpegBuffer, "JPG", 90);

	const auto scaled = [&](int size) {
		return image.scaled(
			size,
			size,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	};
	const auto push = [&](
			const char *type,
			QImage &&image,
			QByteArray bytes = QByteArray()) {
		photoSizes.push_back(MTP_photoSize(
			MTP_string(type),
			MTP_int(image.width()),
			MTP_int(image.height()), MTP_int(0)));
		photoThumbs.emplace(type[0], PreparedPhotoThumb{
			.image = std::move(image),
			.bytes = std::move(bytes)
		});
	};
	push("a", scaled(160));
	push("b", scaled(320));
	push("c", std::move(image), jpeg);

	const auto id = base::RandomValue<PhotoId>();
	const auto photo = MTP_photo(
		MTP_flags(0),
		MTP_long(id),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(base::unixtime::now()),
		MTP_vector<MTPPhotoSize>(photoSizes),
		MTPVector<MTPVideoSize>(),
		MTP_int(dcId));

	auto result = MakePreparedFile({
		.id = id,
		.type = SendMediaType::Photo,
	});
	result->type = SendMediaType::Photo;
	result->setFileData(jpeg);
	result->thumbId = id;
	result->thumbname = "thumb.jpg";
	result->photo = photo;
	result->photoThumbs = photoThumbs;
	return result;
}

[[nodiscard]] MTPVector<MTPInputPrivacyRule> PrivacyRules(int privacy) {
	auto rules = QVector<MTPInputPrivacyRule>();
	switch (privacy) {
	case 1: rules.push_back(MTP_inputPrivacyValueAllowContacts()); break;
	case 2: rules.push_back(MTP_inputPrivacyValueAllowCloseFriends()); break;
	default: rules.push_back(MTP_inputPrivacyValueAllowAll()); break;
	}
	return MTP_vector<MTPInputPrivacyRule>(rules);
}

struct Upload {
	rpl::lifetime lifetime;
	bool done = false;
};

void SendStory(
		not_null<Window::SessionController*> controller,
		QImage image,
		const Options &options) {
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	const auto toast = [=](const QString &text) {
		if (const auto strong = weak.get()) {
			strong->showToast(text);
		}
	};

	const auto self = session->user();
	const auto fakeId = FullMsgId(
		self->id,
		session->data().nextLocalMessageId());
	const auto prepared = PreparePhoto(
		session->mtp().mainDcId(),
		self->id,
		Prepared(std::move(image)));
	const auto state = std::make_shared<Upload>();

	const auto finish = [=] {
		state->done = true;
		crl::on_main([=] {
			state->lifetime.destroy();
		});
	};

	session->uploader().photoReady(
	) | rpl::filter([=](const Storage::UploadedMedia &data) {
		return !state->done && (data.fullId == fakeId);
	}) | rpl::on_next([=](const Storage::UploadedMedia &data) {
		finish();

		auto flags = MTPstories_SendStory::Flags(0);
		if (!options.caption.isEmpty()) {
			flags |= MTPstories_SendStory::Flag::f_caption;
		}
		flags |= MTPstories_SendStory::Flag::f_period;
		if (options.noForwards) {
			flags |= MTPstories_SendStory::Flag::f_noforwards;
		}
		if (options.pinned) {
			flags |= MTPstories_SendStory::Flag::f_pinned;
		}
		const auto media = MTP_inputMediaUploadedPhoto(
			MTP_flags(0),
			data.info.file,
			MTP_vector<MTPInputDocument>(QVector<MTPInputDocument>()),
			MTPint(),
			MTPInputDocument());
		session->api().request(MTPstories_SendStory(
			MTP_flags(flags),
			session->user()->input(),
			media,
			MTPVector<MTPMediaArea>(),
			MTP_string(options.caption),
			MTPVector<MTPMessageEntity>(),
			PrivacyRules(options.privacy),
			MTP_long(base::RandomValue<uint64>()),
			MTP_int(options.period),
			MTPInputPeer(),
			MTPint(),
			MTPVector<MTPint>(),
			MTPInputDocument()
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			toast(u"Story qo'yildi"_q);
		}).fail([=](const MTP::Error &error) {
			toast(u"Story qo'yib bo'lmadi: "_q + error.type());
		}).send();
	}, state->lifetime);

	session->uploader().photoFailed(
	) | rpl::filter([=](const FullMsgId &id) {
		return !state->done && (id == fakeId);
	}) | rpl::on_next([=](const FullMsgId &) {
		finish();
		toast(u"Rasmni yuklab bo'lmadi."_q);
	}, state->lifetime);

	toast(u"Story yuklanmoqda..."_q);
	session->uploader().upload(fakeId, prepared);
}

void OptionsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		QImage image) {
	box->setTitle(rpl::single(u"Yangi story"_q));
	const auto container = box->verticalLayout();

	// Preview of the picture.
	const auto preview = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		st::boxRowPadding);
	preview->resize(preview->width(), kPreviewHeight);
	preview->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(preview);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = image.size().scaled(
			preview->width(),
			kPreviewHeight,
			Qt::KeepAspectRatio);
		const auto target = QRect(
			QPoint(
				(preview->width() - size.width()) / 2,
				(kPreviewHeight - size.height()) / 2),
			size);
		p.drawImage(target, image);
	}, preview->lifetime());

	const auto premium = controller->session().premium();
	const auto input = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Izoh (ixtiyoriy)"_q)),
		st::boxRowPadding);
	input->setMaxLength(premium ? 2048 : 200);

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Kim ko'radi"_q));
	const auto privacy = std::make_shared<Ui::RadiobuttonGroup>(0);
	const auto addPrivacy = [&](int value, const QString &text) {
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				privacy,
				value,
				text,
				st::defaultBoxCheckbox),
			st::boxRowPadding);
	};
	addPrivacy(0, u"Hamma"_q);
	addPrivacy(1, u"Kontaktlar"_q);
	addPrivacy(2, u"Yaqin do'stlar"_q);

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"Muddat"_q));
	const auto period = std::make_shared<Ui::RadiobuttonGroup>(86400);
	const auto addPeriod = [&](int seconds, const QString &text) {
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				period,
				seconds,
				text,
				st::defaultBoxCheckbox),
			st::boxRowPadding);
	};
	addPeriod(6 * 3600, u"6 soat"_q);
	addPeriod(12 * 3600, u"12 soat"_q);
	addPeriod(24 * 3600, u"24 soat"_q);
	addPeriod(48 * 3600, u"48 soat"_q);

	Ui::AddSkip(container);
	const auto noForwards = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			u"Nusxalash va ulashishni taqiqlash"_q,
			false,
			st::defaultBoxCheckbox),
		st::boxRowPadding);
	const auto pinned = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			u"Profilda saqlash"_q,
			false,
			st::defaultBoxCheckbox),
		st::boxRowPadding);

	box->addButton(rpl::single(u"Qo'yish"_q), [=] {
		auto options = Options{
			.caption = input->getLastText().trimmed(),
			.privacy = privacy->current(),
			.period = period->current(),
			.noForwards = noForwards->checked(),
			.pinned = pinned->checked(),
		};
		box->closeBox();
		SendStory(controller, image, options);
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void Start(not_null<Window::SessionController*> controller) {
	Editor::PrepareStoryPhotoFromFile(
		controller->content(),
		&controller->window(),
		[=](QImage &&image) {
			controller->show(Box(OptionsBox, controller, std::move(image)));
		});
}

} // namespace Paradoks::Story
