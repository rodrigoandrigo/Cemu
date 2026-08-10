#include "Cemu/CemuEmbed.h"

#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Account/Account.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/TitleList/TitleList.h"
#include "Common/CemuRuntime.h"
#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "input/api/Controller.h"
#include "input/api/UWP/UWPGamepadController.h"
#include "interface/WindowSystem.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <boost/nowide/convert.hpp>
#include <fmt/format.h>
#ifdef HAS_SDL
#include <SDL3/SDL_error.h>
#endif

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

std::pair<uint32_t, uint32_t> CountGraphicPacksForTitle(uint64_t titleId) {
	uint32_t compatible{};
	uint32_t enabled{};
	for (const auto& pack : GraphicPack2::GetGraphicPacks()) {
		if (!pack->ContainsTitleId(titleId))
			continue;
		++compatible;
		if (pack->IsEnabled())
			++enabled;
	}
	return {compatible, enabled};
}

void SaveGraphicPackState(const GraphicPackPtr& pack) {
	auto& entries = GetConfigHandle().data().graphic_pack_entries;
	const auto path = _utf8ToPath(pack->GetNormalizedPathString());
	if (pack->IsEnabled()) {
		auto& entry = entries[path];
		entry.clear();
		for (const auto& preset : pack->GetActivePresets())
			entry.try_emplace(preset->category, preset->name);
	} else if (pack->IsDefaultEnabled()) {
		auto& entry = entries[path];
		entry.clear();
		entry.try_emplace("_disabled", "true");
	} else {
		entries.erase(path);
	}
}

struct BrokeredCopyContext {
	const CemuEmbedBrokeredStorage& storage;
	fs::path destination;
	uint64_t totalBytes{};
	size_t transferBufferSize{1024 * 1024};
	uint64_t copiedBytes{};
	uint64_t lastReportedBytes{};
	std::string error;
	std::vector<uint8_t> transferBuffer;
	bool usedDirectCopy{};
	bool usedBufferedCopy{};
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

uint64_t StableGraphicPackId(const fs::path& rulesPath, std::string_view identity = {}) {
	std::string rules;
	if (!ReadSmallTextFile(rulesPath, rules))
		rules = _pathToUtf8(rulesPath.filename());
	uint64_t hash = 1469598103934665603ull;
	for (const unsigned char value : rules) {
		hash ^= value;
		hash *= 1099511628211ull;
	}
	for (const unsigned char value : identity) {
		hash ^= value;
		hash *= 1099511628211ull;
	}
	return hash;
}

bool SamePath(const fs::path& left, const fs::path& right) {
	std::error_code error;
	const bool equivalent = fs::equivalent(left, right, error);
	if (!error)
		return equivalent;
#ifdef _WIN32
	auto leftText = _pathToUtf8(left.lexically_normal());
	auto rightText = _pathToUtf8(right.lexically_normal());
	std::transform(leftText.begin(), leftText.end(), leftText.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	std::transform(rightText.begin(), rightText.end(), rightText.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	return leftText == rightText;
#else
	return left.lexically_normal() == right.lexically_normal();
#endif
}

bool CopyFileInChunks(const fs::path& source, const fs::path& destination,
	std::error_code& error) {
	error.clear();
	std::ifstream input(source, std::ios::binary);
	if (!input) {
		error = std::make_error_code(std::errc::io_error);
		return false;
	}
	std::ofstream output(destination, std::ios::binary | std::ios::trunc);
	if (!output) {
		error = std::make_error_code(std::errc::io_error);
		return false;
	}

	// Keep peak memory bounded and avoid the all-at-once copy path used by
	// std::filesystem::copy_file. This is important for UWP when importing the
	// full community graphic-pack repository.
	std::vector<char> buffer(1024 * 1024);
	while (input) {
		input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const auto count = input.gcount();
		if (count > 0) {
			output.write(buffer.data(), count);
			if (!output) {
				error = std::make_error_code(std::errc::io_error);
				return false;
			}
		}
	}
	if (!input.eof()) {
		error = std::make_error_code(std::errc::io_error);
		return false;
	}
	output.flush();
	if (!output) {
		error = std::make_error_code(std::errc::io_error);
		return false;
	}
	return true;
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
	if (context.storage.copy_file &&
		context.storage.copy_file(context.storage.user_data, fileHandle,
			_pathToUtf8(destination).c_str()) == CEMU_EMBED_OK) {
		context.copiedBytes += size;
		context.usedDirectCopy = true;
		if (context.storage.progress &&
			(context.copiedBytes == context.totalBytes ||
				context.copiedBytes - context.lastReportedBytes >= 64ull * 1024 * 1024)) {
			context.lastReportedBytes = context.copiedBytes;
			context.storage.progress(context.storage.user_data, context.copiedBytes,
				context.totalBytes, relativePathUtf8);
		}
		return CEMU_EMBED_OK;
	}
	void* stream = nullptr;
	context.usedBufferedCopy = true;
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
	// The caller selects the transfer size: ordinary staging and graphic packs
	// retain the 1 MiB default, while installed titles use a larger sequential
	// block to reduce broker and stream overhead.
	auto& buffer = context.transferBuffer;
	if (buffer.size() != context.transferBufferSize)
		buffer.resize(context.transferBufferSize);
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
	const CemuEmbedBrokeredStorage& storage, std::string_view stagingName,
	bool normalizeMergedMetadata, fs::path& stagedPath, bool compactPath = false,
	size_t transferBufferSize = 1024 * 1024) {
	const fs::path cachePath = _utf8ToPath(instance->cachePath);
	stagedPath = compactPath
		? cachePath / stagingName
		: cachePath / "brokered-titles" / stagingName;
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
	BrokeredCopyContext context{storage, stagedPath, scan.totalBytes, transferBufferSize};
	if (storage.progress)
		storage.progress(storage.user_data, 0, scan.totalBytes, "");
	const auto copyStart = std::chrono::steady_clock::now();
	if (storage.enumerate_recursive(storage.user_data, folderHandle, CopyBrokeredEntry, &context) != CEMU_EMBED_OK) {
		fs::remove_all(stagedPath, error);
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED, context.error.empty() ? "The broker could not enumerate the selected title folder." : context.error.c_str());
		return CEMU_EMBED_STORAGE_FAILED;
	}
	const auto copySeconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - copyStart).count();
	const double copiedMiB = static_cast<double>(context.copiedBytes) / (1024.0 * 1024.0);
	const char* copyMode = context.usedDirectCopy
		? (context.usedBufferedCopy ? "mixed direct/buffered" : "platform direct")
		: "buffered fallback";
	if (context.copiedBytes != scan.totalBytes) {
		fs::remove_all(stagedPath, error);
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"The selected title changed while it was being staged.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	cemuLog_log(LogType::Force,
		"Brokered staging complete: {:.1f} MiB in {:.2f}s ({:.1f} MiB/s, {})",
		copiedMiB, copySeconds, copySeconds > 0.0 ? copiedMiB / copySeconds : 0.0,
		copyMode);
	if (normalizeMergedMetadata) {
		std::string normalizationError;
		const auto normalizationResult = NormalizeMergedTitleMetadata(stagedPath, normalizationError);
		if (normalizationResult != CEMU_EMBED_OK) {
			ReportError(instance, normalizationResult, normalizationError.c_str());
			return normalizationResult;
		}
	}
	return CEMU_EMBED_OK;
}

bool IsExpectedInstallType(TitleIdParser::TITLE_TYPE actual, CemuEmbedInstallType expected) {
	switch (expected) {
	case CEMU_EMBED_INSTALL_AUTO:
		return true;
	case CEMU_EMBED_INSTALL_BASE_GAME:
		return actual == TitleIdParser::TITLE_TYPE::BASE_TITLE ||
			actual == TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO;
	case CEMU_EMBED_INSTALL_UPDATE:
		return actual == TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE;
	case CEMU_EMBED_INSTALL_DLC:
		return actual == TitleIdParser::TITLE_TYPE::AOC;
	default:
		return false;
	}
}

const char* InstallTypeName(TitleIdParser::TITLE_TYPE type) {
	switch (type) {
	case TitleIdParser::TITLE_TYPE::BASE_TITLE:
	case TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO:
		return "base game";
	case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE:
		return "update";
	case TitleIdParser::TITLE_TYPE::AOC:
		return "DLC";
	default:
		return "unsupported title";
	}
}

std::string RegionName(CafeConsoleRegion region) {
	std::vector<std::string_view> names;
	if (HAS_FLAG(region, CafeConsoleRegion::JPN)) names.emplace_back("Japan");
	if (HAS_FLAG(region, CafeConsoleRegion::USA)) names.emplace_back("USA");
	if (HAS_FLAG(region, CafeConsoleRegion::EUR)) names.emplace_back("Europe");
	if (HAS_FLAG(region, CafeConsoleRegion::AUS_DEPR)) names.emplace_back("Australia");
	if (HAS_FLAG(region, CafeConsoleRegion::CHN)) names.emplace_back("China");
	if (HAS_FLAG(region, CafeConsoleRegion::KOR)) names.emplace_back("Korea");
	if (HAS_FLAG(region, CafeConsoleRegion::TWN)) names.emplace_back("Taiwan");
	if (names.empty())
		return "Unknown";
	std::string result;
	for (const auto name : names) {
		if (!result.empty()) result += ", ";
		result += name;
	}
	return result;
}

void RefreshInstalledTitles() {
	CafeTitleList::Refresh();
	while (CafeTitleList::IsScanning())
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

CemuEmbedResult InstallTitleFromStaging(CemuEmbedInstance* instance,
	const fs::path& stagedPath, CemuEmbedInstallType expectedType,
	uint64_t* installedBaseTitleId) {
	TitleInfo title{stagedPath};
	if (!title.IsValid() || !title.ParseXmlInfo() ||
		title.GetFormat() != TitleInfo::TitleDataFormat::HOST_FS) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"The selected folder is not an installable extracted Wii U title.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	const auto titleType = title.GetTitleType();
	if (titleType != TitleIdParser::TITLE_TYPE::BASE_TITLE &&
		titleType != TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO &&
		titleType != TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE &&
		titleType != TitleIdParser::TITLE_TYPE::AOC) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"The selected folder is not a base game, update or DLC.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (!IsExpectedInstallType(titleType, expectedType)) {
		const auto message = fmt::format(
			"The selected folder contains a {}, not the requested installation type.",
			InstallTypeName(titleType));
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED, message.c_str());
		return CEMU_EMBED_STORAGE_FAILED;
	}

	const TitleId titleId = title.GetAppTitleId();
	const TitleId baseTitleId = TitleIdParser::MakeBaseTitleId(
		titleType == TitleIdParser::TITLE_TYPE::AOC
			? (titleId & ~0xFF00000000ull)
			: titleId);
	const fs::path target = ActiveSettings::GetMlcPath(title.GetInstallPath());
	fs::path backup = target;
	backup += ".cemu-embed-backup";
	std::error_code error;
	fs::create_directories(target.parent_path(), error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not create the title installation directory.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	fs::remove_all(backup, error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not clear a previous installation backup.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (fs::exists(target, error)) {
		fs::rename(target, backup, error);
		if (error) {
			ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
				"Cemu could not back up the currently installed title.");
			return CEMU_EMBED_STORAGE_FAILED;
		}
	}

	fs::rename(stagedPath, target, error);
	if (error) {
		std::error_code restoreError;
		if (fs::exists(backup, restoreError))
			fs::rename(backup, target, restoreError);
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not commit the staged title to the MLC.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	fs::remove_all(backup, error);
	if (error)
		cemuLog_log(LogType::Force, "Unable to remove installation backup {}", _pathToUtf8(backup));
	cemuLog_log(LogType::Force, "Installed {} {:016x} v{} to {}",
		InstallTypeName(titleType), titleId, title.GetAppTitleVersion(), _pathToUtf8(target));
	RefreshInstalledTitles();
	if (installedBaseTitleId)
		*installedBaseTitleId = baseTitleId;
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
		// Embedded hosts own the session overlay state through the ABI. Start
		// hidden even if a desktop settings.xml enabled the overlay previously.
		LatteOverlay_init();
		LatteOverlay_setHostPerformanceMetrics(false);
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
	const auto stageResult = StageBrokeredFolder(instance, folderHandle, *storage,
		"current", true, stagedPath);
	return stageResult == CEMU_EMBED_OK ? LaunchGameFromPath(instance, stagedPath) : stageResult;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_InstallTitleFromBrokeredFolder(
	CemuEmbedInstance* instance, void* folderHandle,
	const CemuEmbedBrokeredStorage* storage, CemuEmbedInstallType expectedType,
	uint64_t* installedBaseTitleId) {
	if (!instance || !folderHandle || !HasRequiredBrokeredStorage(storage))
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (expectedType < CEMU_EMBED_INSTALL_AUTO || expectedType > CEMU_EMBED_INSTALL_DLC)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY) {
		ReportError(instance, CEMU_EMBED_INVALID_STATE,
			"Cemu is not ready to install a title yet.");
		return CEMU_EMBED_INVALID_STATE;
	}
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY,
			"Stop the running title before installing library content.");
		return CEMU_EMBED_BUSY;
	}

	fs::path stagedPath;
	const auto stageResult = StageBrokeredFolder(instance, folderHandle, *storage,
		// The UWP DataReader owns an intermediate buffer in addition to Cemu's
		// transfer buffer. Eight MiB caused long broker stalls and memory pressure
		// on Xbox. Four MiB keeps sequential throughput high while allowing the
		// storage service to complete each request promptly. The independent
		// 1 MiB graphic-pack import path is intentionally unchanged.
		"installing", false, stagedPath, false, 4 * 1024 * 1024);
	if (stageResult != CEMU_EMBED_OK)
		return stageResult;
	const auto installResult = InstallTitleFromStaging(instance, stagedPath,
		expectedType, installedBaseTitleId);
	if (installResult != CEMU_EMBED_OK) {
		std::error_code cleanupError;
		fs::remove_all(stagedPath, cleanupError);
	}
	return installResult;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_EnumerateInstalledTitles(
	CemuEmbedInstance* instance, CemuEmbedInstalledTitleCallback callback,
	void* userData) {
	if (!instance || !callback)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;

	RefreshInstalledTitles();
	std::set<TitleId> baseTitleIds;
	for (const auto titleId : CafeTitleList::GetAllTitleIds()) {
		TitleId baseTitleId{};
		if (CafeTitleList::FindBaseTitleId(titleId, baseTitleId))
			baseTitleIds.emplace(baseTitleId);
	}

	for (const auto baseTitleId : baseTitleIds) {
		auto gameInfo = CafeTitleList::GetGameInfo(baseTitleId);
		if (!gameInfo.IsValid())
			continue;
		auto& base = gameInfo.GetBase();
		const std::string name = gameInfo.GetTitleName();
		const std::string region = RegionName(gameInfo.GetRegion());
		const auto aoc = gameInfo.GetAOC();
		const auto [compatibleGraphicPacks, enabledGraphicPacks] =
			CountGraphicPacksForTitle(baseTitleId);
		CemuEmbedInstalledTitle record{
			sizeof(CemuEmbedInstalledTitle),
			CEMU_EMBED_LIBRARY_VERSION,
			baseTitleId,
			base.GetAppTitleVersion(),
			gameInfo.GetVersion(),
			gameInfo.HasUpdate() ? gameInfo.GetUpdate().GetAppTitleVersion() : uint16_t{},
			gameInfo.GetAOCVersion(),
			static_cast<uint32_t>(aoc.size()),
			static_cast<uint32_t>(gameInfo.GetRegion()),
			name.c_str(),
			region.c_str(),
			compatibleGraphicPacks,
			enabledGraphicPacks
		};
		const auto result = callback(userData, &record);
		if (result != CEMU_EMBED_OK)
			return result;
	}
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_LaunchInstalledTitle(
	CemuEmbedInstance* instance, uint64_t baseTitleId) {
	if (!instance || !baseTitleId)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY) {
		ReportError(instance, CEMU_EMBED_INVALID_STATE,
			"Cemu is not ready to launch an installed title yet.");
		return CEMU_EMBED_INVALID_STATE;
	}
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY, "A title is already running.");
		return CEMU_EMBED_BUSY;
	}

	baseTitleId = TitleIdParser::MakeBaseTitleId(baseTitleId);
	auto gameInfo = CafeTitleList::GetGameInfo(baseTitleId);
	if (!gameInfo.IsValid()) {
		RefreshInstalledTitles();
		gameInfo = CafeTitleList::GetGameInfo(baseTitleId);
	}
	if (!gameInfo.IsValid()) {
		ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
			"The selected installed base game was not found.");
		return CEMU_EMBED_LAUNCH_FAILED;
	}
	if (CafeSystem::PrepareForegroundTitle(baseTitleId) !=
		CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
		ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
			"Cemu could not mount the installed base game, update and DLC.");
		return CEMU_EMBED_LAUNCH_FAILED;
	}
	CafeSystem::LaunchForegroundTitle();
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_InstallGraphicPacksFromBrokeredFolder(
	CemuEmbedInstance* instance, void* folderHandle,
	const CemuEmbedBrokeredStorage* storage, uint32_t* importedPackCount) {
	if (!instance || !folderHandle || !HasRequiredBrokeredStorage(storage))
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (importedPackCount)
		*importedPackCount = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY,
			"Stop the running title before importing graphic packs.");
		return CEMU_EMBED_BUSY;
	}

	fs::path stagedPath;
	const auto stageResult = StageBrokeredFolder(instance, folderHandle, *storage,
		// Keep this component deliberately short. Community graphic-pack paths
		// can already be deeply nested and the UWP package/cache prefix is long;
		// using "graphic-packs" here pushed valid files to MAX_PATH before the
		// std::filesystem scan could import them.
		"gp", false, stagedPath, true);
	if (stageResult != CEMU_EMBED_OK)
		return stageResult;

	std::error_code error;
	fs::path source = stagedPath;
	if (fs::is_directory(stagedPath / "graphicPacks", error))
		source /= "graphicPacks";
	else if (fs::is_directory(stagedPath / "downloadedGraphicPacks", error))
		source /= "downloadedGraphicPacks";
	const fs::path destination = ActiveSettings::GetUserDataPath("graphicPacks");
	fs::create_directories(destination, error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not create the persistent graphicPacks directory.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	std::vector<fs::path> sourceRules;
	for (fs::recursive_directory_iterator it(source,
		fs::directory_options::skip_permission_denied, error);
		!error && it != fs::recursive_directory_iterator(); it.increment(error)) {
		if (!it->is_regular_file(error))
			continue;
		auto filename = _pathToUtf8(it->path().filename());
		std::transform(filename.begin(), filename.end(), filename.begin(),
			[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		if (filename == "rules.txt")
			sourceRules.emplace_back(it->path());
	}
	if (error || sourceRules.empty()) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			error ? "Cemu could not scan the selected graphic packs."
			      : "The selected folder does not contain any Cemu rules.txt graphic packs.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	// Install each rules.txt directory below a short, stable private root.
	// Besides making a directly selected pack visible to LoadAll(), this avoids
	// reproducing the very deep community-repository hierarchy below the long
	// UWP LocalState prefix.
	const fs::path importedRoot = destination / "imported";
	fs::create_directories(importedRoot, error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not create the imported graphic-pack directory.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	std::vector<fs::path> installedRules;
	for (const auto& rulesPath : sourceRules) {
		const auto relativeRules = rulesPath.lexically_relative(source);
		if (relativeRules.empty()) {
			error = std::make_error_code(std::errc::invalid_argument);
			break;
		}
		const auto identity = _pathToUtf8(relativeRules);
		const fs::path packSource = rulesPath.parent_path();
		const fs::path packDestination = importedRoot /
			fmt::format("{:016x}", StableGraphicPackId(rulesPath, identity));

		fs::remove_all(packDestination, error);
		if (error)
			break;
		fs::create_directories(packDestination, error);
		if (error)
			break;

		for (fs::recursive_directory_iterator it(packSource,
			fs::directory_options::skip_permission_denied, error);
			!error && it != fs::recursive_directory_iterator(); it.increment(error)) {
			const auto relative = it->path().lexically_relative(packSource);
			if (relative.empty()) {
				error = std::make_error_code(std::errc::invalid_argument);
				break;
			}
			const auto target = packDestination / relative;
			if (it->is_directory(error)) {
				fs::create_directories(target, error);
			} else if (it->is_regular_file(error)) {
				fs::create_directories(target.parent_path(), error);
				if (!error && !CopyFileInChunks(it->path(), target, error))
					break;
			}
		}
		if (error)
			break;
		installedRules.emplace_back(packDestination / rulesPath.filename());
	}
	if (error) {
		cemuLog_log(LogType::Force,
			"Graphic-pack chunked copy failed: {}", error.message());
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not copy the selected graphic packs in chunks.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	GraphicPack2::ClearGraphicPacks();
	GraphicPack2::LoadAll();
	uint32_t loadedCount{};
	for (const auto& installedRulesPath : installedRules) {
		for (const auto& pack : GraphicPack2::GetGraphicPacks()) {
			if (SamePath(pack->GetRulesPath(), installedRulesPath)) {
				++loadedCount;
				break;
			}
		}
	}
	if (loadedCount == 0) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"The selected rules.txt files were copied, but Cemu rejected every graphic pack. Check the [Definition] section and pack version.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (importedPackCount)
		*importedPackCount = loadedCount;
	if (loadedCount != sourceRules.size())
		cemuLog_log(LogType::Force,
			"Imported {} of {} selected graphic pack(s); invalid rules were skipped",
			loadedCount, sourceRules.size());
	else
		cemuLog_log(LogType::Force, "Imported {} graphic pack(s) into {}",
			loadedCount, _pathToUtf8(destination));
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetGraphicPacksEnabledForTitle(
	CemuEmbedInstance* instance, uint64_t baseTitleId, int32_t enabled,
	uint32_t* affectedPackCount) {
	if (!instance || !baseTitleId)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (affectedPackCount)
		*affectedPackCount = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning())
		return CEMU_EMBED_BUSY;

	baseTitleId = TitleIdParser::MakeBaseTitleId(baseTitleId);
	uint32_t affected{};
	for (const auto& pack : GraphicPack2::GetGraphicPacks()) {
		if (!pack->ContainsTitleId(baseTitleId))
			continue;
		pack->SetEnabled(enabled != 0);
		SaveGraphicPackState(pack);
		++affected;
	}
	GetConfigHandle().Save();
	if (affectedPackCount)
		*affectedPackCount = affected;
	cemuLog_log(LogType::Force, "{} {} compatible graphic pack(s) for title {:016x}",
		enabled ? "Enabled" : "Disabled", affected, baseTitleId);
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_ApplySafeGraphicPackPolicyForTitle(
	CemuEmbedInstance* instance, uint64_t baseTitleId,
	uint32_t* affectedPackCount) {
	if (!instance || !baseTitleId)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (affectedPackCount)
		*affectedPackCount = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning())
		return CEMU_EMBED_BUSY;

	baseTitleId = TitleIdParser::MakeBaseTitleId(baseTitleId);
	uint32_t affected{};
	for (const auto& pack : GraphicPack2::GetGraphicPacks()) {
		if (!pack->ContainsTitleId(baseTitleId))
			continue;
		const std::string& path = pack->GetVirtualPath();
		const bool isWorkaround = path.find("/Workarounds/") != std::string::npos;
		const bool isExecutableModification =
			path.find("/Mods/") != std::string::npos ||
			path.find("/Cheats/") != std::string::npos;
		if (!isWorkaround && !isExecutableModification)
			continue;

		const bool shouldEnable = isWorkaround;
		if (pack->IsEnabled() == shouldEnable)
			continue;
		pack->SetEnabled(shouldEnable);
		SaveGraphicPackState(pack);
		++affected;
		cemuLog_log(LogType::Force, "{} graphic pack under safe host policy: {}",
			shouldEnable ? "Enabled" : "Disabled", path);
	}
	GetConfigHandle().Save();
	if (affectedPackCount)
		*affectedPackCount = affected;
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_EnsureDefaultGamepadProfile(
	CemuEmbedInstance* instance, int32_t* profileReady) {
	if (!instance || !profileReady)
		return CEMU_EMBED_INVALID_ARGUMENT;
	*profileReady = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
#if defined(CEMU_UWP)
	// On Xbox, WGI objects are apartment-affine. Do not ask SDL to open a WGI
	// controller from Cemu's worker thread; the host publishes a plain state
	// snapshot instead. This also replaces stale desktop SDL profiles.
	if (UWPGamepadController::IsHostGamepadConnected()) {
		try {
		auto& input = InputManager::instance();
		auto emulated = input.get_controller(0);
		std::shared_ptr<ControllerBase> controller;
		if (emulated) {
			for (const auto& configured : emulated->get_controllers()) {
				if (configured && configured->api() == InputAPI::WGIGamepad) {
					controller = configured;
					break;
				}
			}
		}
		if (!controller) {
			// Build a complete replacement before publishing it to InputManager.
			// Mutating an active EmulatedController in place can race the 1 ms
			// input update thread on Xbox and caused crashes when a game first
			// consumed controller input.
			auto replacement = ControllerFactory::CreateEmulatedController(
				0, EmulatedController::Type::VPAD);
			if (!replacement)
				return CEMU_EMBED_INITIALIZATION_FAILED;
			controller = std::make_shared<UWPGamepadController>();
			replacement->add_controller(controller);
			if (!replacement->set_default_mapping(controller))
				return CEMU_EMBED_INITIALIZATION_FAILED;
			input.set_controller(replacement);
			if (!input.save(0))
				cemuLog_log(LogType::Force, "Could not save the host Xbox GamePad profile");
			input.on_device_changed();
			cemuLog_log(LogType::Force,
				"Configured the host Xbox GamePad profile safely without SDL/WGI cross-thread access");
		}
		*profileReady = controller->is_connected() ? 1 : 0;
		return CEMU_EMBED_OK;
		}
		catch (const std::exception& exception) {
			cemuLog_log(LogType::Force,
				"Could not configure the host Xbox GamePad profile: {}", exception.what());
			return CEMU_EMBED_INITIALIZATION_FAILED;
		}
		catch (...) {
			cemuLog_log(LogType::Force,
				"Could not configure the host Xbox GamePad profile due to an unknown error");
			return CEMU_EMBED_INITIALIZATION_FAILED;
		}
	}
#endif
#ifdef HAS_SDL
	auto& input = InputManager::instance();
	auto emulated = input.get_controller(0);
	if (emulated) {
		for (const auto& configuredController : emulated->get_controllers()) {
			if (configuredController && configuredController->connect()) {
				*profileReady = 1;
				return CEMU_EMBED_OK;
			}
		}
	}
	if (!input.is_api_available(InputAPI::SDLController))
		return CEMU_EMBED_OK;
	const auto provider = input.get_api_provider(InputAPI::SDLController);
	auto controllers = provider ? provider->get_controllers() :
		std::vector<std::shared_ptr<ControllerBase>>{};
	if (controllers.empty())
		return CEMU_EMBED_OK;

	auto selected = controllers.front();
	for (const auto& controller : controllers) {
		std::string name = controller->display_name();
		std::transform(name.begin(), name.end(), name.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (name.find("xbox") != std::string::npos) {
			selected = controller;
			break;
		}
	}
	if (!emulated)
		emulated = input.set_controller(0, EmulatedController::Type::VPAD);
	if (!emulated)
		return CEMU_EMBED_INITIALIZATION_FAILED;
	if (!selected->connect()) {
		cemuLog_log(LogType::Force,
			"Windows.Gaming.Input exposed {}, but SDL could not open it: {}",
			selected->display_name(), SDL_GetError());
		return CEMU_EMBED_OK;
	}
	if (!emulated->get_controllers().empty())
		emulated->clear_controllers();
	emulated->add_controller(selected);
	if (!emulated->set_default_mapping(selected)) {
		emulated->clear_controllers();
		cemuLog_log(LogType::Force,
			"Could not create the default Wii U GamePad mapping for {}",
			selected->display_name());
		return CEMU_EMBED_OK;
	}
	if (!input.save(0)) {
		cemuLog_log(LogType::Force,
			"The Xbox controller is active, but its Wii U GamePad profile could not be saved");
	}
	input.on_device_changed();
	*profileReady = 1;
	cemuLog_log(LogType::Force,
		"Created the default Wii U GamePad profile for {}", selected->display_name());
#endif
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetHostGamepadState(
	CemuEmbedInstance* instance, const CemuEmbedGamepadState* state) {
	if (!instance || !state || state->struct_size < sizeof(CemuEmbedGamepadState) ||
		state->abi_version != CEMU_EMBED_GAMEPAD_VERSION)
		return CEMU_EMBED_INVALID_ARGUMENT;
	UWPGamepadController::SetHostState(state->connected != 0, state->buttons,
		state->left_x, state->left_y, state->right_x, state->right_y,
		state->left_trigger, state->right_trigger);
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetVirtualMouse(
	CemuEmbedInstance* instance, int32_t x, int32_t y,
	int32_t leftDown, int32_t enabled) {
	if (!instance)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	InputManager::instance().set_virtual_mouse(
		enabled != 0,
		{ (std::max)(x, 0), (std::max)(y, 0) },
		leftDown != 0);
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetPerformanceMetrics(
	CemuEmbedInstance* instance, int32_t enabled) {
	if (!instance)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;

	LatteOverlay_setHostPerformanceMetrics(enabled != 0);
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_Pump(CemuEmbedInstance* instance) {
	if (!instance) return CEMU_EMBED_INVALID_ARGUMENT;
	if (CemuRuntime::HasOutOfMemory()) {
		ReportError(instance, CEMU_EMBED_INITIALIZATION_FAILED,
			"Cemu exhausted the Xbox shared memory budget while running the title.");
		SetState(instance, CEMU_EMBED_STATE_FAILED);
		CemuRuntime::ClearFatalError();
		return CEMU_EMBED_INITIALIZATION_FAILED;
	}
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
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_GetActiveAccount(
	CemuEmbedInstance* instance, CemuEmbedActiveAccount* account) {
	if (!instance || !account || account->struct_size < sizeof(CemuEmbedActiveAccount) ||
		account->abi_version != CEMU_EMBED_ACCOUNT_VERSION)
		return CEMU_EMBED_INVALID_ARGUMENT;
	const auto state = instance->state.load(std::memory_order_acquire);
	if (state != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;

	const auto& activeAccount = Account::GetCurrentAccount();
	const auto miiName = boost::nowide::narrow(std::wstring(activeAccount.GetMiiName()));
	const auto accountId = activeAccount.GetAccountId();
	account->persistent_id = activeAccount.GetPersistentId();
	account->online_enabled = ActiveSettings::IsOnlineEnabled() ? 1 : 0;
	memset(account->mii_name_utf8, 0, sizeof(account->mii_name_utf8));
	memset(account->account_id_utf8, 0, sizeof(account->account_id_utf8));
	memcpy(account->mii_name_utf8, miiName.data(),
		std::min(miiName.size(), sizeof(account->mii_name_utf8) - 1));
	memcpy(account->account_id_utf8, accountId.data(),
		std::min(accountId.size(), sizeof(account->account_id_utf8) - 1));
	return CEMU_EMBED_OK;
}
extern "C" void CEMU_EMBED_CALL CemuEmbed_Destroy(CemuEmbedInstance* instance) {
	if (!instance) return;
	CemuEmbed_RequestStop(instance);
	if (instance->initializationThread.joinable()) instance->initializationThread.join();
	if (instance->state.load(std::memory_order_acquire) == CEMU_EMBED_STATE_STOPPING) SetState(instance, CEMU_EMBED_STATE_STOPPED);
	if (instance->initialized.load(std::memory_order_acquire)) {
		InputManager::instance().set_virtual_mouse(false, {}, false);
		CafeSystem::Shutdown();
	}
	{ std::lock_guard lock(s_instanceMutex); if (s_instance == instance) s_instance = nullptr; }
	if (instance->loggingCallbacksInstalled.load(std::memory_order_acquire)) cemuLog_clearCallbacks();
	delete instance;
}
