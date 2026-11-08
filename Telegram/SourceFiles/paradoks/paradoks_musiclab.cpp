/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_musiclab.h"

#include "paradoks/paradoks_debug.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "history/history.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/format_song_document_name.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace Paradoks::MusicLab {
namespace {

constexpr auto kRate = 44100;
constexpr auto kMaxSeconds = 15 * 60;
constexpr auto kPreviewSeconds = 30;
constexpr auto kPi = 3.14159265358979323846f;

struct Pcm {
	std::vector<float> l;
	std::vector<float> r;

	[[nodiscard]] int64 frames() const {
		return int64(l.size());
	}
	[[nodiscard]] float seconds() const {
		return l.size() / float(kRate);
	}
};

struct Params {
	bool karaoke = false;
	bool bass = false;
	bool eight = false;
	bool echo = false;
	bool reverb = false;
	bool slow = false;
	bool nightcore = false;
	float bassDb = 10.f;
	float eightHz = 0.15f;
	float echoTime = 0.38f;
	float reverbMix = 0.4f;
	float eq[5] = { 0.f, 0.f, 0.f, 0.f, 0.f }; // 60, 250, 1k, 4k, 12k (dB)

	[[nodiscard]] float speed() const {
		auto result = 1.f;
		if (slow) {
			result *= 0.85f;
		}
		if (nightcore) {
			result *= 1.25f;
		}
		return result;
	}
	[[nodiscard]] bool anyEffect() const {
		if (karaoke || bass || eight || echo || reverb || speed() != 1.f) {
			return true;
		}
		for (const auto gain : eq) {
			if (std::abs(gain) > 0.01f) {
				return true;
			}
		}
		return false;
	}
};

constexpr float kEqFreq[5] = { 60.f, 250.f, 1000.f, 4000.f, 12000.f };
const char *kEqTitles[5] = { "60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz" };

[[nodiscard]] QString Describe(const Params &p) {
	auto parts = QStringList();
	if (p.karaoke) parts << u"Karaoke"_q;
	if (p.bass) parts << u"Bass"_q;
	if (p.eight) parts << u"8D"_q;
	if (p.echo) parts << u"Echo"_q;
	if (p.reverb) parts << u"Reverb"_q;
	if (p.slow) parts << u"Slow"_q;
	if (p.nightcore) parts << u"Nightcore"_q;
	for (const auto gain : p.eq) {
		if (std::abs(gain) > 0.01f) {
			parts << u"EQ"_q;
			break;
		}
	}
	return parts.isEmpty()
		? u"Asl holat"_q
		: parts.join(u" + "_q);
}

// ---- Decoding (ffmpeg -> 44.1 kHz stereo float) ----

[[nodiscard]] bool Decode(
		const QString &path,
		Pcm &out,
		const std::atomic<bool> &cancel) {
	const auto utf8 = path.toUtf8();
	auto format = (AVFormatContext*)nullptr;
	if (avformat_open_input(&format, utf8.constData(), nullptr, nullptr) < 0) {
		return false;
	}
	const auto closeFormat = gsl::finally([&] {
		avformat_close_input(&format);
	});
	if (avformat_find_stream_info(format, nullptr) < 0) {
		return false;
	}
	const auto index = av_find_best_stream(
		format,
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (index < 0) {
		return false;
	}
	const auto stream = format->streams[index];
	const auto codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		return false;
	}
	auto context = avcodec_alloc_context3(codec);
	if (!context) {
		return false;
	}
	const auto freeContext = gsl::finally([&] {
		avcodec_free_context(&context);
	});
	if (avcodec_parameters_to_context(context, stream->codecpar) < 0
		|| avcodec_open2(context, codec, nullptr) < 0) {
		return false;
	}

	auto outLayout = AVChannelLayout();
	av_channel_layout_default(&outLayout, 2);
	auto swr = (SwrContext*)nullptr;
	if (swr_alloc_set_opts2(
			&swr,
			&outLayout,
			AV_SAMPLE_FMT_FLT,
			kRate,
			&context->ch_layout,
			context->sample_fmt,
			context->sample_rate,
			0,
			nullptr) < 0
		|| !swr
		|| swr_init(swr) < 0) {
		if (swr) {
			swr_free(&swr);
		}
		return false;
	}
	const auto freeSwr = gsl::finally([&] {
		swr_free(&swr);
	});

	auto packet = av_packet_alloc();
	auto frame = av_frame_alloc();
	const auto freeAll = gsl::finally([&] {
		av_packet_free(&packet);
		av_frame_free(&frame);
	});

	const auto limit = int64(kMaxSeconds) * kRate;
	auto buffer = std::vector<float>();
	const auto push = [&](const float *interleaved, int frames) {
		const auto room = limit - out.frames();
		const auto take = int(std::min<int64>(frames, room));
		for (auto i = 0; i != take; ++i) {
			out.l.push_back(interleaved[i * 2]);
			out.r.push_back(interleaved[i * 2 + 1]);
		}
	};
	const auto convertFrame = [&](const AVFrame *f) {
		const auto capacity = swr_get_out_samples(swr, f ? f->nb_samples : 0);
		if (capacity <= 0) {
			return;
		}
		buffer.resize(size_t(capacity) * 2);
		auto *dst = reinterpret_cast<uint8_t*>(buffer.data());
		const auto got = swr_convert(
			swr,
			&dst,
			capacity,
			f ? (const uint8_t**)f->extended_data : nullptr,
			f ? f->nb_samples : 0);
		if (got > 0) {
			push(buffer.data(), got);
		}
	};

	while (!cancel.load() && out.frames() < limit) {
		const auto read = av_read_frame(format, packet);
		if (read < 0) {
			break;
		}
		if (packet->stream_index == index
			&& avcodec_send_packet(context, packet) >= 0) {
			while (avcodec_receive_frame(context, frame) >= 0) {
				convertFrame(frame);
				av_frame_unref(frame);
			}
		}
		av_packet_unref(packet);
	}
	// Flush the resampler tail.
	for (;;) {
		const auto capacity = swr_get_out_samples(swr, 0);
		if (capacity <= 0) {
			break;
		}
		buffer.resize(size_t(capacity) * 2);
		auto *dst = reinterpret_cast<uint8_t*>(buffer.data());
		const auto got = swr_convert(swr, &dst, capacity, nullptr, 0);
		if (got <= 0) {
			break;
		}
		push(buffer.data(), got);
	}
	return !cancel.load() && out.frames() > kRate / 4;
}

// ---- DSP ----

struct Biquad {
	float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
	float z1[2] = { 0.f, 0.f };
	float z2[2] = { 0.f, 0.f };

	void set(float nb0, float nb1, float nb2, float na0, float na1, float na2) {
		b0 = nb0 / na0;
		b1 = nb1 / na0;
		b2 = nb2 / na0;
		a1 = na1 / na0;
		a2 = na2 / na0;
	}
	// RBJ audio EQ cookbook.
	void lowShelf(float freq, float gainDb) {
		const auto A = std::pow(10.f, gainDb / 40.f);
		const auto w = 2.f * kPi * freq / kRate;
		const auto cs = std::cos(w);
		const auto alpha = std::sin(w) / 2.f * std::sqrt(2.f);
		const auto sq = 2.f * std::sqrt(A) * alpha;
		set(
			A * ((A + 1) - (A - 1) * cs + sq),
			2 * A * ((A - 1) - (A + 1) * cs),
			A * ((A + 1) - (A - 1) * cs - sq),
			(A + 1) + (A - 1) * cs + sq,
			-2 * ((A - 1) + (A + 1) * cs),
			(A + 1) + (A - 1) * cs - sq);
	}
	void highShelf(float freq, float gainDb) {
		const auto A = std::pow(10.f, gainDb / 40.f);
		const auto w = 2.f * kPi * freq / kRate;
		const auto cs = std::cos(w);
		const auto alpha = std::sin(w) / 2.f * std::sqrt(2.f);
		const auto sq = 2.f * std::sqrt(A) * alpha;
		set(
			A * ((A + 1) + (A - 1) * cs + sq),
			-2 * A * ((A - 1) + (A + 1) * cs),
			A * ((A + 1) + (A - 1) * cs - sq),
			(A + 1) - (A - 1) * cs + sq,
			2 * ((A - 1) - (A + 1) * cs),
			(A + 1) - (A - 1) * cs - sq);
	}
	void peaking(float freq, float gainDb, float q) {
		const auto A = std::pow(10.f, gainDb / 40.f);
		const auto w = 2.f * kPi * freq / kRate;
		const auto cs = std::cos(w);
		const auto alpha = std::sin(w) / (2.f * q);
		set(
			1 + alpha * A,
			-2 * cs,
			1 - alpha * A,
			1 + alpha / A,
			-2 * cs,
			1 - alpha / A);
	}
	[[nodiscard]] float run(int ch, float x) {
		const auto y = b0 * x + z1[ch];
		z1[ch] = b1 * x - a1 * y + z2[ch];
		z2[ch] = b2 * x - a2 * y;
		return y;
	}
};

class Delay {
public:
	explicit Delay(int size = 1) : _buf(std::max(1, size), 0.f) {
	}
	[[nodiscard]] float read() const {
		return _buf[_pos];
	}
	void write(float v) {
		_buf[_pos] = v;
		if (++_pos >= int(_buf.size())) {
			_pos = 0;
		}
	}
	[[nodiscard]] int size() const {
		return int(_buf.size());
	}

private:
	std::vector<float> _buf;
	int _pos = 0;

};

struct Comb {
	Delay delay;
	float store = 0.f;
	explicit Comb(int size) : delay(size) {
	}
	float run(float x, float feedback, float damp) {
		const auto out = delay.read();
		store = out * (1.f - damp) + store * damp;
		delay.write(x + store * feedback);
		return out;
	}
};

struct Allpass {
	Delay delay;
	explicit Allpass(int size) : delay(size) {
	}
	float run(float x) {
		const auto buf = delay.read();
		const auto out = -x + buf;
		delay.write(x + buf * 0.5f);
		return out;
	}
};

class Chain {
public:
	explicit Chain(const Params &p)
	: _p(p)
	, _echoL(std::max(1, int(p.echoTime * kRate)))
	, _echoR(std::max(1, int(p.echoTime * kRate)))
	, _combL{ Comb(1116), Comb(1188), Comb(1277), Comb(1356) }
	, _combR{ Comb(1139), Comb(1211), Comb(1300), Comb(1379) }
	, _allL{ Allpass(556), Allpass(441) }
	, _allR{ Allpass(579), Allpass(464) } {
		for (auto i = 0; i != 5; ++i) {
			if (std::abs(p.eq[i]) <= 0.01f) {
				continue;
			}
			auto &b = _eq[_eqCount++];
			if (i == 0) {
				b.lowShelf(kEqFreq[i], p.eq[i]);
			} else if (i == 4) {
				b.highShelf(kEqFreq[i], p.eq[i]);
			} else {
				b.peaking(kEqFreq[i], p.eq[i], 1.0f);
			}
		}
		if (p.bass) {
			_bass.lowShelf(110.f, p.bassDb);
		}
	}

	void process(float &l, float &r, int64 index) {
		if (_p.karaoke) {
			// Remove what is identical in both channels (usually vocals).
			const auto nl = l - 0.95f * r;
			const auto nr = r - 0.95f * l;
			l = nl * 0.8f;
			r = nr * 0.8f;
		}
		for (auto i = 0; i != _eqCount; ++i) {
			l = _eq[i].run(0, l);
			r = _eq[i].run(1, r);
		}
		if (_p.bass) {
			l = _bass.run(0, l);
			r = _bass.run(1, r);
		}
		if (_p.eight) {
			const auto pan = float(std::sin(2.0 * 3.14159265358979323846 * double(_p.eightHz) * double(index) / kRate));
			const auto angle = (pan + 1.f) * kPi / 4.f;
			const auto mono = (l + r) * 0.5f;
			l = mono * std::cos(angle) * 1.4142f;
			r = mono * std::sin(angle) * 1.4142f;
		}
		if (_p.echo) {
			const auto dl = _echoL.read();
			const auto dr = _echoR.read();
			_echoL.write(l + dr * 0.42f);
			_echoR.write(r + dl * 0.42f);
			l += dl * 0.45f;
			r += dr * 0.45f;
		}
		if (_p.reverb) {
			const auto in = (l + r) * 0.5f * 0.03f;
			auto wl = 0.f;
			auto wr = 0.f;
			for (auto i = 0; i != 4; ++i) {
				wl += _combL[i].run(in, 0.84f, 0.2f);
				wr += _combR[i].run(in, 0.84f, 0.2f);
			}
			for (auto i = 0; i != 2; ++i) {
				wl = _allL[i].run(wl);
				wr = _allR[i].run(wr);
			}
			const auto mix = _p.reverbMix;
			l = l * (1.f - mix * 0.5f) + wl * mix * 3.f;
			r = r * (1.f - mix * 0.5f) + wr * mix * 3.f;
		}
		l = Soft(l);
		r = Soft(r);
	}

private:
	[[nodiscard]] static float Soft(float x) {
		const auto a = std::abs(x);
		if (a <= 0.85f) {
			return x;
		}
		const auto over = (a - 0.85f) / 0.15f;
		const auto shaped = 0.85f + 0.15f * std::tanh(over);
		return (x < 0.f) ? -shaped : shaped;
	}

	const Params _p;
	Biquad _eq[5];
	int _eqCount = 0;
	Biquad _bass;
	Delay _echoL;
	Delay _echoR;
	std::vector<Comb> _combL;
	std::vector<Comb> _combR;
	std::vector<Allpass> _allL;
	std::vector<Allpass> _allR;

};

// Rough tempo estimate: onset envelope autocorrelation, 70..180 BPM.
[[nodiscard]] int EstimateBpm(const Pcm &pcm) {
	constexpr auto kHop = 441; // 100 envelope values per second.
	const auto limit = std::min<int64>(pcm.frames(), int64(90) * kRate);
	const auto count = int(limit / kHop);
	if (count < 400) {
		return 0;
	}
	auto env = std::vector<float>(count, 0.f);
	for (auto i = 0; i != count; ++i) {
		auto sum = 0.f;
		for (auto j = 0; j != kHop; ++j) {
			const auto k = size_t(i) * kHop + j;
			const auto m = (pcm.l[k] + pcm.r[k]) * 0.5f;
			sum += m * m;
		}
		env[i] = std::sqrt(sum / kHop);
	}
	auto onset = std::vector<float>(count, 0.f);
	for (auto i = 1; i < count; ++i) {
		onset[i] = std::max(0.f, env[i] - env[i - 1]);
	}
	auto best = 0;
	auto bestScore = 0.f;
	for (auto bpm = 70; bpm <= 180; ++bpm) {
		const auto lag = int(std::round(6000.f / bpm));
		auto score = 0.f;
		for (auto i = lag; i < count; ++i) {
			score += onset[i] * onset[i - lag];
		}
		score /= float(count - lag);
		if (score > bestScore) {
			bestScore = score;
			best = bpm;
		}
	}
	return best;
}

[[nodiscard]] bool WriteWav(
		const QString &path,
		const Pcm &pcm,
		const Params &p,
		int64 fromFrame,
		int64 toFrame,
		Fn<void(int)> progress,
		const std::atomic<bool> &cancel) {
	const auto speed = p.speed();
	const auto total = std::min<int64>(toFrame, pcm.frames());
	fromFrame = std::clamp<int64>(fromFrame, 0, total);
	const auto outFrames = int64((total - fromFrame) / speed);
	if (outFrames <= 0) {
		return false;
	}
	QFile file(path);
	QDir().mkpath(QFileInfo(path).absolutePath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	const auto dataBytes = quint32(outFrames * 4);
	const auto put32 = [&](quint32 v) {
		const char b[4] = {
			char(v & 0xFF),
			char((v >> 8) & 0xFF),
			char((v >> 16) & 0xFF),
			char((v >> 24) & 0xFF) };
		file.write(b, 4);
	};
	const auto put16 = [&](quint16 v) {
		const char b[2] = { char(v & 0xFF), char((v >> 8) & 0xFF) };
		file.write(b, 2);
	};
	file.write("RIFF", 4);
	put32(36 + dataBytes);
	file.write("WAVEfmt ", 8);
	put32(16);
	put16(1);
	put16(2);
	put32(kRate);
	put32(kRate * 4);
	put16(4);
	put16(16);
	file.write("data", 4);
	put32(dataBytes);

	auto chain = Chain(p);
	constexpr auto kChunk = 1 << 14;
	auto chunk = std::vector<qint16>(size_t(kChunk) * 2);
	auto lastPercent = -1;
	for (auto done = int64(0); done < outFrames;) {
		if (cancel.load()) {
			file.close();
			file.remove();
			return false;
		}
		const auto take = int(std::min<int64>(kChunk, outFrames - done));
		for (auto i = 0; i != take; ++i) {
			// Linear-interpolation resampling: speed changes tempo + pitch.
			const auto pos = fromFrame + (done + i) * double(speed);
			const auto i0 = int64(pos);
			const auto frac = float(pos - i0);
			const auto i1 = std::min<int64>(i0 + 1, total - 1);
			auto l = pcm.l[i0] + (pcm.l[i1] - pcm.l[i0]) * frac;
			auto r = pcm.r[i0] + (pcm.r[i1] - pcm.r[i0]) * frac;
			chain.process(l, r, done + i);
			chunk[size_t(i) * 2] = qint16(std::clamp(l, -1.f, 1.f) * 32767.f);
			chunk[size_t(i) * 2 + 1] = qint16(std::clamp(r, -1.f, 1.f) * 32767.f);
		}
		file.write(
			reinterpret_cast<const char*>(chunk.data()),
			qint64(take) * 4);
		done += take;
		const auto percent = int(done * 100 / outFrames);
		if (progress && percent != lastPercent) {
			lastPercent = percent;
			progress(percent);
		}
	}
	file.close();
	return true;
}

// ---- Helpers ----

[[nodiscard]] QString WorkDir() {
	return QStandardPaths::writableLocation(QStandardPaths::TempLocation)
		+ u"/ParadoksMusicLab"_q;
}

[[nodiscard]] QString SafeName(QString name) {
	for (const auto bad : QString(u"\\/:*?\"<>|"_q)) {
		name.replace(bad, QChar('_'));
	}
	name = name.trimmed();
	return name.isEmpty() ? u"musiqa"_q : name.left(80);
}

[[nodiscard]] QString Duration(float seconds) {
	const auto s = int(std::round(seconds));
	return QString::number(s / 60) + ':' + QString::number(s % 60).rightJustified(2, '0');
}

void SendFile(
		not_null<Main::Session*> session,
		not_null<Data::Thread*> thread,
		const QString &path,
		const QString &caption) {
	auto list = Storage::PrepareMediaList(
		QStringList(path),
		256,
		session->premium());
	if (list.error != Ui::PreparedList::Error::None
		|| list.files.empty()) {
		Debug::Log(u"music lab send failed: cannot prepare "_q + path);
		return;
	}
	list.files.front().caption = TextWithTags{ caption };
	session->api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		Api::SendAction(thread));
}

struct State {
	QString path;
	QString name;
	Pcm pcm;
	Params params;
	int bpm = 0;
	bool loaded = false;
	bool busy = false;
	bool dirty = true;
	QString output;
	std::atomic<bool> cancel = false;
	std::atomic<bool> closed = false;
	rpl::variable<QString> status = QString();
};

void LabBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		const QString &path,
		const QString &name) {
	box->setTitle(rpl::single(u"Musiqiy ijodxona"_q));
	const auto state = std::make_shared<State>();
	state->path = path;
	state->name = name;
	state->status = u"Musiqa o'qilmoqda..."_q;
	box->lifetime().add([=] {
		state->closed = true;
		state->cancel = true;
	});
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);

	box->addRow(
		object_ptr<Ui::FlatLabel>(box, rpl::single(name), st::boxLabel),
		st::boxRowPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(box, state->status.value(), st::boxLabel),
		st::boxRowPadding);

	const auto markDirty = [=] {
		state->dirty = true;
	};
	const auto addToggle = [&](const QString &title, bool *flag) {
		const auto check = box->addRow(
			object_ptr<Ui::Checkbox>(box, title, *flag, st::defaultBoxCheckbox),
			st::boxRowPadding);
		check->checkedChanges() | rpl::on_next([=](bool value) {
			*flag = value;
			markDirty();
		}, check->lifetime());
	};
	const auto addSlider = [&](
			const QString &title,
			float min,
			float max,
			float *value,
			const QString &unit) {
		const auto label = box->addRow(
			object_ptr<Ui::FlatLabel>(box, rpl::single(QString()), st::boxLabel),
			st::boxRowPadding);
		const auto text = [=] {
			label->setText(
				title + u": "_q + QString::number(*value, 'f', 2) + unit);
		};
		text();
		const auto slider = box->addRow(
			object_ptr<Ui::MediaSlider>(box, st::settingsScale),
			st::boxRowPadding);
		slider->resize(slider->width(), st::settingsScale.seekSize.height());
		slider->setValue((*value - min) / (max - min));
		slider->setChangeProgressCallback([=](float64 progress) {
			*value = min + float(progress) * (max - min);
			text();
			markDirty();
		});
	};

	auto &p = state->params;
	addToggle(u"Karaoke (ovozni kamaytirish)"_q, &p.karaoke);
	addToggle(u"Bass boost"_q, &p.bass);
	addToggle(u"8D audio"_q, &p.eight);
	addToggle(u"Echo"_q, &p.echo);
	addToggle(u"Reverb"_q, &p.reverb);
	addToggle(u"Slow (sekinlashtirish)"_q, &p.slow);
	addToggle(u"Nightcore (tezlashtirish)"_q, &p.nightcore);
	addSlider(u"Bass"_q, 0.f, 18.f, &p.bassDb, u" dB"_q);
	addSlider(u"8D tezligi"_q, 0.05f, 0.6f, &p.eightHz, u" Hz"_q);
	addSlider(u"Echo vaqti"_q, 0.1f, 1.f, &p.echoTime, u" s"_q);
	addSlider(u"Reverb"_q, 0.f, 1.f, &p.reverbMix, QString());
	for (auto i = 0; i != 5; ++i) {
		addSlider(
			u"Equalizer "_q + QString::fromLatin1(kEqTitles[i]),
			-12.f,
			12.f,
			&p.eq[i],
			u" dB"_q);
	}

	// Decode in the background.
	crl::async([=] {
		auto pcm = Pcm();
		const auto ok = Decode(state->path, pcm, state->cancel);
		auto bpm = ok ? EstimateBpm(pcm) : 0;
		crl::on_main([=, pcm = std::move(pcm)]() mutable {
			if (state->closed) {
				return;
			}
			if (!ok) {
				state->status = u"Musiqani o'qib bo'lmadi"_q;
				return;
			}
			state->pcm = std::move(pcm);
			state->bpm = bpm;
			state->loaded = true;
			state->status = u"Tayyor: "_q
				+ Duration(state->pcm.seconds())
				+ (bpm ? (u"  •  ~"_q + QString::number(bpm) + u" BPM"_q) : QString());
		});
	});

	// Render to a WAV file, then hand the path to `done`.
	const auto render = [=](bool preview, Fn<void(QString)> done) {
		if (!state->loaded) {
			box->showToast(u"Musiqa hali o'qilmoqda"_q);
			return;
		} else if (state->busy) {
			return;
		}
		const auto reuse = !preview
			&& !state->dirty
			&& !state->output.isEmpty()
			&& QFile::exists(state->output);
		if (reuse) {
			done(state->output);
			return;
		}
		state->busy = true;
		state->status = u"Tayyorlanmoqda: 0%"_q;
		const auto params = state->params;
		const auto base = WorkDir() + '/' + SafeName(state->name);
		const auto target = preview
			? WorkDir() + u"/preview.wav"_q
			: base + u" (vibe).wav"_q;
		crl::async([=] {
			const auto frames = preview
				? int64(kPreviewSeconds) * kRate
				: state->pcm.frames();
			const auto ok = WriteWav(
				target,
				state->pcm,
				params,
				0,
				frames,
				[=](int percent) {
					crl::on_main([=] {
						if (!state->closed) {
							state->status = u"Tayyorlanmoqda: "_q
								+ QString::number(percent) + '%';
						}
					});
				},
				state->cancel);
			crl::on_main([=] {
				state->busy = false;
				if (state->closed) {
					return;
				}
				if (!ok) {
					state->status = u"Tayyorlab bo'lmadi"_q;
					return;
				}
				if (!preview) {
					state->output = target;
					state->dirty = false;
				}
				state->status = u"Tayyor: "_q + Describe(params);
				done(target);
			});
		});
	};

	box->addLeftButton(rpl::single(u"Eshitib ko'rish"_q), [=] {
		render(true, [=](QString file) {
			File::Launch(file);
		});
	});
	box->addButton(rpl::single(u"Saved Messages'ga yuklash"_q), [=] {
		render(false, [=](QString file) {
			const auto history = session->data().history(session->userPeerId());
			SendFile(session, history, file, u"🎧 "_q + Describe(state->params));
			box->showToast(u"Saved Messages'ga yuborildi"_q);
		});
	});
	box->addButton(rpl::single(u"Ulashish"_q), [=] {
		render(false, [=](QString file) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto caption = u"🎧 "_q + Describe(state->params);
			Window::ShowChooseRecipientBox(
				strong,
				[=](not_null<Data::Thread*> thread) {
					SendFile(session, thread, file, caption);
					return true;
				},
				rpl::single(u"Ulashish uchun chatni tanlang"_q));
		});
	});
}

[[nodiscard]] bool IsAvailable(not_null<DocumentData*> document) {
	return !document->filepath(true).isEmpty()
		|| document->createMediaView()->loaded();
}

[[nodiscard]] QString ResolvePath(
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	auto path = document->filepath(true);
	if (!path.isEmpty()) {
		return path;
	}
	// Cached (not saved) file: write its bytes to a temporary file.
	const auto media = document->createMediaView();
	const auto bytes = media->bytes();
	if (bytes.isEmpty()) {
		return QString();
	}
	QDir().mkpath(WorkDir());
	const auto ext = QFileInfo(document->filename()).suffix();
	path = WorkDir() + u"/source_"_q + QString::number(document->id)
		+ (ext.isEmpty() ? QString() : (u"."_q + ext));
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
		|| file.write(bytes) != bytes.size()) {
		return QString();
	}
	return path;
}

void WaitAndOpen(
		base::weak_ptr<Window::SessionController> weak,
		not_null<DocumentData*> document,
		FullMsgId contextId,
		int tries) {
	const auto controller = weak.get();
	if (!controller) {
		return;
	}
	const auto path = IsAvailable(document)
		? ResolvePath(document, contextId)
		: QString();
	if (!path.isEmpty()) {
		const auto name = Ui::Text::FormatSongNameFor(document).string();
		controller->show(Box(
			LabBox,
			controller,
			path,
			name.isEmpty() ? document->filename() : name));
		return;
	} else if (tries <= 0) {
		controller->showToast(u"Musiqani yuklab bo'lmadi"_q);
		return;
	}
	base::call_delayed(500, controller, [=] {
		WaitAndOpen(weak, document, contextId, tries - 1);
	});
}

} // namespace

void Open(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	if (!IsAvailable(document)) {
		controller->showToast(u"Musiqa yuklanmoqda..."_q);
		DocumentSaveClickHandler::SaveAndTrack(
			contextId,
			document,
			DocumentSaveClickHandler::Mode::ToCacheOrFile);
	}
	WaitAndOpen(base::make_weak(controller), document, contextId, 240);
}

void AddMenuAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	menu->addAction(u"Musiqiy ijodxonani ochish"_q, [=] {
		Open(controller, document, contextId);
	}, &st::menuIconMusicLab);
}

} // namespace Paradoks::MusicLab
