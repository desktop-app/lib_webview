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
#include "base/unique_qptr.h"
#include "ui/rp_widget.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>

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
	~Instance();

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
	base::unique_qptr<QWindow> _window;
	HWND _handle = nullptr;
	WebViewControlProcess _process;
	WebViewControl _webview = nullptr;
	base::unique_qptr<Ui::RpWidget> _widget;
	QPointer<QWidget> _embed;
	std::string _initScript;

	rpl::variable<NavigationHistoryState> _navigationHistoryState;

	std::vector<ReadyStep> _waitingForReady;
	bool _pendingFocus = false;
	bool _readyFlag = false;

};

Instance::Instance(Config config)
: _config(std::move(config))
, _window(MakeFramelessWindow())
, _handle(HWND(_window->winId()))
, _widget(base::make_unique_q<Ui::RpWidget>(config.parent)) {
	_widget->show();

	setOpaqueBg(_config.opaqueBg);
	init("window.external.invoke = s => window.external.notify(s)");

	init_apartment(apartment_type::single_threaded);
	const auto weak = base::make_weak(this);
	_process.CreateWebViewControlAsync(
		reinterpret_cast<int64_t>(_handle),
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
					that->_widget = nullptr;
					that->_handle = nullptr;
					that->_window = nullptr;
				}
			});
		}
	});

	_process.ProcessExited([=](const auto &sender, const auto &args) {
		_webview = nullptr;
		crl::on_main([=] {
			if (const auto that = weak.get()) {
				that->_embed = nullptr;
				that->_widget = nullptr;
				that->_handle = nullptr;
				that->_window = nullptr;
			}
		});
	});
}

Instance::~Instance() {
	if (ready()) {
		base::WinRT::Try([&] {
			std::exchange(_webview, WebViewControl(nullptr)).Close();
		});
	}
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

	_embed = QWidget::createWindowContainer(
		_window,
		_widget.get(),
		Qt::FramelessWindowHint);
	_embed->show();

	_readyFlag = true;
	processReadySteps();
}

void Instance::updateHistoryStates() {
	base::WinRT::Try([&] {
		_navigationHistoryState = NavigationHistoryState{
			.url = winrt::to_string(_webview.Source().AbsoluteUri()),
			.title = winrt::to_string(_webview.DocumentTitle()),
			.canGoBack = _webview.CanGoBack(),
			.canGoForward = _webview.CanGoForward(),
		};
	});
}

bool Instance::ready() const {
	return _handle && _webview && _readyFlag;
}

void Instance::processReadySteps() {
	Expects(ready());

	const auto guard = base::make_weak(this);
	if (!_webview) {
		_widget = nullptr;
		_handle = nullptr;
		_window = nullptr;
		return;
	}
	if (guard) {
		base::WinRT::Try([&] {
			_webview.Settings().IsScriptNotifyAllowed(true);
			_webview.IsVisible(true);
		});
	}
	if (guard) {
		_widget->sizeValue() | rpl::on_next([=](QSize size) {
			_embed->setGeometry(QRect(QPoint(), size));

			if (!ready()) {
				return;
			}
			RECT r;
			GetClientRect(_handle, &r);
			Rect bounds(r.left, r.top, r.right - r.left, r.bottom - r.top);
			base::WinRT::Try([&] {
				_webview.Bounds(bounds);
			});
		}, _widget->lifetime());
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

void Instance::navigate(std::string url) {
	if (!ready()) {
		_waitingForReady.push_back(NavigateToUrl{ std::move(url) });
		return;
	}
	base::WinRT::Try([&] {
		_webview.Navigate(Uri(winrt::to_hstring(url)));
	});
}

void Instance::navigateToData(std::string id) {
	Unexpected("EdgeHtml::Instance::navigateToData.");
}

void Instance::reload() {
	if (!ready()) {
		return;
	}
	base::WinRT::Try([&] {
		_webview.Refresh();
	});
}

void Instance::init(std::string js) {
	_initScript = _initScript + "(function(){" + js + "})();";
}

void Instance::eval(std::string js) {
	if (!ready()) {
		_waitingForReady.push_back(EvalScript{ std::move(js) });
		return;
	}
	base::WinRT::Try([&] {
		_webview.InvokeScriptAsync(
			L"eval",
			single_threaded_vector<hstring>({ winrt::to_hstring(js) }));
		_webview.InvokeScriptAsync(
			L"eval",
			single_threaded_vector<hstring>({ winrt::to_hstring(
				"document.body.style.backgroundColor='transparent';") }));
		_webview.InvokeScriptAsync(
			L"eval",
			single_threaded_vector<hstring>({ winrt::to_hstring(
				"document.getElementsByTagName('html')[0].style.backgroundColor='transparent';") }));
	});
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
	base::WinRT::Try([&] {
		_webview.MoveFocus(WebViewControlMoveFocusReason::Programmatic);
	});
}

QWidget *Instance::widget() {
	return _widget.get();
}

void Instance::refreshNavigationHistoryState() {
	updateHistoryStates();
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
	base::WinRT::Try([&] {
		_webview.DefaultBackgroundColor({
			uchar(opaqueBg.alpha()),
			uchar(opaqueBg.red()),
			uchar(opaqueBg.green()),
			uchar(opaqueBg.blue())
		});
	});
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
