// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/basic_types.h"
#include "webview/webview_data_stream.h"

#include <QtCore/QByteArray>

namespace Webview {

class DataStreamFromMemory final : public DataStream {
public:
	DataStreamFromMemory(QByteArray data, std::string mime);

	[[nodiscard]] std::int64_t size() override;
	[[nodiscard]] std::string mime() override;

	std::int64_t seek(int origin, std::int64_t position) override;
	std::int64_t read(void *buffer, std::int64_t requested) override;

	[[nodiscard]] const char *bytes() const {
		return _data.data();
	}

private:
	QByteArray _data;
	std::string _mime;
	int64 _offset = 0;

};

} // namespace Webview
