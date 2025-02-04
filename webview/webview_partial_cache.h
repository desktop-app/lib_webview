// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/basic_types.h"
#include "base/flat_map.h"

namespace Webview {

struct DataRequest;

class PartialCache final {
public:
	struct CachedFields {
		std::string mime;
		int64 total = 0;
	};
	[[nodiscard]] CachedFields fill(
		const DataRequest &request,
		Fn<void(const void *data, int64 size, int64 total)> record);
	void maybeAdd(
		const std::string &resourceId,
		int64 offset,
		int64 length,
		int64 total,
		const std::string &mime,
		std::unique_ptr<char[]> bytes);

private:
	using CacheKey = uint64;

	struct PartData {
		std::unique_ptr<char[]> bytes;
		int64 length = 0;
	};
	struct PartialResource {
		uint32 index = 0;
		uint32 total = 0;
		std::string mime;
	};

	void addToCache(uint32 resourceIndex, int64 offset, PartData data);
	void removeCacheEntry(CacheKey key);
	void pruneCache();

	[[nodiscard]] static CacheKey KeyFromValues(
		uint32 resourceIndex,
		int64 offset);
	[[nodiscard]] static uint32 ResourceIndexFromKey(CacheKey key);
	[[nodiscard]] static int64 OffsetFromKey(CacheKey key);

	base::flat_map<std::string, PartialResource> _partialResources;
	base::flat_map<CacheKey, PartData> _partsCache;
	std::vector<CacheKey> _partsLRU;
	int64 _cacheTotal = 0;

};

} // namespace Webview
