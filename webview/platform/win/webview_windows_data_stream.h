// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/platform/win/base_windows_wrl.h"
#include "base/platform/win/wrl/wrl_implements_h.h"
#include "webview/webview_data_stream.h"

#include <memory>

namespace Webview {

class DataStreamCOM final
	: public Microsoft::WRL::RuntimeClass<
		Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
		IStream> {
public:
	explicit DataStreamCOM(std::unique_ptr<DataStream> wrapped);
	~DataStreamCOM() = default;

	HRESULT STDMETHODCALLTYPE Read(
		_Out_writes_bytes_to_(cb, *pcbRead)  void *pv,
		_In_  ULONG cb,
		_Out_opt_  ULONG *pcbRead) override;

	HRESULT STDMETHODCALLTYPE Write(
		_In_reads_bytes_(cb)  const void *pv,
		_In_  ULONG cb,
		_Out_opt_  ULONG *pcbWritten) override;

	HRESULT STDMETHODCALLTYPE Seek(
		/* [in] */ LARGE_INTEGER dlibMove,
		/* [in] */ DWORD dwOrigin,
		/* [annotation] */
		_Out_opt_  ULARGE_INTEGER *plibNewPosition) override;

	HRESULT STDMETHODCALLTYPE SetSize(
		/* [in] */ ULARGE_INTEGER libNewSize) override;

	HRESULT STDMETHODCALLTYPE CopyTo(
		/* [annotation][unique][in] */
		_In_  IStream *pstm,
		/* [in] */ ULARGE_INTEGER cb,
		/* [annotation] */
		_Out_opt_  ULARGE_INTEGER *pcbRead,
		/* [annotation] */
		_Out_opt_  ULARGE_INTEGER *pcbWritten) override;

	HRESULT STDMETHODCALLTYPE Commit(
		/* [in] */ DWORD grfCommitFlags) override;

	HRESULT STDMETHODCALLTYPE Revert(void) override;

	HRESULT STDMETHODCALLTYPE LockRegion(
		/* [in] */ ULARGE_INTEGER libOffset,
		/* [in] */ ULARGE_INTEGER cb,
		/* [in] */ DWORD dwLockType) override;

	HRESULT STDMETHODCALLTYPE UnlockRegion(
		/* [in] */ ULARGE_INTEGER libOffset,
		/* [in] */ ULARGE_INTEGER cb,
		/* [in] */ DWORD dwLockType) override;

	HRESULT STDMETHODCALLTYPE Stat(
		/* [out] */ __RPC__out STATSTG *pstatstg,
		/* [in] */ DWORD grfStatFlag) override;

	HRESULT STDMETHODCALLTYPE Clone(
		/* [out] */ __RPC__deref_out_opt IStream **ppstm) override;

private:
	const std::unique_ptr<DataStream> _wrapped;

};

} // namespace Webview
