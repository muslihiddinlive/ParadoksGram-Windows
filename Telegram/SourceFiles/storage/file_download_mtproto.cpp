/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/file_download_mtproto.h"

#include "paradoks/paradoks_config.h"
#include "paradoks/paradoks_debug.h"

#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "storage/cache/storage_cache_types.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/mtproto_auth_key.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>

mtpFileLoader::mtpFileLoader(
	not_null<Main::Session*> session,
	const StorageFileLocation &location,
	Data::FileOrigin origin,
	LocationType type,
	const QString &to,
	int64 loadSize,
	int64 fullSize,
	LoadToCacheSetting toCache,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	to,
	loadSize,
	fullSize,
	type,
	toCache,
	fromCloud,
	autoLoading,
	cacheTag)
, DownloadMtprotoTask(&session->downloader(), location, origin) {
	// Big documents are downloaded with 1 MB parts (the server maximum).
	constexpr auto kBigDocumentFrom = int64(4 * 1024 * 1024);
	if (location.type() == StorageFileLocation::Type::Document
		&& fullSize >= kBigDocumentFrom
		&& loadSize >= kBigDocumentFrom
		&& Paradoks::StringSetting(u"download/bigparts"_q) != u"0"_q) {
		_partSize = Storage::kBigDownloadPartSize;
	}
}

mtpFileLoader::mtpFileLoader(
	not_null<Main::Session*> session,
	const WebFileLocation &location,
	int64 loadSize,
	int64 fullSize,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	QString(),
	loadSize,
	fullSize,
	UnknownFileLocation,
	LoadToCacheAsWell,
	fromCloud,
	autoLoading,
	cacheTag)
, DownloadMtprotoTask(
	&session->downloader(),
	session->serverConfig().webFileDcId,
	{ location }) {
}

mtpFileLoader::mtpFileLoader(
	not_null<Main::Session*> session,
	const GeoPointLocation &location,
	int64 loadSize,
	int64 fullSize,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	QString(),
	loadSize,
	fullSize,
	UnknownFileLocation,
	LoadToCacheAsWell,
	fromCloud,
	autoLoading,
	cacheTag)
, DownloadMtprotoTask(
	&session->downloader(),
	session->serverConfig().webFileDcId,
	{ location }) {
}

mtpFileLoader::mtpFileLoader(
	not_null<Main::Session*> session,
	const AudioAlbumThumbLocation &location,
	int64 loadSize,
	int64 fullSize,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	QString(),
	loadSize,
	fullSize,
	UnknownFileLocation,
	LoadToCacheAsWell,
	fromCloud,
	autoLoading,
	cacheTag)
, DownloadMtprotoTask(
	&session->downloader(),
	session->serverConfig().webFileDcId,
	{ location }) {
}

namespace {

// ParadoksGram: a failed download keeps what was downloaded in a side file
// and the next attempt continues from there.
constexpr auto kResumeAlign = 4096;
constexpr auto kResumeMinimum = int64(1024 * 1024);

[[nodiscard]] QString PartPath(const QString &filename) {
	return filename + u".paradoks-part"_q;
}

[[nodiscard]] QString MetaPath(const QString &filename) {
	return PartPath(filename) + u".meta"_q;
}

} // namespace

mtpFileLoader::~mtpFileLoader() {
	if (!_finished) {
		_keepFileOnCancel = true;
		cancel();
	}
}

void mtpFileLoader::noteDone(int64 offset, int64 size) {
	if (size <= 0 || offset < _contiguous) {
		return;
	}
	if (offset > _contiguous) {
		_ahead.emplace(offset, size);
		return;
	}
	_contiguous += size;
	while (!_ahead.empty() && _ahead.begin()->first <= _contiguous) {
		_contiguous = std::max(
			_contiguous,
			_ahead.begin()->first + _ahead.begin()->second);
		_ahead.erase(_ahead.begin());
	}
}

void mtpFileLoader::removePartFiles() {
	if (_filename.isEmpty()) {
		return;
	}
	QFile::remove(PartPath(_filename));
	QFile::remove(MetaPath(_filename));
}

void mtpFileLoader::keepPartialFile() {
	const auto filename = _file.fileName();
	const auto length = _contiguous - (_contiguous % kResumeAlign);
	if (filename.isEmpty()
		|| length < kResumeMinimum
		|| !_fullSize
		|| _loadSize != _fullSize
		|| length >= _fullSize) {
		_file.remove();
		return;
	}
	const auto part = PartPath(filename);
	QFile::remove(part);
	QFile::remove(MetaPath(filename));
	if (!QFile::rename(filename, part)) {
		_file.remove();
		return;
	}
	auto meta = QFile(MetaPath(filename));
	if (meta.open(QIODevice::WriteOnly)) {
		const auto key = cacheKey();
		meta.write((u"pg1 %1 %2 %3 %4"_q
			).arg(key.high
			).arg(key.low
			).arg(_fullSize
			).arg(length).toUtf8());
		meta.close();
		Paradoks::Debug::Log((u"download: kept %1 of %2 bytes for resume"_q
			).arg(length
			).arg(_fullSize));
	} else {
		QFile::remove(part);
	}
}

bool mtpFileLoader::tryResumeFromPart() {
	if (!_fileIsOpen
		|| _filename.isEmpty()
		|| !_fullSize
		|| _loadSize != _fullSize
		|| _nextRequestOffset != 0
		|| _contiguous != 0) {
		return true;
	}
	const auto part = PartPath(_filename);
	auto meta = QFile(MetaPath(_filename));
	if (!QFile::exists(part) || !meta.open(QIODevice::ReadOnly)) {
		return true;
	}
	const auto fields = QString::fromUtf8(meta.readAll()).split(' ');
	meta.close();
	const auto key = cacheKey();
	auto valid = (fields.size() == 5) && (fields[0] == u"pg1"_q);
	const auto length = valid ? fields[4].toLongLong() : int64();
	if (valid) {
		valid = (fields[1].toULongLong() == uint64(key.high))
			&& (fields[2].toULongLong() == uint64(key.low))
			&& (fields[3].toLongLong() == _fullSize)
			&& (length >= kResumeMinimum)
			&& !(length % kResumeAlign)
			&& (length < _fullSize)
			&& (QFileInfo(part).size() >= length);
	}
	if (!valid) {
		QFile::remove(part);
		QFile::remove(MetaPath(_filename));
		return true;
	}
	_file.close();
	_fileIsOpen = false;
	QFile::remove(_filename);
	if (QFile::rename(part, _filename)
		&& _file.open(QIODevice::ReadWrite)
		&& _file.resize(length)) {
		_fileIsOpen = true;
		_contiguous = length;
		_nextRequestOffset = length;
		_resumed = true;
		_skippedBytes = 0;
		QFile::remove(MetaPath(_filename));
		Paradoks::Debug::Log((u"download: resumed from %1 of %2 bytes"_q
			).arg(length
			).arg(_fullSize));
		notifyAboutProgress();
		return true;
	}
	// Could not resume, start from scratch.
	_file.close();
	QFile::remove(_filename);
	QFile::remove(part);
	QFile::remove(MetaPath(_filename));
	_fileIsOpen = _file.open(QIODevice::WriteOnly);
	if (!_fileIsOpen) {
		cancel(FailureReason::FileWriteFailure);
		return false;
	}
	return true;
}

Data::FileOrigin mtpFileLoader::fileOrigin() const {
	return DownloadMtprotoTask::fileOrigin();
}

uint64 mtpFileLoader::objId() const {
	return DownloadMtprotoTask::objectId();
}

bool mtpFileLoader::readyToRequest() const {
	return !_finished
		&& !_lastComplete
		&& (_fullSize != 0 || !haveSentRequests())
		&& (!_fullSize || _nextRequestOffset < _loadSize);
}

int mtpFileLoader::partSize() const {
	// After a resume the offset is aligned only to 4 KB, so the first
	// requests are smaller, aligned pieces up to the next big part boundary.
	auto size = _partSize;
	while (size > kResumeAlign && (_nextRequestOffset % size)) {
		size /= 2;
	}
	return size;
}

bool mtpFileLoader::cdnSupported() const {
	return !_resumed && (_partSize == Storage::kDownloadPartSize);
}

int64 mtpFileLoader::takeNextRequestOffset() {
	Expects(readyToRequest());

	const auto result = _nextRequestOffset;
	_nextRequestOffset += _partSize;
	return result;
}

bool mtpFileLoader::feedPart(int64 offset, const QByteArray &bytes) {
	if (!_paradoksResume) {
		Paradoks::NoteDownloaded(bytes.size());
	}
	const auto buffer = bytes::make_span(bytes);
	if (!writeResultPart(offset, buffer)) {
		return false;
	}
	noteDone(offset, buffer.size());
	if (buffer.empty() || (buffer.size() % 1024)) { // bad next offset
		_lastComplete = true;
	}
	const auto finished = !haveSentRequests()
		&& (_lastComplete || (_fullSize && _nextRequestOffset >= _loadSize));
	if (finished) {
		removeFromQueue();
		removePartFiles();
		if (!finalizeResult()) {
			return false;
		}
	} else {
		notifyAboutProgress();
	}
	return true;
}

void mtpFileLoader::cancelOnFail() {
	_keepFileOnCancel = true;
	cancel(FailureReason::OtherFailure);
}

bool mtpFileLoader::setWebFileSizeHook(int64 size) {
	if (!_fullSize || _fullSize == size) {
		_fullSize = _loadSize = size;
		return true;
	}
	LOG(("MTP Error: "
		"Bad size provided by bot for webDocument: %1, real: %2"
		).arg(_fullSize
		).arg(size));
	cancel(FailureReason::OtherFailure);
	return false;
}

void mtpFileLoader::startLoading() {
	if (!tryResumeFromPart()) {
		return;
	}
	addToQueue();
}

void mtpFileLoader::startLoadingWithPartial(const QByteArray &data) {
	Expects(data.startsWith("partial:"));

	constexpr auto kPrefix = 8;
	const auto parts = (data.size() - kPrefix) / _partSize;
	const auto use = parts * int64(_partSize);
	if (use > 0) {
		_nextRequestOffset = use;
		_paradoksResume = true;
		feedPart(0, QByteArray::fromRawData(data.data() + kPrefix, use));
		_paradoksResume = false;
	}
	startLoading();
}

void mtpFileLoader::cancelHook() {
	cancelAllRequests();
}

Storage::Cache::Key mtpFileLoader::cacheKey() const {
	return v::match(location().data, [&](const WebFileLocation &location) {
		return Data::WebDocumentCacheKey(location);
	}, [&](const GeoPointLocation &location) {
		return Data::GeoPointCacheKey(location);
	}, [&](const StorageFileLocation &location) {
		return location.cacheKey();
	}, [&](const AudioAlbumThumbLocation &location) {
		return Data::AudioAlbumThumbCacheKey(location);
	});
}

std::optional<MediaKey> mtpFileLoader::fileLocationKey() const {
	if (_locationType != UnknownFileLocation) {
		return mediaKey(_locationType, dcId(), objId());
	}
	return std::nullopt;
}
