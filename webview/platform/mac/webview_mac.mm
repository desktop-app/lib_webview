// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/mac/webview_mac.h"

#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>


@interface Handler : NSObject<WKScriptMessageHandler, WKNavigationDelegate> {
}

- (id) initWithMessageCallback:(std::function<void(std::string)>)messageCallback navigationStartCallback:(std::function<bool(std::string)>)navigationStartCallback navigationDoneCallback:(std::function<void(bool)>)navigationDoneCallback;
- (void) userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message;
- (void) webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler;
- (void) webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation;
- (void) webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error;
- (void) dealloc;

@end // @interface ChooseApplicationDelegate

@implementation Handler {
	std::function<void(std::string)> _messageCallback;
	std::function<bool(std::string)> _navigationStartCallback;
	std::function<void(bool)> _navigationDoneCallback;
}

- (id) initWithMessageCallback:(std::function<void(std::string)>)messageCallback navigationStartCallback:(std::function<bool(std::string)>)navigationStartCallback navigationDoneCallback:(std::function<void(bool)>)navigationDoneCallback {
	if (self = [super init]) {
		_messageCallback = std::move(messageCallback);
		_navigationStartCallback = std::move(navigationStartCallback);
		_navigationDoneCallback = std::move(navigationDoneCallback);
	}
	return self;
}

- (void) userContentController:(WKUserContentController *)userContentController
	   didReceiveScriptMessage:(WKScriptMessage *)message {
	id body = [message body];
	if ([body isKindOfClass:[NSString class]]) {
		NSString *string = (NSString*)body;
		_messageCallback([string UTF8String]);
	}
}

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
	NSString *string = [[[navigationAction request] URL] absoluteString];
	if (_navigationStartCallback && !_navigationStartCallback([string UTF8String])) {
		decisionHandler(WKNavigationActionPolicyCancel);
	} else {
		decisionHandler(WKNavigationActionPolicyAllow);
	}
}

- (void) webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
	if (_navigationDoneCallback) {
		_navigationDoneCallback(true);
	}
}

- (void) webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
	if (_navigationDoneCallback) {
		_navigationDoneCallback(false);
	}
}
- (void) dealloc {
	[super dealloc];
}

@end // @implementation Handler

namespace Webview {
namespace {

class Instance final : public Interface {
public:
	explicit Instance(Config config);
	~Instance();

	bool finishEmbedding() override;

	void navigate(std::string url) override;

	void resizeToWindow() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void *winId() override;

private:
	WKUserContentController *_manager = nullptr;
	WKWebView *_webview = nullptr;
	Handler *_handler = nullptr;

};

Instance::Instance(Config config) {
	WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
	_manager = configuration.userContentController;
	_webview = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:configuration];
	_handler = [[Handler alloc] initWithMessageCallback:config.messageHandler navigationStartCallback:config.navigationStartHandler navigationDoneCallback:config.navigationDoneHandler];
	[_manager addScriptMessageHandler:_handler name:@"external"];
	[_webview setNavigationDelegate:_handler];
	[configuration release];

	init(R"(
window.external = {
	invoke: function(s) {
		window.webkit.messageHandlers.external.postMessage(s);
	}
};)");
}

Instance::~Instance() {
	[_manager removeScriptMessageHandlerForName:@"external"];
	[_webview setNavigationDelegate:nil];
	[_handler release];
	[_webview release];
}

bool Instance::finishEmbedding() {
	return true;
}

void Instance::navigate(std::string url) {
	NSString *string = [NSString stringWithUTF8String:url.c_str()];
	NSURL *native = [NSURL URLWithString:string];
	[_webview loadRequest:[NSURLRequest requestWithURL:native]];
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

void *Instance::winId() {
	return _webview;
}

void Instance::resizeToWindow() {
}

} // namespace

Available Availability() {
	return Available{};
}

bool SupportsEmbedAfterCreate() {
	return true;
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	if (!Supported()) {
		return nullptr;
	}
	return std::make_unique<Instance>(std::move(config));
}

} // namespace Webview
