// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/webview_interface.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>
#include <crl/crl_time.h>

#include "base/call_delayed.h"
#include "base/debug_log.h"
#include "base/platform/win/base_windows_winrt.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <windows.h>

namespace Webview {
namespace {

constexpr auto kRandomLength = std::size_t(6);
constexpr auto kStagingSuffix = std::wstring_view(L".removing");
constexpr auto kEngineFolder = std::wstring_view(L"EBWebView");
constexpr auto kLetters = std::wstring_view(
	L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
constexpr auto kForbidden = std::wstring_view(L"<>:\"/\\|?*");
constexpr auto kMinimalAge = std::int64_t(10) * 10'000'000;
constexpr auto kMaxDepth = 64;
constexpr auto kRenameAttempts = 4;
constexpr auto kMaxPath = std::size_t(32768);
constexpr auto kListBufferSize = std::size_t(64 * 1024);
constexpr auto kFastRetries = 10;
constexpr auto kFastRetryDelay = crl::time(1000);
constexpr auto kMaxRetryDelay = crl::time(30000);
constexpr auto kFirstSweepDelay = crl::time(5000);
constexpr auto kSecondSweepDelay = crl::time(30000);
constexpr auto kMaxSweepDelay = crl::time(600000);
constexpr auto kLeafAccess = DWORD(DELETE
	| FILE_READ_ATTRIBUTES
	| FILE_WRITE_ATTRIBUTES);
constexpr auto kDirectoryAccess = DWORD(kLeafAccess | FILE_LIST_DIRECTORY);
constexpr auto kShareAll = DWORD(FILE_SHARE_READ
	| FILE_SHARE_WRITE
	| FILE_SHARE_DELETE);
constexpr auto kLocalDriveTypes = std::array<UINT, 3>{
	DRIVE_FIXED,
	DRIVE_REMOVABLE,
	DRIVE_RAMDISK,
};

enum class NameKind {
	None,
	Plain,
	Staging,
};

enum class Outcome {
	Removed,
	Skipped,
	Busy,
	Failed,
	Unfinished,
};

struct Entry {
	std::wstring name;
	std::uint64_t id = 0;
	std::int64_t created = 0;
	DWORD attributes = 0;
};

struct Identity {
	std::uint64_t volume = 0;
	std::array<std::uint64_t, 2> id = {};
	bool full = false;

	friend inline bool operator==(Identity, Identity) = default;
};

struct Parent {
	winrt::file_handle handle;
	std::wstring path;
	DWORD volume = 0;
};

struct Target {
	std::wstring name;
	std::wstring appName;
	std::uint64_t id = 0;
	DWORD volume = 0;
	std::shared_ptr<winrt::file_handle> reference;
};

struct Retired {
	std::wstring parent;
	Target target;
	int attempt = 0;
	bool legacy = false;
};

struct Sweep {
	std::wstring parent;
	std::wstring appName;
	std::int64_t started = 0;
};

struct Summary {
	std::vector<Target> claimed;
	int removed = 0;
	int busy = 0;
	int failed = 0;
	bool retry = false;
};

[[nodiscard]] bool IsAsciiLetter(wchar_t ch) {
	return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
}

[[nodiscard]] bool IsPlainDirectory(DWORD attributes) {
	return (attributes & FILE_ATTRIBUTE_DIRECTORY)
		&& !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

[[nodiscard]] bool IsGone(DWORD error) {
	return (error == ERROR_FILE_NOT_FOUND)
		|| (error == ERROR_PATH_NOT_FOUND)
		|| (error == ERROR_DELETE_PENDING);
}

[[nodiscard]] bool Unimplemented(DWORD error) {
	return (error == ERROR_INVALID_PARAMETER)
		|| (error == ERROR_NOT_SUPPORTED)
		|| (error == ERROR_INVALID_FUNCTION);
}

[[nodiscard]] std::int64_t Ticks(FILETIME time) {
	return std::int64_t((std::uint64_t(time.dwHighDateTime) << 32)
		| time.dwLowDateTime);
}

[[nodiscard]] std::uint64_t FileId(const BY_HANDLE_FILE_INFORMATION &info) {
	return (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

[[nodiscard]] bool ValidComponent(std::wstring_view component) {
	return !component.empty()
		&& component.back() != L'.'
		&& component.back() != L' '
		&& std::none_of(component.begin(), component.end(), [](wchar_t ch) {
			return (ch < 0x20) || (kForbidden.find(ch) != kForbidden.npos);
		});
}

[[nodiscard]] std::wstring ToExtended(const QString &path) {
	const auto native = QDir::toNativeSeparators(path).toStdWString();
	if (native.size() < 2
		|| !IsAsciiLetter(native[0])
		|| native[1] != L':'
		|| (native.size() > 2 && native[2] != L'\\')) {
		return {};
	}
	auto result = L"\\\\?\\" + native.substr(0, 2) + L'\\';
	if (native.size() <= 3) {
		return result;
	}
	auto rest = std::wstring_view(native).substr(3);
	while (true) {
		const auto separator = rest.find(L'\\');
		const auto component = rest.substr(0, separator);
		if (!ValidComponent(component)) {
			return {};
		}
		result += component;
		if (separator == rest.npos) {
			return result;
		}
		result += L'\\';
		rest.remove_prefix(separator + 1);
	}
}

[[nodiscard]] std::wstring ChildPath(
		const std::wstring &parent,
		std::wstring_view name) {
	auto result = parent;
	if (!result.ends_with(L'\\')) {
		result += L'\\';
	}
	result += name;
	return result;
}

[[nodiscard]] std::wstring AppName() {
	const auto result = QCoreApplication::applicationName().toStdWString();
	return ValidComponent(result) ? result : std::wstring();
}

[[nodiscard]] NameKind Classify(
		std::wstring_view name,
		std::wstring_view appName) {
	const auto prefix = appName.size() + 1;
	if (name.size() < prefix + kRandomLength
		|| !name.starts_with(appName)
		|| name[appName.size()] != L'-'
		|| !std::all_of(
			name.begin() + prefix,
			name.begin() + prefix + kRandomLength,
			IsAsciiLetter)) {
		return NameKind::None;
	}
	const auto rest = name.substr(prefix + kRandomLength);
	return rest.empty()
		? NameKind::Plain
		: (rest == kStagingSuffix)
		? NameKind::Staging
		: NameKind::None;
}

[[nodiscard]] std::wstring StagingName(const std::wstring &appName) {
	auto device = std::random_device();
	auto letter = std::uniform_int_distribution<std::size_t>(
		0,
		kLetters.size() - 1);
	auto result = appName + L'-';
	for (auto i = std::size_t(); i != kRandomLength; ++i) {
		result += kLetters[letter(device)];
	}
	result += kStagingSuffix;
	return result;
}

[[nodiscard]] winrt::file_handle OpenNoFollow(
		const std::wstring &path,
		DWORD access,
		DWORD share = FILE_SHARE_READ) {
	return winrt::file_handle(CreateFileW(
		path.c_str(),
		access,
		share,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr));
}

[[nodiscard]] std::wstring FinalPath(HANDLE handle) {
	auto result = std::wstring(kMaxPath, wchar_t());
	const auto length = GetFinalPathNameByHandleW(
		handle,
		result.data(),
		DWORD(result.size()),
		FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (!length || length >= result.size()) {
		return {};
	}
	result.resize(length);
	return result;
}

[[nodiscard]] Parent OpenParent(const std::wstring &path) {
	// Renaming a child opens this parent for writing, so keep that shared.
	auto handle = winrt::file_handle(CreateFileW(
		path.c_str(),
		FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		nullptr));
	auto info = BY_HANDLE_FILE_INFORMATION();
	auto canonical = (handle && GetFileInformationByHandle(handle.get(), &info))
		? FinalPath(handle.get())
		: std::wstring();
	return {
		std::move(handle),
		std::move(canonical),
		info.dwVolumeSerialNumber,
	};
}

[[nodiscard]] bool LocalVolume(const std::wstring &path) {
	if (path.size() < 7
		|| !path.starts_with(L"\\\\?\\")
		|| !IsAsciiLetter(path[4])
		|| path.compare(5, 2, L":\\") != 0) {
		return false;
	}
	const auto type = GetDriveTypeW(path.substr(4, 3).c_str());
	return std::ranges::find(kLocalDriveTypes, type)
		!= kLocalDriveTypes.end();
}

[[nodiscard]] std::optional<Identity> QueryIdentity(HANDLE handle) {
	auto full = FILE_ID_INFO();
	if (GetFileInformationByHandleEx(
			handle,
			FileIdInfo,
			&full,
			sizeof(full))) {
		auto result = Identity{
			.volume = full.VolumeSerialNumber,
			.full = true,
		};
		std::memcpy(
			result.id.data(),
			full.FileId.Identifier,
			sizeof(result.id));
		return result;
	} else if (!Unimplemented(GetLastError())) {
		return std::nullopt;
	}
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!GetFileInformationByHandle(handle, &info)) {
		return std::nullopt;
	}
	return Identity{
		.volume = info.dwVolumeSerialNumber,
		.id = { FileId(info), 0 },
	};
}

[[nodiscard]] bool SameObject(HANDLE first, HANDLE second) {
	const auto a = QueryIdentity(first);
	const auto b = QueryIdentity(second);
	return a && b && (*a == *b);
}

[[nodiscard]] winrt::file_handle OpenReference(const std::wstring &path) {
	auto result = OpenNoFollow(path, FILE_READ_ATTRIBUTES, kShareAll);
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!result
		|| !GetFileInformationByHandle(result.get(), &info)
		|| !IsPlainDirectory(info.dwFileAttributes)
		|| !LocalVolume(FinalPath(result.get()))) {
		return {};
	}
	return result;
}

[[nodiscard]] std::optional<std::vector<Entry>> ListDirectory(
		HANDLE directory) {
	auto buffer = std::vector<std::uint64_t>(
		kListBufferSize / sizeof(std::uint64_t));
	auto result = std::vector<Entry>();
	auto infoClass = FileIdBothDirectoryRestartInfo;
	while (GetFileInformationByHandleEx(
			directory,
			infoClass,
			buffer.data(),
			DWORD(buffer.size() * sizeof(std::uint64_t)))) {
		infoClass = FileIdBothDirectoryInfo;
		auto bytes = reinterpret_cast<const char*>(buffer.data());
		while (true) {
			const auto info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
				bytes);
			const auto name = std::wstring_view(
				info->FileName,
				info->FileNameLength / sizeof(wchar_t));
			if (name != L"." && name != L"..") {
				result.push_back({
					.name = std::wstring(name),
					.id = std::uint64_t(info->FileId.QuadPart),
					.created = info->CreationTime.QuadPart,
					.attributes = info->FileAttributes,
				});
			}
			if (!info->NextEntryOffset) {
				break;
			}
			bytes += info->NextEntryOffset;
		}
	}
	if (GetLastError() != ERROR_NO_MORE_FILES) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] bool EngineShape(
		const std::vector<Entry> &entries,
		bool allowEmpty) {
	return entries.empty()
		? allowEmpty
		: (entries.size() == 1
			&& entries.front().name == kEngineFolder
			&& IsPlainDirectory(entries.front().attributes));
}

[[nodiscard]] bool DeleteByHandle(HANDLE handle, DWORD attributes) {
	if (attributes & FILE_ATTRIBUTE_READONLY) {
		auto basic = FILE_BASIC_INFO();
		basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
		if (!SetFileInformationByHandle(
				handle,
				FileBasicInfo,
				&basic,
				sizeof(basic))) {
			return false;
		}
	}
	auto disposition = FILE_DISPOSITION_INFO{ TRUE };
	return SetFileInformationByHandle(
		handle,
		FileDispositionInfo,
		&disposition,
		sizeof(disposition)) != FALSE;
}

[[nodiscard]] std::optional<std::vector<Entry>> ListSame(
		const std::wstring &path,
		HANDLE reference) {
	const auto handle = OpenNoFollow(
		path,
		FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
		kShareAll);
	if (!handle || !SameObject(handle.get(), reference)) {
		return std::nullopt;
	}
	return ListDirectory(handle.get());
}

[[nodiscard]] bool RemoveEmpty(const std::wstring &path, HANDLE reference) {
	const auto handle = OpenNoFollow(path, kDirectoryAccess);
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!handle
		|| !GetFileInformationByHandle(handle.get(), &info)
		|| !IsPlainDirectory(info.dwFileAttributes)
		|| !SameObject(handle.get(), reference)) {
		return false;
	}
	const auto entries = ListDirectory(handle.get());
	return entries
		&& entries->empty()
		&& DeleteByHandle(handle.get(), info.dwFileAttributes);
}

[[nodiscard]] bool RemoveChildren(
	const std::wstring &path,
	const std::vector<Entry> &entries,
	int depth);

[[nodiscard]] bool RemoveEntry(
		const std::wstring &path,
		const Entry &entry,
		int depth) {
	const auto directory = IsPlainDirectory(entry.attributes);
	const auto handle = OpenNoFollow(
		path,
		directory ? kDirectoryAccess : kLeafAccess);
	if (!handle) {
		return IsGone(GetLastError());
	}
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!GetFileInformationByHandle(handle.get(), &info)
		|| IsPlainDirectory(info.dwFileAttributes) != directory) {
		return false;
	}
	if (directory) {
		auto children = std::optional<std::vector<Entry>>();
		if (depth < kMaxDepth) {
			children = ListDirectory(handle.get());
		}
		if (!children || !RemoveChildren(path, *children, depth + 1)) {
			return false;
		}
	}
	return DeleteByHandle(handle.get(), info.dwFileAttributes);
}

bool RemoveChildren(
		const std::wstring &path,
		const std::vector<Entry> &entries,
		int depth) {
	auto complete = true;
	for (const auto &entry : entries) {
		if (!RemoveEntry(path + L'\\' + entry.name, entry, depth)) {
			complete = false;
		}
	}
	return complete;
}

[[nodiscard]] DWORD RenameToStaging(
		HANDLE handle,
		const std::wstring &parent,
		Target &target) {
	for (auto i = 0; i != kRenameAttempts; ++i) {
		auto name = StagingName(target.appName);
		if (name == target.name) {
			continue;
		}
		const auto path = ChildPath(parent, name);
		const auto length = path.size() * sizeof(wchar_t);
		auto buffer = std::vector<std::uint64_t>(1
			+ (sizeof(FILE_RENAME_INFO) + length) / sizeof(std::uint64_t));
		const auto info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
		info->FileNameLength = DWORD(length);
		std::memcpy(info->FileName, path.data(), length);
		if (SetFileInformationByHandle(
				handle,
				FileRenameInfo,
				info,
				DWORD(buffer.size() * sizeof(std::uint64_t)))) {
			target.name = std::move(name);
			return ERROR_SUCCESS;
		}
		const auto error = GetLastError();
		if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
			return error;
		}
	}
	return ERROR_ALREADY_EXISTS;
}

[[nodiscard]] bool Matches(
		HANDLE claim,
		const BY_HANDLE_FILE_INFORMATION &info,
		const Target &target) {
	if (!IsPlainDirectory(info.dwFileAttributes)) {
		return false;
	} else if (target.reference) {
		return SameObject(claim, target.reference->get());
	}
	return (info.dwVolumeSerialNumber == target.volume)
		&& (FileId(info) == target.id);
}

[[nodiscard]] bool IsDeletePending(const Target &target) {
	auto info = FILE_STANDARD_INFO();
	return target.reference
		&& GetFileInformationByHandleEx(
			target.reference->get(),
			FileStandardInfo,
			&info,
			sizeof(info))
		&& info.DeletePending;
}

[[nodiscard]] Outcome KeepClaimed(
		HANDLE claim,
		const std::wstring &path,
		Target &target) {
	if (!target.reference) {
		auto reference = OpenNoFollow(path, FILE_READ_ATTRIBUTES, kShareAll);
		if (!reference || !SameObject(reference.get(), claim)) {
			return Outcome::Failed;
		}
		target.reference = std::make_shared<winrt::file_handle>(
			std::move(reference));
	}
	return Outcome::Unfinished;
}

[[nodiscard]] Outcome ClaimAndRemove(
		const std::wstring &parent,
		Target &target,
		bool legacy) {
	const auto claim = OpenNoFollow(
		ChildPath(parent, target.name),
		kDirectoryAccess);
	if (!claim) {
		const auto error = GetLastError();
		return (IsGone(error) || IsDeletePending(target))
			? Outcome::Skipped
			: (error == ERROR_SHARING_VIOLATION)
			? Outcome::Busy
			: Outcome::Failed;
	}
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!GetFileInformationByHandle(claim.get(), &info)) {
		return Outcome::Failed;
	} else if (!Matches(claim.get(), info, target)) {
		return Outcome::Skipped;
	}
	if (legacy) {
		const auto entries = ListDirectory(claim.get());
		const auto staging = (Classify(target.name, target.appName)
			== NameKind::Staging);
		if (!entries) {
			return Outcome::Failed;
		} else if (!EngineShape(*entries, staging)) {
			return Outcome::Skipped;
		}
	}
	// WHY: a directory cannot be renamed while a file below it is open, so
	// only a rename proves no engine still uses this tree, and the fresh
	// name keeps everyone else from reopening it by the old path.
	if (const auto error = RenameToStaging(claim.get(), parent, target)) {
		return (error == ERROR_ACCESS_DENIED
			|| error == ERROR_SHARING_VIOLATION)
			? Outcome::Busy
			: Outcome::Failed;
	}
	const auto path = ChildPath(parent, target.name);
	const auto entries = ListDirectory(claim.get());
	const auto removed = (FinalPath(claim.get()) == path)
		&& entries
		&& (!legacy || EngineShape(*entries, true))
		&& RemoveChildren(path, *entries, 1)
		&& DeleteByHandle(claim.get(), info.dwFileAttributes);
	return removed ? Outcome::Removed : KeepClaimed(claim.get(), path, target);
}

[[nodiscard]] Summary SweepOnce(const Sweep &sweep) {
	const auto parent = OpenParent(sweep.parent);
	if (!parent.path.empty() && !LocalVolume(parent.path)) {
		return {};
	}
	auto entries = std::optional<std::vector<Entry>>();
	if (!parent.path.empty()) {
		entries = ListDirectory(parent.handle.get());
	}
	if (!entries) {
		return { .failed = 1, .retry = true };
	}
	auto now = FILETIME();
	GetSystemTimeAsFileTime(&now);
	const auto young = Ticks(now) - kMinimalAge;
	auto result = Summary();
	for (const auto &entry : *entries) {
		if (Classify(entry.name, sweep.appName) == NameKind::None
			|| !IsPlainDirectory(entry.attributes)
			|| entry.created >= sweep.started) {
			continue;
		} else if (entry.created > young) {
			result.retry = true;
			continue;
		}
		auto target = Target{
			.name = entry.name,
			.appName = sweep.appName,
			.id = entry.id,
			.volume = parent.volume,
		};
		switch (ClaimAndRemove(parent.path, target, true)) {
		case Outcome::Removed: ++result.removed; break;
		case Outcome::Busy: ++result.busy; break;
		case Outcome::Failed: ++result.failed; break;
		case Outcome::Skipped: break;
		case Outcome::Unfinished:
			result.claimed.push_back(std::move(target));
			break;
		}
	}
	result.retry = result.retry || result.busy || result.failed;
	return result;
}

[[nodiscard]] bool RemoveRetired(Retired &retired) {
	const auto parent = OpenParent(retired.parent);
	const auto outcome = parent.path.empty()
		? Outcome::Failed
		: ClaimAndRemove(parent.path, retired.target, retired.legacy);
	return (outcome == Outcome::Removed) || (outcome == Outcome::Skipped);
}

[[nodiscard]] crl::time RetryDelay(int attempt) {
	if (attempt < kFastRetries) {
		return kFastRetryDelay;
	}
	const auto shift = std::min(attempt - kFastRetries + 1, 5);
	return std::min(kFastRetryDelay << shift, kMaxRetryDelay);
}

void ScheduleRemoval(Retired retired) {
	const auto delay = RetryDelay(retired.attempt++);
	base::call_delayed(delay, [retired = std::move(retired)]() mutable {
		crl::async([retired = std::move(retired)]() mutable {
			if (!RemoveRetired(retired)) {
				crl::on_main([retired = std::move(retired)]() mutable {
					ScheduleRemoval(std::move(retired));
				});
			}
		});
	});
}

void ScheduleSweep(Sweep sweep, crl::time delay, crl::time next) {
	base::call_delayed(delay, [=] {
		crl::async([=] {
			auto summary = SweepOnce(sweep);
			crl::on_main([=, summary = std::move(summary)]() mutable {
				const auto unfinished = int(summary.claimed.size());
				if (summary.removed
					|| summary.busy
					|| summary.failed
					|| unfinished) {
					LOG(("WebView Info: Temporary storage cleanup, "
						"removed: %1, busy: %2, failed: %3, unfinished: %4."
						).arg(summary.removed
						).arg(summary.busy
						).arg(summary.failed
						).arg(unfinished));
				}
				for (auto &target : summary.claimed) {
					ScheduleRemoval({
						.parent = sweep.parent,
						.target = std::move(target),
						.legacy = true,
					});
				}
				if (summary.retry) {
					ScheduleSweep(
						sweep,
						next,
						std::min(next * 2, kMaxSweepDelay));
				}
			});
		});
	});
}

} // namespace

bool DeferTemporaryStorageRemoval(const QString &path) {
	const auto native = QDir::toNativeSeparators(path);
	const auto separator = native.lastIndexOf(QChar('\\'));
	if (separator < 0) {
		return false;
	}
	auto retired = Retired{
		.parent = ToExtended(native.left(separator)),
		.target = {
			.name = native.mid(separator + 1).toStdWString(),
			.appName = AppName(),
		},
	};
	auto &target = retired.target;
	const auto folder = ChildPath(retired.parent, target.name);
	if (!QCoreApplication::instance()
		|| retired.parent.empty()
		|| target.appName.empty()
		|| Classify(target.name, target.appName) != NameKind::Plain) {
		return false;
	}
	auto reference = OpenReference(folder);
	const auto entries = reference
		? ListSame(folder, reference.get())
		: std::nullopt;
	if (!entries) {
		return false;
	} else if (entries->empty()) {
		return RemoveEmpty(folder, reference.get());
	} else if (!EngineShape(*entries, false)) {
		return false;
	}
	target.reference = std::make_shared<winrt::file_handle>(
		std::move(reference));
	ScheduleRemoval(std::move(retired));
	return true;
}

void ClearStaleTemporaryStorage() {
	auto times = std::array<FILETIME, 4>();
	auto sweep = Sweep{
		.parent = ToExtended(QDir::tempPath()),
		.appName = AppName(),
	};
	if (sweep.parent.empty()
		|| sweep.appName.empty()
		|| !GetProcessTimes(
			GetCurrentProcess(),
			&times[0],
			&times[1],
			&times[2],
			&times[3])) {
		return;
	}
	sweep.started = Ticks(times[0]);
	ScheduleSweep(std::move(sweep), kFirstSweepDelay, kSecondSweepDelay);
}

} // namespace Webview
