// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/webview_embed.h"

#include "webview/webview_interface.h"
#include "base/event_filter.h"
#include "base/invoke_queued.h"
#include "base/platform/base_platform_info.h"
#include "base/integration.h"

#include <QtWidgets/QWidget>
#include <QtGui/QWindow>
#include <QtCore/QJsonDocument>

namespace Webview {
namespace {

[[nodiscard]] QWindow *CreateContainerWindow() {
	if constexpr (Platform::IsWindows()) {
		const auto result = new QWindow();
		result->setFlag(Qt::FramelessWindowHint);
		return result;
	} else {
		return nullptr;
	}
}

[[nodiscard]] QWindow *CreateContainerWindow(not_null<Interface*> webview) {
	const auto id = webview->winId();
	return id ? QWindow::fromWinId(WId(id)) : nullptr;
}

} // namespace

Window::Window(QWidget *parent, WindowConfig config)
: _window(CreateContainerWindow()) {
	if (SupportsEmbedAfterCreate()) {
		if (!createWebView(config)) {
			return;
		}
		if (!_window) {
			_window = CreateContainerWindow(_webview.get());
		}
	}
	if (!_window) {
		return;
	}
	_widget.reset(
		QWidget::createWindowContainer(
			_window,
			parent,
			Qt::FramelessWindowHint));
	_widget->show();
	if (!createWebView(config) || !finishWebviewEmbedding()) {
		return;
	}
	_webview->resizeToWindow();
	base::install_event_filter(_widget, [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Resize || e->type() == QEvent::Move) {
			InvokeQueued(_widget.get(), [=] { _webview->resizeToWindow(); });
		}
		return base::EventFilterResult::Continue;
	});
}

Window::~Window() = default;

bool Window::createWebView(const WindowConfig &config) {
	if (!_webview) {
		_webview = CreateInstance({
			.window = _window ? (void*)_window->winId() : nullptr,
			.messageHandler = messageHandler(),
			.navigationStartHandler = navigationStartHandler(),
			.navigationDoneHandler = navigationDoneHandler(),
			.userDataPath = config.userDataPath.toStdString(),
		});
	}
	if (_webview) {
		return true;
	}
	delete _window;
	_window = nullptr;
	_widget = nullptr;
	return false;
}

bool Window::finishWebviewEmbedding() {
	Expects(_webview != nullptr);
	Expects(_widget != nullptr);
	Expects(_window != nullptr);

	if (_webview->finishEmbedding()) {
		return true;
	}
	delete _window;
	_window = nullptr;
	_widget = nullptr;
	_webview = nullptr;
	return false;
}

void Window::navigate(const QString &url) {
	Expects(_webview != nullptr);

	_webview->navigate(url.toStdString());
}

void Window::init(const QByteArray &js) {
	Expects(_webview != nullptr);

	_webview->init(js.toStdString());
}

void Window::eval(const QByteArray &js) {
	Expects(_webview != nullptr);

	_webview->eval(js.toStdString());
}

void Window::setMessageHandler(Fn<void(std::string)> handler) {
	_messageHandler = std::move(handler);
}

void Window::setMessageHandler(Fn<void(const QJsonDocument&)> handler) {
	if (!handler) {
		setMessageHandler(Fn<void(std::string)>());
		return;
	}
	setMessageHandler([=](std::string text) {
		auto error = QJsonParseError();
		auto document = QJsonDocument::fromJson(
			QByteArray::fromRawData(text.data(), text.size()),
			&error);
		if (error.error == QJsonParseError::NoError) {
			handler(std::move(document));
		}
	});
}

Fn<void(std::string)> Window::messageHandler() const {
	return [=](std::string message) {
		if (_messageHandler) {
			base::Integration::Instance().enterFromEventLoop([&] {
				_messageHandler(std::move(message));
			});
		}
	};
}

void Window::setNavigationStartHandler(Fn<bool(QString)> handler) {
	if (!handler) {
		_navigationStartHandler = nullptr;
		return;
	}
	_navigationStartHandler = [=](std::string uri) {
		return handler(QString::fromStdString(uri));
	};
}

void Window::setNavigationDoneHandler(Fn<void(bool)> handler) {
	_navigationDoneHandler = std::move(handler);
}

Fn<bool(std::string)> Window::navigationStartHandler() const {
	return [=](std::string message) {
		auto result = true;
		if (_navigationStartHandler) {
			base::Integration::Instance().enterFromEventLoop([&] {
				result = _navigationStartHandler(std::move(message));
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

} // namespace Webview
