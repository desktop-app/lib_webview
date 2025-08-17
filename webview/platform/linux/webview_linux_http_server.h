// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/flat_map.h"

#include <QtNetwork/QTcpServer>

#include <gsl/gsl>

namespace Webview {

class HttpServer : public QTcpServer {
public:
	using Guard = gsl::final_action<std::function<void()>>;

	HttpServer(
		const QByteArray &password,
		const std::function<void(
			QTcpSocket *socket,
			const QByteArray &id,
			const ::base::flat_map<QByteArray, QByteArray> &headers,
			const std::shared_ptr<Guard> &guard)> &handler);

	~HttpServer();

private:
	struct Private;
	const std::unique_ptr<Private> _private;
};

} // namespace Webview
