// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/webview_embed.h"

#include "webview/details/webview_wrap.h"
#include "base/event_filter.h"
#include "base/invoke_queued.h"

#include <QtWidgets/QWidget>
#include <QtGui/QWindow>

namespace Webview {
namespace {

QWindow *CreateContainerWindow() {
	const auto result = new QWindow();
	result->setFlag(Qt::FramelessWindowHint);
	return result;
}

} // namespace

Window::Window(QWidget *parent)
: _window(CreateContainerWindow())
, _handle((void*)_window->winId())
, _wrap(false, &_handle)
, _widget(
	QWidget::createWindowContainer(
		_window,
		parent,
		Qt::FramelessWindowHint)) {
	base::install_event_filter(_widget, [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Resize || e->type() == QEvent::Move) {
			InvokeQueued(_widget.get(), [=] { _wrap.resizeToWindow(); });
		}
		return base::EventFilterResult::Continue;
	});
	_widget->show();
}

void Window::navigate(const QString &url) {
	_wrap.navigate(url.toStdString());
}

void Window::init(const QByteArray &js) {
	_wrap.init(js.toStdString());
}

void Window::eval(const QByteArray &js) {
	_wrap.eval(js.toStdString());
}

void Window::bind(const QString &name, Fn<void(QByteArray)> callback) {
	Expects(callback != nullptr);

	_wrap.bind(name.toStdString(), [=](
			const std::string&,
			const std::string &result,
			void*) {
		callback(QByteArray::fromStdString(result));
	}, nullptr);
}

} // namespace Webview
