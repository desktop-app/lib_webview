// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_http_server.h"

#include <QtCore/QPointer>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <crl/crl.h>

namespace Webview {

struct HttpServer::Private {
	void handleRequest(QTcpSocket *socket);

	bool processRedirect(
		QTcpSocket *socket,
		const QByteArray &id,
		const ::base::flat_map<QByteArray, QByteArray> &headers,
		const std::shared_ptr<Guard> &guard);

	QNetworkAccessManager manager;
	QByteArray password;
	std::function<void(
		QTcpSocket *socket,
		const QByteArray &id,
		const ::base::flat_map<QByteArray, QByteArray> &headers,
		const std::shared_ptr<Guard> &guard)> handler;
};

void HttpServer::Private::handleRequest(QTcpSocket *socket) {
	const auto guard = std::make_shared<Guard>(crl::guard(socket, [=] {
		QMetaObject::invokeMethod(socket, [=] {
			socket->disconnectFromHost();
		});
	}));

	const auto firstLine = socket->readLine().simplified().split(' ');
	if (firstLine.size() < 2 || firstLine[0] != "GET") {
		return;
	}

	const auto headers = [&] {
		auto result = ::base::flat_map<QByteArray, QByteArray>();
		while (true) {
			const auto line = socket->readLine();
			const auto separator = line.indexOf(':');
			if (separator <= 0) {
				break;
			}

			const auto name = line.mid(0, separator).simplified();
			const auto value = line.mid(separator + 1).simplified();
			result.emplace(name, value);
		}
		return result;
	}();

	const auto getHeader = [&](const QByteArray &key) {
		const auto it = headers.find(key);
		return it != headers.end()
			? it->second
			: QByteArray();
	};

	const auto authed = [&] {
		const auto auth = getHeader("Authorization");
		if (auth.startsWith("Basic ")) {
			const auto userPass = QByteArray::fromBase64(auth.mid(6));
			if (userPass == ':' + password) {
				return true;
			}
		}
		return false;
	}();

	if (!authed) {
		socket->write("HTTP/1.1 401 Unauthorized\r\n");
		socket->write("WWW-Authenticate: Basic realm=\"\"\r\n");
		socket->write("\r\n");
		return;
	}

	const auto id = firstLine[1].mid(1);
	if (processRedirect(socket, id, headers, guard) || !handler) {
		return;
	}

	handler(socket, id, headers, guard);
}

bool HttpServer::Private::processRedirect(
		QTcpSocket *socket,
		const QByteArray &id,
		const ::base::flat_map<QByteArray, QByteArray> &headers,
		const std::shared_ptr<Guard> &guard) {
	const auto dot = id.indexOf('.');
	const auto slash = id.indexOf('/');
	if (dot < 0 || slash < 0 || dot > slash) {
		return false;
	}

	auto request = QNetworkRequest();
	request.setUrl(QString::fromUtf8("https://" + id));

	if (!headers.empty()) {
		const auto headersToCopy = {
			"Accept",
			"User-Agent",
			"Accept-Language",
			"Accept-Encoding",
		};
		for (const auto name : headersToCopy) {
			const auto it = headers.find(name);
			if (it == headers.end()) {
				continue;
			}
			request.setRawHeader(name, it->second.constData());
		}
	}

	// Always set our own Referer
	request.setRawHeader("Referer", "http://desktop-app-resource/page.html");

	const auto reply = manager.get(request);
	connect(socket, &QObject::destroyed, reply, &QObject::deleteLater);
	connect(reply, &QNetworkReply::finished, socket, [=] {
		(void) guard;
		const auto input = reply->readAll();
		socket->write("HTTP/1.1 200 OK\r\n");
		const auto headersToCopy = {
			"Content-Type",
			"Content-Encoding",
			"Content-Length",
		};
		for (const auto name : headersToCopy) {
			if (!reply->hasRawHeader(name)) {
				continue;
			}
			socket->write(
				std::format(
					"{}: {}\r\n",
					name,
					reply->rawHeader(name).toStdString()
				).c_str()
			);
		}
		socket->write("Cache-Control: no-store\r\n");
		socket->write("\r\n");
		socket->write(input);
	}, Qt::SingleShotConnection);

	return true;
}

HttpServer::HttpServer(
		const QByteArray &password,
		const std::function<void(
			QTcpSocket *socket,
			const QByteArray &id,
			const ::base::flat_map<QByteArray, QByteArray> &headers,
			const std::shared_ptr<Guard> &guard)> &handler)
: _private(std::make_unique<Private>()) {
	_private->password = password;
	_private->handler = handler;

	connect(this, &QTcpServer::newConnection, [=] {
		while (const auto socket = nextPendingConnection()) {
			connect(
				socket,
				&QAbstractSocket::disconnected,
				socket,
				&QObject::deleteLater);

			connect(socket, &QIODevice::readyRead, this, [=] {
				_private->handleRequest(socket);
			}, Qt::SingleShotConnection);
		}
	});
}

HttpServer::~HttpServer() = default;

} // namespace Webview
