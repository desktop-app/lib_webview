// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/win/webview_windows_data_stream.h"

#include <windows.h>

namespace Webview {

DataStreamCOM::DataStreamCOM(
	std::unique_ptr<DataStream> wrapped)
: _wrapped(std::move(wrapped)) {
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::Read(
		_Out_writes_bytes_to_(cb, *pcbRead)  void *pv,
		_In_  ULONG cb,
		_Out_opt_  ULONG *pcbRead) {
	const auto read = _wrapped->read(pv, cb);
	if (read < 0) {
		return E_FAIL;
	} else if (pcbRead) {
		*pcbRead = read;
	}
	return (read == cb) ? S_OK : S_FALSE;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::Write(
		_In_reads_bytes_(cb)  const void *pv,
		_In_  ULONG cb,
		_Out_opt_  ULONG *pcbWritten) {
	return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::Seek(
		/* [in] */ LARGE_INTEGER dlibMove,
		/* [in] */ DWORD dwOrigin,
		/* [annotation] */
		_Out_opt_  ULARGE_INTEGER *plibNewPosition) {
	const auto origin = [&] {
		switch (dwOrigin) {
		case STREAM_SEEK_SET: return SEEK_SET;
		case STREAM_SEEK_CUR: return SEEK_CUR;
		case STREAM_SEEK_END: return SEEK_END;
		}
		return -1;
	}();
	if (origin < 0) {
		return E_FAIL;
	}

	const auto position = _wrapped->seek(origin, dlibMove.QuadPart);
	if (position < 0) {
		return E_FAIL;
	} else if (plibNewPosition) {
		plibNewPosition->QuadPart = position;
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::SetSize(
		/* [in] */ ULARGE_INTEGER libNewSize) {
	return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::CopyTo(
		/* [annotation][unique][in] */
		_In_  IStream *pstm,
		/* [in] */ ULARGE_INTEGER cb,
		/* [annotation] */
		_Out_opt_  ULARGE_INTEGER *pcbRead,
		/* [annotation] */
		_Out_opt_  ULARGE_INTEGER *pcbWritten) {
	return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::Commit(
		/* [in] */ DWORD grfCommitFlags) {
	return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::Revert(void) {
	return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::LockRegion(
		/* [in] */ ULARGE_INTEGER libOffset,
		/* [in] */ ULARGE_INTEGER cb,
		/* [in] */ DWORD dwLockType) {
	return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::UnlockRegion(
		/* [in] */ ULARGE_INTEGER libOffset,
		/* [in] */ ULARGE_INTEGER cb,
		/* [in] */ DWORD dwLockType) {
	return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::Stat(
		/* [out] */ __RPC__out STATSTG *pstatstg,
		/* [in] */ DWORD grfStatFlag) {
	const auto size = _wrapped->size();
	if (size >= 0) {
		pstatstg->cbSize.QuadPart = size;
		return S_OK;
	}
	return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DataStreamCOM::Clone(
		/* [out] */ __RPC__deref_out_opt IStream **ppstm) {
	return E_NOTIMPL;
}

} // namespace Webview
