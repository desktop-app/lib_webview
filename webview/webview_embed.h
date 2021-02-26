// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "webview/details/webview_wrap.h"
#include "base/unique_qptr.h"
#include "base/basic_types.h"

class QString;
class QWidget;
class QWindow;

namespace Webview {

class Window final {
public:
	explicit Window(QWidget *parent = nullptr);

	QWidget *widget() {
		return _widget.get();
	}

	void navigate(const QString &url);
	void init(const QByteArray &js);
	void eval(const QByteArray &js);
	void bind(const QString &name, Fn<void(QByteArray)> callback);

private:
	QWindow *_window = nullptr;
	void *_handle = nullptr;
	details::Wrap _wrap;
	base::unique_qptr<QWidget> _widget;

};

} // namespace Webview
