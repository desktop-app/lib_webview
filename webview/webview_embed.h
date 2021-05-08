// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/unique_qptr.h"
#include "base/basic_types.h"

class QString;
class QWidget;
class QWindow;

namespace Webview {

class Interface;
struct Config;

struct WindowConfig {
	QString userDataPath;
};

class Window final {
public:
	explicit Window(
		QWidget *parent = nullptr,
		WindowConfig config = WindowConfig());
	~Window();

	// Returns 'nullptr' in case of an error.
	QWidget *widget() {
		return _widget.get();
	}

	void navigate(const QString &url);
	void setMessageHandler(Fn<void(std::string)> handler);
	void setMessageHandler(Fn<void(const QJsonDocument&)> handler);
	void setNavigationStartHandler(Fn<bool(QString)> handler);
	void setNavigationDoneHandler(Fn<void(bool)> handler);
	void init(const QByteArray &js);
	void eval(const QByteArray &js);

private:
	bool createWebView(const WindowConfig &config);
	bool finishWebviewEmbedding();
	[[nodiscard]] Fn<void(std::string)> messageHandler() const;
	[[nodiscard]] Fn<bool(std::string)> navigationStartHandler() const;
	[[nodiscard]] Fn<void(bool)> navigationDoneHandler() const;

	QWindow *_window = nullptr;
	std::unique_ptr<Interface> _webview;
	base::unique_qptr<QWidget> _widget;
	Fn<void(std::string)> _messageHandler;
	Fn<bool(std::string)> _navigationStartHandler;
	Fn<void(bool)> _navigationDoneHandler;

};

} // namespace Webview
