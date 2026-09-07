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
#include "Cafe/OS/libs/nsyshid/Dimensions.h"
#include "Cafe/Filesystem/FST/KeyCache.h"
#include "Cafe/Filesystem/fscDeviceBrokered.h"
#include "Common/CemuRuntime.h"
#include "Common/FileStream.h"
#include "Common/VirtualFile.h"
#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "input/api/Controller.h"
#include "input/api/UWP/UWPGamepadController.h"
#include "interface/WindowSystem.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <boost/nowide/convert.hpp>
#include <curl/curl.h>
#include <fmt/format.h>
#include <rapidjson/document.h>
#include <zip.h>
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
	std::atomic_bool dimensionsToypadEnabled{false};
	std::vector<fs::path> externalVirtualFiles;
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
		storage->enumerate_recursive && storage->open_read && storage->read && storage->close &&
		storage->open_relative_read;
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
	auto& entry = entries[path];
	entry.clear();
	for (const auto& preset : pack->GetActivePresets())
		entry.try_emplace(preset->category, preset->name);
	if (!pack->IsEnabled())
		entry.try_emplace("_disabled", "true");
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

struct BrokeredIndexContext {
	std::shared_ptr<FSCBrokeredFilesystem> filesystem;
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

constexpr size_t kGraphicPackDownloadLimit = 256ull * 1024ull * 1024ull;
constexpr uint64_t kGraphicPackArchiveEntryLimit = 128ull * 1024ull * 1024ull;

struct HttpDownloadBuffer {
	std::vector<uint8_t> bytes;
	bool exceededLimit{};
};

size_t CurlWriteToBuffer(char* source, size_t size, size_t count, void* userData) {
	auto* buffer = static_cast<HttpDownloadBuffer*>(userData);
	if (!buffer || (size != 0 && count > (std::numeric_limits<size_t>::max)() / size))
		return 0;
	const size_t byteCount = size * count;
	if (byteCount > kGraphicPackDownloadLimit ||
		buffer->bytes.size() > kGraphicPackDownloadLimit - byteCount) {
		buffer->exceededLimit = true;
		return 0;
	}
	const auto previousSize = buffer->bytes.size();
	buffer->bytes.resize(previousSize + byteCount);
	std::memcpy(buffer->bytes.data() + previousSize, source, byteCount);
	return byteCount;
}

bool DownloadHttpsFile(const std::string& url, HttpDownloadBuffer& buffer) {
	buffer.bytes.clear();
	buffer.exceededLimit = false;
	CURL* curl = curl_easy_init();
	if (!curl)
		return false;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToBuffer);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Cemu-UWP-Host");
	// Keep libcurl's normal certificate validation. The desktop UI predates
	// Cemu's UWP host and disabled it; a managed download must not do that.
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	const CURLcode result = curl_easy_perform(curl);
	long httpStatus{};
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
	curl_easy_cleanup(curl);
	return result == CURLE_OK && !buffer.exceededLimit &&
		httpStatus >= 200 && httpStatus < 300 && !buffer.bytes.empty();
}

bool IsRulesFile(const fs::path& path) {
	auto name = _pathToUtf8(path.filename());
	std::transform(name.begin(), name.end(), name.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	return name == "rules.txt";
}

uint32_t CountGraphicPackRules(const fs::path& root) {
	std::error_code error;
	uint32_t count{};
	for (fs::recursive_directory_iterator it(root,
		fs::directory_options::skip_permission_denied, error);
		!error && it != fs::recursive_directory_iterator(); it.increment(error)) {
		if (it->is_regular_file(error) && IsRulesFile(it->path()))
			++count;
	}
	return error ? 0 : count;
}

bool IsSafeArchivePath(const char* pathName, fs::path& relativePath) {
	if (!MakeSafeRelativePath(pathName, relativePath))
		return false;
	const auto utf8Path = _pathToUtf8(relativePath);
	return utf8Path.find(':') == std::string::npos;
}

bool WriteGraphicPackVersion(const fs::path& path, const std::string& version) {
	std::unique_ptr<FileStream> output(FileStream::createFile2(path));
	if (!output)
		return false;
	if (version.size() > static_cast<size_t>((std::numeric_limits<sint32>::max)()))
		return false;
	const auto length = static_cast<sint32>(version.size());
	if (output->writeData(version.data(), length) != length)
		return false;
	const char newline = '\n';
	return output->writeData(&newline, 1) == 1;
}

bool ExtractGraphicPackArchive(const std::vector<uint8_t>& archive,
	const fs::path& destination, std::string& errorText) {
	zip_error_t zipError{};
	zip_error_init(&zipError);
	zip_source_t* source = zip_source_buffer_create(archive.data(), archive.size(),
		0, &zipError);
	if (!source) {
		errorText = "Cemu could not create the Graphic Pack archive reader.";
		zip_error_fini(&zipError);
		return false;
	}
	zip_t* zip = zip_open_from_source(source, 0, &zipError);
	if (!zip) {
		errorText = "The downloaded Graphic Pack archive is invalid.";
		zip_source_free(source);
		zip_error_fini(&zipError);
		return false;
	}

	std::error_code error;
	fs::create_directories(destination, error);
	if (error) {
		errorText = "Cemu could not create the Graphic Pack staging directory.";
		zip_close(zip);
		zip_error_fini(&zipError);
		return false;
	}

	bool extractedFile{};
	const zip_int64_t entries = zip_get_num_entries(zip, 0);
	for (zip_int64_t index = 0; index < entries; ++index) {
		zip_stat_t stat{};
		zip_stat_init(&stat);
		if (zip_stat_index(zip, static_cast<zip_uint64_t>(index), 0, &stat) != 0 ||
			!stat.name) {
			errorText = "Cemu could not inspect the downloaded Graphic Pack archive.";
			zip_close(zip);
			zip_error_fini(&zipError);
			return false;
		}
		fs::path relative;
		if (!IsSafeArchivePath(stat.name, relative))
			continue;
		const size_t nameLength = std::strlen(stat.name);
		if (nameLength != 0 && (stat.name[nameLength - 1] == '/' ||
			stat.name[nameLength - 1] == '\\')) {
			fs::create_directories(destination / relative, error);
			if (error) {
				errorText = "Cemu could not create a Graphic Pack directory.";
				zip_close(zip);
				zip_error_fini(&zipError);
				return false;
			}
			continue;
		}
		if (stat.size > kGraphicPackArchiveEntryLimit) {
			errorText = "The Graphic Pack archive contains an unexpectedly large file.";
			zip_close(zip);
			zip_error_fini(&zipError);
			return false;
		}
		const auto target = destination / relative;
		fs::create_directories(target.parent_path(), error);
		if (error) {
			errorText = "Cemu could not prepare a Graphic Pack file destination.";
			zip_close(zip);
			zip_error_fini(&zipError);
			return false;
		}
		zip_file_t* file = zip_fopen_index(zip, static_cast<zip_uint64_t>(index), 0);
		if (!file) {
			errorText = "Cemu could not read a Graphic Pack archive entry.";
			zip_close(zip);
			zip_error_fini(&zipError);
			return false;
		}
		std::unique_ptr<FileStream> output(FileStream::createFile2(target));
		if (!output) {
			zip_fclose(file);
			errorText = fmt::format(
				"Cemu could not create Graphic Pack file '{}'.",
				_pathToUtf8(relative));
			zip_close(zip);
			zip_error_fini(&zipError);
			return false;
		}
		std::vector<char> chunk(64 * 1024);
		uint64_t remaining = stat.size;
		while (remaining != 0) {
			const auto requestSize = static_cast<zip_uint64_t>((std::min)(
				remaining, static_cast<uint64_t>(chunk.size())));
			const zip_int64_t read = zip_fread(file, chunk.data(), requestSize);
			if (read <= 0) {
				zip_fclose(file);
				errorText = "Cemu could not extract a Graphic Pack file.";
				zip_close(zip);
				zip_error_fini(&zipError);
				return false;
			}
			const auto requested = static_cast<sint32>(read);
			if (output->writeData(chunk.data(), requested) != requested) {
				zip_fclose(file);
				errorText = fmt::format(
					"Cemu could not write Graphic Pack file '{}'.",
					_pathToUtf8(relative));
				zip_close(zip);
				zip_error_fini(&zipError);
				return false;
			}
			remaining -= static_cast<uint64_t>(read);
		}
		output.reset();
		zip_fclose(file);
		extractedFile = true;
	}
	if (zip_close(zip) != 0) {
		errorText = "Cemu could not finalize the Graphic Pack archive.";
		zip_error_fini(&zipError);
		return false;
	}
	zip_error_fini(&zipError);
	if (!extractedFile) {
		errorText = "The downloaded Graphic Pack archive is empty.";
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

CemuEmbedResult CEMU_EMBED_CALL IndexBrokeredEntry(void* userData, const char* relativePathUtf8,
	CemuEmbedBrokeredEntryType type, uint64_t size, void* fileHandle) {
	auto& context = *static_cast<BrokeredIndexContext*>(userData);
	fs::path safePath;
	if (!MakeSafeRelativePath(relativePathUtf8, safePath)) {
		context.error = "The broker supplied an invalid relative path.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if ((type != CEMU_EMBED_BROKERED_FILE && type != CEMU_EMBED_BROKERED_DIRECTORY) ||
		(type == CEMU_EMBED_BROKERED_FILE && !fileHandle)) {
		context.error = "The broker supplied an invalid entry.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (++context.entryCount > 1000000) {
		context.error = "The selected folder contains too many entries.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	const std::string normalizedPath = _pathToUtf8(safePath.generic_string());
	if (!context.filesystem->AddEntry(normalizedPath,
		type == CEMU_EMBED_BROKERED_DIRECTORY, size)) {
		context.error = "The broker supplied conflicting directory entries.";
		return CEMU_EMBED_STORAGE_FAILED;
	}
	return CEMU_EMBED_OK;
}

std::shared_ptr<FSCBrokeredFilesystem> BuildBrokeredFilesystem(
	void* folderHandle, const CemuEmbedBrokeredStorage& storage,
	std::string identity, std::string& error) {
	if (!folderHandle) {
		error = "The brokered title folder is unavailable.";
		return {};
	}
	// Copy the ABI table into each mounted filesystem. This prevents a later
	// host-side settings/UI update from changing callbacks while emulation I/O
	// is in flight.
	const CemuEmbedBrokeredStorage storageCopy = storage;
	auto filesystem = std::make_shared<FSCBrokeredFilesystem>(std::move(identity),
		[storageCopy, folderHandle](std::string_view path, void*& stream) {
			stream = nullptr;
			const std::string pathCopy(path);
			return storageCopy.open_relative_read(storageCopy.user_data, folderHandle,
				pathCopy.c_str(), &stream) == CEMU_EMBED_OK && stream;
		},
		[storageCopy](void* stream, uint64 offset, uint8* buffer, uint32 size) {
			uint32 bytesRead{};
			if (storageCopy.read(storageCopy.user_data, stream, offset, buffer, size,
				&bytesRead) != CEMU_EMBED_OK || bytesRead > size)
				return uint32{};
			return bytesRead;
		},
		[storageCopy](void* stream) {
			storageCopy.close(storageCopy.user_data, stream);
		});

	BrokeredIndexContext context{filesystem};
	if (storage.enumerate_recursive(storage.user_data, folderHandle,
		IndexBrokeredEntry, &context) != CEMU_EMBED_OK) {
		error = context.error.empty()
			? "The broker could not index the selected title folder."
			: context.error;
		return {};
	}
	if (context.entryCount == 0) {
		error = "The selected title folder contains no files.";
		return {};
	}
	return filesystem;
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
	// This buffer is used only when the platform's direct brokered CopyAsync
	// path is unavailable. Normal UWP/Xbox staging copies through that native
	// path and is not constrained by this fallback block size.
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

void ClearExternalVirtualFiles(CemuEmbedInstance* instance)
{
	for (const auto& path : instance->externalVirtualFiles)
		VirtualFile::Unregister(path);
	instance->externalVirtualFiles.clear();
}

bool RegisterExternalVirtualFiles(CemuEmbedInstance* instance,
	const std::shared_ptr<FSCBrokeredFilesystem>& filesystem,
	const fs::path& virtualRoot)
{
	for (const auto& [relativePath, size] : filesystem->GetFiles())
	{
		const fs::path path = (virtualRoot / _utf8ToPath(relativePath)).lexically_normal();
		VirtualFile::Source source;
		source.size = size;
		source.open = [filesystem, relativePath](void*& stream) {
			return filesystem->OpenRead(relativePath, stream);
		};
		source.read = [filesystem](void* stream, uint64 offset, uint8* buffer, uint32 length) {
			return filesystem->Read(stream, offset, buffer, length);
		};
		source.close = [filesystem](void* stream) { filesystem->Close(stream); };
		if (!VirtualFile::Register(path, std::move(source)))
			return false;
		instance->externalVirtualFiles.emplace_back(path);
	}
	return true;
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
	fs::path launchPath = gamePath;
	std::error_code pathError;
	// A brokered folder containing NUS content is represented by its title.tmd.
	// Extracted code/content/meta folders continue to use the folder itself.
	if (fs::is_directory(launchPath, pathError) &&
		!fs::is_directory(launchPath / "code", pathError)) {
		if (fs::is_regular_file(launchPath / "title.tmd", pathError)) {
			launchPath /= "title.tmd";
		} else {
			// A selected standalone executable folder may include adjacent RPLs.
			// Preserve the whole brokered folder and select its RPX/ELF locally.
			pathError.clear();
			for (const auto& entry : fs::directory_iterator(launchPath, pathError)) {
				if (!entry.is_regular_file(pathError))
					continue;
				const auto extension = _pathToUtf8(entry.path().extension());
				if (boost::iequals(extension, ".rpx") || boost::iequals(extension, ".elf")) {
					launchPath = entry.path();
					break;
				}
			}
		}
	}

	TitleInfo title{ launchPath };
	if (!title.IsValid()) {
		const auto fileType = DetermineCafeSystemFileType(launchPath);
		if (fileType == CafeTitleFileType::RPX || fileType == CafeTitleFileType::ELF) {
			const auto result = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(launchPath);
			if (result != CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
				ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
					"Cemu could not prepare the selected standalone RPX/ELF executable.");
				return CEMU_EMBED_LAUNCH_FAILED;
			}
			CafeSystem::LaunchForegroundTitle();
			return CEMU_EMBED_OK;
		}
		ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
			"The selected item is not a supported Wii U title (.wud, .wux, .iso, .wua, .wuhb, .rpx, .elf, title.tmd, or extracted title folder).");
		return CEMU_EMBED_LAUNCH_FAILED;
	}

	CafeTitleList::AddTitleFromPath(launchPath);
	TitleId baseTitleId{};
	if (!CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), baseTitleId)) {
		ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
			"Cemu found the title but could not resolve its runnable base title.");
		return CEMU_EMBED_LAUNCH_FAILED;
	}
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
			executable = launchPath / "code" / _utf8ToPath(executableName);
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

uint32_t HashStandaloneExecutable(const uint8_t* data, size_t size) {
	uint32_t hash = 0x3416DCBF;
	for (size_t index = 0; index < size; ++index) {
		hash = (hash << 3) | (hash >> 29);
		hash += data[index];
	}
	return hash;
}

TitleId NormalizeGraphicPackTitleId(TitleInfo& title) {
	if (!title.IsValid())
		return 0;
	TitleId titleId = title.GetAppTitleId();
	if (!titleId)
		return 0;
	if (title.GetTitleType() == TitleIdParser::TITLE_TYPE::AOC)
		titleId &= ~0xFF00000000ull;
	return TitleIdParser::MakeBaseTitleId(titleId);
}

TitleId IdentifyGamePathForGraphicPacks(fs::path gamePath) {
	std::error_code error;
	if (fs::is_directory(gamePath, error) &&
		!fs::is_directory(gamePath / "code", error) &&
		fs::is_regular_file(gamePath / "title.tmd", error))
		gamePath /= "title.tmd";
	const auto type = DetermineCafeSystemFileType(gamePath);
	if (type == CafeTitleFileType::RPX || type == CafeTitleFileType::ELF) {
		auto data = FileStream::LoadIntoMemory(gamePath);
		if (!data || data->empty())
			return 0;
		return 0xFFFFFFFF00000000ull |
			static_cast<TitleId>(HashStandaloneExecutable(data->data(), data->size()));
	}
	TitleInfo title{ gamePath };
	return NormalizeGraphicPackTitleId(title);
}

TitleId IdentifyBrokeredGameForGraphicPacks(CemuEmbedInstance* instance,
	const std::shared_ptr<FSCBrokeredFilesystem>& filesystem,
	std::string_view selectedRelativePath) {
	if (!filesystem)
		return 0;
	if (selectedRelativePath.empty()) {
		if (filesystem->ContainsFile("title.tmd")) {
			ClearExternalVirtualFiles(instance);
			const fs::path virtualRoot = fs::path("brokered-identify") /
				fmt::format("{:016X}", static_cast<uint64>(
					reinterpret_cast<uintptr_t>(filesystem.get())));
			if (!RegisterExternalVirtualFiles(instance, filesystem, virtualRoot)) {
				ClearExternalVirtualFiles(instance);
				return 0;
			}
			TitleInfo title{ virtualRoot / "title.tmd" };
			const TitleId titleId = NormalizeGraphicPackTitleId(title);
			ClearExternalVirtualFiles(instance);
			return titleId;
		}
		TitleInfo title{ filesystem, filesystem->GetIdentity() };
		return NormalizeGraphicPackTitleId(title);
	}
	fs::path relativePath;
	const std::string selectedRelativePathString(selectedRelativePath);
	if (!MakeSafeRelativePath(selectedRelativePathString.c_str(), relativePath))
		return 0;
	const std::string normalized = _pathToUtf8(relativePath.lexically_normal());
	if (!filesystem->ContainsFile(normalized))
		return 0;
	const auto type = DetermineCafeSystemFileType(relativePath);
	if (type == CafeTitleFileType::RPX || type == CafeTitleFileType::ELF) {
		const uint64 size = filesystem->GetFileSize(normalized);
		if (!size || size > (std::numeric_limits<size_t>::max)())
			return 0;
		void* stream{};
		if (!filesystem->OpenRead(normalized, stream))
			return 0;
		std::vector<uint8_t> data(static_cast<size_t>(size));
		uint64 offset{};
		while (offset < size) {
			const uint32 request = static_cast<uint32>((std::min)(
				size - offset, 1024ull * 1024ull));
			const uint32 read = filesystem->Read(stream, offset,
				data.data() + static_cast<size_t>(offset), request);
			if (!read)
				break;
			offset += read;
		}
		filesystem->Close(stream);
		if (offset != size)
			return 0;
		return 0xFFFFFFFF00000000ull |
			static_cast<TitleId>(HashStandaloneExecutable(data.data(), data.size()));
	}
	ClearExternalVirtualFiles(instance);
	const fs::path virtualRoot = fs::path("brokered-identify") /
		fmt::format("{:016X}", static_cast<uint64>(reinterpret_cast<uintptr_t>(filesystem.get())));
	if (!RegisterExternalVirtualFiles(instance, filesystem, virtualRoot)) {
		ClearExternalVirtualFiles(instance);
		return 0;
	}
	TitleInfo title{ virtualRoot / relativePath };
	const TitleId titleId = NormalizeGraphicPackTitleId(title);
	ClearExternalVirtualFiles(instance);
	return titleId;
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
		// The embedded host chooses this before initialization. Apply it after
		// settings.xml is loaded and before the emulated USB backend is attached.
		GetConfig().emulated_usb_devices.emulate_dimensions_toypad =
			instance->dimensionsToypadEnabled.load(std::memory_order_acquire);
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
	CafeTitleList::ClearBrokeredTitles();
	return LaunchGameFromPath(instance, _utf8ToPath(game_path_utf8));
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_LaunchGameWithExternalTitles(
	CemuEmbedInstance* instance, const char* game_path_utf8,
	const char* const* supplemental_title_paths, uint32_t supplemental_title_count) {
	if (!instance || !game_path_utf8 || !*game_path_utf8 ||
		(supplemental_title_count && !supplemental_title_paths))
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY) {
		ReportError(instance, CEMU_EMBED_INVALID_STATE,
			"Cemu is not ready to launch a title yet.");
		return CEMU_EMBED_INVALID_STATE;
	}
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY, "A title is already running.");
		return CEMU_EMBED_BUSY;
	}
	CafeTitleList::ClearBrokeredTitles();

	uint32_t registeredTitleCount{};
	// Register every valid title found on the removable device before resolving
	// the selected item. This allows the regular title list to associate a base
	// title with an update and DLC while all payloads remain at their source.
	for (uint32_t index = 0; index < supplemental_title_count; ++index) {
		const auto* rawPath = supplemental_title_paths[index];
		if (!rawPath || !*rawPath)
			continue;
		fs::path titlePath = _utf8ToPath(rawPath);
		std::error_code pathError;
		if (fs::is_directory(titlePath, pathError)) {
			pathError.clear();
			const bool hasCodeDirectory = fs::is_directory(titlePath / "code", pathError);
			pathError.clear();
			if (!hasCodeDirectory && fs::is_regular_file(titlePath / "title.tmd", pathError))
				titlePath /= "title.tmd";
		}
		TitleInfo title{ titlePath };
		if (title.IsValid()) {
			CafeTitleList::AddTitleFromPath(titlePath);
			++registeredTitleCount;
		}
	}
	cemuLog_log(LogType::Force,
		"External launch selected {} with {}/{} valid base, update, or DLC path(s) registered",
		game_path_utf8, registeredTitleCount, supplemental_title_count);

	return LaunchGameFromPath(instance, _utf8ToPath(game_path_utf8));
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_LaunchGameFromBrokeredFolder(
	CemuEmbedInstance* instance, void* folderHandle, const CemuEmbedBrokeredStorage* storage) {
	return CemuEmbed_LaunchGameFromBrokeredFolders(instance, folderHandle, nullptr,
		nullptr, 0, storage);
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_LaunchGameFromBrokeredFolders(
	CemuEmbedInstance* instance, void* selectedFolderHandle,
	const char* selectedRelativePathUtf8,
	void* const* supplementalFolderHandles, uint32_t supplementalFolderCount,
	const CemuEmbedBrokeredStorage* storage) {
	if (!instance || !selectedFolderHandle || !HasRequiredBrokeredStorage(storage) ||
		(supplementalFolderCount && !supplementalFolderHandles))
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY) {
		ReportError(instance, CEMU_EMBED_INVALID_STATE,
			"Cemu is not ready to launch an external title yet.");
		return CEMU_EMBED_INVALID_STATE;
	}
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY, "A title is already running.");
		return CEMU_EMBED_BUSY;
	}
	CafeTitleList::ClearBrokeredTitles();
	ClearExternalVirtualFiles(instance);

	if (selectedRelativePathUtf8 && *selectedRelativePathUtf8) {
		fs::path selectedRelativePath;
		if (!MakeSafeRelativePath(selectedRelativePathUtf8, selectedRelativePath)) {
			ReportError(instance, CEMU_EMBED_INVALID_ARGUMENT,
				"The selected external game file has an invalid relative path.");
			return CEMU_EMBED_INVALID_ARGUMENT;
		}
		std::string indexError;
		auto filesystem = BuildBrokeredFilesystem(selectedFolderHandle, *storage,
			fmt::format("external-file/{:016X}",
				static_cast<uint64>(reinterpret_cast<uintptr_t>(selectedFolderHandle))),
			indexError);
		const std::string normalizedSelected = _pathToUtf8(selectedRelativePath.lexically_normal());
		if (!filesystem || !filesystem->ContainsFile(normalizedSelected)) {
			ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
				filesystem ? "The selected external game file is no longer available."
					: indexError.c_str());
			return CEMU_EMBED_STORAGE_FAILED;
		}
		const fs::path virtualRoot = fs::path("brokered-files") /
			fmt::format("{:016X}", static_cast<uint64>(reinterpret_cast<uintptr_t>(selectedFolderHandle)));
		if (!RegisterExternalVirtualFiles(instance, filesystem, virtualRoot)) {
			ClearExternalVirtualFiles(instance);
			ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
				"Cemu could not register the external game files for direct access.");
			return CEMU_EMBED_STORAGE_FAILED;
		}
		const fs::path virtualSelected = virtualRoot / selectedRelativePath;
		const auto fileType = DetermineCafeSystemFileType(virtualSelected);
		if (fileType == CafeTitleFileType::RPX || fileType == CafeTitleFileType::ELF) {
			const auto result = CafeSystem::PrepareForegroundTitleFromBrokeredStandaloneRPX(
				filesystem, normalizedSelected);
			if (result != CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
				ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
					"Cemu could not prepare the external RPX/ELF directly from its storage device.");
				return CEMU_EMBED_LAUNCH_FAILED;
			}
			CafeSystem::LaunchForegroundTitle();
			return CEMU_EMBED_OK;
		}
		cemuLog_log(LogType::Force, "Launching external game directly without copying: {}",
			normalizedSelected);
		return LaunchGameFromPath(instance, virtualSelected);
	}

	std::vector<void*> folderHandles;
	folderHandles.reserve(static_cast<size_t>(supplementalFolderCount) + 1);
	folderHandles.emplace_back(selectedFolderHandle);
	for (uint32_t index = 0; index < supplementalFolderCount; ++index) {
		void* const folderHandle = supplementalFolderHandles[index];
		if (folderHandle && std::find(folderHandles.begin(), folderHandles.end(), folderHandle) == folderHandles.end())
			folderHandles.emplace_back(folderHandle);
	}

	// The old bridge copied every file to cache before adding it to the title
	// list. Build a metadata/index tree instead. FSCDeviceBrokered opens source
	// files by relative path only when GX2 or the game asks for them.
	// Reclaim only the superseded external-title staging cache. Saves, settings,
	// shader caches and every other cache category are intentionally untouched.
	std::error_code externalCacheError;
	const fs::path oldExternalStagingPath = _utf8ToPath(instance->cachePath) /
		"brokered-titles" / "external";
	fs::remove_all(oldExternalStagingPath, externalCacheError);
	if (externalCacheError)
		cemuLog_log(LogType::Force, "Unable to remove obsolete external-title staging cache {}",
			_pathToUtf8(oldExternalStagingPath));
	if (storage->progress)
		storage->progress(storage->user_data, 0, 0, "Indexing external title folders");

	uint32_t registeredTitleCount{};
	TitleId selectedTitleId{};
	for (size_t index = 0; index < folderHandles.size(); ++index) {
		std::string indexError;
		auto filesystem = BuildBrokeredFilesystem(folderHandles[index], *storage,
			fmt::format("external/{:016X}",
				static_cast<uint64>(reinterpret_cast<uintptr_t>(folderHandles[index]))),
			indexError);
		if (!filesystem) {
			CafeTitleList::ClearBrokeredTitles();
			ReportError(instance, CEMU_EMBED_STORAGE_FAILED, indexError.c_str());
			return CEMU_EMBED_STORAGE_FAILED;
		}
		uint64 titleId{};
		if (!CafeTitleList::AddBrokeredTitle(filesystem, &titleId)) {
			if (index == 0) {
				CafeTitleList::ClearBrokeredTitles();
				ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
					"The selected external folder is not a complete extracted Wii U title (code, content and meta with valid XML files are required).");
				return CEMU_EMBED_LAUNCH_FAILED;
			}
			cemuLog_log(LogType::Force,
				"Ignoring external companion folder {} because it is not an extracted Wii U title",
				filesystem->GetIdentity());
			continue;
		}
		if (index == 0)
			selectedTitleId = titleId;
		++registeredTitleCount;
	}

	if (selectedTitleId == 0) {
		CafeTitleList::ClearBrokeredTitles();
		ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
			"The selected external folder did not expose a launchable Wii U title.");
		return CEMU_EMBED_LAUNCH_FAILED;
	}
	TitleId baseTitleId{};
	if (!CafeTitleList::FindBaseTitleId(selectedTitleId, baseTitleId)) {
		CafeTitleList::ClearBrokeredTitles();
		ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
			"Cemu could not resolve the base title for the selected external folder.");
		return CEMU_EMBED_LAUNCH_FAILED;
	}
	const auto prepareResult = CafeSystem::PrepareForegroundTitle(baseTitleId);
	if (prepareResult != CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
		CafeTitleList::ClearBrokeredTitles();
		ReportError(instance, CEMU_EMBED_LAUNCH_FAILED,
			"Cemu could not mount the selected external title and its available update/DLC folders.");
		return CEMU_EMBED_LAUNCH_FAILED;
	}
	cemuLog_log(LogType::Force,
		"Brokered external launch mounted {}/{} base, update, or DLC title folder(s) directly from external storage (no title cache staging)",
		registeredTitleCount, folderHandles.size());
	if (storage->progress)
		storage->progress(storage->user_data, 1, 1, "External title mounted");
	CafeSystem::LaunchForegroundTitle();
	return CEMU_EMBED_OK;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_IdentifyGamePath(
	CemuEmbedInstance* instance, const char* gamePathUtf8,
	uint64_t* baseTitleId) {
	if (!instance || !gamePathUtf8 || !*gamePathUtf8 || !baseTitleId)
		return CEMU_EMBED_INVALID_ARGUMENT;
	*baseTitleId = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning())
		return CEMU_EMBED_BUSY;
	*baseTitleId = IdentifyGamePathForGraphicPacks(_utf8ToPath(gamePathUtf8));
	return *baseTitleId ? CEMU_EMBED_OK : CEMU_EMBED_LAUNCH_FAILED;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_IdentifyGameFromBrokeredFolder(
	CemuEmbedInstance* instance, void* folderHandle,
	const char* selectedRelativePathUtf8,
	const CemuEmbedBrokeredStorage* storage, uint64_t* baseTitleId) {
	if (!instance || !folderHandle || !HasRequiredBrokeredStorage(storage) || !baseTitleId)
		return CEMU_EMBED_INVALID_ARGUMENT;
	*baseTitleId = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning())
		return CEMU_EMBED_BUSY;
	std::string indexError;
	auto filesystem = BuildBrokeredFilesystem(folderHandle, *storage,
		fmt::format("identify/{:016X}",
			static_cast<uint64>(reinterpret_cast<uintptr_t>(folderHandle))), indexError);
	if (!filesystem)
		return CEMU_EMBED_STORAGE_FAILED;
	*baseTitleId = IdentifyBrokeredGameForGraphicPacks(instance, filesystem,
		selectedRelativePathUtf8 ? selectedRelativePathUtf8 : "");
	return *baseTitleId ? CEMU_EMBED_OK : CEMU_EMBED_LAUNCH_FAILED;
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

	CafeTitleList::ClearBrokeredTitles();
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

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_DeleteInstalledTitle(
	CemuEmbedInstance* instance, uint64_t baseTitleId,
	uint32_t* removedInstallFolderCount) {
	if (!instance || !baseTitleId)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (removedInstallFolderCount)
		*removedInstallFolderCount = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY,
			"Stop the running title before deleting library content.");
		return CEMU_EMBED_BUSY;
	}

	baseTitleId = TitleIdParser::MakeBaseTitleId(baseTitleId);
	auto gameInfo = CafeTitleList::GetGameInfo(baseTitleId);
	if (!gameInfo.IsValid()) {
		RefreshInstalledTitles();
		gameInfo = CafeTitleList::GetGameInfo(baseTitleId);
	}
	if (!gameInfo.IsValid()) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"The selected installed base game was not found.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	std::vector<std::string> installPaths;
	installPaths.emplace_back(gameInfo.GetBase().GetInstallPath());
	if (gameInfo.HasUpdate())
		installPaths.emplace_back(gameInfo.GetUpdate().GetInstallPath());
	for (auto& aoc : gameInfo.GetAOC())
		installPaths.emplace_back(aoc.GetInstallPath());
	std::sort(installPaths.begin(), installPaths.end());
	installPaths.erase(std::unique(installPaths.begin(), installPaths.end()),
		installPaths.end());

	std::vector<fs::path> targets;
	for (const auto& installPath : installPaths) {
		fs::path relative;
		if (!MakeSafeRelativePath(installPath.c_str(), relative)) {
			ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
				"Cemu rejected an unsafe installed-title path.");
			return CEMU_EMBED_STORAGE_FAILED;
		}
		auto component = relative.begin();
		const bool isTitleDirectory = component != relative.end() &&
			_pathToUtf8(*component) == "usr" &&
			++component != relative.end() && _pathToUtf8(*component) == "title";
		if (!isTitleDirectory) {
			ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
				"Cemu rejected a title path outside the MLC title directory.");
			return CEMU_EMBED_STORAGE_FAILED;
		}
		// GetMlcPath's variadic helper accepts a format string, not an already
		// constructed filesystem path. Append the validated path directly.
		targets.emplace_back(ActiveSettings::GetMlcPath() / relative);
	}

	std::error_code error;
	uint32_t removed{};
	for (const auto& target : targets) {
		if (!fs::exists(target, error)) {
			if (error) {
				ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
					"Cemu could not inspect an installed title directory.");
				return CEMU_EMBED_STORAGE_FAILED;
			}
			continue;
		}
		fs::remove_all(target, error);
		if (error) {
			ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
				"Cemu could not delete all files for the selected installed title.");
			return CEMU_EMBED_STORAGE_FAILED;
		}
		++removed;
	}
	RefreshInstalledTitles();
	if (removedInstallFolderCount)
		*removedInstallFolderCount = removed;
	cemuLog_log(LogType::Force,
		"Removed {} installed content folder(s) for title {:016x}; saves were retained",
		removed, baseTitleId);
	return CEMU_EMBED_OK;
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_InstallDownloadedGraphicPacks(
	CemuEmbedInstance* instance, const void* archiveData, uint64_t archiveSize,
	const char* releaseNameUtf8, uint32_t* installedPackCount,
	int32_t* alreadyCurrent) {
	if (!instance || !archiveData || archiveSize == 0 ||
		archiveSize > kGraphicPackDownloadLimit || !releaseNameUtf8 || !*releaseNameUtf8 ||
		archiveSize > (std::numeric_limits<size_t>::max)())
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (installedPackCount)
		*installedPackCount = 0;
	if (alreadyCurrent)
		*alreadyCurrent = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY,
			"Stop the running title before installing Graphic Packs.");
		return CEMU_EMBED_BUSY;
	}

	const auto* archiveBytes = static_cast<const uint8_t*>(archiveData);
	std::vector<uint8_t> archive(archiveBytes,
		archiveBytes + static_cast<size_t>(archiveSize));
	const std::string releaseName(releaseNameUtf8);
	const fs::path destination =
		ActiveSettings::GetUserDataPath("graphicPacks/downloadedGraphicPacks");
	const fs::path versionPath = destination / "version.txt";
	std::string installedVersion;
	if (ReadSmallTextFile(versionPath, installedVersion)) {
		const auto newline = installedVersion.find_first_of("\r\n");
		if (newline != std::string::npos)
			installedVersion.erase(newline);
	}
	const uint32_t existingPackCount = CountGraphicPackRules(destination);
	if (installedVersion == releaseName && existingPackCount != 0) {
		GraphicPack2::ClearGraphicPacks();
		GraphicPack2::LoadAll();
		if (installedPackCount)
			*installedPackCount = existingPackCount;
		if (alreadyCurrent)
			*alreadyCurrent = 1;
		return CEMU_EMBED_OK;
	}

	const fs::path staging = destination.parent_path() /
		"gp-download.tmp";
	const fs::path backup = destination.parent_path() /
		"gp-download.bak";
	const fs::path legacyStaging = destination.parent_path() /
		"downloadedGraphicPacks.cemu-embed-staging";
	const fs::path legacyBackup = destination.parent_path() /
		"downloadedGraphicPacks.cemu-embed-previous";
	std::error_code error;
	fs::create_directories(destination.parent_path(), error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not create the Graphic Pack download directory.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	fs::remove_all(staging, error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not clear the previous Graphic Pack staging directory.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	fs::remove_all(backup, error);
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not clear the previous Graphic Pack backup directory.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	for (const auto& legacyPath : { legacyStaging, legacyBackup }) {
		error.clear();
		fs::remove_all(legacyPath, error);
		if (error)
			cemuLog_log(LogType::Force,
				"Could not completely remove legacy Graphic Pack staging path {}: {}",
				_pathToUtf8(legacyPath), error.message());
	}
	error.clear();
	std::string extractionError;
	if (!ExtractGraphicPackArchive(archive, staging, extractionError)) {
		fs::remove_all(staging, error);
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED, extractionError.c_str());
		return CEMU_EMBED_STORAGE_FAILED;
	}
	const uint32_t extractedPackCount = CountGraphicPackRules(staging);
	if (extractedPackCount == 0 ||
		!WriteGraphicPackVersion(staging / "version.txt", releaseName)) {
		fs::remove_all(staging, error);
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			extractedPackCount == 0
				? "The downloaded archive does not contain any Cemu rules.txt Graphic Packs."
				: "Cemu could not record the installed Graphic Pack release version.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (fs::exists(destination, error))
		fs::rename(destination, backup, error);
	if (!error)
		fs::rename(staging, destination, error);
	if (error) {
		std::error_code restoreError;
		if (fs::exists(backup, restoreError) && !fs::exists(destination, restoreError))
			fs::rename(backup, destination, restoreError);
		fs::remove_all(staging, restoreError);
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not activate the downloaded Graphic Packs.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	fs::remove_all(backup, error);
	GraphicPack2::ClearGraphicPacks();
	GraphicPack2::LoadAll();
	if (installedPackCount)
		*installedPackCount = extractedPackCount;
	cemuLog_log(LogType::Force, "Installed {} downloaded Graphic Pack(s), release {}",
		extractedPackCount, releaseName);
	return CEMU_EMBED_OK;
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_DownloadGraphicPacks(
	CemuEmbedInstance* instance, uint32_t* downloadedPackCount,
	int32_t* alreadyCurrent) {
	if (!instance)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (downloadedPackCount)
		*downloadedPackCount = 0;
	if (alreadyCurrent)
		*alreadyCurrent = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY,
			"Stop the running title before downloading Graphic Packs.");
		return CEMU_EMBED_BUSY;
	}

	HttpDownloadBuffer releaseManifest;
	if (!DownloadHttpsFile(
		"https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest",
		releaseManifest)) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not download the current Graphic Pack release information.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	rapidjson::Document release;
	release.Parse(reinterpret_cast<const char*>(releaseManifest.bytes.data()),
		releaseManifest.bytes.size());
	if (release.HasParseError() || !release.IsObject()) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu received an invalid Graphic Pack release response.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	const auto name = release.FindMember("name");
	const auto assets = release.FindMember("assets");
	if (name == release.MemberEnd() || !name->value.IsString() ||
		assets == release.MemberEnd() || !assets->value.IsArray()) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"The Graphic Pack release response does not contain a version or archive.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	std::string downloadUrl;
	for (const auto& asset : assets->value.GetArray()) {
		if (!asset.IsObject())
			continue;
		const auto url = asset.FindMember("browser_download_url");
		if (url == asset.MemberEnd() || !url->value.IsString())
			continue;
		const auto assetName = asset.FindMember("name");
		if (assetName != asset.MemberEnd() && assetName->value.IsString()) {
			std::string candidateName = assetName->value.GetString();
			std::transform(candidateName.begin(), candidateName.end(), candidateName.begin(),
				[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
			if (candidateName.size() < 4 ||
				candidateName.compare(candidateName.size() - 4, 4, ".zip") != 0)
				continue;
		}
		downloadUrl = url->value.GetString();
		break;
	}
	if (downloadUrl.empty()) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"The current Graphic Pack release does not provide a ZIP archive.");
		return CEMU_EMBED_STORAGE_FAILED;
	}

	const std::string releaseName = name->value.GetString();
	const fs::path destination =
		ActiveSettings::GetUserDataPath("graphicPacks/downloadedGraphicPacks");
	const fs::path versionPath = destination / "version.txt";
	std::string installedVersion;
	if (ReadSmallTextFile(versionPath, installedVersion)) {
		const auto newline = installedVersion.find_first_of("\r\n");
		if (newline != std::string::npos)
			installedVersion.erase(newline);
	}
	const uint32_t installedPackCount = CountGraphicPackRules(destination);
	if (installedVersion == releaseName && installedPackCount != 0) {
		GraphicPack2::ClearGraphicPacks();
		GraphicPack2::LoadAll();
		if (downloadedPackCount)
			*downloadedPackCount = installedPackCount;
		if (alreadyCurrent)
			*alreadyCurrent = 1;
		return CEMU_EMBED_OK;
	}

	HttpDownloadBuffer archive;
	if (!DownloadHttpsFile(downloadUrl, archive)) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not download the current Graphic Pack archive.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	return CemuEmbed_InstallDownloadedGraphicPacks(instance, archive.bytes.data(),
		archive.bytes.size(), releaseName.c_str(), downloadedPackCount, alreadyCurrent);
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_ClearShaderCaches(
	CemuEmbedInstance* instance, uint32_t* removedEntryCount) {
	if (!instance)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (removedEntryCount)
		*removedEntryCount = 0;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning()) {
		ReportError(instance, CEMU_EMBED_BUSY,
			"Stop the running title before clearing the shader cache.");
		return CEMU_EMBED_BUSY;
	}

	std::error_code error;
	uintmax_t removed{};
	// Cemu's transferable/precompiled caches live under cache_path, while the
	// Xbox D3D11 backend keeps its driver-specific binaries under user_data.
	// Clear both roots so the command does what its UI label promises.
	std::vector<fs::path> cacheRoots{
		ActiveSettings::GetCachePath("shaderCache"),
		ActiveSettings::GetUserDataPath("shaderCache")
	};
	std::sort(cacheRoots.begin(), cacheRoots.end());
	cacheRoots.erase(std::unique(cacheRoots.begin(), cacheRoots.end()),
		cacheRoots.end());
	for (const auto& cacheRoot : cacheRoots) {
		error.clear();
		if (fs::exists(cacheRoot, error)) {
			for (fs::directory_iterator it(cacheRoot,
				fs::directory_options::skip_permission_denied, error);
				!error && it != fs::directory_iterator(); it.increment(error)) {
				removed += fs::remove_all(it->path(), error);
				if (error)
					break;
			}
		}
		if (error)
			break;
		fs::create_directories(cacheRoot, error);
		if (error)
			break;
	}
	if (error) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not clear every shader-cache entry.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	if (removedEntryCount)
		*removedEntryCount = static_cast<uint32_t>((std::min)(removed,
			static_cast<uintmax_t>((std::numeric_limits<uint32_t>::max)())));
	cemuLog_log(LogType::Force, "Cleared {} shader-cache entrie(s)", removed);
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

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_EnumerateGraphicPacksForTitle(
	CemuEmbedInstance* instance, uint64_t baseTitleId,
	CemuEmbedGraphicPackCallback callback, void* userData) {
	if (!instance || !baseTitleId || !callback)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	baseTitleId = TitleIdParser::MakeBaseTitleId(baseTitleId);
	for (const auto& pack : GraphicPack2::GetGraphicPacks()) {
		if (!pack->ContainsTitleId(baseTitleId))
			continue;
		const std::string identity = pack->GetNormalizedPathString();
		const std::string& virtualPath = pack->GetVirtualPath();
		const size_t separator = virtualPath.find_last_of('/');
		const std::string category = separator == std::string::npos
			? std::string{} : virtualPath.substr(0, separator);
		CemuEmbedGraphicPack info{
			sizeof(info), CEMU_EMBED_GRAPHIC_PACK_VERSION,
			identity.c_str(), pack->GetName().c_str(), category.c_str(),
			pack->GetDescription().c_str(), pack->IsEnabled() ? 1 : 0,
			pack->IsDefaultEnabled() ? 1 : 0
		};
		const auto result = callback(userData, &info);
		if (result != CEMU_EMBED_OK)
			return result;
	}
	return CEMU_EMBED_OK;
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetGraphicPackEnabled(
	CemuEmbedInstance* instance, uint64_t baseTitleId,
	const char* identityUtf8, int32_t enabled) {
	if (!instance || !baseTitleId || !identityUtf8 || !*identityUtf8)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning())
		return CEMU_EMBED_BUSY;
	baseTitleId = TitleIdParser::MakeBaseTitleId(baseTitleId);
	for (const auto& pack : GraphicPack2::GetGraphicPacks()) {
		if (!pack->ContainsTitleId(baseTitleId) ||
			pack->GetNormalizedPathString() != identityUtf8)
			continue;
		pack->SetEnabled(enabled != 0);
		SaveGraphicPackState(pack);
		if (!GetConfigHandle().Save()) {
			ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
				"Cemu could not persist the Graphic Pack state.");
			return CEMU_EMBED_STORAGE_FAILED;
		}
		cemuLog_log(LogType::Force, "{} graphic pack {} for title {:016x}",
			enabled ? "Enabled" : "Disabled", pack->GetVirtualPath(), baseTitleId);
		return CEMU_EMBED_OK;
	}
	return CEMU_EMBED_INVALID_ARGUMENT;
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
			const auto selectedType = emulated ? emulated->type() : EmulatedController::Type::VPAD;
			auto replacement = ControllerFactory::CreateEmulatedController(0, selectedType);
			if (!replacement)
				return CEMU_EMBED_INITIALIZATION_FAILED;
			controller = std::make_shared<UWPGamepadController>();
			replacement->add_controller(controller);
			if (!replacement->set_default_mapping(controller))
				return CEMU_EMBED_INITIALIZATION_FAILED;
			input.set_controller(replacement);
			if (!input.save(0))
				cemuLog_log(LogType::Force, "Could not save the host Xbox controller profile");
			input.on_device_changed();
			cemuLog_log(LogType::Force,
				"Configured the host Xbox controller as {} safely without SDL/WGI cross-thread access",
				replacement->type_string());
		}
		*profileReady = controller->is_connected() ? 1 : 0;
		return CEMU_EMBED_OK;
		}
		catch (const std::exception& exception) {
			cemuLog_log(LogType::Force,
				"Could not configure the host Xbox controller profile: {}", exception.what());
			return CEMU_EMBED_INITIALIZATION_FAILED;
		}
		catch (...) {
			cemuLog_log(LogType::Force,
				"Could not configure the host Xbox controller profile due to an unknown error");
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
			"Could not create the default {} mapping for {}", emulated->type_string(),
			selected->display_name());
		return CEMU_EMBED_OK;
	}
	if (!input.save(0)) {
		cemuLog_log(LogType::Force,
			"The physical controller is active, but its {} profile could not be saved",
			emulated->type_string());
	}
	input.on_device_changed();
	*profileReady = 1;
	cemuLog_log(LogType::Force,
		"Created the default {} profile for {}", emulated->type_string(), selected->display_name());
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

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_GetSettings(
	CemuEmbedInstance* instance, CemuEmbedSettings* settings) {
	if (!instance || !settings || settings->struct_size < sizeof(CemuEmbedSettings) ||
		settings->abi_version != CEMU_EMBED_SETTINGS_VERSION)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	const auto& c = GetConfig();
	settings->cpu_mode = static_cast<int32_t>(c.cpu_mode.GetValue());
	settings->console_language = static_cast<int32_t>(c.console_language.GetValue());
	settings->vsync = c.vsync.GetValue();
	settings->gx2drawdone_sync = c.gx2drawdone_sync.GetValue();
	settings->async_compile = c.async_compile.GetValue();
	settings->render_upside_down = c.render_upside_down.GetValue();
	settings->play_boot_sound = c.play_boot_sound.GetValue();
	settings->disable_screensaver = c.disable_screensaver.GetValue();
	settings->override_gamma = c.overrideAppGammaPreference.GetValue();
	settings->override_gamma_value = c.overrideGammaValue.GetValue();
	settings->display_gamma = c.userDisplayGamma.GetValue();
	settings->upscale_filter = c.upscale_filter.GetValue();
	settings->downscale_filter = c.downscale_filter.GetValue();
	settings->fullscreen_scaling = c.fullscreen_scaling.GetValue();
	settings->overlay_position = static_cast<int32_t>(c.overlay.position);
	settings->overlay_text_scale = c.overlay.text_scale;
	settings->overlay_fps = c.overlay.fps;
	settings->overlay_drawcalls = c.overlay.drawcalls;
	settings->overlay_cpu_usage = c.overlay.cpu_usage;
	settings->overlay_cpu_per_core = c.overlay.cpu_per_core_usage;
	settings->overlay_ram_usage = c.overlay.ram_usage;
	settings->overlay_vram_usage = c.overlay.vram_usage;
	settings->notification_position = static_cast<int32_t>(c.notification.position);
	settings->notification_text_scale = c.notification.text_scale;
	settings->notification_controller_profiles = c.notification.controller_profiles;
	settings->notification_controller_battery = c.notification.controller_battery;
	settings->notification_shader_compiling = c.notification.shader_compiling;
	settings->notification_friends = c.notification.friends;
	settings->audio_api = c.audio_api;
	settings->audio_delay = c.audio_delay;
	settings->tv_channels = static_cast<int32_t>(c.tv_channels);
	settings->pad_channels = static_cast<int32_t>(c.pad_channels);
	settings->input_channels = static_cast<int32_t>(c.input_channels);
	settings->tv_volume = c.tv_volume;
	settings->pad_volume = c.pad_volume;
	settings->input_volume = c.input_volume;
	settings->portal_volume = c.portal_volume;
	settings->emulate_skylander_portal = c.emulated_usb_devices.emulate_skylander_portal.GetValue();
	settings->emulate_infinity_base = c.emulated_usb_devices.emulate_infinity_base.GetValue();
	settings->emulate_dimensions_toypad = c.emulated_usb_devices.emulate_dimensions_toypad.GetValue();
	const auto playerOne = InputManager::instance().get_controller(0);
	settings->emulated_controller_type = playerOne ?
		static_cast<int32_t>(playerOne->type()) : static_cast<int32_t>(EmulatedController::Type::VPAD);
	return CEMU_EMBED_OK;
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetSettings(
	CemuEmbedInstance* instance, const CemuEmbedSettings* settings) {
	if (!instance || !settings || settings->struct_size < sizeof(CemuEmbedSettings) ||
		settings->abi_version != CEMU_EMBED_SETTINGS_VERSION)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning())
		return CEMU_EMBED_BUSY;
	const auto requestedControllerType = static_cast<EmulatedController::Type>(
		(std::clamp)(settings->emulated_controller_type, 0,
			static_cast<int32_t>(EmulatedController::Type::MAX) - 1));
	auto& input = InputManager::instance();
	auto currentController = input.get_controller(0);
	if (!currentController || currentController->type() != requestedControllerType) {
		try {
			auto replacement = ControllerFactory::CreateEmulatedController(0, requestedControllerType);
			if (!replacement)
				return CEMU_EMBED_INITIALIZATION_FAILED;
			std::shared_ptr<ControllerBase> physicalController;
			if (currentController) {
				for (const auto& configured : currentController->get_controllers()) {
					if (configured && configured->api() == InputAPI::WGIGamepad) {
						physicalController = configured;
						break;
					}
				}
			}
#if defined(CEMU_UWP)
			if (!physicalController && UWPGamepadController::IsHostGamepadConnected())
				physicalController = std::make_shared<UWPGamepadController>();
#endif
			if (physicalController) {
				replacement->add_controller(physicalController);
				if (!replacement->set_default_mapping(physicalController))
					return CEMU_EMBED_INITIALIZATION_FAILED;
			}
			auto previous = input.set_controller(replacement);
			if (!input.save(0)) {
				if (previous)
					input.set_controller(previous);
				else
					input.delete_controller(0);
				return CEMU_EMBED_STORAGE_FAILED;
			}
			input.on_device_changed();
			cemuLog_log(LogType::Force, "Player one emulated controller changed to {}",
				replacement->type_string());
		}
		catch (const std::exception& exception) {
			cemuLog_log(LogType::Force, "Could not change the emulated controller: {}",
				exception.what());
			return CEMU_EMBED_INITIALIZATION_FAILED;
		}
	}
	auto& c = GetConfig();
	c.cpu_mode.SetValue(static_cast<CPUMode>((std::clamp)(settings->cpu_mode, 0, 4)));
	c.console_language.SetValue(static_cast<CafeConsoleLanguage>((std::clamp)(settings->console_language, 0, 11)));
	c.vsync.SetValue((std::clamp)(settings->vsync, 0, 4));
	c.gx2drawdone_sync.SetValue(settings->gx2drawdone_sync != 0);
	c.async_compile.SetValue(settings->async_compile != 0);
	c.render_upside_down.SetValue(settings->render_upside_down != 0);
	c.play_boot_sound.SetValue(settings->play_boot_sound != 0);
	c.disable_screensaver.SetValue(settings->disable_screensaver != 0);
	c.overrideAppGammaPreference.SetValue(settings->override_gamma != 0);
	c.overrideGammaValue.SetValue((std::clamp)(settings->override_gamma_value, 1.0f, 3.0f));
	c.userDisplayGamma.SetValue((std::clamp)(settings->display_gamma, 0.0f, 3.0f));
	c.upscale_filter.SetValue((std::clamp)(settings->upscale_filter, 0, 3));
	c.downscale_filter.SetValue((std::clamp)(settings->downscale_filter, 0, 3));
	c.fullscreen_scaling.SetValue((std::clamp)(settings->fullscreen_scaling, 0, 1));
	c.overlay.position = static_cast<ScreenPosition>((std::clamp)(settings->overlay_position, 0, 6));
	c.overlay.text_scale = (std::clamp)(settings->overlay_text_scale, 50, 300);
	c.overlay.fps = settings->overlay_fps != 0;
	c.overlay.drawcalls = settings->overlay_drawcalls != 0;
	c.overlay.cpu_usage = settings->overlay_cpu_usage != 0;
	c.overlay.cpu_per_core_usage = settings->overlay_cpu_per_core != 0;
	c.overlay.ram_usage = settings->overlay_ram_usage != 0;
	c.overlay.vram_usage = settings->overlay_vram_usage != 0;
	c.notification.position = static_cast<ScreenPosition>((std::clamp)(settings->notification_position, 0, 6));
	c.notification.text_scale = (std::clamp)(settings->notification_text_scale, 50, 300);
	c.notification.controller_profiles = settings->notification_controller_profiles != 0;
	c.notification.controller_battery = settings->notification_controller_battery != 0;
	c.notification.shader_compiling = settings->notification_shader_compiling != 0;
	c.notification.friends = settings->notification_friends != 0;
	c.audio_api = (std::clamp)(settings->audio_api, 0, 4);
	c.audio_delay = (std::clamp)(settings->audio_delay, 0, 10);
	c.tv_channels = static_cast<AudioChannels>((std::clamp)(settings->tv_channels, 0, 2));
	c.pad_channels = static_cast<AudioChannels>((std::clamp)(settings->pad_channels, 0, 2));
	c.input_channels = static_cast<AudioChannels>((std::clamp)(settings->input_channels, 0, 2));
	c.tv_volume = (std::clamp)(settings->tv_volume, 0, 100);
	c.pad_volume = (std::clamp)(settings->pad_volume, 0, 100);
	c.input_volume = (std::clamp)(settings->input_volume, 0, 100);
	c.portal_volume = (std::clamp)(settings->portal_volume, 0, 100);
	c.emulated_usb_devices.emulate_skylander_portal.SetValue(settings->emulate_skylander_portal != 0);
	c.emulated_usb_devices.emulate_infinity_base.SetValue(settings->emulate_infinity_base != 0);
	c.emulated_usb_devices.emulate_dimensions_toypad.SetValue(settings->emulate_dimensions_toypad != 0);
	if (!GetConfigHandle().Save()) {
		ReportError(instance, CEMU_EMBED_STORAGE_FAILED,
			"Cemu could not persist settings.xml in the application data folder.");
		return CEMU_EMBED_STORAGE_FAILED;
	}
	return CEMU_EMBED_OK;
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_EnableDimensionsToypad(
	CemuEmbedInstance* instance, int32_t enabled) {
	if (!instance)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_CREATED)
		return CEMU_EMBED_INVALID_STATE;
	instance->dimensionsToypadEnabled.store(enabled != 0, std::memory_order_release);
	return CEMU_EMBED_OK;
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_EnumerateDimensionsFigures(
	CemuEmbedInstance* instance, CemuEmbedDimensionsFigureCallback callback,
	void* user_data) {
	if (!instance || !callback)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;

	auto enumerate = [&](const auto& figures, CemuEmbedDimensionsFigureType type) {
		for (const auto& [id, name] : figures) {
			CemuEmbedDimensionsFigure figure{
				sizeof(figure), CEMU_EMBED_DIMENSIONS_VERSION, id, type, name
			};
			const auto result = callback(user_data, &figure);
			if (result != CEMU_EMBED_OK)
				return result;
		}
		return CEMU_EMBED_OK;
	};

	if (const auto result = enumerate(nsyshid::DimensionsUSB::GetListMinifigs(),
		CEMU_EMBED_DIMENSIONS_CHARACTER); result != CEMU_EMBED_OK)
		return result;
	return enumerate(nsyshid::DimensionsUSB::GetListTokens(),
		CEMU_EMBED_DIMENSIONS_VEHICLE_OR_GADGET);
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_PlaceDimensionsFigure(
	CemuEmbedInstance* instance, uint32_t figure_id, uint8_t slot) {
	if (!instance || slot >= 7)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY ||
		!instance->dimensionsToypadEnabled.load(std::memory_order_acquire))
		return CEMU_EMBED_INVALID_STATE;

	static constexpr std::array<uint8_t, 7> pads{2, 1, 3, 2, 2, 3, 3};
	try {
		const fs::path tagDirectory = _utf8ToPath(instance->userDataPath) / "dimensions";
		std::error_code directoryError;
		fs::create_directories(tagDirectory, directoryError);
		if (directoryError)
			return CEMU_EMBED_STORAGE_FAILED;
		const fs::path tagPath = tagDirectory /
			fmt::format("slot_{}_figure_{}.bin", slot + 1, figure_id);

		std::error_code fileError;
		if (!fs::is_regular_file(tagPath, fileError) &&
			!nsyshid::g_dimensionstoypad.CreateFigure(tagPath, figure_id))
			return CEMU_EMBED_STORAGE_FAILED;

		std::unique_ptr<FileStream> tagFile(FileStream::openFile2(tagPath, true));
		if (!tagFile)
			return CEMU_EMBED_STORAGE_FAILED;
		std::array<uint8, 0x2D * 0x04> tagData{};
		if (tagFile->readData(tagData.data(), tagData.size()) != tagData.size())
			return CEMU_EMBED_STORAGE_FAILED;

		// Replacing a position must first notify the game that the old tag left.
		nsyshid::g_dimensionstoypad.RemoveFigure(pads[slot], slot, true);
		nsyshid::g_dimensionstoypad.LoadFigure(tagData, std::move(tagFile), pads[slot], slot);
		return CEMU_EMBED_OK;
	} catch (...) {
		return CEMU_EMBED_STORAGE_FAILED;
	}
}

extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_RemoveDimensionsFigure(
	CemuEmbedInstance* instance, uint8_t slot) {
	if (!instance || slot >= 7)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY ||
		!instance->dimensionsToypadEnabled.load(std::memory_order_acquire))
		return CEMU_EMBED_INVALID_STATE;
	static constexpr std::array<uint8_t, 7> pads{2, 1, 3, 2, 2, 3, 3};
	return nsyshid::g_dimensionstoypad.RemoveFigure(pads[slot], slot, true)
		? CEMU_EMBED_OK : CEMU_EMBED_INVALID_STATE;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_MoveDimensionsFigure(
	CemuEmbedInstance* instance, uint8_t source_slot, uint8_t destination_slot) {
	if (!instance || source_slot >= 7 || destination_slot >= 7)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY ||
		!instance->dimensionsToypadEnabled.load(std::memory_order_acquire))
		return CEMU_EMBED_INVALID_STATE;
	if (source_slot == destination_slot)
		return CEMU_EMBED_OK;
	static constexpr std::array<uint8_t, 7> pads{2, 1, 3, 2, 2, 3, 3};
	return nsyshid::g_dimensionstoypad.MoveFigure(
		pads[destination_slot], destination_slot, pads[source_slot], source_slot)
		? CEMU_EMBED_OK : CEMU_EMBED_INVALID_STATE;
}
extern "C" CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_ImportKeys(
	CemuEmbedInstance* instance, const uint8_t* data, uint32_t data_size,
	uint32_t* valid_key_count) {
	if (!instance || !data || data_size == 0 || data_size > 4 * 1024 * 1024)
		return CEMU_EMBED_INVALID_ARGUMENT;
	if (instance->state.load(std::memory_order_acquire) != CEMU_EMBED_STATE_READY)
		return CEMU_EMBED_INVALID_STATE;
	if (CafeSystem::IsTitleRunning())
		return CEMU_EMBED_BUSY;
	// Validate before replacing the existing file so a wrong selection cannot
	// destroy a working keys.txt.
	uint32_t parsedKeyCount{};
	std::string keysText(reinterpret_cast<const char*>(data), data_size);
	std::istringstream lines(keysText);
	std::string line;
	while (std::getline(lines, line)) {
		if (const auto comment = line.find_first_of("#;"); comment != std::string::npos)
			line.resize(comment);
		line.erase(std::remove_if(line.begin(), line.end(), [](char c) {
			return c == ' ' || c == '\t' || c == '\r' || c == '-' || c == '_';
		}), line.end());
		if (line.size() == 32 && std::all_of(line.begin(), line.end(), [](unsigned char c) {
			return std::isxdigit(c) != 0;
		}))
			++parsedKeyCount;
	}
	if (parsedKeyCount == 0)
		return CEMU_EMBED_INVALID_ARGUMENT;

	const fs::path keysPath = ActiveSettings::GetUserDataPath("keys.txt");
	const fs::path temporaryPath = ActiveSettings::GetUserDataPath("keys.txt.importing");
	std::unique_ptr<FileStream> output(FileStream::createFile2(temporaryPath));
	if (!output || output->writeData(data, static_cast<sint32>(data_size)) !=
		static_cast<sint32>(data_size))
		return CEMU_EMBED_STORAGE_FAILED;
	output.reset();

	std::error_code copyError;
	fs::copy_file(temporaryPath, keysPath, fs::copy_options::overwrite_existing, copyError);
	std::error_code cleanupError;
	fs::remove(temporaryPath, cleanupError);
	if (copyError)
		return CEMU_EMBED_STORAGE_FAILED;

	const uint32_t count = KeyCache_Reload();
	if (valid_key_count)
		*valid_key_count = count;
	return count != 0 ? CEMU_EMBED_OK : CEMU_EMBED_INVALID_ARGUMENT;
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
	CafeTitleList::ClearBrokeredTitles();
	ClearExternalVirtualFiles(instance);
	{ std::lock_guard lock(s_instanceMutex); if (s_instance == instance) s_instance = nullptr; }
	if (instance->loggingCallbacksInstalled.load(std::memory_order_acquire)) cemuLog_clearCallbacks();
	delete instance;
}
