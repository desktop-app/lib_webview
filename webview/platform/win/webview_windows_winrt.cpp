// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/win/webview_windows_winrt.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace Webview::WinRT {
namespace {

[[nodiscard]] HINSTANCE SafeLoadLibrary(const std::wstring &name) {
	static auto Loaded = std::unordered_map<std::wstring, HINSTANCE>();
	static const auto SystemPath = [] {
		WCHAR buffer[MAX_PATH + 1] = { 0 };
		return GetSystemDirectory(buffer, MAX_PATH)
			? std::wstring(buffer)
			: std::wstring();
	}();
	static const auto WindowsPath = [] {
		WCHAR buffer[MAX_PATH + 1] = { 0 };
		return GetWindowsDirectory(buffer, MAX_PATH)
			? std::wstring(buffer)
			: std::wstring();
	}();
	const auto tryPath = [&](const std::wstring &path) {
		if (!path.empty()) {
			const auto full = (path + L'\\' + name);
			if (const auto result = HINSTANCE(LoadLibrary(full.c_str()))) {
				Loaded.emplace(name, result);
				return result;
			}
		}
		return HINSTANCE(nullptr);
	};
	if (const auto i = Loaded.find(name); i != Loaded.end()) {
		return i->second;
	} else if (const auto result1 = tryPath(SystemPath)) {
		return result1;
	} else if (const auto result2 = tryPath(WindowsPath)) {
		return result2;
	}
	Loaded.emplace(name, nullptr);
	return nullptr;
}

int32_t(__stdcall *CoIncrementMTAUsage)(void** cookie);
int32_t(__stdcall *RoInitialize)(uint32_t type);
int32_t(__stdcall *GetRestrictedErrorInfo)(void** info);
int32_t(__stdcall *RoGetActivationFactory)(void* classId, winrt::guid const& iid, void** factory);
int32_t(__stdcall *RoOriginateLanguageException)(int32_t error, void* message, void* exception);
int32_t(__stdcall *SetRestrictedErrorInfo)(void* info);
int32_t(__stdcall *WindowsCreateString)(wchar_t const* sourceString, uint32_t length, void** string);
int32_t(__stdcall *WindowsCreateStringReference)(wchar_t const* sourceString, uint32_t length, void* hstringHeader, void** string);
int32_t(__stdcall *WindowsDuplicateString)(void* string, void** newString);
int32_t(__stdcall *WindowsDeleteString)(void* string);
int32_t(__stdcall *WindowsPreallocateStringBuffer)(uint32_t length, wchar_t** charBuffer, void** bufferHandle);
int32_t(__stdcall *WindowsDeleteStringBuffer)(void* bufferHandle);
int32_t(__stdcall *WindowsPromoteStringBuffer)(void* bufferHandle, void** string);
wchar_t const*(__stdcall *WindowsGetStringRawBuffer)(void* string, uint32_t* length);

template <typename Method>
bool ResolveOne(HINSTANCE library, Method &method, LPCSTR name) {
	if (!library) {
		return false;
	}
	method = reinterpret_cast<Method>(GetProcAddress(library, name));
	return (method != nullptr);
}

#define RESOLVE_ONE(library, method) (ResolveOne(library, method, #method))

} // namespace

bool Resolve() {
	const auto ole32 = SafeLoadLibrary(L"ole32.dll");
	const auto combase = SafeLoadLibrary(L"combase.dll");
	return RESOLVE_ONE(ole32, CoIncrementMTAUsage)
		&& RESOLVE_ONE(combase, RoInitialize)
		&& RESOLVE_ONE(combase, GetRestrictedErrorInfo)
		&& RESOLVE_ONE(combase, RoGetActivationFactory)
		&& RESOLVE_ONE(combase, RoOriginateLanguageException)
		&& RESOLVE_ONE(combase, SetRestrictedErrorInfo)
		&& RESOLVE_ONE(combase, WindowsCreateString)
		&& RESOLVE_ONE(combase, WindowsCreateStringReference)
		&& RESOLVE_ONE(combase, WindowsDuplicateString)
		&& RESOLVE_ONE(combase, WindowsDeleteString)
		&& RESOLVE_ONE(combase, WindowsPreallocateStringBuffer)
		&& RESOLVE_ONE(combase, WindowsDeleteStringBuffer)
		&& RESOLVE_ONE(combase, WindowsPromoteStringBuffer)
		&& RESOLVE_ONE(combase, WindowsGetStringRawBuffer);
}

} // namespace Webview::WinRT

using namespace Webview::WinRT;

extern "C" {

int32_t __stdcall WINRT_CoIncrementMTAUsage(void** cookie) noexcept {
	return CoIncrementMTAUsage(cookie);
}

int32_t __stdcall WINRT_RoInitialize(uint32_t type) noexcept {
	return RoInitialize(type);
}

int32_t __stdcall WINRT_GetRestrictedErrorInfo(void** info) noexcept {
	return GetRestrictedErrorInfo(info);
}

int32_t __stdcall WINRT_RoGetActivationFactory(void* classId, winrt::guid const& iid, void** factory) noexcept {
	return RoGetActivationFactory(classId, iid, factory);
}

int32_t __stdcall WINRT_RoOriginateLanguageException(int32_t error, void* message, void* exception) noexcept {
	return RoOriginateLanguageException(error, message, exception);
}

int32_t __stdcall WINRT_SetRestrictedErrorInfo(void* info) noexcept {
	return SetRestrictedErrorInfo(info);
}

int32_t __stdcall WINRT_WindowsCreateString(wchar_t const* sourceString, uint32_t length, void** string) noexcept {
	return WindowsCreateString(sourceString, length, string);
}

int32_t __stdcall WINRT_WindowsCreateStringReference(wchar_t const* sourceString, uint32_t length, void* hstringHeader, void** string) noexcept {
	return WindowsCreateStringReference(sourceString, length, hstringHeader, string);
}

int32_t __stdcall WINRT_WindowsDuplicateString(void* string, void** newString) noexcept {
	return WindowsDuplicateString(string, newString);
}

int32_t __stdcall WINRT_WindowsDeleteString(void* string) noexcept {
	return WindowsDeleteString(string);
}

int32_t __stdcall WINRT_WindowsPreallocateStringBuffer(uint32_t length, wchar_t** charBuffer, void** bufferHandle) noexcept {
	return WindowsPreallocateStringBuffer(length, charBuffer, bufferHandle);
}

int32_t __stdcall WINRT_WindowsDeleteStringBuffer(void* bufferHandle) noexcept {
	return WindowsDeleteStringBuffer(bufferHandle);
}

int32_t __stdcall WINRT_WindowsPromoteStringBuffer(void* bufferHandle, void** string) noexcept {
	return WindowsPromoteStringBuffer(bufferHandle, string);
}

wchar_t const* __stdcall WINRT_WindowsGetStringRawBuffer(void* string, uint32_t* length) noexcept {
	return WindowsGetStringRawBuffer(string, length);
}

} // extern "C"
