// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/webview_data_stream_memory.h"

namespace Webview {

DataStreamFromMemory::DataStreamFromMemory(
	QByteArray data,
	std::string mime)
: _data(data)
, _mime(mime) {
}

std::int64_t DataStreamFromMemory::size() {
	return _data.size();
}

std::string DataStreamFromMemory::mime() {
	return _mime;
}

std::int64_t DataStreamFromMemory::seek(
		int origin,
		std::int64_t position) {
	const auto length = size();
	switch (origin) {
	case SEEK_SET:
		return (position >= 0 && position <= length)
			? ((_offset = position))
			: -1;
	case SEEK_CUR:
		return (_offset + position >= 0 && _offset + position <= length)
			? ((_offset += position))
			: -1;
	case SEEK_END:
		return (length + position >= 0 && position <= 0)
			? ((_offset = length + position))
			: -1;
	}
	return -1;
}

std::int64_t DataStreamFromMemory::read(
		void *buffer,
		std::int64_t requested) {
	if (requested < 0) {
		return -1;
	}
	const auto copy = std::min(std::int64_t(size() - _offset), requested);
	if (copy > 0) {
		memcpy(buffer, _data.constData() + _offset, copy);
		_offset += copy;
	}
	return copy;
}

} // namespace Webview
