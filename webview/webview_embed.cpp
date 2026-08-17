// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/webview_embed.h"

#include "base/platform/base_platform_info.h"
#include "base/debug_log.h"
#include "base/event_filter.h"
#include "base/integration.h"
#include "base/invoke_queued.h"
#include "base/options.h"
#include "webview/webview_data_stream.h"
#include "webview/webview_dialog.h"
#include "webview/webview_interface.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>

#include <charconv>

namespace Webview {
namespace {

base::options::toggle OptionWebviewDebugEnabled({
	.id = kOptionWebviewDebugEnabled,
	.name = "Enable webview inspecting",
	.description = "Right click and choose Inspect in the webview windows. (on macOS launch Safari, open from Develop menu)",
});

base::options::toggle OptionWebviewLegacyEdge({
	.id = kOptionWebviewLegacyEdge,
	.name = "Force legacy Edge WebView",
	.description = "Skip modern CoreWebView2 check and force using legacy Edge WebView on Windows.",
	.scope = base::options::windows,
	.restartRequired = true,
});

[[nodiscard]] QByteArray RestrictedScript(const QString &origin) {
	const auto url = QUrl(origin, QUrl::StrictMode);
	if (!url.isValid()
		|| url.scheme() != u"https"_q
		|| url.host().isEmpty()
		|| !url.path().isEmpty()
		|| !url.query().isEmpty()
		|| !url.fragment().isEmpty()
		|| !url.userInfo().isEmpty()
		|| url.port(-1) != -1) {
		return {};
	}

	auto websocket = url;
	websocket.setScheme(u"wss"_q);
	const auto policy = u"default-src 'none'; "_q
		+ u"base-uri 'none'; "_q
		+ u"child-src 'none'; "_q
		+ u"connect-src "_q
		+ origin
		+ u" "_q
		+ websocket.toString(QUrl::FullyEncoded)
		+ u"; font-src 'none'; "_q
		+ u"form-action 'none'; "_q
		+ u"frame-src 'none'; "_q
		+ u"img-src 'none'; "_q
		+ u"manifest-src 'none'; "_q
		+ u"media-src 'none'; "_q
		+ u"object-src 'none'; "_q
		+ u"script-src 'unsafe-inline'; "_q
		+ u"style-src 'none'; "_q
		+ u"worker-src 'none'"_q;
	const auto encoded = QJsonDocument(QJsonArray{ policy }).toJson(
		QJsonDocument::Compact);
	const auto literal = encoded.mid(1, encoded.size() - 2);
	return "(()=>{const policy="
		+ literal
		+ R"JS(;
const root=document.documentElement||document.appendChild(document.createElement('html'));
const head=document.head||root.insertBefore(document.createElement('head'),root.firstChild);
const dns=document.createElement('meta');
dns.httpEquiv='x-dns-prefetch-control';
dns.content='off';
head.insertBefore(dns,head.firstChild);
const meta=document.createElement('meta');
meta.httpEquiv='Content-Security-Policy';
meta.content=policy;
head.insertBefore(meta,head.firstChild);
const lock=(object,name,value)=>{try{Object.defineProperty(object,name,{value,configurable:false,writable:false})}catch(error){}};
for(const name of ['localStorage','sessionStorage','indexedDB','caches','Worker','SharedWorker','BroadcastChannel','Audio','AudioContext','webkitAudioContext','OfflineAudioContext','webkitOfflineAudioContext','speechSynthesis','SpeechSynthesisUtterance'])lock(globalThis,name,undefined);
for(const name of ['serviceWorker','clipboard','geolocation','mediaDevices','usb','serial','hid','bluetooth'])lock(navigator,name,undefined);
lock(globalThis,'open',()=>null);
lock(globalThis,'print',()=>{});
lock(globalThis,'alert',()=>{});
lock(globalThis,'confirm',()=>false);
lock(globalThis,'prompt',()=>null);
try{Object.defineProperty(document,'cookie',{get:()=>'',set:()=>true,configurable:false})}catch(error){}
})())JS";
}

} // namespace

const char kOptionWebviewDebugEnabled[] = "webview-debug-enabled";

const char kOptionWebviewLegacyEdge[] = "webview-legacy-edge";

Window::Window(QWidget *parent, WindowConfig config) {
	if (createWebView(parent, config)
		&& config.mode != WindowMode::Hidden) {
		setDialogHandler(nullptr);
	}
}

Window::~Window() = default;

bool Window::valid() const {
	return (_webview != nullptr);
}

bool Window::createWebView(QWidget *parent, const WindowConfig &config) {
	Expects(!_webview);
	const auto restrictedScript = config.restrictedOrigin.isEmpty()
		? QByteArray()
		: RestrictedScript(config.restrictedOrigin);
	if (!config.restrictedOrigin.isEmpty() && restrictedScript.isEmpty()) {
		return false;
	}
	auto userDataPath = config.storageId.path;
	if (!config.restrictedOrigin.isEmpty()) {
		_temporaryStorage = std::make_unique<QTemporaryDir>();
		if (!_temporaryStorage->isValid()) {
			_temporaryStorage = nullptr;
			return false;
		}
		userDataPath = _temporaryStorage->path();
	}

	_webview = CreateInstance({
		.parent = parent,
		.opaqueBg = config.opaqueBg,
		.messageHandler = messageHandler(),
		.navigationStartHandler = navigationStartHandler(),
		.navigationDoneHandler = navigationDoneHandler(),
		.externalWindowCloseHandler = externalWindowCloseHandler(),
		.dialogHandler = dialogHandler(),
		.asyncDialogHandler = asyncDialogHandler(),
		.dataRequestHandler = dataRequestHandler(),
		.dataProtocolOverride = config.dataProtocolOverride.toStdString(),
		.dataRequestRedirectHost = config.dataRequestRedirectHost.toStdString(),
		.userDataPath = userDataPath.toStdString(),
		.userDataToken = config.storageId.token.toStdString(),
		.debug = OptionWebviewDebugEnabled.value(),
		.safe = config.safe,
		.allowThirdPartyCookies = config.allowThirdPartyCookies,
		.mode = config.mode,
		.windowStyle = config.windowStyle,
		.windowMargins = config.windowMargins,
		.initialSize = config.initialSize,
		.shellMessageToken = config.shellMessageToken.toStdString(),
		.restrictedOrigin = config.restrictedOrigin.toStdString(),
	});
	if (_webview && !config.restrictedOrigin.isEmpty()) {
		_webview->init(restrictedScript.toStdString());
	}
	return (_webview != nullptr);
}

QWidget *Window::widget() const {
	return _webview ? _webview->widget() : nullptr;
}

void *Window::winId() const {
	return _webview ? _webview->winId() : nullptr;
}

PopupAnchor Window::popupAnchor() const {
	return _webview ? _webview->popupAnchor() : PopupAnchor();
}

void Window::updateTheme(
		QColor opaqueBg,
		QColor scrollBg,
		QColor scrollBgOver,
		QColor scrollBarBg,
		QColor scrollBarBgOver) {
	if (!_webview) {
		return;
	}
#ifndef Q_OS_MAC
	const auto wrap = [](QColor color) {
		return u"rgba(%1, %2, %3, %4)"_q
			.arg(color.red())
			.arg(color.green())
			.arg(color.blue())
			.arg(color.alphaF()).toStdString();
	};
	const auto function = R"(
function() {
	const style = document.createElement('style');
	style.textContent = ' \
::-webkit-scrollbar { \
	border-radius: 5px !important; \
	border: 3px solid transparent !important; \
	background-color: )" + wrap(scrollBg) + R"( !important; \
	background-clip: content-box !important; \
	width: 10px !important; \
} \
::-webkit-scrollbar:hover { \
	background-color: )" + wrap(scrollBgOver) + R"( !important; \
} \
::-webkit-scrollbar-thumb { \
	border-radius: 5px !important; \
	border: 3px solid transparent !important; \
	background-color: )" + wrap(scrollBarBg) + R"( !important; \
	background-clip: content-box !important; \
} \
::-webkit-scrollbar-thumb:hover { \
	background-color: )" + wrap(scrollBarBgOver) + R"( !important; \
} \
';
  document.head.append(style);
}
)";
	_webview->init(
		"document.addEventListener('DOMContentLoaded', "
		+ function
		+ ", false);");
	_webview->eval("(" + function + "());");
#endif
	_webview->setOpaqueBg(opaqueBg);
}

void Window::navigate(const QString &url) {
	Expects(_webview != nullptr);

	_webview->navigate(url.toStdString());
}

void Window::navigateToData(const QString &id) {
	Expects(_webview != nullptr);

	_webview->navigateToData(id.toStdString());
}

void Window::loadHtml(const QString &html, const QString &baseUrl) {
	Expects(_webview != nullptr);

	_webview->loadHtml(html.toStdString(), baseUrl.toStdString());
}

void Window::reload() {
	Expects(_webview != nullptr);

	_webview->reload();
}

void Window::init(const QByteArray &js) {
	Expects(_webview != nullptr);

	_webview->init(js.toStdString());
}

void Window::eval(const QByteArray &js) {
	Expects(_webview != nullptr);

	_webview->eval(js.toStdString());
}

void Window::focus() {
	Expects(_webview != nullptr);

	_webview->focus();
}

void Window::resize(QSize size) {
	Expects(_webview != nullptr);

	_webview->resize(size.width(), size.height());
}

void Window::setFullscreen(bool fullscreen) {
	Expects(_webview != nullptr);

	_webview->setFullscreen(fullscreen);
}

void Window::setInteractionHandler(Fn<void()> handler) {
	_interactionHandler = std::move(handler);
	if (_webview) {
		_webview->setInteractionHandler([=] {
			if (_interactionHandler) {
				base::Integration::Instance().enterFromEventLoop([&] {
					_interactionHandler();
				});
			}
		});
	}
}

void Window::refreshNavigationHistoryState() {
	Expects(_webview != nullptr);

	_webview->refreshNavigationHistoryState();
}

auto Window::navigationHistoryState() const
-> rpl::producer<NavigationHistoryState>{
	Expects(_webview != nullptr);

	return [data = _webview->navigationHistoryState()](
			auto consumer) mutable {
		auto result = rpl::lifetime();

		std::move(
			data
		) | rpl::on_next([=](NavigationHistoryState state) {
			base::Integration::Instance().enterFromEventLoop([&] {
				consumer.put_next_copy(state);
			});
		}, result);

		return result;
	};
}

ZoomController *Window::zoomController() const {
	return _webview ? _webview->zoomController() : nullptr;
}

void Window::setMessageHandler(Fn<void(Message)> handler) {
	_messageHandler = std::move(handler);
}

void Window::setMessageHandler(Fn<void(std::string)> handler) {
	if (!handler) {
		setMessageHandler(Fn<void(Message)>());
		return;
	}
	setMessageHandler([=](Message message) {
		handler(std::move(message.text));
	});
}

void Window::setMessageHandler(Fn<void(const QJsonDocument&)> handler) {
	if (!handler) {
		setMessageHandler(Fn<void(Message)>());
		return;
	}
	setMessageHandler([=](Message message) {
		auto error = QJsonParseError();
		auto document = QJsonDocument::fromJson(
			QByteArray::fromRawData(message.text.data(), message.text.size()),
			&error);
		if (error.error == QJsonParseError::NoError) {
			handler(std::move(document));
		}
	});
}

Fn<void(Message)> Window::messageHandler() const {
	return [=](Message message) {
		if (_messageHandler) {
			base::Integration::Instance().enterFromEventLoop([&] {
				_messageHandler(std::move(message));
			});
		}
	};
}

void Window::setNavigationStartHandler(Fn<bool(QString,bool)> handler) {
	if (!handler) {
		_navigationStartHandler = nullptr;
		return;
	}
	_navigationStartHandler = [=](std::string uri, bool newWindow) {
		return handler(QString::fromStdString(uri), newWindow);
	};
}

void Window::setNavigationDoneHandler(Fn<void(bool)> handler) {
	_navigationDoneHandler = std::move(handler);
}

void Window::setExternalWindowCloseHandler(Fn<void()> handler) {
	_externalWindowCloseHandler = std::move(handler);
}

void Window::setDialogHandler(Fn<DialogResult(DialogArgs)> handler) {
	_dialogHandler = handler ? handler : DefaultDialogHandler;
}

void Window::setAsyncDialogHandler(AsyncDialogHandler handler) {
	_asyncDialogHandler = std::move(handler);
}

void Window::setDataRequestHandler(Fn<DataResult(DataRequest)> handler) {
	_dataRequestHandler = std::move(handler);
}

Fn<bool(std::string,bool)> Window::navigationStartHandler() const {
	return [=](std::string message, bool newWindow) {
		const auto lower = QString::fromStdString(message).toLower();
		if (!lower.startsWith(u"http://"_q)
			&& !lower.startsWith(u"https://"_q)
			&& !lower.startsWith(u"tonsite://"_q)
			&& !lower.startsWith(u"ton://"_q)) {
			return false;
		}
		auto result = true;
		if (_navigationStartHandler) {
			base::Integration::Instance().enterFromEventLoop([&] {
				result = _navigationStartHandler(
					std::move(message),
					newWindow);
			});
		}
		return result;
	};
}

Fn<void(bool)> Window::navigationDoneHandler() const {
	return [=](bool success) {
		if (_navigationDoneHandler) {
			base::Integration::Instance().enterFromEventLoop([&] {
				_navigationDoneHandler(success);
			});
		}
	};
}

Fn<void()> Window::externalWindowCloseHandler() const {
	return [=] {
		if (_externalWindowCloseHandler) {
			base::Integration::Instance().enterFromEventLoop([&] {
				_externalWindowCloseHandler();
			});
		}
	};
}

Fn<DialogResult(DialogArgs)> Window::dialogHandler() const {
	return [=](DialogArgs args) {
		auto result = DialogResult();
		if (_dialogHandler) {
			base::Integration::Instance().enterFromEventLoop([&] {
				args.parent = widget();
				result = _dialogHandler(std::move(args));
			});
		}
		return result;
	};
}

AsyncDialogHandler Window::asyncDialogHandler() const {
	return [=](
			DialogArgs args,
			std::function<void(DialogResult)> done) {
		if (!_asyncDialogHandler) {
			return false;
		}
		auto result = false;
		base::Integration::Instance().enterFromEventLoop([&] {
			args.parent = widget();
			result = _asyncDialogHandler(std::move(args), std::move(done));
		});
		return result;
	};
}

Fn<DataResult(DataRequest)> Window::dataRequestHandler() const {
	return [=](DataRequest request) {
		return _dataRequestHandler
			? _dataRequestHandler(std::move(request))
			: DataResult::Failed;
	};
}

void ParseRangeHeaderFor(DataRequest &request, std::string_view header) {
	const auto unsupported = [&] {
		LOG(("Unsupported range header: ")
			+ QString::fromUtf8(header.data(), header.size()));
	};
	if (header.compare(0, 6, "bytes=")) {
		return unsupported();
	}
	const auto range = std::string_view(header).substr(6);
	const auto separator = range.find('-');
	if (separator == range.npos) {
		return unsupported();
	}
	const auto startFrom = range.data();
	const auto startTill = startFrom + separator;
	const auto finishFrom = startTill + 1;
	const auto finishTill = startFrom + range.size();
	if (finishTill > finishFrom) {
		const auto done = std::from_chars(
			finishFrom,
			finishTill,
			request.limit);
		if (done.ec != std::errc() || done.ptr != finishTill) {
			request.limit = 0;
			return unsupported();
		}
		request.limit += 1; // 0-499 means first 500 bytes.
	} else {
		request.limit = -1;
	}
	if (startTill > startFrom) {
		const auto done = std::from_chars(
			startFrom,
			startTill,
			request.offset);
		if (done.ec != std::errc() || done.ptr != startTill) {
			request.offset = request.limit = 0;
			return unsupported();
		} else if (request.limit > 0) {
			request.limit -= request.offset;
			if (request.limit <= 0) {
				request.offset = request.limit = 0;
				return unsupported();
			}
		}
	}
}

} // namespace Webview
