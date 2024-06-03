// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/webview_data_stream_memory.h"

#if !defined Q_OS_WIN && !defined Q_OS_MAC
#include <sys/mman.h>
#endif // !Q_OS_WIN && !Q_OS_MAC

namespace Webview {

DataStreamFromMemory::DataStreamFromMemory(
	QByteArray data,
	std::string mime)
: _data(data)
, _mime(mime) {
#if !defined Q_OS_WIN && !defined Q_OS_MAC
	const auto handle = memfd_create("webview-data-stream", MFD_CLOEXEC);
	if (handle == -1) {
		return;
	}
	if (ftruncate(handle, data.size()) != 0) {
		close(handle);
		return;
	}
	const auto shared = mmap(
		nullptr,
		data.size(),
		PROT_READ | PROT_WRITE,
		MAP_SHARED,
		handle,
		0);
	if (shared == MAP_FAILED) {
		close(handle);
		return;
	}
	memcpy(shared, data.constData(), data.size());
	_handle = handle;
	_data.setRawData((char*)shared, data.size());
#endif // !Q_OS_WIN && !Q_OS_MAC
}

DataStreamFromMemory::~DataStreamFromMemory() {
#if !defined Q_OS_WIN && !defined Q_OS_MAC
	if (_handle) {
		munmap((void*)_data.constData(), _data.size());
		close(_handle);
	}
#endif // !Q_OS_WIN && !Q_OS_MAC
}

int DataStreamFromMemory::handle() {
	return _handle;
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
