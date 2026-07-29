#include "Cemu/CemuEmbed.h"

#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Common/CemuRuntime.h"
#include "interface/WindowSystem.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

void CemuCommonInit(bool embedded);

struct CemuEmbedInstance;
namespace {
std::mutex s_instanceMutex;
CemuEmbedInstance* s_instance = nullptr;
bool s_instanceWasCreated = false;
void SetState(CemuEmbedInstance* instance, CemuEmbedState state);
void ReportError(CemuEmbedInstance* instance, CemuEmbedResult result, const char* message);

class EmbedLoggingCallbacks final : public LoggingCallbacks {
public:
	explicit EmbedLoggingCallbacks(const CemuEmbedCallbacks& callbacks) : m_callbacks(callbacks) {}
	void Log(std::string_view category, std::string_view message) override {
		if (!m_callbacks.log) return;
		std::string categoryCopy(category);
		std::string messageCopy(message);
		m_callbacks.log(m_callbacks.user_data, categoryCopy.c_str(), messageCopy.c_str());
	}
private:
	CemuEmbedCallbacks m_callbacks{};
};
}

struct CemuEmbedInstance {
	std::string executablePath;
	std::string userDataPath;
	std::string configPath;
	std::string cachePath;
	std::string dataPath;
	CemuEmbedConfig config{};
	CemuEmbedCallbacks callbacks{};
	EmbedLoggingCallbacks loggingCallbacks;
	std::atomic<CemuEmbedState> state{CEMU_EMBED_STATE_CREATED};
	std::thread initializationThread;
	std::atomic_bool loggingCallbacksInstalled{false};
	std::atomic_bool initialized{false};
	std::atomic_bool surfaceConfigured{false};
	CemuEmbedInstance(const CemuEmbedConfig& config, const CemuEmbedCallbacks& callbacks)
		: executablePath(config.executable_path_utf8), userDataPath(config.user_data_path_utf8), configPath(config.config_path_utf8), cachePath(config.cache_path_utf8), dataPath(config.data_path_utf8), config(config), callbacks(callbacks), loggingCallbacks(callbacks) {
		this->config.executable_path_utf8 = executablePath.c_str();
		this->config.user_data_path_utf8 = userDataPath.c_str();
		this->config.config_path_utf8 = configPath.c_str();
		this->config.cache_path_utf8 = cachePath.c_str();
		this->config.data_path_utf8 = dataPath.c_str();
	}
};

namespace {
void SetState(CemuEmbedInstance* instance, CemuEmbedState state) {
	instance->state.store(state, std::memory_order_release);
	if (instance->callbacks.state_changed) instance->callbacks.state_changed(instance->callbacks.user_data, state);
}
void ReportError(CemuEmbedInstance* instance, CemuEmbedResult result, const char* message) {
	if (instance->callbacks.error) instance->callbacks.error(instance->callbacks.user_data, result, message);
}
bool HasRequiredConfig(const CemuEmbedConfig* config) {
	return config && config->struct_size >= sizeof(CemuEmbedConfig) && config->abi_version == CEMU_EMBED_ABI_VERSION &&
		config->executable_path_utf8 && config->user_data_path_utf8 && config->config_path_utf8 && config->cache_path_utf8 && config->data_path_utf8;
}

bool HasRequiredBrokeredStorage(const CemuEmbedBrokeredStorage* storage) {
	return storage && storage->struct_size >= sizeof(CemuEmbedBrokeredStorage) &&
		storage->abi_version == CEMU_EMBED_BROKERED_STORAGE_VERSION &&
		storage->enumerate_recursive && storage->open_read && storage->read && storage->close;
}

struct BrokeredCopyContext {
	const CemuEmbedBrokeredStorage& storage;
	fs::path destination;
	uint64_t totalBytes{};
	uint64_t copiedBytes{};
	uint64_t lastReportedBytes{};
	std::string error;
};

struct BrokeredScanContext {
	uint64_t totalBytes{};
	uint64_t entryCount{};
	std::string error;
};

bool MakeSafeRelativePath(const char* input, fs::path& output) {
	if (!input || !*input)
		return false;
	const fs::path path = _utf8ToPath(input);
	if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
		return false;
	for (const auto& component : path) {
		if (component == "." || component == "..")
			return false;
	}
	output = path.lexically_normal();
	return true;
}

bool ReadSmallTextFile(const fs::path& path, std::string& text) {
	std::error_code error;
	const auto size = fs::file_size(path, error);
	if (error || size > 1024 * 1024)
		return false;
	std::ifstream input(path, std::ios::binary);
	if (!input)
		return false;
	text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	return input.good() || input.eof();
}

bool FindXmlElementValue(const std::string& xml, std::string_view element,
	size_t& valueBegin, size_t& valueEnd) {
	const std::string opening = "<" + std::string(element);
	const auto elementBegin = xml.find(opening);
	if (elementBegin == std::string::npos)
		return false;
	valueBegin = xml.find('>', elementBegin + opening.size());
	if (valueBegin == std::string::npos)
		return false;
	++valueBegin;
	const std::string closing = "</" + std::string(element) + ">";
	valueEnd = xml.find(closing, valueBegin);
	return valueEnd != std::string::npos && valueEnd > valueBegin;
}

bool ParseTitleId(std::string_view text, uint64_t& titleId) {
	if (text.size() != 16)
		return false;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), titleId, 16);
	return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

CemuEmbedResult NormalizeMergedTitleMetadata(const fs::path& stagedPath, std::string& errorMessage) {
	const auto appPath = stagedPath / "code" / "app.xml";
	const auto metaPath = stagedPath / "meta" / "meta.xml";
	std::string appXml;
	std::string metaXml;
	if (!ReadSmallTextFile(appPath, appXml) || !ReadSmallTextFile(metaPath, metaXml))
		return CEMU_EMBED_OK;

	size_t appBegin{};
	size_t appEnd{};
	size_t metaBegin{};
	size_t metaEnd{};
	if (!FindXmlElementValue(appXml, "title_id", appBegin, appEnd) ||
		!FindXmlElementValue(metaXml, "title_id", metaBegin, metaEnd))
		return CEMU_EMBED_OK;

	uint64_t appTitleId{};
	uint64_t metaTitleId{};
	if (!ParseTitleId(std::string_view(appXml).substr(appBegin, appEnd - appBegin), appTitleId) ||
		!ParseTitleId(std::string_view(metaXml).substr(metaBegin, metaEnd - metaBegin), metaTitleId))
		return CEMU_EMBED_OK;

	const bool isMergedBaseWithUpdateApp =
		(appTitleId >> 32) == 0x0005000Eull &&
		(metaTitleId >> 32) == 0x00050000ull &&
		static_cast<uint32_t>(appTitleId) == static_cast<uint32_t>(metaTitleId) &&
		fs::is_directory(stagedPath / "content");
	if (!isMergedBaseWithUpdateApp)
		return CEMU_EMBED_OK;

	const auto normalizedTitleId = fmt::format("{:016X}", metaTitleId);
	appXml.replace(appBegin, appEnd - appBegin, normalizedTitleId);
	std::ofstream output(appPath, std::ios::binary | std::ios::trunc);
	if (!output) {
		errorMessage = "Cemu could not normalize the staged title metadata.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	output.write(appXml.data(), static_cast<std::streamsize>(appXml.size()));
	if (!output) {
		errorMessage = "Cemu could not write the normalized staged title metadata.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	cemuLog_log(LogType::Force,
		"Brokered title has merged base metadata with an update app.xml; normalized staged title id from {:016x} to {:016x}",
		appTitleId, metaTitleId);
	return CEMU_EMBED_OK;
}

CemuEmbedResult CEMU_EMBED_CALL ScanBrokeredEntry(void* userData, const char* relativePathUtf8,
	CemuEmbedBrokeredEntryType type, uint64_t size, void* fileHandle) {
	auto& context = *static_cast<BrokeredScanContext*>(userData);
	fs::path relativePath;
	if (!MakeSafeRelativePath(relativePathUtf8, relativePath)) {
		context.error = "The broker supplied an invalid relative path.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (type != CEMU_EMBED_BROKERED_DIRECTORY && (type != CEMU_EMBED_BROKERED_FILE || !fileHandle)) {
		context.error = "The broker supplied an invalid entry.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (++context.entryCount > 1000000) {
		context.error = "The selected folder contains too many entries.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (type == CEMU_EMBED_BROKERED_FILE) {
		if (size > std::numeric_limits<uint64_t>::max() - context.totalBytes) {
			context.error = "The selected folder size exceeds the supported 64-bit range.";
			return CEMU_EMBED_STORAGE_FAILED;
		}
		context.totalBytes += size;
	}
	return CEMU_EMBED_OK;
}

CemuEmbedResult CEMU_EMBED_CALL CopyBrokeredEntry(void* userData, const char* relativePathUtf8,
	CemuEmbedBrokeredEntryType type, uint64_t size, void* fileHandle) {
	auto& context = *static_cast<BrokeredCopyContext*>(userData);
	fs::path relativePath;
	if (!MakeSafeRelativePath(relativePathUtf8, relativePath)) {
		context.error = "The broker supplied an invalid relative path.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	const fs::path destination = context.destination / relativePath;
	std::error_code error;
	if (type == CEMU_EMBED_BROKERED_DIRECTORY) {
		fs::create_directories(destination, error);
		if (error) {
			context.error = "Cemu could not create a brokered-title directory.";
			return CEMU_EMBED_STORAGE_FAILED;
		}
		return CEMU_EMBED_OK;
	}
	if (type != CEMU_EMBED_BROKERED_FILE || !fileHandle) {
		context.error = "The broker supplied an invalid file entry.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	fs::create_directories(destination.parent_path(), error);
	if (error) {
		context.error = "Cemu could not create a brokered-title parent directory.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	void* stream = nullptr;
	if (context.storage.open_read(context.storage.user_data, fileHandle, &stream) != CEMU_EMBED_OK || !stream) {
		context.error = "The broker could not open a title file for reading.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	std::ofstream output(destination, std::ios::binary | std::ios::trunc);
	if (!output) {
		context.storage.close(context.storage.user_data, stream);
		context.error = "Cemu could not create a staged title file.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	// Broker callbacks commonly run on a thread-pool worker whose stack can be
	// close to 1 MiB. Keep the transfer buffer on the heap so merely entering
	// this callback cannot exhaust that stack (including directory entries).
	// Four MiB keeps callback overhead low for multi-gigabyte titles while the
	// heap allocation keeps thread-pool stack usage small.
	std::vector<uint8_t> buffer(4 * 1024 * 1024);
	uint64_t offset = 0;
	while (offset < size) {
		const uint32_t requested = static_cast<uint32_t>(std::min<uint64_t>(buffer.size(), size - offset));
		uint32_t received = 0;
		if (context.storage.read(context.storage.user_data, stream, offset, buffer.data(), requested, &received) != CEMU_EMBED_OK || received == 0 || received > requested) {
			context.storage.close(context.storage.user_data, stream);
			context.error = "The broker returned an incomplete title stream.";
			return CEMU_EMBED_STORAGE_FAILED;
		}
		output.write(reinterpret_cast<const char*>(buffer.data()), received);
		if (!output) {
			context.storage.close(context.storage.user_data, stream);
			context.error = "Cemu could not write a staged title file.";
			return CEMU_EMBED_STORAGE_FAILED;
		}
		offset += received;
		context.copiedBytes += received;
		if (context.storage.progress &&
			(context.copiedBytes == context.totalBytes ||
				context.copiedBytes - context.lastReportedBytes >= 64ull * 1024 * 1024)) {
			context.lastReportedBytes = context.copiedBytes;
			context.storage.progress(context.storage.user_data, context.copiedBytes,
				context.totalBytes, relativePathUtf8);
		}
	}
	context.storage.close(context.storage.user_data, stream);
	if (offset != size) {
		context.error = "The brokered title file size changed while it was being copied.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	return CEMU_EMBED_OK;
}

CemuEmbedResult StageBrokeredFolder(CemuEmbedInstance* instance, void* folderHandle,
	const CemuEmbedBrokeredStorage& storage, fs::path& stagedPath) {
	stagedPath = _utf8ToPath(instance->cachePath) / "brokered-titles" / "current";
	std::error_code error;
	fs::remove_all(stagedPath, error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED, "Cemu could not clear the previous brokered-title cache.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	BrokeredScanContext scan;
	if (storage.enumerate_recursive(storage.user_data, folderHandle, ScanBrokeredEntry, &scan) != CEMU_EMBED_OK) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			scan.error.empty() ? "The broker could not scan the selected title folder." : scan.error.c_str());
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (scan.totalBytes == 0) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED, "The selected title folder contains no file data.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	const auto cacheRoot = stagedPath.parent_path();
	fs::create_directories(cacheRoot, error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED, "Cemu could not create its brokered-title cache root.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	const auto diskSpace = fs::space(cacheRoot, error);
	if (!error) {
		const uint64_t reserve = std::min<uint64_t>(scan.totalBytes / 20, 1024ull * 1024 * 1024);
		if (diskSpace.available < scan.totalBytes ||
			diskSpace.available - scan.totalBytes < reserve) {
			ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
				"There is not enough free space to stage the selected title.");
			return CEMU_EMBED_STORAGE_FAILED;
		}
	}
	fs::create_directories(stagedPath, error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED, "Cemu could not create its brokered-title cache.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	BrokeredCopyContext context{storage, stagedPath, scan.totalBytes};
	if (storage.progress)
		storage.progress(storage.user_data, 0, scan.totalBytes, "");
	if (storage.enumerate_recursive(storage.user_data, folderHandle, CopyBrokeredEntry, &context) != CEMU_EMBED_OK) {
		fs::remove_all(stagedPath, error);
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED, context.error.empty() ? "The broker could not enumerate the selected title folder." : context.error.c_str());
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (context.copiedBytes != scan.totalBytes) {
		fs::remove_all(stagedPath, error);
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"The selected title changed while it was being staged.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	std::string normalizationError;
	const auto normalizationResult = NormalizeMergedTitleMetadata(stagedPath, normalizationError);
	if (normalizationResult != CEMU_EMBED_OK) {
		ReportError(instance, normalizationResult, normalizationError.c_str());
		return normalizationResult;
	}
	return CEMU_EMBED_OK;
}

CemuEmbedResult LaunchGameFromPath(CemuEmbedInstance* instance, const fs::path& gamePath) {
	TitleInfo title{ gamePath };
	if (!title.IsValid()) {
		ReportError(instance, CEMU_EMBED_LAUNCH_FAILED, "The selected folder is not a valid Wii U title directory.");
		return CEMU_EMBED_LAUNCH_FAILED;
	}

	CafeTitleList::AddTitleFromPath(gamePath);
	TitleId baseTitleId;
	CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), baseTitleId);
	auto prepareResult = CafeSystem::PrepareForegroundTitle(baseTitleId);

	// Some extracted/merged titles keep an update (0005000E) app.xml even
	// though the selected directory contains a directly runnable code/content
	// tree. If no base title is available, use Cemu's established standalone
	// RPX path instead of rejecting that otherwise usable folder.
	if (prepareResult != CafeSystem::PREPARE_STATUS_CODE::SUCCESS &&
		title.GetTitleType() == TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE &&
		title.GetFormat() == TitleInfo::TitleDataFormat::HOST_FS &&
		title.ParseXmlInfo()) {
		fs::path executable;
		const auto argString = title.GetArgStr();
		const auto rpxEnd = argString.find(".rpx");
		if (rpxEnd != std::string::npos) {
			auto executableName = argString.substr(0, rpxEnd + 4);
			while (!executableName.empty() &&
				(executableName.front() == ' ' || executableName.front() == '\t' || executableName.front() == '"'))
				executableName.erase(executableName.begin());
			executable = gamePath / "code" / _utf8ToPath(executableName);
		}
		std::error_code executableError;
		if (!executable.empty() && fs::is_regular_file(executable, executableError)) {
			cemuLog_log(LogType::Force,
				"Selected title uses update title id {:016x} without a discoverable base; launching {} in standalone mode",
				title.GetAppTitleId(), _pathToUtf8(executable));
			prepareResult = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(executable);
		}
	}

	if (prepareResult != CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
		if (title.GetTitleType() == TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE)
			ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
				"The selected folder is an update title (0005000E). Its base title was not found and its RPX could not be launched standalone.");
		else
			ReportError(instance, CEMU_EMBED_LAUNCH_FAILED, "Cemu could not mount or prepare the selected title.");
		return CEMU_EMBED_LAUNCH_FAILED;
	}
	CafeSystem::LaunchForegroundTitle();
	return CEMU_EMBED_OK;
}
void Initialize(CemuEmbedInstance* instance) {
	try {
		std::set<fs::path> failedWriteAccess;
		ActiveSettings::SetPaths(false, _utf8ToPath(instance->config.executable_path_utf8), _utf8ToPath(instance->config.user_data_path_utf8), _utf8ToPath(instance->config.config_path_utf8), _utf8ToPath(instance->config.cache_path_utf8), _utf8ToPath(instance->config.data_path_utf8), failedWriteAccess);
		if (!failedWriteAccess.empty()) {
			ReportError(instance, CEMU_EMBED_INITIALIZATION_FAILED, "Cemu could not write to one or more host-provided paths.");
			SetState(instance, CEMU_EMBED_STATE_FAILED);
			return;
		}
		GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
		NetworkConfig::LoadOnce();
		ActiveSettings::Init();
		cemuLog_setCallbacks(&instance->loggingCallbacks);
		instance->loggingCallbacksInstalled.store(true, std::memory_order_release);
		cemuLog_createLogFile(false);
		CemuCommonInit(true);
		instance->initialized.store(true, std::memory_order_release);
		SetState(instance, instance->state.load(std::memory_order_acquire) == CEMU_EMBED_STATE_STOPPING ? CEMU_EMBED_STATE_STOPPED : CEMU_EMBED_STATE_READY);
	} catch (const std::exception& exception) {
		ReportError(instance, CEMU_EMBED_INITIALIZATION_FAILED, exception.what());
		SetState(instance, CEMU_EMBED_STATE_FAILED);
	} catch (...) {
		ReportError(instance, CEMU_EMBED_INITIALIZATION_FAILED, "Cemu initialization failed with an unknown error.");
		SetState(instance, CEMU_EMBED_STATE_FAILED);
	}
}
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_Create(const CemuEmbedConfig* config, const CemuEmbedCallbacks* callbacks, CemuEmbedInstance** instance) {
	if (!instance || !HasRequiredConfig(config) || !callbacks || callbacks->struct_size < sizeof(CemuEmbedCallbacks)) return CEMU_EMBED_INVALID_ARGUMENT;
	std::lock_guard lock(s_instanceMutex);
	if (s_instance || s_instanceWasCreated) return CEMU_EMBED_BUSY;
	*instance = new CemuEmbedInstance(*config, *callbacks);
	s_instance = *instance;
	s_instanceWasCreated = true;
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetSurface(CemuEmbedInstance* instance, const CemuEmbedSurface* surface) {
	if (!instance || !surface || surface->struct_size < sizeof(CemuEmbedSurface) || !surface->window) return CEMU_EMBED_INVALID_ARGUMENT;
	const auto state = instance->state.load(std::memory_order_acquire);
	if (state == CEMU_EMBED_STATE_STOPPING || state == CEMU_EMBED_STATE_STOPPED || state == CEMU_EMBED_STATE_FAILED)
		return CEMU_EMBED_INVALID_STATE;
	if (state == CEMU_EMBED_STATE_CREATED)
		WindowSystem::SetEmbeddedSurface(surface->window, surface->canvas, surface->width, surface->height, surface->dpi_scale);
	else
		WindowSystem::ResizeEmbeddedSurface(surface->width, surface->height, surface->dpi_scale);
	instance->surfaceConfigured.store(true, std::memory_order_release);
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_InitializeAsync(CemuEmbedInstance* instance) {
	if (!instance) return CEMU_EMBED_INVALID_ARGUMENT;
	if (!instance->surfaceConfigured.load(std::memory_order_acquire)) return CEMU_EMBED_INVALID_STATE;
	CemuEmbedState expected = CEMU_EMBED_STATE_CREATED;
	if (!instance->state.compare_exchange_strong(expected, CEMU_EMBED_STATE_INITIALIZING)) return CEMU_EMBED_INVALID_STATE;
	SetState(instance, CEMU_EMBED_STATE_INITIALIZING);
	instance->initializationThread = std::thread(Initialize, instance);
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_LaunchGame(CemuEmbedInstance* instance, const char* game_path_utf8) {
	if (!instance || !game_path_utf8 || !*game_path_utf8) return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY) {
		ReportError(instance, CEMU_EMBED_INVALID_STATE, "Cemu is not ready to launch a title yet.");
		return CEMU_EMBED_INVALID_STATE;
	}
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY, "A title is already running.");
		return CEMU_EMBED_BUSY;
	}

	return LaunchGameFromPath(instance, _utf8ToPath(game_path_utf8));
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_LaunchGameFromBrokeredFolder(
	CemuEmbedInstance* instance, void* folderHandle, const CemuEmbedBrokeredStorage* storage) {
	if (!instance || !folderHandle || !HasRequiredBrokeredStorage(storage)) return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY) {
		ReportError(instance, CEMU_EMBED_INVALID_STATE, "Cemu is not ready to stage or launch a title yet.");
		return CEMU_EMBED_INVALID_STATE;
	}
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY, "A title is already running.");
		return CEMU_EMBED_BUSY;
	}
	fs::path stagedPath;
	const auto stageResult = StageBrokeredFolder(instance, folderHandle, *storage, stagedPath);
	return stageResult == CEMU_EMBED_OK ? LaunchGameFromPath(instance, stagedPath) : stageResult;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_Pump(CemuEmbedInstance* instance) {
	if (!instance) return CEMU_EMBED_INVALID_ARGUMENT;
	if (CemuRuntime::HasFatalError()) {
		const auto message = CemuRuntime::GetFatalError();
		ReportError(instance, CEMU_EMBED_INITIALIZATION_FAILED, message.c_str());
		SetState(instance, CEMU_EMBED_STATE_FAILED);
		CemuRuntime::ClearFatalError();
		return CEMU_EMBED_INITIALIZATION_FAILED;
	}
	return instance->state.load(std::memory_order_acquire) == CEMU_EMBED_STATE_FAILED ? CEMU_EMBED_INITIALIZATION_FAILED : CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_RequestStop(CemuEmbedInstance* instance) {
	if (!instance) return CEMU_EMBED_INVALID_ARGUMENT;
	const auto previous = instance->state.exchange(CEMU_EMBED_STATE_STOPPING, std::memory_order_acq_rel);
	if (previous == CEMU_EMBED_STATE_STOPPED || previous == CEMU_EMBED_STATE_FAILED) return CEMU_EMBED_INVALID_STATE;
	SetState(instance, CEMU_EMBED_STATE_STOPPING);
	if (previous == CEMU_EMBED_STATE_CREATED) SetState(instance, CEMU_EMBED_STATE_STOPPED);
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedState CEMU_EMBED_CALL CemuEmbed_GetState(const CemuEmbedInstance* instance) {
	return instance ? instance->state.load(std::memory_order_acquire) : CEMU_EMBED_STATE_FAILED;
}
extern "C" void CEMU_EMBED_CALL CemuEmbed_Destroy(CemuEmbedInstance* instance) {
	if (!instance) return;
	CemuEmbed_RequestStop(instance);
	if (instance->initializationThread.joinable()) instance->initializationThread.join();
	if (instance->state.load(std::memory_order_acquire) == CEMU_EMBED_STATE_STOPPING) SetState(instance, CEMU_EMBED_STATE_STOPPED);
	if (instance->initialized.load(std::memory_order_acquire)) CafeSystem::Shutdown();
	{ std::lock_guard lock(s_instanceMutex); if (s_instance == instance) s_instance = nullptr; }
	if (instance->loggingCallbacksInstalled.load(std::memory_order_acquire)) cemuLog_clearCallbacks();
	delete instance;
}
