// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/win/webview_windows_edge_chromium.h"

#include <string>
#include <locale>
#include <shlwapi.h>
#include <webview2.h>

namespace Webview::EdgeChromium {
namespace {

[[nodiscard]] std::wstring ToWide(std::string_view string) {
	const auto length = MultiByteToWideChar(
		CP_UTF8,
		0,
		string.data(),
		string.size(),
		nullptr,
		0);
	auto result = std::wstring(length, wchar_t{});
	MultiByteToWideChar(
		CP_UTF8,
		0,
		string.data(),
		string.size(),
		result.data(),
		result.size());
	return result;
}

[[nodiscard]] std::string FromWide(std::wstring_view string) {
	const auto length = WideCharToMultiByte(
		CP_UTF8,
		0,
		string.data(),
		string.size(),
		nullptr,
		0,
		nullptr,
		nullptr);
	auto result = std::string(length, char{});
	WideCharToMultiByte(
		CP_UTF8,
		0,
		string.data(),
		string.size(),
		result.data(),
		result.size(),
		nullptr,
		nullptr);
	return result;
}

class Handler final
	: public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
	, public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
	, public ICoreWebView2WebMessageReceivedEventHandler
	, public ICoreWebView2PermissionRequestedEventHandler
	, public ICoreWebView2NavigationStartingEventHandler {

public:
	Handler(
		Config config,
		std::function<void(ICoreWebView2Controller*)> readyHandler);

	ULONG STDMETHODCALLTYPE AddRef();
	ULONG STDMETHODCALLTYPE Release();
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID *ppv);
	HRESULT STDMETHODCALLTYPE Invoke(
		HRESULT res,
		ICoreWebView2Environment *env);
	HRESULT STDMETHODCALLTYPE Invoke(
		HRESULT res,
		ICoreWebView2Controller *controller);
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2WebMessageReceivedEventArgs *args);
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2PermissionRequestedEventArgs *args);
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2NavigationStartingEventArgs *args);

private:
	HWND _window;
	std::function<void(std::string)> _messageHandler;
	std::function<void(std::string)> _navigationHandler;
	std::function<void(ICoreWebView2Controller*)> _readyHandler;

};

Handler::Handler(
	Config config,
	std::function<void(ICoreWebView2Controller*)> readyHandler)
: _window(static_cast<HWND>(config.window))
, _messageHandler(std::move(config.messageHandler))
, _navigationHandler(std::move(config.navigationHandler))
, _readyHandler(std::move(readyHandler)) {
}

ULONG STDMETHODCALLTYPE Handler::AddRef() {
	return 1;
}

ULONG STDMETHODCALLTYPE Handler::Release() {
	return 1;
}

HRESULT STDMETHODCALLTYPE Handler::QueryInterface(REFIID riid, LPVOID *ppv) {
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		HRESULT res,
		ICoreWebView2Environment *env) {
	if (!env) {
		return S_FALSE;
	}
	env->CreateCoreWebView2Controller(_window, this);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		HRESULT res,
		ICoreWebView2Controller *controller) {
	controller->AddRef();

	auto webview = (ICoreWebView2*)nullptr;
	auto token = ::EventRegistrationToken();
	controller->get_CoreWebView2(&webview);
	webview->add_WebMessageReceived(this, &token);
	webview->add_PermissionRequested(this, &token);
	webview->add_NavigationStarting(this, &token);

	_readyHandler(controller);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2WebMessageReceivedEventArgs *args) {
	auto message = LPWSTR{};
	const auto result = args->TryGetWebMessageAsString(&message);

	if (result == S_OK && message) {
		_messageHandler(FromWide(message));
		sender->PostWebMessageAsString(message);
	}

	CoTaskMemFree(message);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2PermissionRequestedEventArgs *args) {
	auto kind = COREWEBVIEW2_PERMISSION_KIND{};
	const auto result = args->get_PermissionKind(&kind);
	if (result == S_OK) {
		if (kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ) {
			args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
		}
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2NavigationStartingEventArgs *args) {
	auto uri = LPWSTR{};
	const auto result = args->get_Uri(&uri);

	if (result == S_OK && uri) {
		_navigationHandler(FromWide(uri));
	}

	CoTaskMemFree(uri);
	return S_OK;
}

class Instance final : public Interface {
public:
	Instance(
		void *window,
		ICoreWebView2Controller *controller,
		ICoreWebView2 *webview);

	void navigate(std::string url) override;

	void resizeToWindow() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void *winId() override;

private:
	HWND _window = nullptr;
	ICoreWebView2Controller *_controller = nullptr;
	ICoreWebView2 *_webview = nullptr;

};

Instance::Instance(
	void *window,
	ICoreWebView2Controller *controller,
	ICoreWebView2 *webview)
: _window(static_cast<HWND>(window))
, _controller(controller)
, _webview(webview) {
	init("window.external={invoke:s=>window.chrome.webview.postMessage(s)}");
}

void Instance::navigate(std::string url) {
	const auto wide = ToWide(url);
	_webview->Navigate(wide.c_str());
}

void Instance::resizeToWindow() {
	auto bounds = RECT{};
	GetClientRect(_window, &bounds);
	const auto result = _controller->put_Bounds(bounds);
	int a = (int)result;
}

void Instance::init(std::string js) {
	const auto wide = ToWide(js);
	_webview->AddScriptToExecuteOnDocumentCreated(wide.c_str(), nullptr);
}

void Instance::eval(std::string js) {
	const auto wide = ToWide(js);
	_webview->ExecuteScript(wide.c_str(), nullptr);
}

void *Instance::winId() {
	return nullptr;
}

} // namespace

bool Supported() {
	return false;
	auto version = LPWSTR(nullptr);
	const auto result = GetAvailableCoreWebView2BrowserVersionString(
		nullptr,
		&version);
	return (result == S_OK);
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	if (!Supported()) {
		return nullptr;
	}
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	auto controller = (ICoreWebView2Controller*)nullptr;
	auto webview = (ICoreWebView2*)nullptr;
	const auto event = CreateEvent(nullptr, false, false, nullptr);
	const auto ready = [&](ICoreWebView2Controller *created) {
		controller = created;
		if (!created) {
			SetEvent(event);
			return;
		}
		controller->get_CoreWebView2(&webview);
		if (!webview) {
			SetEvent(event);
			return;
		}
		webview->AddRef();
		auto settings = (ICoreWebView2Settings*)nullptr;
		const auto result = webview->get_Settings(&settings);
		if (result != S_OK || !settings) {
			SetEvent(event);
			return;
		}
		settings->put_AreDefaultContextMenusEnabled(FALSE);
		settings->put_AreDevToolsEnabled(FALSE);
		settings->put_IsStatusBarEnabled(FALSE);
		SetEvent(event);
	};
	const auto result = CreateCoreWebView2EnvironmentWithOptions(
		nullptr,
		nullptr,
		nullptr,
		new Handler(config, ready));
	if (result != S_OK) {
		CoUninitialize();
		return nullptr;
	}
	HANDLE handles[] = { event };
	auto index = DWORD{};
	const auto flags = COWAIT_DISPATCH_WINDOW_MESSAGES |
		COWAIT_DISPATCH_CALLS |
		COWAIT_INPUTAVAILABLE;
	CoWaitForMultipleHandles(flags, INFINITE, 1, handles, &index);
	if (!controller || !webview) {
		return nullptr;
	}
	return std::make_unique<Instance>(config.window, controller, webview);
}

} // namespace Webview::EdgeChromium

