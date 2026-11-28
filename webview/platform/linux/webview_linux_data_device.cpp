// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_data_device.h"

#include "base/flat_map.h"
#include "base/qt_signal_producer.h"
#include "base/weak_ptr.h"

#include <QtCore/QBuffer>
#include <QtCore/QMimeData>
#include <QtCore/QPointer>
#include <QtCore/QSocketNotifier>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtWaylandCompositor/QWaylandClient>
#include <QtWaylandCompositor/QWaylandCompositor>
#include <QtWaylandCompositor/QWaylandSeat>
#include <QtWaylandCompositor/QWaylandSurface>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-primary-selection-unstable-v1-server-protocol.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <array>

namespace Webview {
namespace {

constexpr auto kVersion = 3;
constexpr auto kPrimaryVersion = 1;
constexpr auto kReadTimeout = 1000;
constexpr auto kWriteTimeout = 5000;
constexpr auto kCopy = uint32_t(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
constexpr auto kMove = uint32_t(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
constexpr auto kAsk = uint32_t(WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK);
constexpr auto kActions = kCopy | kMove | kAsk;

[[nodiscard]] QString TextFormat() {
	return u"text/plain;charset=utf-8"_q;
}

[[nodiscard]] QString ImageFormat() {
	return u"image/png"_q;
}

[[nodiscard]] QString PlainTextFormat() {
	return u"text/plain"_q;
}

[[nodiscard]] QString QtImageFormat() {
	return u"application/x-qt-image"_q;
}

[[nodiscard]] QString ImageSourceFormat(const QStringList &formats) {
	if (formats.contains(ImageFormat())) {
		return ImageFormat();
	}
	const auto image = [](const QString &format) {
		return format.startsWith(u"image/"_q);
	};
	const auto i = std::ranges::find_if(formats, image);
	return (i != formats.end()) ? *i : QString();
}

[[nodiscard]] QStringList MirrorFormats(QStringList formats) {
	if (formats.contains(TextFormat())
			&& !formats.contains(PlainTextFormat())) {
		formats.push_back(PlainTextFormat());
	}
	if (!ImageSourceFormat(formats).isEmpty()
			&& !formats.contains(QtImageFormat())) {
		formats.push_back(QtImageFormat());
	}
	return formats;
}

[[nodiscard]] QWaylandSurfaceRole *IconRole() {
	static QWaylandSurfaceRole result("wl_data_device-icon");
	return &result;
}

template <typename Type>
[[nodiscard]] Type *Data(wl_resource *resource) {
	return resource
		? static_cast<Type*>(wl_resource_get_user_data(resource))
		: nullptr;
}

[[nodiscard]] bool SupportsActions(not_null<wl_resource*> resource) {
	return wl_resource_get_version(resource) >= 3;
}

[[nodiscard]] uint32_t WaylandActions(Qt::DropActions actions) {
	return (actions.testFlag(Qt::CopyAction) ? kCopy : 0)
		| (actions.testFlag(Qt::MoveAction) ? kMove : 0);
}

[[nodiscard]] bool HostSupports(QClipboard::Mode mode) {
	return (mode == QClipboard::Clipboard)
		|| QGuiApplication::clipboard()->supportsSelection();
}

void BindManager(
		wl_client *client,
		uint32_t version,
		uint32_t id,
		const wl_interface *interface,
		const void *implementation,
		void *data) {
	const auto resource = wl_resource_create(
		client,
		interface,
		int(version),
		id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(
		resource,
		implementation,
		data,
		nullptr);
}

void SendSourceData(
		not_null<wl_resource*> source,
		QClipboard::Mode mode,
		const QString &mimeType,
		int fd) {
	const auto utf8 = mimeType.toUtf8();
	if (mode == QClipboard::Selection) {
		zwp_primary_selection_source_v1_send_send(
			source,
			utf8.constData(),
			fd);
	} else {
		wl_data_source_send_send(source, utf8.constData(), fd);
	}
}

void SendSourceCancelled(
		not_null<wl_resource*> source,
		QClipboard::Mode mode) {
	if (mode == QClipboard::Selection) {
		zwp_primary_selection_source_v1_send_cancelled(source);
	} else {
		wl_data_source_send_cancelled(source);
	}
}

[[nodiscard]] uint32_t ChooseAction(
		uint32_t source,
		uint32_t offer,
		uint32_t preferred) {
	const auto available = source & offer;
	if (preferred & available) {
		return preferred;
	}
	return available & (~available + 1);
}

[[nodiscard]] QStringList OfferedFormats(not_null<const QMimeData*> data) {
	auto result = data->formats();
	if (result.contains(PlainTextFormat())
			&& !result.contains(TextFormat())) {
		result.push_back(TextFormat());
	}
	if (data->hasImage() && !result.contains(ImageFormat())) {
		result.push_back(ImageFormat());
	}
	return result;
}

[[nodiscard]] QByteArray FormatData(
		not_null<const QMimeData*> data,
		const QString &mimeType) {
	if (data->formats().contains(mimeType)) {
		return data->data(mimeType);
	} else if (mimeType == TextFormat()) {
		return data->text().toUtf8();
	} else if (mimeType != ImageFormat()) {
		return QByteArray();
	}
	auto result = QByteArray();
	QBuffer buffer(&result);
	qvariant_cast<QImage>(data->imageData()).save(&buffer, "PNG");
	return result;
}

[[nodiscard]] ssize_t WriteNoSignal(int fd, const char *data, size_t size) {
	sigset_t pipe;
	sigset_t old;
	sigemptyset(&pipe);
	sigaddset(&pipe, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe, &old);
	const auto result = ::write(fd, data, size);
	const auto error = errno;
	if (result < 0 && error == EPIPE) {
		const auto zero = timespec();
		sigtimedwait(&pipe, nullptr, &zero);
	}
	pthread_sigmask(SIG_SETMASK, &old, nullptr);
	errno = error;
	return result;
}

[[nodiscard]] QByteArray ReadSource(
		not_null<wl_resource*> source,
		QClipboard::Mode mode,
		const QString &mimeType) {
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		return QByteArray();
	}
	SendSourceData(source, mode, mimeType, fds[1]);
	close(fds[1]);
	wl_client_flush(wl_resource_get_client(source));

	auto result = QByteArray();
	auto buffer = std::array<char, 4096>();
	auto descriptor = pollfd{ .fd = fds[0], .events = POLLIN };
	while (true) {
		const auto ready = poll(&descriptor, 1, kReadTimeout);
		if (ready < 0 && errno == EINTR) {
			continue;
		} else if (ready <= 0) {
			break;
		}
		const auto read = ::read(
			fds[0],
			buffer.data(),
			buffer.size());
		if (read < 0 && errno == EINTR) {
			continue;
		} else if (read <= 0) {
			break;
		}
		result.append(buffer.data(), read);
	}
	close(fds[0]);
	return result;
}

class Writer final : public QObject {
public:
	Writer(not_null<QObject*> parent, int fd, QByteArray data);
	~Writer();

private:
	void write();
	void finish();

	const int _fd = -1;
	const QByteArray _data;
	qsizetype _offset = 0;
	QSocketNotifier _notifier;
	QTimer _timeout;

};

Writer::Writer(not_null<QObject*> parent, int fd, QByteArray data)
: QObject(parent)
, _fd(fd)
, _data(std::move(data))
, _notifier(fd, QSocketNotifier::Write) {
	fcntl(_fd, F_SETFL, fcntl(_fd, F_GETFL) | O_NONBLOCK);
	connect(
		&_notifier,
		&QSocketNotifier::activated,
		this,
		[=] { write(); });
	_timeout.setSingleShot(true);
	connect(&_timeout, &QTimer::timeout, this, [=] { finish(); });
	_timeout.start(kWriteTimeout);
}

Writer::~Writer() {
	close(_fd);
}

void Writer::write() {
	while (_offset < _data.size()) {
		const auto written = WriteNoSignal(
			_fd,
			_data.constData() + _offset,
			_data.size() - _offset);
		if (written > 0) {
			_offset += written;
		} else if (written < 0 && errno == EINTR) {
			continue;
		} else if (written < 0 && errno == EAGAIN) {
			return;
		} else {
			break;
		}
	}
	finish();
}

void Writer::finish() {
	_notifier.setEnabled(false);
	_timeout.stop();
	deleteLater();
}

} // namespace

struct DataDeviceManager::Private {
	struct Source final : base::has_weak_ptr {
		Source(
			not_null<Private*> manager,
			not_null<wl_resource*> resource,
			QClipboard::Mode mode);

		const not_null<Private*> manager;
		const not_null<wl_resource*> resource;
		const QClipboard::Mode mode = QClipboard::Clipboard;
		QStringList mimeTypes;
		uint32_t actions = 0;
		bool actionsSet = false;
		bool used = false;
	};

	struct Offer final : base::has_weak_ptr {
		Offer(
			not_null<Private*> manager,
			not_null<wl_resource*> resource,
			QClipboard::Mode mode,
			Source *source,
			bool dnd);

		const not_null<Private*> manager;
		const not_null<wl_resource*> resource;
		const QClipboard::Mode mode = QClipboard::Clipboard;
		const base::weak_ptr<Source> source;
		const bool host = false;
		const bool dnd = false;
		QStringList mimeTypes;
		base::flat_map<QString, QByteArray> snapshot;
		QPointer<const QMimeData> hostData;
		uint32_t hostActions = 0;
		QString accepted;
		uint32_t actions = 0;
		uint32_t preferred = 0;
		uint32_t action = 0;
		bool dropped = false;
		bool finished = false;
	};

	struct Drag {
		bool active = false;
		base::weak_ptr<Source> source;
		wl_client *origin = nullptr;
		QPointer<QWaylandSurface> focus;
		wl_resource *device = nullptr;
		base::weak_ptr<Offer> offer;
		QPointer<QWaylandSurface> icon;
		QPoint iconOffset;
		rpl::lifetime iconLifetime;
	};

	struct SelectionState {
		base::weak_ptr<Source> source;
		QPointer<QMimeData> mirror;
	};

	class Mirror;

	Private(
		not_null<QWaylandCompositor*> compositor,
		not_null<QWaylandSeat*> seat,
		Fn<void()> dragStarted);
	~Private();

	static const struct wl_data_device_manager_interface kManager;
	static const struct wl_data_device_interface kDevice;
	static const struct wl_data_source_interface kSource;
	static const struct wl_data_offer_interface kOffer;
	static const struct zwp_primary_selection_device_manager_v1_interface
		kPrimaryManager;
	static const struct zwp_primary_selection_device_v1_interface
		kPrimaryDevice;
	static const struct zwp_primary_selection_source_v1_interface
		kPrimarySource;
	static const struct zwp_primary_selection_offer_v1_interface
		kPrimaryOffer;

	[[nodiscard]] static uint32_t SourceActions(not_null<Source*> source);
	[[nodiscard]] static uint32_t OfferActions(not_null<Offer*> offer);
	[[nodiscard]] static bool Accepted(not_null<Offer*> offer);

	void createSource(
		wl_client *client,
		int version,
		uint32_t id,
		QClipboard::Mode mode);
	void createDevice(wl_client *client, int version, uint32_t id);
	void createPrimaryDevice(wl_client *client, int version, uint32_t id);
	void deviceDestroyed(not_null<wl_resource*> device);
	void sourceDestroyed(not_null<Source*> source);
	void offerDestroyed(not_null<Offer*> offer);
	[[nodiscard]] wl_resource *deviceFor(wl_client *client) const;
	[[nodiscard]] Offer *createOffer(
		QClipboard::Mode mode,
		not_null<wl_resource*> device,
		Source *source,
		const QStringList &mimeTypes,
		bool dnd);

	[[nodiscard]] SelectionState &selectionFor(QClipboard::Mode mode);
	void setSelection(QClipboard::Mode mode, Source *source);
	void hostChanged(QClipboard::Mode mode);
	void offerSelection(wl_client *client);
	void offerSelection(
		QClipboard::Mode mode,
		not_null<wl_resource*> device);
	void offerSelectionToFocus(QClipboard::Mode mode);
	void receive(not_null<Offer*> offer, const QString &mimeType, int fd);

	void startDrag(
		not_null<wl_resource*> device,
		Source *source,
		wl_resource *icon);
	void enterDrag(
		not_null<QWaylandSurface*> target,
		QPointF position,
		const QMimeData *data,
		Qt::DropActions actions);
	void leaveDrag();
	void updateAction(not_null<Offer*> offer);
	void finish(not_null<Offer*> offer);

	const not_null<QWaylandCompositor*> compositor;
	const not_null<QWaylandSeat*> seat;
	const Fn<void()> dragStarted;
	wl_global *global = nullptr;
	wl_global *primaryGlobal = nullptr;
	std::vector<not_null<wl_resource*>> devices;
	std::vector<not_null<wl_resource*>> primaryDevices;
	SelectionState selection;
	SelectionState primarySelection;
	Drag drag;
	rpl::lifetime lifetime;
};

class DataDeviceManager::Private::Mirror final : public QMimeData {
public:
	explicit Mirror(not_null<Source*> source);

	QStringList formats() const override;
	bool hasFormat(const QString &mimeType) const override;

protected:
	QVariant retrieveData(
		const QString &mimeType,
		QMetaType) const override;

private:
	const base::weak_ptr<Source> _source;
	const QStringList _offered;
	const QStringList _formats;

};

DataDeviceManager::Private::Mirror::Mirror(not_null<Source*> source)
: _source(base::make_weak(source))
, _offered(source->mimeTypes)
, _formats(MirrorFormats(_offered)) {
}

QStringList DataDeviceManager::Private::Mirror::formats() const {
	return _formats;
}

bool DataDeviceManager::Private::Mirror::hasFormat(
		const QString &mimeType) const {
	return _formats.contains(mimeType);
}

QVariant DataDeviceManager::Private::Mirror::retrieveData(
		const QString &mimeType,
		QMetaType) const {
	const auto source = _source.get();
	if (!source || !_formats.contains(mimeType)) {
		return QVariant();
	}
	const auto read = [&](const QString &format) {
		return ReadSource(source->resource, source->mode, format);
	};
	if (_offered.contains(mimeType)) {
		return read(mimeType);
	} else if (mimeType == PlainTextFormat()) {
		return read(TextFormat());
	}
	return QImage::fromData(read(ImageSourceFormat(_offered)));
}

DataDeviceManager::Private::Source::Source(
	not_null<Private*> manager,
	not_null<wl_resource*> resource,
	QClipboard::Mode mode)
: manager(manager)
, resource(resource)
, mode(mode) {
}

DataDeviceManager::Private::Offer::Offer(
	not_null<Private*> manager,
	not_null<wl_resource*> resource,
	QClipboard::Mode mode,
	Source *source,
	bool dnd)
: manager(manager)
, resource(resource)
, mode(mode)
, source(source ? base::make_weak(source) : nullptr)
, host(!source)
, dnd(dnd) {
}

const struct wl_data_device_manager_interface
DataDeviceManager::Private::kManager = {
	.create_data_source = [](
			wl_client *client,
			wl_resource *resource,
			uint32_t id) {
		const auto version = wl_resource_get_version(resource);
		Data<Private>(resource)->createSource(
			client,
			version,
			id,
			QClipboard::Clipboard);
	},
	.get_data_device = [](
			wl_client *client,
			wl_resource *resource,
			uint32_t id,
			wl_resource *) {
		const auto version = wl_resource_get_version(resource);
		Data<Private>(resource)->createDevice(client, version, id);
	},
};

const struct wl_data_device_interface DataDeviceManager::Private::kDevice = {
	.start_drag = [](
			wl_client *,
			wl_resource *resource,
			wl_resource *source,
			wl_resource *,
			wl_resource *icon,
			uint32_t) {
		Data<Private>(resource)->startDrag(
			resource,
			Data<Source>(source),
			icon);
	},
	.set_selection = [](
			wl_client *,
			wl_resource *resource,
			wl_resource *source,
			uint32_t) {
		const auto data = Data<Source>(source);
		if (data && data->actionsSet) {
			wl_resource_post_error(
				source,
				WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
				"source can't be used for selection");
			return;
		}
		Data<Private>(resource)->setSelection(
			QClipboard::Clipboard,
			data);
	},
	.release = [](wl_client *, wl_resource *resource) {
		wl_resource_destroy(resource);
	},
};

const struct wl_data_source_interface DataDeviceManager::Private::kSource = {
	.offer = [](
			wl_client *,
			wl_resource *resource,
			const char *mimeType) {
		Data<Source>(resource)->mimeTypes.push_back(
			QString::fromUtf8(mimeType));
	},
	.destroy = [](wl_client *, wl_resource *resource) {
		wl_resource_destroy(resource);
	},
	.set_actions = [](
			wl_client *,
			wl_resource *resource,
			uint32_t actions) {
		const auto source = Data<Source>(resource);
		if (actions & ~kActions) {
			wl_resource_post_error(
				resource,
				WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
				"invalid action mask %x",
				actions);
		} else if (source->used) {
			wl_resource_post_error(
				resource,
				WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
				"source is already used");
		} else {
			source->actions = actions;
			source->actionsSet = true;
		}
	},
};

const struct wl_data_offer_interface DataDeviceManager::Private::kOffer = {
	.accept = [](
			wl_client *,
			wl_resource *resource,
			uint32_t,
			const char *mimeType) {
		const auto offer = Data<Offer>(resource);
		if (!offer->dnd || offer->finished) {
			return;
		}
		offer->accepted = mimeType
			? QString::fromUtf8(mimeType)
			: QString();
		if (const auto source = offer->source.get()) {
			const auto resource = source->resource.get();
			wl_data_source_send_target(resource, mimeType);
		}
	},
	.receive = [](
			wl_client *,
			wl_resource *resource,
			const char *mimeType,
			int32_t fd) {
		const auto offer = Data<Offer>(resource);
		const auto type = QString::fromUtf8(mimeType);
		offer->manager->receive(offer, type, fd);
	},
	.destroy = [](wl_client *, wl_resource *resource) {
		wl_resource_destroy(resource);
	},
	.finish = [](wl_client *, wl_resource *resource) {
		const auto offer = Data<Offer>(resource);
		offer->manager->finish(offer);
	},
	.set_actions = [](
			wl_client *,
			wl_resource *resource,
			uint32_t actions,
			uint32_t preferred) {
		const auto offer = Data<Offer>(resource);
		if (!offer->dnd) {
			wl_resource_post_error(
				resource,
				WL_DATA_OFFER_ERROR_INVALID_OFFER,
				"offer isn't for drag-and-drop");
		} else if (actions & ~kActions) {
			wl_resource_post_error(
				resource,
				WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK,
				"invalid action mask %x",
				actions);
		} else if ((preferred & (preferred - 1))
				|| (preferred & ~actions)) {
			wl_resource_post_error(
				resource,
				WL_DATA_OFFER_ERROR_INVALID_ACTION,
				"invalid preferred action %x",
				preferred);
		} else {
			offer->actions = actions;
			offer->preferred = preferred;
			offer->manager->updateAction(offer);
		}
	},
};

const struct zwp_primary_selection_device_manager_v1_interface
DataDeviceManager::Private::kPrimaryManager = {
	.create_source = [](
			wl_client *client,
			wl_resource *resource,
			uint32_t id) {
		const auto version = wl_resource_get_version(resource);
		Data<Private>(resource)->createSource(
			client,
			version,
			id,
			QClipboard::Selection);
	},
	.get_device = [](
			wl_client *client,
			wl_resource *resource,
			uint32_t id,
			wl_resource *) {
		const auto version = wl_resource_get_version(resource);
		Data<Private>(resource)->createPrimaryDevice(
			client,
			version,
			id);
	},
	.destroy = [](wl_client *, wl_resource *resource) {
		wl_resource_destroy(resource);
	},
};

const struct zwp_primary_selection_device_v1_interface
DataDeviceManager::Private::kPrimaryDevice = {
	.set_selection = [](
			wl_client *,
			wl_resource *resource,
			wl_resource *source,
			uint32_t) {
		Data<Private>(resource)->setSelection(
			QClipboard::Selection,
			Data<Source>(source));
	},
	.destroy = [](wl_client *, wl_resource *resource) {
		wl_resource_destroy(resource);
	},
};

const struct zwp_primary_selection_source_v1_interface
DataDeviceManager::Private::kPrimarySource = {
	.offer = [](
			wl_client *,
			wl_resource *resource,
			const char *mimeType) {
		Data<Source>(resource)->mimeTypes.push_back(
			QString::fromUtf8(mimeType));
	},
	.destroy = [](wl_client *, wl_resource *resource) {
		wl_resource_destroy(resource);
	},
};

const struct zwp_primary_selection_offer_v1_interface
DataDeviceManager::Private::kPrimaryOffer = {
	.receive = [](
			wl_client *,
			wl_resource *resource,
			const char *mimeType,
			int32_t fd) {
		const auto offer = Data<Offer>(resource);
		const auto type = QString::fromUtf8(mimeType);
		offer->manager->receive(offer, type, fd);
	},
	.destroy = [](wl_client *, wl_resource *resource) {
		wl_resource_destroy(resource);
	},
};

DataDeviceManager::Private::Private(
	not_null<QWaylandCompositor*> compositor,
	not_null<QWaylandSeat*> seat,
	Fn<void()> dragStarted)
: compositor(compositor)
, seat(seat)
, dragStarted(std::move(dragStarted))
, global(wl_global_create(
	compositor->display(),
	&wl_data_device_manager_interface,
	kVersion,
	this,
	[](wl_client *client, void *data, uint32_t version, uint32_t id) {
		BindManager(
			client,
			version,
			id,
			&wl_data_device_manager_interface,
			&kManager,
			data);
	}))
, primaryGlobal(wl_global_create(
	compositor->display(),
	&zwp_primary_selection_device_manager_v1_interface,
	kPrimaryVersion,
	this,
	[](wl_client *client, void *data, uint32_t version, uint32_t id) {
		BindManager(
			client,
			version,
			id,
			&zwp_primary_selection_device_manager_v1_interface,
			&kPrimaryManager,
			data);
	})) {
	base::qt_signal_producer(
		seat.get(),
		&QWaylandSeat::keyboardFocusChanged
	) | rpl::on_next([=](QWaylandSurface *now, QWaylandSurface *) {
		if (now && now->client()) {
			offerSelection(now->client()->client());
		}
	}, lifetime);

	base::qt_signal_producer(
		QGuiApplication::clipboard(),
		&QClipboard::changed
	) | rpl::on_next([=](QClipboard::Mode mode) {
		if (mode == QClipboard::Clipboard
				|| mode == QClipboard::Selection) {
			hostChanged(mode);
		}
	}, lifetime);
}

DataDeviceManager::Private::~Private() {
	if (primaryGlobal) {
		wl_global_destroy(primaryGlobal);
	}
	if (global) {
		wl_global_destroy(global);
	}
}

uint32_t DataDeviceManager::Private::SourceActions(
		not_null<Source*> source) {
	return SupportsActions(source->resource) ? source->actions : kCopy;
}

uint32_t DataDeviceManager::Private::OfferActions(not_null<Offer*> offer) {
	return SupportsActions(offer->resource) ? offer->actions : kCopy;
}

bool DataDeviceManager::Private::Accepted(not_null<Offer*> offer) {
	return !offer->accepted.isEmpty() && offer->action;
}

void DataDeviceManager::Private::createSource(
		wl_client *client,
		int version,
		uint32_t id,
		QClipboard::Mode mode) {
	const auto primary = (mode == QClipboard::Selection);
	const auto resource = wl_resource_create(
		client,
		(primary
			? &zwp_primary_selection_source_v1_interface
			: &wl_data_source_interface),
		version,
		id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(
		resource,
		(primary
			? static_cast<const void*>(&kPrimarySource)
			: static_cast<const void*>(&kSource)),
		new Source(this, resource, mode),
		[](wl_resource *resource) {
			const auto source = Data<Source>(resource);
			source->manager->sourceDestroyed(source);
			delete source;
		});
}

void DataDeviceManager::Private::createDevice(
		wl_client *client,
		int version,
		uint32_t id) {
	const auto resource = wl_resource_create(
		client,
		&wl_data_device_interface,
		version,
		id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(
		resource,
		&kDevice,
		this,
		[](wl_resource *resource) {
			Data<Private>(resource)->deviceDestroyed(resource);
		});
	devices.push_back(resource);

	const auto focus = seat->keyboardFocus();
	if (focus && focus->client() && focus->client()->client() == client) {
		offerSelection(QClipboard::Clipboard, resource);
	}
}

void DataDeviceManager::Private::createPrimaryDevice(
		wl_client *client,
		int version,
		uint32_t id) {
	const auto resource = wl_resource_create(
		client,
		&zwp_primary_selection_device_v1_interface,
		version,
		id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(
		resource,
		&kPrimaryDevice,
		this,
		[](wl_resource *resource) {
			const auto that = Data<Private>(resource);
			std::erase(that->primaryDevices, resource);
		});
	primaryDevices.push_back(resource);

	const auto focus = seat->keyboardFocus();
	if (focus && focus->client() && focus->client()->client() == client) {
		offerSelection(QClipboard::Selection, resource);
	}
}

void DataDeviceManager::Private::deviceDestroyed(
		not_null<wl_resource*> device) {
	std::erase(devices, device);
	if (drag.device == device) {
		drag.device = nullptr;
		drag.focus = nullptr;
		drag.offer = nullptr;
	}
}

void DataDeviceManager::Private::sourceDestroyed(not_null<Source*> source) {
	if (drag.active && drag.source.get() == source) {
		leaveDrag();
		drag = Drag();
	}
	const auto mode = source->mode;
	auto &state = selectionFor(mode);
	if (state.source.get() != source) {
		return;
	}
	state.source = nullptr;
	const auto clipboard = QGuiApplication::clipboard();
	const auto mirror = state.mirror.data();
	if (mirror && clipboard->mimeData(mode) == mirror) {
		clipboard->clear(mode);
	} else {
		offerSelectionToFocus(mode);
	}
}

void DataDeviceManager::Private::offerDestroyed(not_null<Offer*> offer) {
	const auto source = offer->source.get();
	if (!source
			|| !offer->dropped
			|| offer->finished
			|| !SupportsActions(source->resource)) {
		return;
	} else if (SupportsActions(offer->resource)) {
		wl_data_source_send_cancelled(source->resource);
	} else {
		wl_data_source_send_dnd_finished(source->resource);
	}
}

wl_resource *DataDeviceManager::Private::deviceFor(wl_client *client) const {
	const auto i = std::ranges::find_if(
		devices,
		[&](wl_resource *device) {
			return wl_resource_get_client(device) == client;
		});
	return (i != end(devices)) ? i->get() : nullptr;
}

auto DataDeviceManager::Private::createOffer(
		QClipboard::Mode mode,
		not_null<wl_resource*> device,
		Source *source,
		const QStringList &mimeTypes,
		bool dnd) -> Offer* {
	const auto primary = (mode == QClipboard::Selection);
	const auto client = wl_resource_get_client(device);
	const auto resource = wl_resource_create(
		client,
		(primary
			? &zwp_primary_selection_offer_v1_interface
			: &wl_data_offer_interface),
		wl_resource_get_version(device),
		0);
	if (!resource) {
		wl_client_post_no_memory(client);
		return nullptr;
	}
	const auto offer = new Offer(this, resource, mode, source, dnd);
	offer->mimeTypes = mimeTypes;
	wl_resource_set_implementation(
		resource,
		(primary
			? static_cast<const void*>(&kPrimaryOffer)
			: static_cast<const void*>(&kOffer)),
		offer,
		[](wl_resource *resource) {
			const auto offer = Data<Offer>(resource);
			offer->manager->offerDestroyed(offer);
			delete offer;
		});
	if (primary) {
		zwp_primary_selection_device_v1_send_data_offer(
			device,
			resource);
	} else {
		wl_data_device_send_data_offer(device, resource);
	}
	for (const auto &mimeType : mimeTypes) {
		const auto utf8 = mimeType.toUtf8();
		if (primary) {
			zwp_primary_selection_offer_v1_send_offer(
				resource,
				utf8.constData());
		} else {
			wl_data_offer_send_offer(resource, utf8.constData());
		}
	}
	return offer;
}

auto DataDeviceManager::Private::selectionFor(QClipboard::Mode mode)
-> SelectionState& {
	return (mode == QClipboard::Selection) ? primarySelection : selection;
}

void DataDeviceManager::Private::setSelection(
		QClipboard::Mode mode,
		Source *source) {
	auto &state = selectionFor(mode);
	const auto previous = state.source.get();
	if (previous && previous != source) {
		SendSourceCancelled(previous->resource, mode);
	}
	state.source = source ? base::make_weak(source) : nullptr;
	if (source) {
		source->used = true;
	}
	if (HostSupports(mode)) {
		const auto clipboard = QGuiApplication::clipboard();
		if (source) {
			state.mirror = new Mirror(source);
			clipboard->setMimeData(state.mirror.data(), mode);
		} else if (const auto mirror = state.mirror.data()) {
			if (clipboard->mimeData(mode) == mirror) {
				clipboard->clear(mode);
			}
		}
	}
	offerSelectionToFocus(mode);
}

void DataDeviceManager::Private::hostChanged(QClipboard::Mode mode) {
	auto &state = selectionFor(mode);
	const auto data = QGuiApplication::clipboard()->mimeData(mode);
	if (state.mirror && data == state.mirror.data()) {
		return;
	} else if (const auto source = state.source.get()) {
		state.source = nullptr;
		SendSourceCancelled(source->resource, mode);
	}
	offerSelectionToFocus(mode);
}

void DataDeviceManager::Private::offerSelection(wl_client *client) {
	for (const auto &device : devices) {
		if (wl_resource_get_client(device) == client) {
			offerSelection(QClipboard::Clipboard, device);
		}
	}
	for (const auto &device : primaryDevices) {
		if (wl_resource_get_client(device) == client) {
			offerSelection(QClipboard::Selection, device);
		}
	}
}

void DataDeviceManager::Private::offerSelection(
		QClipboard::Mode mode,
		not_null<wl_resource*> device) {
	const auto source = selectionFor(mode).source.get();
	const auto data = (source || !HostSupports(mode))
		? nullptr
		: QGuiApplication::clipboard()->mimeData(mode);
	const auto mimeTypes = source
		? source->mimeTypes
		: data
		? OfferedFormats(data)
		: QStringList();
	const auto offer = !mimeTypes.isEmpty()
		? createOffer(mode, device, source, mimeTypes, false)
		: nullptr;
	const auto resource = offer ? offer->resource.get() : nullptr;
	if (mode == QClipboard::Selection) {
		zwp_primary_selection_device_v1_send_selection(
			device,
			resource);
	} else {
		wl_data_device_send_selection(device, resource);
	}
}

void DataDeviceManager::Private::offerSelectionToFocus(
		QClipboard::Mode mode) {
	const auto focus = seat->keyboardFocus();
	if (!focus || !focus->client()) {
		return;
	}
	const auto client = focus->client()->client();
	const auto &list = (mode == QClipboard::Selection)
		? primaryDevices
		: devices;
	for (const auto &device : list) {
		if (wl_resource_get_client(device) == client) {
			offerSelection(mode, device);
		}
	}
}

void DataDeviceManager::Private::receive(
		not_null<Offer*> offer,
		const QString &mimeType,
		int fd) {
	if (const auto source = offer->source.get()) {
		SendSourceData(source->resource, source->mode, mimeType, fd);
		close(fd);
	} else if (offer->host && offer->dnd) {
		const auto &snapshot = offer->snapshot;
		const auto i = snapshot.find(mimeType);
		const auto live = (drag.offer.get() == offer)
			? offer->hostData.get()
			: nullptr;
		new Writer(
			compositor,
			fd,
			((i != end(snapshot))
				? i->second
				: live
				? FormatData(live, mimeType)
				: QByteArray()));
	} else if (offer->host) {
		const auto clipboard = QGuiApplication::clipboard();
		const auto data = clipboard->mimeData(offer->mode);
		new Writer(
			compositor,
			fd,
			data ? FormatData(data, mimeType) : QByteArray());
	} else {
		close(fd);
	}
}

void DataDeviceManager::Private::startDrag(
		not_null<wl_resource*> device,
		Source *source,
		wl_resource *icon) {
	const auto surface = icon
		? QWaylandSurface::fromResource(icon)
		: nullptr;
	if (surface) {
		const auto error = uint32_t(WL_DATA_DEVICE_ERROR_ROLE);
		if (!surface->setRole(IconRole(), device, error)) {
			return;
		}
	}
	if (drag.active) {
		if (source && SupportsActions(source->resource)) {
			wl_data_source_send_cancelled(source->resource);
		}
		return;
	} else if (source) {
		source->used = true;
	}
	drag = Drag{
		.active = true,
		.source = source ? base::make_weak(source) : nullptr,
		.origin = wl_resource_get_client(device),
		.icon = surface,
	};
	if (surface) {
		base::qt_signal_producer(
			surface,
			&QWaylandSurface::offsetForNextFrame
		) | rpl::on_next([=](const QPoint &offset) {
			drag.iconOffset += offset;
		}, drag.iconLifetime);
	}
	dragStarted();
}

void DataDeviceManager::Private::enterDrag(
		not_null<QWaylandSurface*> target,
		QPointF position,
		const QMimeData *data,
		Qt::DropActions actions) {
	const auto device = deviceFor(target->client()->client());
	if (!device) {
		return;
	}
	const auto source = drag.source.get();
	const auto host = !drag.active && data;
	const auto offer = (source || host)
		? createOffer(
			QClipboard::Clipboard,
			device,
			source,
			source ? source->mimeTypes : OfferedFormats(data),
			true)
		: nullptr;
	if (offer && host) {
		offer->hostData = data;
		offer->hostActions = WaylandActions(actions);
	}
	if (offer && SupportsActions(offer->resource)) {
		wl_data_offer_send_source_actions(
			offer->resource,
			source ? SourceActions(source) : offer->hostActions);
	} else if (offer) {
		updateAction(offer);
	}
	drag.focus = target.get();
	drag.device = device;
	drag.offer = offer ? base::make_weak(offer) : nullptr;
	wl_data_device_send_enter(
		device,
		wl_display_next_serial(compositor->display()),
		target->resource(),
		wl_fixed_from_double(position.x()),
		wl_fixed_from_double(position.y()),
		offer ? offer->resource.get() : nullptr);
}

void DataDeviceManager::Private::leaveDrag() {
	if (drag.device) {
		wl_data_device_send_leave(drag.device);
	}
	drag.focus = nullptr;
	drag.device = nullptr;
	drag.offer = nullptr;
}

void DataDeviceManager::Private::updateAction(not_null<Offer*> offer) {
	const auto source = offer->source.get();
	if ((!source && !offer->host) || offer->dropped) {
		return;
	}
	const auto action = ChooseAction(
		source ? SourceActions(source) : offer->hostActions,
		OfferActions(offer),
		offer->preferred);
	if (offer->action == action) {
		return;
	}
	offer->action = action;
	if (SupportsActions(offer->resource)) {
		wl_data_offer_send_action(offer->resource, action);
	}
	if (source && SupportsActions(source->resource)) {
		wl_data_source_send_action(source->resource, action);
	}
}

void DataDeviceManager::Private::finish(not_null<Offer*> offer) {
	if (!offer->dnd
			|| !offer->dropped
			|| offer->finished
			|| !Accepted(offer)
			|| offer->action == kAsk) {
		wl_resource_post_error(
			offer->resource,
			WL_DATA_OFFER_ERROR_INVALID_FINISH,
			"offer can't be finished");
		return;
	}
	offer->finished = true;
	const auto source = offer->source.get();
	if (source && SupportsActions(source->resource)) {
		wl_data_source_send_dnd_finished(source->resource);
	}
}

DataDeviceManager::DataDeviceManager(
	not_null<QWaylandCompositor*> compositor,
	not_null<QWaylandSeat*> seat,
	Fn<void()> dragStarted)
: _private(std::make_unique<Private>(
	compositor,
	seat,
	std::move(dragStarted))) {
}

DataDeviceManager::~DataDeviceManager() = default;

const wl_global *DataDeviceManager::global() const {
	return _private->global;
}

bool DataDeviceManager::dragging() const {
	return _private->drag.active;
}

QMimeData *DataDeviceManager::createDragData() const {
	const auto source = _private->drag.source.get();
	return source ? new Private::Mirror(source) : new QMimeData();
}

Qt::DropActions DataDeviceManager::dragActions() const {
	const auto source = _private->drag.source.get();
	const auto actions = source ? Private::SourceActions(source) : kCopy;
	auto result = Qt::DropActions();
	if (actions & kCopy) {
		result |= Qt::CopyAction;
	}
	if (actions & kMove) {
		result |= Qt::MoveAction;
	}
	return result ? result : Qt::DropActions(Qt::CopyAction);
}

QWaylandSurface *DataDeviceManager::dragIcon() const {
	const auto &drag = _private->drag;
	return drag.active ? drag.icon.data() : nullptr;
}

QPoint DataDeviceManager::dragIconOffset() const {
	return _private->drag.iconOffset;
}

void DataDeviceManager::dragFinished(Qt::DropAction result) {
	auto &drag = _private->drag;
	if (!drag.active) {
		return;
	}
	_private->leaveDrag();
	const auto source = drag.source.get();
	const auto resource = source ? source->resource.get() : nullptr;
	if (resource && SupportsActions(resource)) {
		if (result == Qt::IgnoreAction) {
			wl_data_source_send_cancelled(resource);
		} else {
			wl_data_source_send_action(
				resource,
				(result == Qt::MoveAction) ? kMove : kCopy);
			wl_data_source_send_dnd_drop_performed(resource);
			wl_data_source_send_dnd_finished(resource);
		}
	}
	drag = Private::Drag();
}

void DataDeviceManager::dragMove(
		QWaylandSurface *target,
		QPointF position,
		const QMimeData *data,
		Qt::DropActions actions) {
	auto &drag = _private->drag;
	if (target
			&& drag.active
			&& !drag.source.get()
			&& target->client()->client() != drag.origin) {
		target = nullptr;
	}
	if (target != drag.focus.data()) {
		_private->leaveDrag();
		if (target) {
			_private->enterDrag(target, position, data, actions);
		}
		return;
	} else if (!drag.device) {
		return;
	}
	const auto offer = drag.offer.get();
	const auto updated = WaylandActions(actions);
	if (offer && offer->host && offer->hostActions != updated) {
		offer->hostActions = updated;
		if (SupportsActions(offer->resource)) {
			wl_data_offer_send_source_actions(
				offer->resource,
				updated);
		}
		_private->updateAction(offer);
	}
	wl_data_device_send_motion(
		drag.device,
		_private->compositor->currentTimeMsecs(),
		wl_fixed_from_double(position.x()),
		wl_fixed_from_double(position.y()));
}

Qt::DropAction DataDeviceManager::dragAction() const {
	const auto &drag = _private->drag;
	if (!drag.device) {
		return Qt::IgnoreAction;
	} else if (drag.active && !drag.source.get()) {
		return Qt::CopyAction;
	}
	const auto offer = drag.offer.get();
	if (!offer || !Private::Accepted(offer)) {
		return Qt::IgnoreAction;
	}
	return (offer->action == kMove) ? Qt::MoveAction : Qt::CopyAction;
}

void DataDeviceManager::dragLeave() {
	_private->leaveDrag();
}

Qt::DropAction DataDeviceManager::drop(const QMimeData *data) {
	auto &drag = _private->drag;
	const auto source = drag.source.get();
	const auto resource = source ? source->resource.get() : nullptr;
	const auto offer = drag.offer.get();
	const auto result = dragAction();
	const auto accepted = (result != Qt::IgnoreAction);
	if (accepted) {
		if (offer) {
			offer->dropped = true;
		}
		if (offer && offer->host && data) {
			for (const auto &mimeType : offer->mimeTypes) {
				offer->snapshot.emplace(
					mimeType,
					FormatData(data, mimeType));
			}
		}
		wl_data_device_send_drop(drag.device);
		if (resource && SupportsActions(resource)) {
			wl_data_source_send_dnd_drop_performed(resource);
		}
	}
	_private->leaveDrag();
	if (drag.active) {
		if (resource && !accepted && SupportsActions(resource)) {
			wl_data_source_send_cancelled(resource);
		}
		drag = Private::Drag();
	}
	return result;
}

void DataDeviceManager::cancelDrag() {
	auto &drag = _private->drag;
	if (!drag.active) {
		return;
	}
	_private->leaveDrag();
	const auto source = drag.source.get();
	if (source && SupportsActions(source->resource)) {
		wl_data_source_send_cancelled(source->resource);
	}
	drag = Private::Drag();
}

} // namespace Webview
