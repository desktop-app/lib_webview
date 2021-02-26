// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/details/webview_wrap.h"

#pragma warning(push)
// class has virtual functions, but destructor is not virtual
#pragma warning(disable:4265)
#include "webview.h"
#pragma warning(pop)

namespace Webview::details {

struct Wrap::Wrapped {
	Wrapped(bool debug, void *wnd) : view(debug, wnd) {
	}

	webview::webview view;
};

Wrap::Wrap(bool debug, void *wnd)
: _wrapped(std::make_unique<Wrapped>(debug, wnd)) {
}

Wrap::Wrap(Wrap &&other) = default;

Wrap &Wrap::operator=(Wrap &&other) = default;

Wrap::~Wrap() = default;

void Wrap::navigate(std::string url) {
	_wrapped->view.navigate(std::move(url));
}

void Wrap::bind(std::string name, sync_binding_t fn) {
	_wrapped->view.bind(std::move(name), std::move(fn));
}

void Wrap::bind(std::string name, binding_t f, void *arg) {
	_wrapped->view.bind(std::move(name), std::move(f), arg);
}

void Wrap::resolve(std::string seq, int status, std::string result) {
	_wrapped->view.resolve(std::move(seq), status, std::move(result));
}

void Wrap::setTitle(std::string title) {
	_wrapped->view.set_title(std::move(title));
}

void Wrap::setWindowSize(int width, int height, Hint hint) {
	_wrapped->view.set_size(width, height, int(hint));
}

void Wrap::resizeToWindow() {
	_wrapped->view.resize_to_window();
}

void Wrap::init(std::string js) {
	_wrapped->view.init(std::move(js));
}

void Wrap::eval(std::string js) {
	_wrapped->view.eval(std::move(js));
}

} // namespace Webview::details
