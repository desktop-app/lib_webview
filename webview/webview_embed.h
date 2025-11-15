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
#include <rpl/producer.h>
#include <QColor>

class QString;
class QWidget;
class QWindow;

namespace Webview {

extern const char kOptionWebviewDebugEnabled[];
extern const char kOptionWebviewLegacyEdge[];

struct DialogArgs;
struct DialogResult;
class Interface;
class ZoomController;
struct Config;
struct DataRequest;
enum class DataResult;
struct NavigationHistoryState;

struct WindowConfig {
	QColor opaqueBg;
	StorageId storageId;
	QString dataProtocolOverride;
	bool safe = false;
};

class Window final {
public:
	explicit Window(
		QWidget *parent = nullptr,
		WindowConfig config = WindowConfig());
	~Window();

	// May be nullptr or destroyed any time (in case webview crashed).
	[[nodiscard]] QWidget *widget() const;

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

	void refreshNavigationHistoryState();
	[[nodiscard]] auto navigationHistoryState() const
	-> rpl::producer<NavigationHistoryState>;

	[[nodiscard]] ZoomController *zoomController() const;

	[[nodiscard]] rpl::lifetime &lifetime() {
		return _lifetime;
	}

private:
	bool createWebView(QWidget *parent, const WindowConfig &config);
	[[nodiscard]] Fn<void(std::string)> messageHandler() const;
	[[nodiscard]] Fn<bool(std::string,bool)> navigationStartHandler() const;
	[[nodiscard]] Fn<void(bool)> navigationDoneHandler() const;
	[[nodiscard]] Fn<DialogResult(DialogArgs)> dialogHandler() const;
	[[nodiscard]] Fn<DataResult(DataRequest)> dataRequestHandler() const;

	std::unique_ptr<Interface> _webview;
	Fn<void(std::string)> _messageHandler;
	Fn<bool(std::string,bool)> _navigationStartHandler;
	Fn<void(bool)> _navigationDoneHandler;
	Fn<DialogResult(DialogArgs)> _dialogHandler;
	Fn<DataResult(DataRequest)> _dataRequestHandler;
	rpl::lifetime _lifetime;

};

} // namespace Webview
