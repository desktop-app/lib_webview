// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/win/webview_windows_edge_chromium.h"

#include "webview/webview_data_stream.h"
#include "webview/platform/win/webview_windows_data_stream.h"
#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/weak_ptr.h"
#include "base/platform/base_platform_info.h"
#include "base/platform/win/base_windows_co_task_mem.h"
#include "base/platform/win/base_windows_winrt.h"
#include "base/platform/win/wrl/wrl_implements_h.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

#include <crl/common/crl_common_on_main_guarded.h>

#include <string>
#include <locale>
#include <shlwapi.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

namespace Webview::EdgeChromium {
namespace {

constexpr auto kDataUrlPrefix
	= std::string_view("http://desktop-app-resource/");

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

[[nodiscard]] std::string FromWide(const base::CoTaskMemString &string) {
	return string ? FromWide(string.data()) : std::string();
}

class Handler final
	: public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
	, public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
	, public ICoreWebView2WebMessageReceivedEventHandler
	, public ICoreWebView2PermissionRequestedEventHandler
	, public ICoreWebView2NavigationStartingEventHandler
	, public ICoreWebView2NavigationCompletedEventHandler
	, public ICoreWebView2NewWindowRequestedEventHandler
	, public ICoreWebView2ScriptDialogOpeningEventHandler
	, public ICoreWebView2WebResourceRequestedEventHandler
	, public base::has_weak_ptr {

public:
	Handler(Config config, std::function<void()> readyHandler);

	const winrt::com_ptr<ICoreWebView2Environment> &environment() const {
		return _environment;
	}
	const winrt::com_ptr<ICoreWebView2Controller> &controller() const {
		return _controller;
	}
	const winrt::com_ptr<ICoreWebView2> &webview() const {
		return _webview;
	}
	[[nodiscard]] bool valid() const {
		return _environment && _controller && _webview;
	}

	void setOpaqueBg(QColor opaqueBg);

	ULONG STDMETHODCALLTYPE AddRef();
	ULONG STDMETHODCALLTYPE Release();
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID *ppv);
	HRESULT STDMETHODCALLTYPE Invoke(
		HRESULT res,
		ICoreWebView2Environment *env) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		HRESULT res,
		ICoreWebView2Controller *controller) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2WebMessageReceivedEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2PermissionRequestedEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2NavigationStartingEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2NavigationCompletedEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2NewWindowRequestedEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2ScriptDialogOpeningEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2WebResourceRequestedEventArgs *args) override;

private:
	HWND _window = nullptr;
	winrt::com_ptr<ICoreWebView2Environment> _environment;
	winrt::com_ptr<ICoreWebView2Controller> _controller;
	winrt::com_ptr<ICoreWebView2> _webview;
	std::function<void(std::string)> _messageHandler;
	std::function<bool(std::string, bool)> _navigationStartHandler;
	std::function<void(bool)> _navigationDoneHandler;
	std::function<DialogResult(DialogArgs)> _dialogHandler;
	std::function<DataResult(DataRequest)> _dataRequestHandler;
	std::function<void()> _readyHandler;
	base::flat_map<
		winrt::com_ptr<ICoreWebView2WebResourceRequestedEventArgs>,
		winrt::com_ptr<ICoreWebView2Deferral>> _pending;

	QColor _opaqueBg;
	bool _debug = false;
};

Handler::Handler(Config config, std::function<void()> readyHandler)
: _window(static_cast<HWND>(config.window))
, _messageHandler(std::move(config.messageHandler))
, _navigationStartHandler(std::move(config.navigationStartHandler))
, _navigationDoneHandler(std::move(config.navigationDoneHandler))
, _dialogHandler(std::move(config.dialogHandler))
, _dataRequestHandler(std::move(config.dataRequestHandler))
, _readyHandler(std::move(readyHandler))
, _opaqueBg(config.opaqueBg)
, _debug(config.debug) {
}

void Handler::setOpaqueBg(QColor opaqueBg) {
	if (Platform::IsWindows10OrGreater()) {
		opaqueBg = QColor(255, 255, 255, 0);
	}
	if (const auto late = _controller.try_as<ICoreWebView2Controller2>()) {
		late->put_DefaultBackgroundColor({
			uchar(opaqueBg.alpha()),
			uchar(opaqueBg.red()),
			uchar(opaqueBg.green()),
			uchar(opaqueBg.blue())
		});
	}
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
	_environment.copy_from(env);
	if (!_environment) {
		return E_FAIL;
	}
	_environment->CreateCoreWebView2Controller(_window, this);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		HRESULT res,
		ICoreWebView2Controller *controller) {
	if (!_readyHandler) {
		return S_OK;
	}
	_controller.copy_from(controller);
	const auto guard = gsl::finally([&] {
		const auto onstack = _readyHandler;
		_readyHandler = nullptr;
		onstack();
	});
	if (!_controller) {
		return E_FAIL;
	}

	_controller->get_CoreWebView2(_webview.put());
	if (!_webview) {
		return E_FAIL;
	}
	auto token = ::EventRegistrationToken();
	_webview->add_WebMessageReceived(this, &token);
	_webview->add_PermissionRequested(this, &token);
	_webview->add_NavigationStarting(this, &token);
	_webview->add_NavigationCompleted(this, &token);
	_webview->add_NewWindowRequested(this, &token);
	_webview->add_ScriptDialogOpening(this, &token);
	_webview->add_WebResourceRequested(this, &token);

	const auto filter = ToWide(kDataUrlPrefix) + L'*';
	auto hr = _webview->AddWebResourceRequestedFilter(
		filter.c_str(),
		COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
	if (hr != S_OK) {
		return E_FAIL;
	}

	auto settings = winrt::com_ptr<ICoreWebView2Settings>();
	hr = _webview->get_Settings(settings.put());
	if (hr != S_OK || !settings) {
		return E_FAIL;
	}
	settings->put_AreDefaultContextMenusEnabled(_debug);
	settings->put_AreDevToolsEnabled(_debug);
	settings->put_AreDefaultScriptDialogsEnabled(FALSE);
	settings->put_IsStatusBarEnabled(FALSE);

	setOpaqueBg(_opaqueBg);

	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2WebMessageReceivedEventArgs *args) {
	auto message = base::CoTaskMemString();
	const auto result = args->TryGetWebMessageAsString(message.put());

	if (result == S_OK && message) {
		_messageHandler(FromWide(message));
		sender->PostWebMessageAsString(message.data());
	}
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
	auto uri = base::CoTaskMemString();
	const auto result = args->get_Uri(uri.put());

	if (result == S_OK && uri) {
		if (_navigationStartHandler
			&& !_navigationStartHandler(FromWide(uri), false)) {
			args->put_Cancel(TRUE);
		}
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2NavigationCompletedEventArgs *args) {
	auto isSuccess = BOOL(FALSE);
	const auto result = args->get_IsSuccess(&isSuccess);

	if (_navigationDoneHandler) {
		_navigationDoneHandler(result == S_OK && isSuccess);
	}

	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2NewWindowRequestedEventArgs *args) {
	auto uri = base::CoTaskMemString();
	const auto result = args->get_Uri(uri.put());
	auto isUserInitiated = BOOL{};
	args->get_IsUserInitiated(&isUserInitiated);
	args->put_Handled(TRUE);

	if (result == S_OK && uri && isUserInitiated) {
		const auto url = FromWide(uri);
		if (_navigationStartHandler && _navigationStartHandler(url, true)) {
			QDesktopServices::openUrl(QString::fromStdString(url));
		}
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2ScriptDialogOpeningEventArgs *args) {
	auto kind = COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT;
	auto hr = args->get_Kind(&kind);
	if (hr != S_OK) {
		return S_OK;
	}

	auto uri = base::CoTaskMemString();
	auto text = base::CoTaskMemString();
	auto value = base::CoTaskMemString();
	hr = args->get_Uri(uri.put());
	if (hr != S_OK || !uri) {
		return S_OK;
	}
	hr = args->get_Message(text.put());
	if (hr != S_OK || !text) {
		return S_OK;
	}
	hr = args->get_DefaultText(value.put());
	if (hr != S_OK || !value) {
		return S_OK;
	}

	const auto type = [&] {
		switch (kind) {
		case COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT:
			return DialogType::Alert;
		case COREWEBVIEW2_SCRIPT_DIALOG_KIND_CONFIRM:
			return DialogType::Confirm;
		case COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT:
			return DialogType::Prompt;
		}
		return DialogType::Alert;
	}();
	const auto result = _dialogHandler(DialogArgs{
		.type = type,
		.value = FromWide(value),
		.text = FromWide(text),
		.url = FromWide(uri),
	});

	if (result.accepted) {
		args->Accept();
		if (kind == COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT) {
			const auto wide = ToWide(result.text);
			args->put_ResultText(wide.c_str());
		}
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2WebResourceRequestedEventArgs *args) {
	auto request = winrt::com_ptr<ICoreWebView2WebResourceRequest>();
	auto hr = args->get_Request(request.put());
	if (hr != S_OK || !request) {
		return S_OK;
	}
	auto uri = base::CoTaskMemString();
	hr = request->get_Uri(uri.put());
	if (hr != S_OK || !uri) {
		return S_OK;
	}
	auto headers = winrt::com_ptr<ICoreWebView2HttpRequestHeaders>();
	hr = request->get_Headers(headers.put());
	if (hr != S_OK || !headers) {
		return S_OK;
	}
	winrt::com_ptr<ICoreWebView2HttpHeadersCollectionIterator> iterator;
	hr = headers->GetIterator(iterator.put());
	if (hr != S_OK || !iterator) {
		return S_OK;
	}
	const auto ansi = FromWide(uri);
	const auto prefix = kDataUrlPrefix.size();
	if (ansi.size() <= prefix || ansi.compare(0, prefix, kDataUrlPrefix)) {
		return S_OK;
	}

	constexpr auto fail = [](
			ICoreWebView2Environment *environment,
			ICoreWebView2WebResourceRequestedEventArgs *args) {
		auto response = winrt::com_ptr<ICoreWebView2WebResourceResponse>();
		auto hr = environment->CreateWebResourceResponse(
			nullptr,
			404,
			L"Not Found",
			L"",
			response.put());
		if (hr == S_OK && response) {
			args->put_Response(response.get());
		}
		return S_OK;
	};
	constexpr auto done = [](
			ICoreWebView2Environment *environment,
			ICoreWebView2WebResourceRequestedEventArgs *args,
			DataResponse resolved) {
		auto &stream = resolved.stream;
		if (!stream) {
			return fail(environment, args);
		}
		const auto length = stream->size();
		const auto offset = resolved.streamOffset;
		const auto total = resolved.totalSize ? resolved.totalSize : length;
		const auto partial = (offset > 0) || (total != length);
		auto headers = L""
			L"Content-Type: " + ToWide(stream->mime()) +
			L"\nAccess-Control-Allow-Origin: *"
			L"\nAccept-Ranges: bytes"
			L"\nCache-Control: no-store"
			L"\nContent-Length: " + std::to_wstring(length);
		if (partial) {
			headers += L"\nContent-Range: bytes "
				+ std::to_wstring(offset)
				+ L'-'
				+ std::to_wstring(offset + length - 1)
				+ L'/'
				+ std::to_wstring(total);
		}
		auto response = winrt::com_ptr<ICoreWebView2WebResourceResponse>();
		auto hr = environment->CreateWebResourceResponse(
			Microsoft::WRL::Make<DataStreamCOM>(std::move(stream)).Detach(),
			partial ? 206 : 200,
			partial ? L"Partial Content" : L"OK",
			headers.c_str(),
			response.put());
		if (hr == S_OK && response) {
			args->put_Response(response.get());
		}
		return S_OK;
	};
	const auto callback = crl::guard(this, [=](DataResponse response) {
		done(_environment.get(), args, std::move(response));
		auto copy = winrt::com_ptr<ICoreWebView2WebResourceRequestedEventArgs>();
		copy.copy_from(args);
		if (const auto deferral = _pending.take(copy)) {
			(*deferral)->Complete();
		}
	});
	auto prepared = DataRequest{
		.id = ansi.substr(prefix),
		.done = std::move(callback),
	};
	while (true) {
		auto hasCurrent = BOOL();
		hr = iterator->get_HasCurrentHeader(&hasCurrent);
		if (hr != S_OK || !hasCurrent) {
			break;
		}
		auto name = base::CoTaskMemString();
		auto value = base::CoTaskMemString();
		hr = iterator->GetCurrentHeader(name.put(), value.put());
		if (hr != S_OK || !name || !value) {
			break;
		} else if (FromWide(name) == "Range") {
			const auto data = FromWide(value);
			ParseRangeHeaderFor(prepared, FromWide(value));
		}
		auto hasNext = BOOL();
		hr = iterator->MoveNext(&hasNext);
		if (hr != S_OK || !hasNext) {
			break;
		}
	}

	const auto result = _dataRequestHandler
		? _dataRequestHandler(prepared)
		: DataResult::Failed;
	if (result == DataResult::Failed) {
		return fail(_environment.get(), args);
	} else if (result == DataResult::Pending) {
		auto deferral = winrt::com_ptr<ICoreWebView2Deferral>();
		hr = args->GetDeferral(deferral.put());
		if (hr != S_OK || !deferral) {
			return fail(_environment.get(), args);
		}
		auto copy = winrt::com_ptr<ICoreWebView2WebResourceRequestedEventArgs>();
		copy.copy_from(args);
		_pending.emplace(std::move(copy), std::move(deferral));
	}
	return S_OK;
}

class Instance final : public Interface {
public:
	Instance(void *window, std::unique_ptr<Handler> handler);
	~Instance();

	bool finishEmbedding() override;

	void navigate(std::string url) override;
	void navigateToData(std::string id) override;
	void reload() override;

	void resizeToWindow() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void focus() override;

	QWidget *widget() override;
	void *winId() override;

	void setOpaqueBg(QColor opaqueBg) override;

private:
	HWND _window = nullptr;
	std::unique_ptr<Handler> _handler;

};

Instance::Instance(void *window, std::unique_ptr<Handler> handler)
: _window(static_cast<HWND>(window))
, _handler(std::move(handler)) {
	init("window.external={invoke:s=>window.chrome.webview.postMessage(s)}");
}

Instance::~Instance() {
	CoUninitialize();
}

bool Instance::finishEmbedding() {
	_handler->controller()->put_IsVisible(TRUE);
	return true;
}

void Instance::navigate(std::string url) {
	const auto wide = ToWide(url);
	_handler->webview()->Navigate(wide.c_str());
}

void Instance::navigateToData(std::string id) {
	auto full = std::string();
	full.reserve(kDataUrlPrefix.size() + id.size());
	full.append(kDataUrlPrefix);
	full.append(id);
	navigate(full);
}

void Instance::reload() {
	_handler->webview()->Reload();
}

void Instance::resizeToWindow() {
	auto bounds = RECT{};
	GetClientRect(_window, &bounds);
	_handler->controller()->put_Bounds(bounds);
}

void Instance::init(std::string js) {
	const auto wide = ToWide(js);
	_handler->webview()->AddScriptToExecuteOnDocumentCreated(
		wide.c_str(),
		nullptr);
}

void Instance::eval(std::string js) {
	const auto wide = ToWide(js);
	_handler->webview()->ExecuteScript(wide.c_str(), nullptr);
}

void Instance::focus() {
	SetForegroundWindow(_window);
	SetFocus(_window);
	_handler->controller()->MoveFocus(
		COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

QWidget *Instance::widget() {
	return nullptr;
}

void *Instance::winId() {
	return nullptr;
}

void Instance::setOpaqueBg(QColor opaqueBg) {
	_handler->setOpaqueBg(opaqueBg);
}

} // namespace

bool Supported() {
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

	auto options = winrt::com_ptr<ICoreWebView2EnvironmentOptions>(
		Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>().Detach(),
		winrt::take_ownership_from_abi);
	options->put_AdditionalBrowserArguments(
		L"--disable-features=ElasticOverscroll");

	const auto event = CreateEvent(nullptr, false, false, nullptr);
	const auto guard = gsl::finally([&] { CloseHandle(event); });
	auto handler = std::make_unique<Handler>(config, [&] {
		SetEvent(event);
	});
	const auto wpath = ToWide(config.userDataPath);
	const auto result = CreateCoreWebView2EnvironmentWithOptions(
		nullptr,
		wpath.empty() ? nullptr : wpath.c_str(),
		options.get(),
		handler.get());
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

	if (!handler->valid()) {
		CoUninitialize();
		return nullptr;
	}
	return std::make_unique<Instance>(config.window, std::move(handler));
}

} // namespace Webview::EdgeChromium

