// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <cinttypes>
#include <string>

namespace Webview {

class DataStream {
public:
	virtual ~DataStream() = default;

	[[nodiscard]] virtual std::int64_t size() = 0;
	[[nodiscard]] virtual std::string mime() = 0;

	virtual std::int64_t seek(int origin, std::int64_t position) = 0;
	virtual std::int64_t read(void *buffer, std::int64_t requested) = 0;
};

} // namespace Webview
