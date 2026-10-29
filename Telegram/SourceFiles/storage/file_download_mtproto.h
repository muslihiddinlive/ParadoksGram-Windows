/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/file_download.h"
#include "storage/download_manager_mtproto.h"
#include "base/flat_map.h"

class mtpFileLoader final
	: public FileLoader
	, private Storage::DownloadMtprotoTask {
public:
	mtpFileLoader(
		not_null<Main::Session*> session,
		const StorageFileLocation &location,
		Data::FileOrigin origin,
		LocationType type,
		const QString &toFile,
		int64 loadSize,
		int64 fullSize,
		LoadToCacheSetting toCache,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);
	mtpFileLoader(
		not_null<Main::Session*> session,
		const WebFileLocation &location,
		int64 loadSize,
		int64 fullSize,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);
	mtpFileLoader(
		not_null<Main::Session*> session,
		const GeoPointLocation &location,
		int64 loadSize,
		int64 fullSize,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);
	mtpFileLoader(
		not_null<Main::Session*> session,
		const AudioAlbumThumbLocation &location,
		int64 loadSize,
		int64 fullSize,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);
	~mtpFileLoader();

	Data::FileOrigin fileOrigin() const override;
	uint64 objId() const override;

private:
	Storage::Cache::Key cacheKey() const override;
	std::optional<MediaKey> fileLocationKey() const override;
	void startLoading() override;
	void startLoadingWithPartial(const QByteArray &data) override;
	void cancelHook() override;

	bool readyToRequest() const override;
	int partSize() const override;
	bool cdnSupported() const override;
	void keepPartialFile() override;
	[[nodiscard]] bool tryResumeFromPart();
	void removePartFiles();
	void noteDone(int64 offset, int64 size);
	int64 takeNextRequestOffset() override;
	bool feedPart(int64 offset, const QByteArray &bytes) override;
	void cancelOnFail() override;
	bool setWebFileSizeHook(int64 size) override;

	int _partSize = Storage::kDownloadPartSize;
	// Length of the contiguous downloaded prefix of the file.
	int64 _contiguous = 0;
	base::flat_map<int64, int64> _ahead;
	bool _resumed = false;
	UserId _userId = 0;
	bool _lastComplete = false;
	bool _paradoksResume = false;
	int64 _nextRequestOffset = 0;

};
