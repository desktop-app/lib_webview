// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/mac/webview_mac.h"

#include "webview/webview_data_stream.h"
#include "webview/webview_data_stream_memory.h"
#include "base/algorithm.h"
#include "base/debug_log.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "base/flat_map.h"

#include <crl/crl_on_main.h>
#include <crl/crl_time.h>
#include <rpl/rpl.h>

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>

#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>

namespace {

constexpr auto kDataUrlScheme = std::string_view("desktop-app-resource");
constexpr auto kFullDomain = std::string_view("desktop-app-resource://domain/");
constexpr auto kPartsCacheLimit = 32 * 1024 * 1024;
constexpr auto kUuidSize = 16;

using TaskPointer = id<WKURLSchemeTask>;

[[nodiscard]] NSString *stdToNS(std::string_view value) {
	return [[NSString alloc]
		initWithBytes:value.data()
		length:value.length()
		encoding:NSUTF8StringEncoding];
}

[[nodiscard]] std::unique_ptr<char[]> WrapBytes(const char *data, int64 length) {
	Expects(length > 0);

	auto result = std::unique_ptr<char[]>(new char[length]);
	memcpy(result.get(), data, length);
	return result;
}

} // namespace

@interface Handler : NSObject<WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate, WKURLSchemeHandler> {
}

- (id) initWithMessageHandler:(std::function<void(std::string)>)messageHandler navigationStartHandler:(std::function<bool(std::string,bool)>)navigationStartHandler navigationDoneHandler:(std::function<void(bool)>)navigationDoneHandler dialogHandler:(std::function<Webview::DialogResult(Webview::DialogArgs)>)dialogHandler dataRequested:(std::function<void(id<WKURLSchemeTask>,bool)>)dataRequested updateStates:(std::function<void()>)updateStates dataDomain:(std::string)dataDomain;
- (void) userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message;
- (void) webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler;
- (void) observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary<NSKeyValueChangeKey,id> *)change context:(void *)context;
- (void) webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation;
- (void) webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error;
- (nullable WKWebView *)webView:(WKWebView *)webView createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration forNavigationAction:(WKNavigationAction *)navigationAction windowFeatures:(WKWindowFeatures *)windowFeatures;
- (void) webView:(WKWebView *)webView runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSArray<NSURL *> * _Nullable URLs))completionHandler;
- (void) webView:(WKWebView *)webView runJavaScriptAlertPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))completionHandler;
- (void) webView:(WKWebView *)webView runJavaScriptConfirmPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL result))completionHandler;
- (void) webView:(WKWebView *)webView runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt defaultText:(NSString *)defaultText initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSString *result))completionHandler;
- (void) webView:(WKWebView *)webView startURLSchemeTask:(id <WKURLSchemeTask>)task;
- (void) webView:(WKWebView *)webView stopURLSchemeTask:(id <WKURLSchemeTask>)task;
- (void) dealloc;

@end // @interface Handler

@implementation Handler {
	std::function<void(std::string)> _messageHandler;
	std::function<bool(std::string,bool)> _navigationStartHandler;
	std::function<void(bool)> _navigationDoneHandler;
	std::function<Webview::DialogResult(Webview::DialogArgs)> _dialogHandler;
	std::function<void(id<WKURLSchemeTask> task, bool started)> _dataRequested;
	std::function<void()> _updateStates;
	std::string _dataDomain;
	base::flat_map<TaskPointer, NSURLSessionDataTask*> _redirectedTasks;
	base::has_weak_ptr _guard;
}

- (id) initWithMessageHandler:(std::function<void(std::string)>)messageHandler navigationStartHandler:(std::function<bool(std::string,bool)>)navigationStartHandler navigationDoneHandler:(std::function<void(bool)>)navigationDoneHandler dialogHandler:(std::function<Webview::DialogResult(Webview::DialogArgs)>)dialogHandler dataRequested:(std::function<void(id<WKURLSchemeTask>,bool)>)dataRequested updateStates:(std::function<void()>)updateStates dataDomain:(std::string)dataDomain {
	if (self = [super init]) {
		_messageHandler = std::move(messageHandler);
		_navigationStartHandler = std::move(navigationStartHandler);
		_navigationDoneHandler = std::move(navigationDoneHandler);
		_dialogHandler = std::move(dialogHandler);
		_dataRequested = std::move(dataRequested);
		_updateStates = std::move(updateStates);
		_dataDomain = std::move(dataDomain);
	}
	return self;
}

- (void) userContentController:(WKUserContentController *)userContentController
	   didReceiveScriptMessage:(WKScriptMessage *)message {
	id body = [message body];
	if ([body isKindOfClass:[NSString class]]) {
		NSString *string = (NSString*)body;
		_messageHandler([string UTF8String]);
	}
}

- (void) webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
	NSString *string = [[[navigationAction request] URL] absoluteString];
	WKFrameInfo *target = [navigationAction targetFrame];
	const auto newWindow = !target;
	const auto url = [string UTF8String];
	if (newWindow) {
		if (_navigationStartHandler && _navigationStartHandler(url, true)) {
			QDesktopServices::openUrl(QString::fromUtf8(url));
		}
		decisionHandler(WKNavigationActionPolicyCancel);
	} else {
		if ([target isMainFrame]
			&& !std::string(url).starts_with(_dataDomain)
			&& _navigationStartHandler
			&& !_navigationStartHandler(url, false)) {
			decisionHandler(WKNavigationActionPolicyCancel);
		} else {
			decisionHandler(WKNavigationActionPolicyAllow);
		}
	}
}

- (void) observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary<NSKeyValueChangeKey,id> *)change context:(void *)context {
	if ([keyPath isEqualToString:@"URL"] || [keyPath isEqualToString:@"title"]) {
		if (_updateStates) {
			_updateStates();
		}
	}
}

- (void) webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
	if (_navigationDoneHandler) {
		_navigationDoneHandler(true);
	}
	if (_updateStates) {
		_updateStates();
	}
}

- (void) webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
	if (_navigationDoneHandler) {
		_navigationDoneHandler(false);
	}
	if (_updateStates) {
		_updateStates();
	}
}

- (nullable WKWebView *) webView:(WKWebView *)webView createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration forNavigationAction:(WKNavigationAction *)navigationAction windowFeatures:(WKWindowFeatures *)windowFeatures {
	NSString *string = [[[navigationAction request] URL] absoluteString];
	const auto url = [string UTF8String];
	if (_navigationStartHandler && _navigationStartHandler(url, true)) {
		QDesktopServices::openUrl(QString::fromUtf8(url));
	}
	return nil;
}

- (void) webView:(WKWebView *)webView runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSArray<NSURL *> * _Nullable URLs))completionHandler {

	NSOpenPanel *openPanel = [NSOpenPanel openPanel];

	if (@available(macOS 10.13.4, *)) {
		[openPanel setCanChooseDirectories:parameters.allowsDirectories];
	}
	[openPanel setCanChooseFiles:YES];
	[openPanel setAllowsMultipleSelection:parameters.allowsMultipleSelection];
	[openPanel setResolvesAliases:YES];

	[openPanel beginWithCompletionHandler:^(NSInteger result){
		if (result == NSModalResponseOK) {
			completionHandler([openPanel URLs]);
		} else {
			completionHandler(nil);
		}
	}];

}

- (void) webView:(WKWebView *)webView runJavaScriptAlertPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))completionHandler {
	auto text = [message UTF8String];
	auto uri = [[[frame request] URL] absoluteString];
	auto url = [uri UTF8String];
	const auto result = _dialogHandler(Webview::DialogArgs{
		.type = Webview::DialogType::Alert,
		.text = text,
		.url = url,
	});
	completionHandler();
}

- (void) webView:(WKWebView *)webView runJavaScriptConfirmPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL result))completionHandler {
	auto text = [message UTF8String];
	auto uri = [[[frame request] URL] absoluteString];
	auto url = [uri UTF8String];
	const auto result = _dialogHandler(Webview::DialogArgs{
		.type = Webview::DialogType::Confirm,
		.text = text,
		.url = url,
	});
	completionHandler(result.accepted ? YES : NO);
}

- (void) webView:(WKWebView *)webView runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt defaultText:(NSString *)defaultText initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSString *result))completionHandler {
	auto text = [prompt UTF8String];
	auto value = [defaultText UTF8String];
	auto uri = [[[frame request] URL] absoluteString];
	auto url = [uri UTF8String];
	const auto result = _dialogHandler(Webview::DialogArgs{
		.type = Webview::DialogType::Prompt,
		.value = value,
		.text = text,
		.url = url,
	});
	if (result.accepted) {
		completionHandler([NSString stringWithUTF8String:result.text.c_str()]);
	} else {
		completionHandler(nil);
	}
}

- (void) webView:(WKWebView *)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
	if (![self processRedirect:task]) {
		_dataRequested(task, true);
	}
}

- (BOOL) processRedirect:(id<WKURLSchemeTask>)task {
	NSString *url = task.request.URL.absoluteString;
	NSString *prefix = stdToNS(_dataDomain);
	NSString *resource = [url substringFromIndex:[prefix length]];
	const auto id = std::string([resource UTF8String]);
	const auto dot = id.find_first_of('.');
	const auto slash = id.find_first_of('/');
	if (dot == std::string::npos
		|| slash == std::string::npos
		|| dot > slash) {
		return NO;
	}
	NSMutableURLRequest *redirected = [task.request mutableCopy];
	redirected.URL = [NSURL URLWithString:[@"https://" stringByAppendingString:resource]];
	[redirected
		setValue:@"http://desktop-app-resource/page.html"
		forHTTPHeaderField:@"Referer"];

	const auto weak = base::make_weak(&_guard);

	NSURLSessionDataTask *dataTask = [[NSURLSession sharedSession]
		dataTaskWithRequest:redirected
		completionHandler:^(
				NSData * _Nullable data,
				NSURLResponse * _Nullable response,
				NSError * _Nullable error) {
			if (response) [response retain];
			if (error) [error retain];
			if (data) [data retain];
			crl::on_main([=] {
				if (weak) {
					const auto i = _redirectedTasks.find(task);
					if (i == end(_redirectedTasks)) {
						return;
					}
					NSURLSessionDataTask *dataTask = i->second;
					_redirectedTasks.erase(i);

					if (error) {
						[task didFailWithError:error];
					} else {
						[task didReceiveResponse:response];
						[task didReceiveData:data];
						[task didFinish];
					}
					[task release];
					[dataTask release];
				}
				if (response) [response release];
				if (error) [error release];
				if (data) [data release];
			});
		}];

	[task retain];
	[dataTask retain];
	_redirectedTasks.emplace(task, dataTask);

	[dataTask resume];
	return YES;
}

- (void) webView:(WKWebView *)webView stopURLSchemeTask:(id <WKURLSchemeTask>)task {
	const auto i = _redirectedTasks.find(task);
	if (i != end(_redirectedTasks)) {
		NSURLSessionDataTask *dataTask = i->second;
		_redirectedTasks.erase(i);

		[task release];
		[dataTask cancel];
		[dataTask release];
	} else {
		_dataRequested(task, false);
	}
}

- (void) dealloc {
	for (const auto &[task, dataTask] : base::take(_redirectedTasks)) {
		NSError *error = [NSError
			errorWithDomain:@"org.telegram.desktop"
			code:404
			userInfo:nil];
		[task didFailWithError:error];
		[task release];
		[dataTask cancel];
		[dataTask release];
	}
	[super dealloc];
}

@end // @implementation Handler

namespace Webview {
namespace {

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
	struct Task {
		int index = 0;
		crl::time started = 0;
	};
	struct PartialResource {
		uint32 index = 0;
		uint32 total = 0;
		std::string mime;
	};
	struct PartData {
		std::unique_ptr<char[]> bytes;
		int64 length = 0;
	};
	struct CachedResult {
		std::string mime;
		NSData *data = nil;
		int64 requestFrom = 0;
		int64 requestLength = 0;
		int64 total = 0;

		explicit operator bool() const {
			return data != nil;
		}
	};
	using CacheKey = uint64;

	static void TaskFail(TaskPointer task);
	void taskFail(TaskPointer task, int indexToCheck);
	void taskDone(
		TaskPointer task,
		int indexToCheck,
		const std::string &mime,
		NSData *data,
		int64 offset,
		int64 total);

	void processDataRequest(TaskPointer task, bool started);

	[[nodiscard]] CachedResult fillFromCache(const DataRequest &request);
	void addToCache(uint32 resourceIndex, int64 offset, PartData data);
	void removeCacheEntry(CacheKey key);
	void pruneCache();

	void updateHistoryStates();

	[[nodiscard]] static CacheKey KeyFromValues(
		uint32 resourceIndex,
		int64 offset);
	[[nodiscard]] static uint32 ResourceIndexFromKey(CacheKey key);
	[[nodiscard]] static int64 OffsetFromKey(CacheKey key);

	WKUserContentController *_manager = nullptr;
	WKWebView *_webview = nullptr;
	Handler *_handler = nullptr;
	base::unique_qptr<QWindow> _window;
	base::unique_qptr<QWidget> _widget;
	std::string _dataProtocol;
	std::string _dataDomain;
	std::function<DataResult(DataRequest)> _dataRequestHandler;
	rpl::variable<NavigationHistoryState> _navigationHistoryState;

	base::flat_map<TaskPointer, Task> _tasks;
	base::flat_map<std::string, PartialResource> _partialResources;
	base::flat_map<CacheKey, PartData> _partsCache;
	std::vector<CacheKey> _partsLRU;
	int64 _cacheTotal = 0;
	int _taskAutoincrement = 0;

};

[[nodiscard]] NSUUID *UuidFromToken(const std::string &token) {
	const auto bytes = reinterpret_cast<const unsigned char*>(token.data());
	return (token.size() == kUuidSize)
		? [[NSUUID alloc] initWithUUIDBytes:bytes]
		: nil;
}

[[nodiscard]] std::string UuidToToken(NSUUID *uuid) {
	if (!uuid) {
		return std::string();
	}
	auto result = std::string(kUuidSize, ' ');
	const auto bytes = reinterpret_cast<unsigned char*>(result.data());
	[uuid getUUIDBytes:bytes];
	return result;
}

Instance::Instance(Config config) {
	const auto weak = base::make_weak(this);
	const auto handleDataRequest = [=](id<WKURLSchemeTask> task, bool started) {
		if (weak) {
			processDataRequest(task, started);
		} else if (started) {
			TaskFail(task);
		}
	};

	WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
	_manager = configuration.userContentController;
	_dataProtocol = kDataUrlScheme;
	_dataDomain = kFullDomain;
	if (!config.dataProtocolOverride.empty()) {
		_dataProtocol = config.dataProtocolOverride;
		_dataDomain = _dataProtocol + "://domain/";
	}
	if (config.debug) {
		[configuration.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
	}
	if (config.safe) {
		[configuration.preferences setValue:@NO forKey:@"fraudulentWebsiteWarningEnabled"];
	}
	const auto updateStates = [=] {
		updateHistoryStates();
	};
	_handler = [[Handler alloc] initWithMessageHandler:config.messageHandler navigationStartHandler:config.navigationStartHandler navigationDoneHandler:config.navigationDoneHandler dialogHandler:config.dialogHandler dataRequested:handleDataRequest updateStates:updateStates dataDomain:_dataDomain];
	_dataRequestHandler = std::move(config.dataRequestHandler);
	[configuration setURLSchemeHandler:_handler forURLScheme:stdToNS(_dataProtocol)];
	if (@available(macOS 14, *)) {
		if (config.userDataToken != LegacyStorageIdToken().toStdString()) {
			NSUUID *uuid = UuidFromToken(config.userDataToken);
			[configuration setWebsiteDataStore:[WKWebsiteDataStore dataStoreForIdentifier:uuid]];
			[uuid release];
		}
	}
	_webview = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:configuration];
	if (@available(macOS 13.3, *)) {
		_webview.inspectable = config.debug ? YES : NO;
	}
	[_manager addScriptMessageHandler:_handler name:@"external"];
	[_webview setNavigationDelegate:_handler];
	[_webview setUIDelegate:_handler];

	[_webview addObserver:_handler forKeyPath:@"URL" options:NSKeyValueObservingOptionNew context:nil];
	[_webview addObserver:_handler forKeyPath:@"title" options:NSKeyValueObservingOptionNew context:nil];

	[configuration release];

	_window.reset(QWindow::fromWinId(WId(_webview)));
	_widget.reset();

	_widget.reset(
		QWidget::createWindowContainer(
			_window.get(),
			config.parent,
			Qt::FramelessWindowHint));
	_widget->show();

	setOpaqueBg(config.opaqueBg);
	init(R"(
window.external = {
	invoke: function(s) {
		window.webkit.messageHandlers.external.postMessage(s);
	}
};)");
}

Instance::~Instance() {
	base::take(_window);
	base::take(_widget);
	[_manager removeScriptMessageHandlerForName:@"external"];
	[_webview setNavigationDelegate:nil];
	[_handler release];
	[_webview release];
}

void Instance::TaskFail(TaskPointer task) {
	[task didFailWithError:[NSError errorWithDomain:@"org.telegram.desktop" code:404 userInfo:nil]];
}

void Instance::taskFail(TaskPointer task, int indexToCheck) {
	if (indexToCheck) {
		const auto i = _tasks.find(task);
		if (i == end(_tasks) || i->second.index != indexToCheck) {
			return;
		}
		_tasks.erase(i);
	}
	TaskFail(task);
}

void Instance::taskDone(
		TaskPointer task,
		int indexToCheck,
		const std::string &mime,
		NSData *data,
		int64 offset,
		int64 total) {
	Expects(data != nil);

	if (indexToCheck) {
		const auto i = _tasks.find(task);
		if (i == end(_tasks) || i->second.index != indexToCheck) {
			return;
		}
		_tasks.erase(i);
	}

	const auto length = int64([data length]);
	const auto partial = (offset > 0) || (total != length);
	NSMutableDictionary *headers = [@{
		@"Content-Type": stdToNS(mime),
		@"Accept-Ranges": @"bytes",
		@"Cache-Control": @"no-store",
		@"Content-Length": stdToNS(std::to_string(length)),
	} mutableCopy];
	if (partial) {
		headers[@"Content-Range"] = stdToNS("bytes "
			+ std::to_string(offset)
			+ '-'
			+ std::to_string(offset + length - 1)
			+ '/'
			+ std::to_string(total));
	}
	NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc]
		initWithURL:task.request.URL
		statusCode:(partial ? 206 : 200)
		HTTPVersion:@"HTTP/1.1"
		headerFields:headers];

	[task didReceiveResponse:response];
	[task didReceiveData:data];
	[task didFinish];
}

Instance::CacheKey Instance::KeyFromValues(uint32 resourceIndex, int64 offset) {
	return (uint64(resourceIndex) << 32) | uint32(offset);
}

uint32 Instance::ResourceIndexFromKey(CacheKey key) {
	return uint32(key >> 32);
}

int64 Instance::OffsetFromKey(CacheKey key) {
	return int64(key & 0xFFFFFFFFULL);
}

void Instance::addToCache(uint32 resourceIndex, int64 offset, PartData data) {
	auto key = KeyFromValues(resourceIndex, offset);
	while (true) { // Remove parts that are already in cache.
		auto i = _partsCache.upper_bound(key);
		if (i != begin(_partsCache)) {
			--i;
			const auto alreadyIndex = ResourceIndexFromKey(i->first);
			if (alreadyIndex == resourceIndex) {
				const auto &already = i->second;
				const auto alreadyOffset = OffsetFromKey(i->first);
				const auto alreadyTill = alreadyOffset + already.length;
				if (alreadyTill >= offset + data.length) {
					return; // Fully in cache.
				} else if (alreadyTill > offset) {
					const auto delta = alreadyTill - offset;
					offset += delta;
					data.length -= delta;
					data.bytes = WrapBytes(data.bytes.get() + delta, data.length);
					key = KeyFromValues(resourceIndex, offset);
					continue;
				}
			}
			++i;
		}
		if (i != end(_partsCache)) {
			const auto alreadyIndex = ResourceIndexFromKey(i->first);
			if (alreadyIndex == resourceIndex) {
				const auto &already = i->second;
				const auto alreadyOffset = OffsetFromKey(i->first);
				Assert(alreadyOffset > offset);
				const auto alreadyTill = alreadyOffset + already.length;
				if (alreadyTill <= offset + data.length) {
					removeCacheEntry(i->first);
					continue;
				} else if (alreadyOffset < offset + data.length) {
					const auto delta = offset + data.length - alreadyOffset;
					data.length -= delta;
					data.bytes = WrapBytes(data.bytes.get(), data.length);
					continue;
				}
			}
		}
		break;
	}
	_partsLRU.push_back(key);
	_cacheTotal += data.length;
	_partsCache[key] = std::move(data);
	pruneCache();
}

void Instance::pruneCache() {
	while (_cacheTotal > kPartsCacheLimit) {
		Assert(!_partsLRU.empty());
		removeCacheEntry(_partsLRU.front());
	}
}

void Instance::updateHistoryStates() {
	NSURL *maybeUrl = [_webview URL];
	NSString *maybeTitle = [_webview title];
	const auto url = maybeUrl
		? std::string([[maybeUrl absoluteString] UTF8String])
		: std::string();
	const auto title = maybeTitle
		? std::string([maybeTitle UTF8String])
		: std::string();
	_navigationHistoryState = NavigationHistoryState{
		.url = url,
		.title = title,
		.canGoBack = ([_webview canGoBack] == YES),
		.canGoForward = ([_webview canGoForward] == YES),
	};
}

void Instance::removeCacheEntry(CacheKey key) {
	auto &part = _partsCache[key];
	Assert(part.length > 0);
	Assert(_cacheTotal >= part.length);
	_cacheTotal -= part.length;
	_partsCache.remove(key);
	_partsLRU.erase(
		std::remove(begin(_partsLRU), end(_partsLRU), key),
		end(_partsLRU));
}

Instance::CachedResult Instance::fillFromCache(
		const DataRequest &request) {
	auto &partial = _partialResources[request.id];
	const auto index = partial.index;
	if (!index) {
		partial.index = uint32(_partialResources.size());
		return {};
	}
	auto i = _partsCache.upper_bound(
		KeyFromValues(partial.index, request.offset));
	if (i == begin(_partsCache)) {
		return {};
	}
	--i;
	if (ResourceIndexFromKey(i->first) != index) {
		return {};
	}
	const auto alreadyOffset = OffsetFromKey(i->first);
	const auto alreadyTill = alreadyOffset + i->second.length;
	if (alreadyTill <= request.offset) {
		return {};
	}
	auto till = alreadyTill;
	for (auto j = i + 1; j != end(_partsCache); ++j) {
		const auto offset = OffsetFromKey(j->first);
		if (ResourceIndexFromKey(j->first) != index || offset > till) {
			break;
		}
		till = offset + j->second.length;
		if (request.limit <= 0 || till >= request.offset + request.limit) {
			break;
		}
	}
	const auto length = (request.limit > 0) ? request.limit : (till - request.offset);
	if (till < request.offset + length) {
		return {};
	}
	auto result = [NSMutableData dataWithLength:length];
	auto from = request.offset;
	auto fill = length;
	auto bytes = static_cast<char*>([result mutableBytes]);
	for (auto j = i; j != end(_partsCache); ++j) {
		const auto offset = OffsetFromKey(j->first);
		const auto copy = std::min(fill, offset + j->second.length - from);
		Assert(copy > 0);
		Assert(from >= offset);
		memcpy(bytes, j->second.bytes.get() + (from - offset), copy);
		from += copy;
		fill -= copy;
		bytes += copy;

		const auto lru = std::find(begin(_partsLRU), end(_partsLRU), j->first);
		Assert(lru != end(_partsLRU));
		if (const auto next = lru + 1; next != end(_partsLRU)) {
			std::rotate(lru, next, end(_partsLRU));
		}

		if (!fill) {
			break;
		}
		Assert(fill > 0);
	}
	Assert(fill == 0);
	return { .mime = partial.mime, .data = result, .total = partial.total };
}

void Instance::processDataRequest(TaskPointer task, bool started) {
	if (!started) {
		_tasks.remove(task);
		return;
	}

	@autoreleasepool {

	NSString *url = task.request.URL.absoluteString;
	NSString *prefix = stdToNS(_dataDomain);
	if (![url hasPrefix:prefix]) {
		taskFail(task, 0);
		return;
	}

	const auto resourceId = std::string([[url substringFromIndex:[prefix length]] UTF8String]);
	auto prepared = DataRequest{
		.id = resourceId,
	};
	NSString *rangeHeader = [task.request valueForHTTPHeaderField:@"Range"];
	if (rangeHeader) {
		ParseRangeHeaderFor(prepared, std::string([rangeHeader UTF8String]));

		if (const auto cached = fillFromCache(prepared)) {
			taskDone(task, 0, cached.mime, cached.data, prepared.offset, cached.total);
			return;
		}
	}

	const auto index = ++_taskAutoincrement;
	_tasks[task] = Task{ .index = index, .started = crl::now() };

	const auto requestedOffset = prepared.offset;
	const auto requestedLimit = prepared.limit;
	prepared.done = crl::guard(this, [=](DataResponse resolved) {
		auto &stream = resolved.stream;
		if (!stream) {
			return taskFail(task, index);
		}
		const auto length = stream->size();
		Assert(length > 0);

		const auto offset = resolved.streamOffset;
		if (requestedOffset >= offset + length || offset > requestedOffset) {
			return taskFail(task, index);
		}

		auto bytes = std::unique_ptr<char[]>(new char[length]);
		const auto read = stream->read(bytes.get(), length);
		Assert(read == length);

		const auto useLength = (requestedLimit > 0)
			? std::min(requestedLimit, (offset + length - requestedOffset))
			: (offset + length - requestedOffset);

		const auto useBytes = bytes.get() + (requestedOffset - offset);
		const auto data = [NSData dataWithBytes:useBytes length:useLength];

		const auto mime = stream->mime();
		const auto total = resolved.totalSize ? resolved.totalSize : length;
		const auto i = _partialResources.find(resourceId);
		if (i != end(_partialResources)) {
			auto &partial = i->second;
			if (partial.mime.empty()) {
				partial.mime = mime;
			}
			if (!partial.total) {
				partial.total = total;
			}
			addToCache(partial.index, offset, { std::move(bytes), length });
		}
		taskDone(task, index, mime, data, requestedOffset, total);
	});
	const auto result = _dataRequestHandler
		? _dataRequestHandler(prepared)
		: DataResult::Failed;
	if (result == DataResult::Failed) {
		return taskFail(task, index);
	}

	}
}

void Instance::navigate(std::string url) {
	NSString *string = [NSString stringWithUTF8String:url.c_str()];
	NSURL *native = [NSURL URLWithString:string];
	[_webview loadRequest:[NSURLRequest requestWithURL:native]];
}

void Instance::navigateToData(std::string id) {
	auto full = std::string();
	full.reserve(_dataDomain.size() + id.size());
	full.append(_dataDomain);
	full.append(id);
	navigate(full);
}

void Instance::reload() {
	[_webview reload];
}

void Instance::init(std::string js) {
	NSString *string = [NSString stringWithUTF8String:js.c_str()];
	WKUserScript *script = [[WKUserScript alloc] initWithSource:string injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES];
	[_manager addUserScript:script];
}

void Instance::eval(std::string js) {
	NSString *string = [NSString stringWithUTF8String:js.c_str()];
	[_webview evaluateJavaScript:string completionHandler:nil];
}

void Instance::focus() {
}

QWidget *Instance::widget() {
	return _widget.get();
}

void Instance::refreshNavigationHistoryState() {
	// Not needed here, there are events.
}

auto Instance::navigationHistoryState()
-> rpl::producer<NavigationHistoryState> {
	return _navigationHistoryState.value();
}

void Instance::setOpaqueBg(QColor opaqueBg) {
	if (@available(macOS 12.0, *)) {
		[_webview setValue: @NO forKey: @"drawsBackground"];
		[_webview setUnderPageBackgroundColor:[NSColor clearColor]];
	}
}

} // namespace

Available Availability() {
	return Available{
		.customSchemeRequests = true,
		.customRangeRequests = true,
		.customReferer = true,
	};
}

bool SupportsEmbedAfterCreate() {
	return true;
}

bool SeparateStorageIdSupported() {
	return true;
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	if (!Supported()) {
		return nullptr;
	}
	return std::make_unique<Instance>(std::move(config));
}

std::string GenerateStorageToken() {
	return UuidToToken([NSUUID UUID]);
}

void ClearStorageDataByToken(const std::string &token) {
	if (@available(macOS 14, *)) {
		if (!token.empty() && token != LegacyStorageIdToken().toStdString()) {
			if (NSUUID *uuid = UuidFromToken(token)) {
				// removeDataStoreForIdentifier crashes without that (if not created first).
				WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
				[configuration setWebsiteDataStore:[WKWebsiteDataStore dataStoreForIdentifier:uuid]];
				[configuration release];

				[WKWebsiteDataStore
					removeDataStoreForIdentifier:uuid
					completionHandler:^(NSError *error) {}];
				[uuid release];
			}
		}
	}
}

} // namespace Webview
