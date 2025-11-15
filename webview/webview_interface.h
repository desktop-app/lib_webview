// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "webview/webview_common.h"

#include <memory>
#include <string>
#include <optional>
#include <functional>

#include <rpl/never.h>
#include <rpl/producer.h>

#include <QtGui/QColor>

// Inspired by https://github.com/webview/webview.

class QWidget;

namespace Webview {

class DataStream;

struct NavigationHistoryState {
	std::string url;
	std::string title;
	bool canGoBack : 1 = false;
	bool canGoForward : 1 = false;

	friend inline constexpr bool operator==(
		NavigationHistoryState,
		NavigationHistoryState) = default;
};

class ZoomController {
public:
	ZoomController() = default;

	[[nodiscard]] virtual rpl::producer<int> zoomValue() {
		return rpl::never<int>();
	}
	virtual void setZoom(int) {
	}
};

class Interface {
public:
	virtual ~Interface() = default;

	virtual void navigate(std::string url) = 0;
	virtual void navigateToData(std::string id) = 0;
	virtual void reload() = 0;

	virtual void init(std::string js) = 0;
	virtual void eval(std::string js) = 0;

	virtual void focus() = 0;

	virtual void setOpaqueBg(QColor opaqueBg) = 0;

	[[nodiscard]] virtual QWidget *widget() = 0;

	virtual void refreshNavigationHistoryState() = 0;
	[[nodiscard]] virtual auto navigationHistoryState()
		-> rpl::producer<NavigationHistoryState> = 0;

	[[nodiscard]] virtual ZoomController *zoomController() {
		return nullptr;
	}

};

enum class DialogType {
	Alert,
	Confirm,
	Prompt,
};

struct DialogArgs {
	QWidget *parent = nullptr;
	DialogType type = DialogType::Alert;
	std::string value;
	std::string text;
	std::string url;
};

struct DialogResult {
	std::string text;
	bool accepted = false;
};

struct DataResponse {
	std::unique_ptr<DataStream> stream;
	std::int64_t streamOffset = 0;
	std::int64_t totalSize = 0;
};

struct DataRequest {
	std::string id;
	std::int64_t offset = 0;
	std::int64_t limit = 0; // < 0 means "Range: bytes=offset-" header.
	std::function<void(DataResponse)> done;
};

enum class DataResult {
	Done,
	Pending,
	Failed,
};

struct Config {
	QWidget *parent = nullptr;
	QColor opaqueBg;
	std::function<void(std::string)> messageHandler;
	std::function<bool(std::string,bool)> navigationStartHandler;
	std::function<void(bool)> navigationDoneHandler;
	std::function<DialogResult(DialogArgs)> dialogHandler;
	std::function<DataResult(DataRequest)> dataRequestHandler;
	std::string dataProtocolOverride;
	std::string userDataPath;
	std::string userDataToken;
	bool debug = false;
	bool safe = false;
};

struct Available {
	enum class Error {
		None,
		NoWebview2,
		NoWebKitGTK,
		OldWindows,
	};
	Error error = Error::None;
	bool customSchemeRequests = false;
	bool customRangeRequests = false;
	bool customReferer = false;
	std::string details;
};

void ParseRangeHeaderFor(DataRequest &request, std::string_view header);

[[nodiscard]] Available Availability();
[[nodiscard]] inline bool Supported() {
	return Availability().error == Available::Error::None;
}
[[nodiscard]] bool SupportsEmbedAfterCreate();
[[nodiscard]] bool SeparateStorageIdSupported();

// HWND on Windows, nullptr on macOS, GtkWindow on Linux.
[[nodiscard]] std::unique_ptr<Interface> CreateInstance(Config config);

[[nodiscard]] std::string GenerateStorageToken();
void ClearStorageDataByToken(const std::string &token);

} // namespace Webview
