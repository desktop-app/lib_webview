// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/unique_qptr.h"
#include "base/basic_types.h"
#include "webview/webview_common.h"

#include <rpl/lifetime.h>
#include <QColor>

class QString;
class QWidget;
class QWindow;

namespace Webview {

extern const char kOptionWebviewDebugEnabled[];

struct DialogArgs;
struct DialogResult;
class Interface;
struct Config;
struct DataRequest;
enum class DataResult;

struct WindowConfig {
	QColor opaqueBg;
	StorageId storageId;
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

	void updateTheme(
		QColor opaqueBg,
		QColor scrollBg,
		QColor scrollBgOver,
		QColor scrollBarBg,
		QColor scrollBarBgOver);
	void navigate(const QString &url);
	void navigateToData(const QString &id);
	void reload();
	void setMessageHandler(Fn<void(std::string)> handler);
	void setMessageHandler(Fn<void(const QJsonDocument&)> handler);
	void setNavigationStartHandler(Fn<bool(QString,bool)> handler);
	void setNavigationDoneHandler(Fn<void(bool)> handler);
	void setDialogHandler(Fn<DialogResult(DialogArgs)> handler);
	void setDataRequestHandler(Fn<DataResult(DataRequest)> handler);
	void init(const QByteArray &js);
	void eval(const QByteArray &js);

	void focus();

	[[nodiscard]] rpl::lifetime &lifetime() {
		return _lifetime;
	}

private:
	bool createWebView(QWidget *parent, const WindowConfig &config);
	bool finishWebviewEmbedding();
	[[nodiscard]] Fn<void(std::string)> messageHandler() const;
	[[nodiscard]] Fn<bool(std::string,bool)> navigationStartHandler() const;
	[[nodiscard]] Fn<void(bool)> navigationDoneHandler() const;
	[[nodiscard]] Fn<DialogResult(DialogArgs)> dialogHandler() const;
	[[nodiscard]] Fn<DataResult(DataRequest)> dataRequestHandler() const;

	std::unique_ptr<Interface> _webview;
	base::unique_qptr<QWidget> _widget;
	base::unique_qptr<QWindow> _window;
	Fn<void(std::string)> _messageHandler;
	Fn<bool(std::string,bool)> _navigationStartHandler;
	Fn<void(bool)> _navigationDoneHandler;
	Fn<DialogResult(DialogArgs)> _dialogHandler;
	Fn<DataResult(DataRequest)> _dataRequestHandler;
	rpl::lifetime _lifetime;

};

} // namespace Webview
