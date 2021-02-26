// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <memory>
#include <string>
#include <functional>

namespace Webview::details {

enum class Hint {
	None, // Width and height are default size
	Min, // Width and height are minimum bounds
	Max, // Width and height are maximum bounds
	Fixed, // Window size can not be changed by a user
};

class Wrap final {
public:
	Wrap(bool debug = false, void *wnd = nullptr);
	Wrap(Wrap &&other);
	Wrap &operator=(Wrap &&other);
	~Wrap();

	void navigate(std::string url);

	using binding_t = std::function<void(std::string, std::string, void*)>;
	using sync_binding_t = std::function<std::string(std::string)>;

	void bind(std::string name, sync_binding_t fn);
	void bind(std::string name, binding_t f, void *arg);

	void resolve(std::string seq, int status, std::string result);

	void setTitle(std::string title);
	void setWindowSize(int width, int height, Hint hint);
	void resizeToWindow();

	void init(std::string js);
	void eval(std::string js);

private:
	struct Wrapped;
	std::unique_ptr<Wrapped> _wrapped;

};

} // namespace Webview::details
