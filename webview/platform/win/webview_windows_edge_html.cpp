// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/win/webview_windows_edge_html.h"

#include "base/platform/win/base_windows_winrt.h"
#include "base/algorithm.h"
#include "base/variant.h"
#include "base/weak_ptr.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

#include <crl/crl_on_main.h>
#include <rpl/variable.h>

#include <windows.h>
#include <objbase.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Web.UI.Interop.h>

namespace Webview::EdgeHtml {
namespace {

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Web::UI;
using namespace Windows::Web::UI::Interop;
using namespace base::WinRT;

class Instance final : public Interface, public base::has_weak_ptr {
public:
	explicit Instance(Config config);

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

	auto navigationHistoryState()
	-> rpl::producer<NavigationHistoryState> override;

	void setOpaqueBg(QColor opaqueBg) override;

private:
	struct NavigateToUrl {
		std::string url;
	};
	struct EvalScript {
		std::string script;
	};
	struct SetOpaqueBg {
		QColor color;
	};

	using ReadyStep = std::variant<
		NavigateToUrl,
		EvalScript,
		SetOpaqueBg>;

	[[nodiscard]] bool ready() const;
	void ready(WebViewControl webview);
	void processReadySteps();
	void updateHistoryStates();

	Config _config;
	HWND _window = nullptr;
	WebViewControlProcess _process;
	WebViewControl _webview = nullptr;
	std::string _initScript;

	rpl::variable<NavigationHistoryState> _navigationHistoryState;

	std::vector<ReadyStep> _waitingForReady;
	bool _pendingResize = false;
	bool _pendingFocus = false;
	bool _readyFlag = false;

};

Instance::Instance(Config config)
: _config(std::move(config))
, _window(static_cast<HWND>(_config.window)) {
	setOpaqueBg(_config.opaqueBg);
	init("window.external.invoke = s => window.external.notify(s)");

	init_apartment(apartment_type::single_threaded);
	const auto weak = base::make_weak(this);
	_process.CreateWebViewControlAsync(
		reinterpret_cast<int64_t>(config.window),
		Rect()
	).Completed([=](
			IAsyncOperation<WebViewControl> that,
			AsyncStatus status) {
		using namespace base::WinRT;
		const auto ok = (status == AsyncStatus::Completed) && Try([&] {
			crl::on_main([=, webview = that.GetResults()]() mutable {
				if (const auto that = weak.get()) {
					that->ready(std::move(webview));
				}
			});
		});
		if (!ok) {
			crl::on_main([=] {
				if (const auto that = weak.get()) {
					that->_window = nullptr;
				}
			});
		}
	});
}

void Instance::ready(WebViewControl webview) {
	auto guard = base::make_weak(this);
	_webview = std::move(webview);

	_webview.ScriptNotify([handler = _config.messageHandler](
			const auto &sender,
			const WebViewControlScriptNotifyEventArgs &args) {
		if (handler) {
			handler(winrt::to_string(args.Value()));
		}
	});
	_webview.NavigationStarting([=, handler = _config.navigationStartHandler](
			const auto &sender,
			const WebViewControlNavigationStartingEventArgs &args) {
		if (handler
			&& !handler(winrt::to_string(args.Uri().AbsoluteUri()), false)) {
			args.Cancel(true);
		} else {
			_webview.AddInitializeScript(winrt::to_hstring(_initScript));
		}
		updateHistoryStates();
	});
	_webview.ContentLoading([=](const auto &sender, const auto &args) {
		updateHistoryStates();
	});
	_webview.DOMContentLoaded([=](const auto &sender, const auto &args) {
		updateHistoryStates();
	});
	_webview.NavigationCompleted([=, handler = _config.navigationDoneHandler](
			const auto &sender,
			const WebViewControlNavigationCompletedEventArgs &args) {
		if (handler) {
			handler(args.IsSuccess());
		}
		updateHistoryStates();
	});
	_webview.NewWindowRequested([=, handler = _config.navigationStartHandler](
			const auto &sender,
			const WebViewControlNewWindowRequestedEventArgs &args) {
		const auto url = winrt::to_string(args.Uri().AbsoluteUri());
		if (handler && handler(url, true)) {
			QDesktopServices::openUrl(QString::fromStdString(url));
		}
	});

	_readyFlag = true;
	processReadySteps();
}

void Instance::updateHistoryStates() {
	_navigationHistoryState = NavigationHistoryState{
		.url = winrt::to_string(_webview.Source().AbsoluteUri()),
		.title = winrt::to_string(_webview.DocumentTitle()),
		.canGoBack = _webview.CanGoBack(),
		.canGoForward = _webview.CanGoForward(),
	};
}

bool Instance::ready() const {
	return _window && _webview && _readyFlag;
}

void Instance::processReadySteps() {
	Expects(ready());

	const auto guard = base::make_weak(this);
	if (!_webview) {
		_window = nullptr;
		return;
	}
	if (guard) {
		_webview.Settings().IsScriptNotifyAllowed(true);
		_webview.IsVisible(true);
	}
	if (guard && _pendingResize) {
		resizeToWindow();
	}
	if (guard) {
		for (const auto &step : base::take(_waitingForReady)) {
			v::match(step, [&](const NavigateToUrl &data) {
				navigate(data.url);
			}, [&](const EvalScript &data) {
				eval(data.script);
			}, [&](const SetOpaqueBg &data) {
				setOpaqueBg(data.color);
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

bool Instance::finishEmbedding() {
	return true;
}

void Instance::navigate(std::string url) {
	if (!ready()) {
		_waitingForReady.push_back(NavigateToUrl{ std::move(url) });
		return;
	}
	_webview.Navigate(Uri(winrt::to_hstring(url)));
}

void Instance::navigateToData(std::string id) {
	Unexpected("EdgeHtml::Instance::navigateToData.");
}

void Instance::reload() {
	_webview.Refresh();
}

void Instance::init(std::string js) {
	_initScript = _initScript + "(function(){" + js + "})();";
}

void Instance::eval(std::string js) {
	if (!ready()) {
		_waitingForReady.push_back(EvalScript{ std::move(js) });
		return;
	}
	_webview.InvokeScriptAsync(
		L"eval",
		single_threaded_vector<hstring>({ winrt::to_hstring(js) }));
	_webview.InvokeScriptAsync(
		L"eval",
		single_threaded_vector<hstring>({ winrt::to_hstring("document.body.style.backgroundColor='transparent';")}));
	_webview.InvokeScriptAsync(
		L"eval",
		single_threaded_vector<hstring>({ winrt::to_hstring("document.getElementsByTagName('html')[0].style.backgroundColor='transparent';") }));
}

void Instance::focus() {
	if (_window) {
		SetForegroundWindow(_window);
		SetFocus(_window);
	}
	if (!ready()) {
		_pendingFocus = true;
		return;
	}
	_webview.MoveFocus(WebViewControlMoveFocusReason::Programmatic);
}

QWidget *Instance::widget() {
	return nullptr;
}

void *Instance::winId() {
	return nullptr;
}

auto Instance::navigationHistoryState()
-> rpl::producer<NavigationHistoryState> {
	return _navigationHistoryState.value();
}

void Instance::setOpaqueBg(QColor opaqueBg) {
	if (!ready()) {
		_waitingForReady.push_back(SetOpaqueBg{ opaqueBg });
		return;
	}
	_webview.DefaultBackgroundColor({
		uchar(opaqueBg.alpha()),
		uchar(opaqueBg.red()),
		uchar(opaqueBg.green()),
		uchar(opaqueBg.blue())
	});
}

void Instance::resizeToWindow() {
	if (!ready()) {
		_pendingResize = true;
		return;
	}
	RECT r;
	GetClientRect(_window, &r);
	Rect bounds(r.left, r.top, r.right - r.left, r.bottom - r.top);
	_webview.Bounds(bounds);
}

} // namespace

bool Supported() {
	return Try([&] {
		return (WebViewControlProcess() != nullptr);
	}).value_or(false);
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	return Try([&]() -> std::unique_ptr<Interface> {
		return std::make_unique<Instance>(std::move(config));
	}).value_or(nullptr);
}

} // namespace Webview::EdgeHtml
