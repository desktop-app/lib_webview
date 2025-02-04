// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/webview_partial_cache.h"

#include "base/assertion.h"
#include "webview/webview_interface.h"

namespace Webview {
namespace {

constexpr auto kPartsCacheLimit = 32 * 1024 * 1024;

[[nodiscard]] std::unique_ptr<char[]> WrapBytes(
		const char *data,
		int64 length) {
	Expects(length > 0);

	auto result = std::unique_ptr<char[]>(new char[length]);
	memcpy(result.get(), data, length);
	return result;
}

} // namespace

auto PartialCache::fill(
	const DataRequest &request,
	Fn<void(const void *data, int64 size, int64 total)> record)
-> CachedFields {
	auto &partial = _partialResources[request.id];
	const auto index = partial.index;
	if (!index) {
		partial.index = uint32(_partialResources.size());
		return {};
	}
	auto i = _partsCache.upper_bound(
		KeyFromValues(partial.index, request.offset));
	if (i == begin(_partsCache)) {
		return {};
	}
	--i;
	if (ResourceIndexFromKey(i->first) != index) {
		return {};
	}
	const auto alreadyOffset = OffsetFromKey(i->first);
	const auto alreadyTill = alreadyOffset + i->second.length;
	if (alreadyTill <= request.offset) {
		return {};
	}
	auto till = alreadyTill;
	for (auto j = i + 1; j != end(_partsCache); ++j) {
		const auto offset = OffsetFromKey(j->first);
		if (ResourceIndexFromKey(j->first) != index || offset > till) {
			break;
		}
		till = offset + j->second.length;
		if (request.limit <= 0 || till >= request.offset + request.limit) {
			break;
		}
	}
	const auto length = (request.limit > 0)
		? request.limit
		: (till - request.offset);
	if (till < request.offset + length) {
		return {};
	}
	auto from = request.offset;
	auto fill = length;
	for (auto j = i; j != end(_partsCache); ++j) {
		const auto offset = OffsetFromKey(j->first);
		const auto copy = std::min(fill, offset + j->second.length - from);
		Assert(copy > 0);
		Assert(from >= offset);
		record(j->second.bytes.get() + (from - offset), copy, length);
		from += copy;
		fill -= copy;

		const auto lru = std::find(begin(_partsLRU), end(_partsLRU), j->first);
		Assert(lru != end(_partsLRU));
		if (const auto next = lru + 1; next != end(_partsLRU)) {
			std::rotate(lru, next, end(_partsLRU));
		}

		if (!fill) {
			break;
		}
		Assert(fill > 0);
	}
	Assert(fill == 0);
	return { .mime = partial.mime, .total = partial.total };
}

void PartialCache::maybeAdd(
		const std::string &resourceId,
		int64 offset,
		int64 length,
		int64 total,
		const std::string &mime,
		std::unique_ptr<char[]> bytes) {
	const auto i = _partialResources.find(resourceId);
	if (i == end(_partialResources)) {
		return;
	}
	auto &partial = i->second;
	if (partial.mime.empty()) {
		partial.mime = mime;
	}
	if (!partial.total) {
		partial.total = total;
	}
	addToCache(partial.index, offset, { std::move(bytes), length });
}

auto PartialCache::KeyFromValues(
	uint32 resourceIndex,
	int64 offset)
-> CacheKey {
	return (uint64(resourceIndex) << 32) | uint32(offset);
}

uint32 PartialCache::ResourceIndexFromKey(CacheKey key) {
	return uint32(key >> 32);
}

int64 PartialCache::OffsetFromKey(CacheKey key) {
	return int64(key & 0xFFFFFFFFULL);
}

void PartialCache::addToCache(uint32 resourceIndex, int64 offset, PartData data) {
	auto key = KeyFromValues(resourceIndex, offset);
	while (true) { // Remove parts that are already in cache.
		auto i = _partsCache.upper_bound(key);
		if (i != begin(_partsCache)) {
			--i;
			const auto alreadyIndex = ResourceIndexFromKey(i->first);
			if (alreadyIndex == resourceIndex) {
				const auto &already = i->second;
				const auto alreadyOffset = OffsetFromKey(i->first);
				const auto alreadyTill = alreadyOffset + already.length;
				if (alreadyTill >= offset + data.length) {
					return; // Fully in cache.
				} else if (alreadyTill > offset) {
					const auto delta = alreadyTill - offset;
					offset += delta;
					data.length -= delta;
					data.bytes = WrapBytes(data.bytes.get() + delta, data.length);
					key = KeyFromValues(resourceIndex, offset);
					continue;
				}
			}
			++i;
		}
		if (i != end(_partsCache)) {
			const auto alreadyIndex = ResourceIndexFromKey(i->first);
			if (alreadyIndex == resourceIndex) {
				const auto &already = i->second;
				const auto alreadyOffset = OffsetFromKey(i->first);
				Assert(alreadyOffset > offset);
				const auto alreadyTill = alreadyOffset + already.length;
				if (alreadyTill <= offset + data.length) {
					removeCacheEntry(i->first);
					continue;
				} else if (alreadyOffset < offset + data.length) {
					const auto delta = offset + data.length - alreadyOffset;
					data.length -= delta;
					data.bytes = WrapBytes(data.bytes.get(), data.length);
					continue;
				}
			}
		}
		break;
	}
	_partsLRU.push_back(key);
	_cacheTotal += data.length;
	_partsCache[key] = std::move(data);
	pruneCache();
}

void PartialCache::pruneCache() {
	while (_cacheTotal > kPartsCacheLimit) {
		Assert(!_partsLRU.empty());
		removeCacheEntry(_partsLRU.front());
	}
}

void PartialCache::removeCacheEntry(CacheKey key) {
	auto &part = _partsCache[key];
	Assert(part.length > 0);
	Assert(_cacheTotal >= part.length);
	_cacheTotal -= part.length;
	_partsCache.remove(key);
	_partsLRU.erase(
		std::remove(begin(_partsLRU), end(_partsLRU), key),
		end(_partsLRU));
}

} // namespace Webview
