/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_stream.h"

#include "paradoks/paradoks_config.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "history/history.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <utility>

namespace Paradoks::Stream {
namespace {

constexpr auto kIdleFinish = crl::time(45000);
constexpr auto kMinParts = 3;
constexpr auto kMaxRawBytes = int64(1200) * 1024 * 1024;
constexpr auto kKeepDays = 7;
constexpr auto kMaxProbedSegments = 10;

struct Segment {
	int64 time = 0;
	QString path;
};

struct Job {
	uint64 callId = 0;
	QString dir;
	QString title;
	std::vector<Segment> audio;
	std::vector<Segment> video;
	int64 firstTime = 0;
	int64 lastTime = 0;
	base::weak_ptr<Main::Session> session;
};

struct Recording {
	Recording() = default;

	uint64 callId = 0;
	base::weak_ptr<Main::Session> session;
	QString dir;
	QString title;
	// (channel, quality) -> time -> file
	std::map<std::pair<int, int>, std::map<int64, QString>> groups;
	int64 rawBytes = 0;
	base::Timer idle;
};

std::map<uint64, std::unique_ptr<Recording>> Active;

[[nodiscard]] QString StreamFolder() {
	return cWorkingDir() + u"tdata/paradoks_streams/"_q;
}

[[nodiscard]] QString SafeTitle(QString title) {
	auto result = QString();
	for (const auto &ch : title) {
		if (ch.unicode() < 32
			|| QString(u"\\/:*?\"<>|"_q).contains(ch)) {
			result += '_';
		} else {
			result += ch;
		}
	}
	result = result.trimmed();
	if (result.size() > 40) {
		result = result.left(40).trimmed();
	}
	return result;
}

struct InputCloser {
	void operator()(AVFormatContext *context) const {
		if (context) {
			avformat_close_input(&context);
		}
	}
};
using InputPtr = std::unique_ptr<AVFormatContext, InputCloser>;

struct PacketDeleter {
	void operator()(AVPacket *packet) const {
		if (packet) {
			av_packet_free(&packet);
		}
	}
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

[[nodiscard]] InputPtr OpenInput(const QString &path) {
	auto context = (AVFormatContext*)nullptr;
	const auto utf8 = path.toUtf8();
	if (avformat_open_input(&context, utf8.constData(), nullptr, nullptr)
		< 0) {
		return nullptr;
	}
	auto result = InputPtr(context);
	if (avformat_find_stream_info(result.get(), nullptr) < 0) {
		return nullptr;
	}
	return result;
}

class Output {
public:
	explicit Output(AVFormatContext *context) : _context(context) {
	}
	Output(const Output &other) = delete;
	Output &operator=(const Output &other) = delete;
	~Output() {
		if (_context) {
			if (_context->pb) {
				avio_closep(&_context->pb);
			}
			avformat_free_context(_context);
		}
	}

	[[nodiscard]] AVFormatContext *get() const {
		return _context;
	}

private:
	AVFormatContext *_context = nullptr;

};

// Joins audio and video parts of the stream into one Matroska file placing
// every part at its own time offset. Returns false if nothing was written.
[[nodiscard]] bool Remux(
		const Job &job,
		const QString &outBase,
		QString &outPath) {
	auto all = std::vector<Segment>();
	all.reserve(job.audio.size() + job.video.size());
	all.insert(end(all), begin(job.audio), end(job.audio));
	all.insert(end(all), begin(job.video), end(job.video));
	if (all.empty()) {
		return false;
	}
	std::stable_sort(begin(all), end(all), [](
			const Segment &a,
			const Segment &b) {
		return a.time < b.time;
	});

	// The build has only mp4 and webm muxers: webm when the codecs fit.
	auto audioCodec = AV_CODEC_ID_NONE;
	auto videoCodec = AV_CODEC_ID_NONE;
	auto probedCodecs = 0;
	for (const auto &segment : all) {
		if ((audioCodec != AV_CODEC_ID_NONE
			&& videoCodec != AV_CODEC_ID_NONE)
			|| (++probedCodecs > kMaxProbedSegments)) {
			break;
		}
		const auto in = OpenInput(segment.path);
		if (!in) {
			continue;
		}
		for (auto i = 0u; i != in->nb_streams; ++i) {
			const auto params = in->streams[i]->codecpar;
			if (params->codec_type == AVMEDIA_TYPE_AUDIO
				&& audioCodec == AV_CODEC_ID_NONE) {
				audioCodec = params->codec_id;
			} else if (params->codec_type == AVMEDIA_TYPE_VIDEO
				&& videoCodec == AV_CODEC_ID_NONE) {
				videoCodec = params->codec_id;
			}
		}
	}
	const auto webm = (videoCodec == AV_CODEC_ID_NONE
		|| videoCodec == AV_CODEC_ID_VP8
		|| videoCodec == AV_CODEC_ID_VP9
		|| videoCodec == AV_CODEC_ID_AV1)
		&& (audioCodec == AV_CODEC_ID_NONE
			|| audioCodec == AV_CODEC_ID_OPUS
			|| audioCodec == AV_CODEC_ID_VORBIS);
	outPath = outBase + (webm ? u".webm"_q : u".mp4"_q);

	auto raw = (AVFormatContext*)nullptr;
	if (avformat_alloc_output_context2(
		&raw,
		nullptr,
		webm ? "webm" : "mp4",
		nullptr) < 0 || !raw) {
		return false;
	}
	auto out = Output(raw);

	auto audioOut = -1;
	auto videoOut = -1;
	const auto addStreams = [&](const QString &path) {
		const auto in = OpenInput(path);
		if (!in) {
			return;
		}
		for (auto i = 0u; i != in->nb_streams; ++i) {
			const auto params = in->streams[i]->codecpar;
			auto slot = (int*)nullptr;
			if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
				slot = &audioOut;
			} else if (params->codec_type == AVMEDIA_TYPE_VIDEO) {
				slot = &videoOut;
			} else {
				continue;
			}
			if (*slot >= 0) {
				continue;
			}
			const auto stream = avformat_new_stream(out.get(), nullptr);
			if (!stream) {
				continue;
			}
			if (avcodec_parameters_copy(stream->codecpar, params) < 0) {
				continue;
			}
			stream->codecpar->codec_tag = 0;
			*slot = stream->index;
		}
	};
	auto probed = 0;
	for (const auto &segment : all) {
		if ((audioOut >= 0 && videoOut >= 0)
			|| (++probed > kMaxProbedSegments)) {
			break;
		}
		addStreams(segment.path);
	}
	if (audioOut < 0 && videoOut < 0) {
		return false;
	}

	const auto utf8 = outPath.toUtf8();
	if (avio_open(&out.get()->pb, utf8.constData(), AVIO_FLAG_WRITE) < 0) {
		return false;
	}
	if (avformat_write_header(out.get(), nullptr) < 0) {
		return false;
	}

	auto lastDts = std::vector<int64_t>(
		out.get()->nb_streams,
		std::numeric_limits<int64_t>::min());
	const auto firstTime = all.front().time;
	const auto packet = PacketPtr(av_packet_alloc());
	if (!packet) {
		return false;
	}
	auto written = 0;
	for (const auto &segment : all) {
		const auto in = OpenInput(segment.path);
		if (!in) {
			continue;
		}
		auto map = std::vector<int>(in->nb_streams, -1);
		for (auto i = 0u; i != in->nb_streams; ++i) {
			const auto type = in->streams[i]->codecpar->codec_type;
			if (type == AVMEDIA_TYPE_AUDIO) {
				map[i] = audioOut;
			} else if (type == AVMEDIA_TYPE_VIDEO) {
				map[i] = videoOut;
			}
		}
		const auto offsetMs = segment.time - firstTime;
		while (av_read_frame(in.get(), packet.get()) >= 0) {
			const auto index = packet->stream_index;
			const auto target = (index >= 0 && index < int(map.size()))
				? map[index]
				: -1;
			if (target < 0) {
				av_packet_unref(packet.get());
				continue;
			}
			const auto ist = in->streams[index];
			const auto ost = out.get()->streams[target];
			const auto base = (ist->start_time != AV_NOPTS_VALUE)
				? ist->start_time
				: int64_t(0);
			const auto offset = av_rescale_q(
				offsetMs,
				AVRational{ 1, 1000 },
				ost->time_base);
			auto pts = (packet->pts != AV_NOPTS_VALUE)
				? (av_rescale_q(
					packet->pts - base,
					ist->time_base,
					ost->time_base) + offset)
				: AV_NOPTS_VALUE;
			auto dts = (packet->dts != AV_NOPTS_VALUE)
				? (av_rescale_q(
					packet->dts - base,
					ist->time_base,
					ost->time_base) + offset)
				: pts;
			if (dts == AV_NOPTS_VALUE) {
				av_packet_unref(packet.get());
				continue;
			}
			if (dts < 0) {
				dts = 0;
			}
			if (dts <= lastDts[target]) {
				dts = lastDts[target] + 1;
			}
			if (pts == AV_NOPTS_VALUE || pts < dts) {
				pts = dts;
			}
			lastDts[target] = dts;
			packet->stream_index = target;
			packet->pts = pts;
			packet->dts = dts;
			packet->duration = av_rescale_q(
				packet->duration,
				ist->time_base,
				ost->time_base);
			packet->pos = -1;
			if (av_interleaved_write_frame(out.get(), packet.get()) >= 0) {
				++written;
			}
			av_packet_unref(packet.get());
		}
	}
	av_write_trailer(out.get());
	return (written > 0);
}

void SendToSaved(
		not_null<Main::Session*> session,
		const QString &path,
		const QString &caption) {
	auto list = Storage::PrepareMediaList(
		QStringList(path),
		256,
		session->premium());
	if (list.error != Ui::PreparedList::Error::None
		|| list.files.empty()) {
		return;
	}
	list.files.front().caption = TextWithTags{ caption };
	session->api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		Api::SendAction(session->data().history(session->userPeerId())));
}

void RemoveRawParts(const QString &dir) {
	const auto parts = QDir(dir).entryList(
		QStringList(u"*.part"_q),
		QDir::Files);
	for (const auto &name : parts) {
		QFile::remove(dir + '/' + name);
	}
}

void Run(Job job) {
	crl::async([job = std::move(job)] {
		auto temp = QString();
		const auto ok = Remux(job, job.dir + u"/rec"_q, temp);
		auto finalPath = QString();
		if (ok) {
			auto name = u"Efir"_q;
			const auto title = SafeTitle(job.title);
			if (!title.isEmpty()) {
				name += '_' + title;
			}
			name += '_' + QDateTime::currentDateTime().toString(
				u"yyyy-MM-dd_HH-mm"_q);
			finalPath = job.dir
				+ '/'
				+ name
				+ (temp.endsWith(u".webm"_q) ? u".webm"_q : u".mp4"_q);
			if (!QFile::rename(temp, finalPath)) {
				finalPath = temp;
			}
			RemoveRawParts(job.dir);
		} else {
			if (!temp.isEmpty()) {
				QFile::remove(temp);
			}
			QDir(job.dir).removeRecursively();
		}
		if (!ok) {
			return;
		}
		const auto seconds = int((job.lastTime - job.firstTime) / 1000) + 1;
		auto caption = u"📡 Efir yozuvi"_q;
		if (!job.title.isEmpty()) {
			caption += u": "_q + job.title;
		}
		caption += u"\n"_q
			+ QString::number(seconds / 60)
			+ u" daqiqa "_q
			+ QString::number(seconds % 60)
			+ u" soniya"_q;
		const auto weak = job.session;
		crl::on_main(weak, [=] {
			if (const auto session = weak.get()) {
				SendToSaved(session, finalPath, caption);
			}
		});
	});
}

void Finish(uint64 callId) {
	const auto i = Active.find(callId);
	if (i == end(Active)) {
		return;
	}
	auto owned = std::move(i->second);
	Active.erase(i);
	owned->idle.cancel();

	auto job = Job{
		.callId = callId,
		.dir = owned->dir,
		.title = owned->title,
		.session = owned->session,
	};
	// Destroy the timer later, we may be inside of its callback.
	const auto holder = std::make_shared<std::unique_ptr<Recording>>(
		std::move(owned));
	crl::on_main([holder] {
		holder->reset();
	});
	const auto &rec = **holder;

	const auto audio = rec.groups.find({ 0, 0 });
	auto bestVideo = rec.groups.end();
	for (auto j = rec.groups.begin(); j != rec.groups.end(); ++j) {
		if (j->first.first <= 0) {
			continue;
		}
		if (bestVideo == rec.groups.end()
			|| j->second.size() > bestVideo->second.size()
			|| (j->second.size() == bestVideo->second.size()
				&& j->first.second > bestVideo->first.second)) {
			bestVideo = j;
		}
	}
	auto first = std::numeric_limits<int64>::max();
	auto last = int64(0);
	const auto take = [&](
			const std::map<int64, QString> &list,
			std::vector<Segment> &to) {
		for (const auto &[time, path] : list) {
			to.push_back({ time, path });
			first = std::min(first, time);
			last = std::max(last, time);
		}
	};
	if (audio != rec.groups.end()) {
		take(audio->second, job.audio);
	}
	if (bestVideo != rec.groups.end()) {
		take(bestVideo->second, job.video);
	}
	if (int(job.audio.size() + job.video.size()) < kMinParts) {
		QDir(job.dir).removeRecursively();
		return;
	}
	job.firstTime = first;
	job.lastTime = last;
	Run(std::move(job));
}

} // namespace

void OnPart(
		not_null<Main::Session*> session,
		uint64 callId,
		uint64 peerId,
		const QString &title,
		bool canSave,
		int64 timeMs,
		int videoChannel,
		int videoQuality,
		const std::vector<uint8_t> &data) {
	Q_UNUSED(peerId);
	if (!Enabled(Flag::RecordStreams) || !canSave || data.empty()) {
		return;
	}
	auto &slot = Active[callId];
	if (!slot) {
		slot = std::make_unique<Recording>();
		slot->callId = callId;
		slot->session = base::make_weak(session);
		slot->dir = StreamFolder()
			+ QString::number(callId)
			+ '_'
			+ QString::number(QDateTime::currentSecsSinceEpoch());
		QDir().mkpath(slot->dir);
		slot->idle.setCallback([=] {
			Finish(callId);
		});
	}
	const auto rec = slot.get();
	if (!title.isEmpty()) {
		rec->title = title;
	}
	const auto channel = std::max(videoChannel, 0);
	const auto quality = channel ? videoQuality : 0;
	auto &group = rec->groups[{ channel, quality }];
	if (group.find(timeMs) == group.end()) {
		const auto path = rec->dir
			+ '/'
			+ QString::number(timeMs)
			+ u"_c"_q + QString::number(channel)
			+ u"_q"_q + QString::number(quality)
			+ u".part"_q;
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(
				reinterpret_cast<const char*>(data.data()),
				qint64(data.size()));
			file.close();
			group[timeMs] = path;
			rec->rawBytes += int64(data.size());
		}
	}
	if (rec->rawBytes >= kMaxRawBytes) {
		Finish(callId);
		return;
	}
	rec->idle.callOnce(kIdleFinish);
}

void OnCallEnded(uint64 callId) {
	Finish(callId);
}

void Cleanup() {
	const auto root = QDir(StreamFolder());
	if (!root.exists()) {
		return;
	}
	const auto limit = QDateTime::currentDateTime().addDays(-kKeepDays);
	const auto dirs = root.entryList(
		QDir::Dirs | QDir::NoDotAndDotDot);
	for (const auto &name : dirs) {
		const auto path = StreamFolder() + name;
		if (QFileInfo(path).lastModified() < limit) {
			QDir(path).removeRecursively();
		}
	}
}

} // namespace Paradoks::Stream
