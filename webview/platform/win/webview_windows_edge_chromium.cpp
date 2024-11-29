// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/win/webview_windows_edge_chromium.h"

#include "webview/webview_data_stream.h"
#include "webview/webview_embed.h"
#include "webview/platform/win/webview_windows_data_stream.h"
#include "base/algorithm.h"
#include "base/basic_types.h"
#include "base/event_filter.h"
#include "base/flat_map.h"
#include "base/invoke_queued.h"
#include "base/options.h"
#include "base/variant.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "base/platform/base_platform_info.h"
#include "base/platform/win/base_windows_co_task_mem.h"
#include "base/platform/win/base_windows_winrt.h"
#include "base/platform/win/wrl/wrl_implements_h.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>

#include <crl/common/crl_common_on_main_guarded.h>
#include <rpl/variable.h>

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

class Handler
	: public winrt::implements<
		Handler,
		ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
		ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
		ICoreWebView2WebMessageReceivedEventHandler,
		ICoreWebView2PermissionRequestedEventHandler,
		ICoreWebView2NavigationStartingEventHandler,
		ICoreWebView2NavigationCompletedEventHandler,
		ICoreWebView2ContentLoadingEventHandler,
		ICoreWebView2DocumentTitleChangedEventHandler,
		ICoreWebView2SourceChangedEventHandler,
		ICoreWebView2NewWindowRequestedEventHandler,
		ICoreWebView2ScriptDialogOpeningEventHandler,
		ICoreWebView2WebResourceRequestedEventHandler,
		ICoreWebView2ZoomFactorChangedEventHandler>
	, public ZoomController
	, public base::has_weak_ptr {

public:
	Handler(
		Config config,
		HWND handle,
		std::function<void(not_null<Handler*>)> saveThis,
		std::function<void()> readyHandler);
	~Handler();

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
		return _window && _environment && _controller && _webview;
	}

	void setOpaqueBg(QColor opaqueBg);

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
		ICoreWebView2ContentLoadingEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		IUnknown *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2SourceChangedEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2NewWindowRequestedEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2ScriptDialogOpeningEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2WebResourceRequestedEventArgs *args) override;
	HRESULT STDMETHODCALLTYPE Invoke(
		ICoreWebView2Controller *sender,
		IUnknown *args) override;

	rpl::producer<int> zoomValue() override {
		return _zoomValue.value();
	}
	void setZoom(int zoom) {
		if (_controller) {
			_controller->put_ZoomFactor(zoom / 100.);
		}
	}

	rpl::producer<NavigationHistoryState> navigationHistoryState() {
		return _navigationHistoryState.value();
	}

	[[nodiscard]] ZoomController *zoomController() {
		return this;
	}

private:
	void updateHistoryStates();

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

	rpl::variable<NavigationHistoryState> _navigationHistoryState;
	rpl::variable<int> _zoomValue;

	QColor _opaqueBg;
	bool _debug = false;

};

Handler::Handler(
	Config config,
	HWND handle,
	std::function<void(not_null<Handler*>)> saveThis,
	std::function<void()> readyHandler)
: _window(handle)
, _messageHandler(std::move(config.messageHandler))
, _navigationStartHandler(std::move(config.navigationStartHandler))
, _navigationDoneHandler(std::move(config.navigationDoneHandler))
, _dialogHandler(std::move(config.dialogHandler))
, _dataRequestHandler(std::move(config.dataRequestHandler))
, _readyHandler(std::move(readyHandler))
, _opaqueBg(config.opaqueBg)
, _debug(config.debug) {
	saveThis(this);
	setOpaqueBg(_opaqueBg);
}

Handler::~Handler() = default;

void Handler::setOpaqueBg(QColor opaqueBg) {
	_opaqueBg = Platform::IsWindows10OrGreater()
		? QColor(255, 255, 255, 0)
		: opaqueBg;
	if (const auto late = _controller.try_as<ICoreWebView2Controller2>()) {
		late->put_DefaultBackgroundColor({
			uchar(_opaqueBg.alpha()),
			uchar(_opaqueBg.red()),
			uchar(_opaqueBg.green()),
			uchar(_opaqueBg.blue())
		});
	}
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
		_window = nullptr;
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
	_webview->add_ContentLoading(this, &token);
	_webview->add_DocumentTitleChanged(this, &token);
	_webview->add_SourceChanged(this, &token);
	_webview->add_NewWindowRequested(this, &token);
	_webview->add_ScriptDialogOpening(this, &token);
	_webview->add_WebResourceRequested(this, &token);

	_controller->add_ZoomFactorChanged(this, &token);

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
			return S_OK;
		}
	}
	updateHistoryStates();
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
	updateHistoryStates();
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2ContentLoadingEventArgs *args) {
	updateHistoryStates();
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		IUnknown *args) {
	updateHistoryStates();
	return S_OK;
}

HRESULT STDMETHODCALLTYPE Handler::Invoke(
		ICoreWebView2 *sender,
		ICoreWebView2SourceChangedEventArgs *args) {
	updateHistoryStates();
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
		ICoreWebView2Controller *sender,
		IUnknown *args) {
	auto zoom = float64(0);
	sender->get_ZoomFactor(&zoom);
	_zoomValue = zoom * 100;
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

void Handler::updateHistoryStates() {
	if (!_webview) {
		return;
	};
	auto canGoBack = BOOL(FALSE);
	auto canGoForward = BOOL(FALSE);
	auto url = base::CoTaskMemString();
	auto title = base::CoTaskMemString();
	_webview->get_CanGoBack(&canGoBack);
	_webview->get_CanGoForward(&canGoForward);
	_webview->get_Source(url.put());
	_webview->get_DocumentTitle(title.put());
	_navigationHistoryState = NavigationHistoryState{
		.url = FromWide(url),
		.title = FromWide(title),
		.canGoBack = (canGoBack == TRUE),
		.canGoForward = (canGoForward == TRUE),
	};
}

class Instance final : public Interface, public base::has_weak_ptr {
public:
	explicit Instance(Config &&config);
	~Instance();

	[[nodiscard]] bool failed() const;

	void navigate(std::string url) override;
	void navigateToData(std::string id) override;
	void reload() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void focus() override;

	QWidget *widget() override;

	void refreshNavigationHistoryState() override;
	auto navigationHistoryState()
	-> rpl::producer<NavigationHistoryState> override;

	void setOpaqueBg(QColor opaqueBg) override;

	[[nodiscard]] ZoomController *zoomController() override;

private:
	struct NavigateToUrl {
		std::string url;
	};
	struct NavigateToData {
		std::string id;
	};
	struct InitScript {
		std::string script;
	};
	struct EvalScript {
		std::string script;
	};

	using ReadyStep = std::variant<
		NavigateToUrl,
		NavigateToData,
		InitScript,
		EvalScript>;

	void start(Config &&config);
	[[nodiscard]] bool ready() const;
	void processReadySteps();
	void resizeToWindow();

	base::unique_qptr<QWindow> _window;
	HWND _handle = nullptr;
	winrt::com_ptr<IUnknown> _ownedHandler;
	Handler *_handler = nullptr;
	std::vector<ReadyStep> _waitingForReady;
	base::unique_qptr<QWidget> _widget;
	bool _pendingFocus = false;
	bool _readyFlag = false;

};

Instance::Instance(Config &&config)
: _window(MakeFramelessWindow())
, _handle(HWND(_window->winId()))
, _widget(
	QWidget::createWindowContainer(
		_window,
		config.parent,
		Qt::FramelessWindowHint)) {
	_widget->show();
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	init("window.external={invoke:s=>window.chrome.webview.postMessage(s)}");
	start(std::move(config));
}

Instance::~Instance() {
	if (_handler) {
		if (_handler->valid()) {
			_handler->controller()->Close();
		}
		_handler = nullptr;
	}
	CoUninitialize();
}

void Instance::start(Config &&config) {
	auto options = winrt::com_ptr<ICoreWebView2EnvironmentOptions>(
		Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>().Detach(),
		winrt::take_ownership_from_abi);
	options->put_AdditionalBrowserArguments(
		L"--disable-features=ElasticOverscroll");

	auto handler = (Handler*)nullptr;
	const auto ready = [=] {
		_readyFlag = true;
		if (_handler) {
			processReadySteps();
		}
	};
	auto owned = winrt::make<Handler>(
		config,
		_handle,
		[&](not_null<Handler*> that) { handler = that; },
		crl::guard(this, ready));
	const auto wpath = ToWide(config.userDataPath);
	const auto result = CreateCoreWebView2EnvironmentWithOptions(
		nullptr,
		wpath.empty() ? nullptr : wpath.c_str(),
		options.get(),
		handler);
	if (result == S_OK) {
		_ownedHandler = std::move(owned);
		_handler = handler;
		if (_readyFlag) {
			processReadySteps();
		}
	}
}

bool Instance::failed() const {
	return !_handler;
}

void Instance::processReadySteps() {
	Expects(ready());

	const auto guard = base::make_weak(this);
	if (!_handler->valid()) {
		_widget = nullptr;
		_handle = nullptr;
		_window = nullptr;
		_handler = nullptr;
		base::take(_ownedHandler);
		return;
	}
	if (guard) {
		_handler->controller()->put_IsVisible(TRUE);
	}
	if (const auto widget = guard ? _widget.get() : nullptr) {
		base::install_event_filter(widget, [=](not_null<QEvent*> e) {
			if (e->type() == QEvent::Resize || e->type() == QEvent::Move) {
				InvokeQueued(widget, [=] { resizeToWindow(); });
			}
			return base::EventFilterResult::Continue;
		});
		resizeToWindow();
	}
	if (guard) {
		for (const auto &step : base::take(_waitingForReady)) {
			v::match(step, [&](const NavigateToUrl &data) {
				navigate(data.url);
			}, [&](const NavigateToData &data) {
				navigateToData(data.id);
			}, [&](const InitScript &data) {
				init(data.script);
			}, [&](const EvalScript &data) {
				eval(data.script);
			});
			if (!guard) {
				return;
			}
		}
	}
	if (guard && _pendingFocus) {
		focus();
	}
}

bool Instance::ready() const {
	return _handle && _handler && _readyFlag;
}

void Instance::navigate(std::string url) {
	if (!ready()) {
		_waitingForReady.push_back(NavigateToUrl{ std::move(url) });
		return;
	}
	const auto wide = ToWide(url);
	_handler->webview()->Navigate(wide.c_str());
}

void Instance::navigateToData(std::string id) {
	if (!ready()) {
		_waitingForReady.push_back(NavigateToData{ std::move(id) });
		return;
	}
	auto full = std::string();
	full.reserve(kDataUrlPrefix.size() + id.size());
	full.append(kDataUrlPrefix);
	full.append(id);
	navigate(full);
}

void Instance::reload() {
	if (ready()) {
		_handler->webview()->Reload();
	}
}

void Instance::resizeToWindow() {
	auto bounds = RECT{};
	GetClientRect(_handle, &bounds);
	_handler->controller()->put_Bounds(bounds);
}

void Instance::init(std::string js) {
	if (!ready()) {
		_waitingForReady.push_back(InitScript{ std::move(js) });
		return;
	}
	const auto wide = ToWide(js);
	_handler->webview()->AddScriptToExecuteOnDocumentCreated(
		wide.c_str(),
		nullptr);
}

void Instance::eval(std::string js) {
	if (!ready()) {
		_waitingForReady.push_back(EvalScript{ std::move(js) });
		return;
	}
	const auto wide = ToWide(js);
	_handler->webview()->ExecuteScript(wide.c_str(), nullptr);
}

void Instance::focus() {
	if (_window) {
		_window->requestActivate();
	}
	if (_handle) {
		SetForegroundWindow(_handle);
		SetFocus(_handle);
	}
	if (!ready()) {
		_pendingFocus = true;
		return;
	}
	_handler->controller()->MoveFocus(
		COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

QWidget *Instance::widget() {
	return _widget.get();
}

void Instance::refreshNavigationHistoryState() {
	// Not needed here, there are events.
}

auto Instance::navigationHistoryState()
-> rpl::producer<NavigationHistoryState> {
	return _handler
		? _handler->navigationHistoryState()
		: rpl::single(NavigationHistoryState());
}

ZoomController *Instance::zoomController() {
	return _handler->zoomController();
}

void Instance::setOpaqueBg(QColor opaqueBg) {
	if (_handler) {
		_handler->setOpaqueBg(opaqueBg);
	}
}

} // namespace

bool Supported() {
	if (base::options::value<bool>(kOptionWebviewLegacyEdge)) {
		return false;
	}
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
	auto result = std::make_unique<Instance>(std::move(config));
	return result->failed() ? nullptr : std::move(result);
}

} // namespace Webview::EdgeChromium

