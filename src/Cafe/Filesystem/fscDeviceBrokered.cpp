#include "Cafe/Filesystem/fscDeviceBrokered.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace
{
class FSCDeviceBrokeredFile final : public FSCVirtualFile
{
public:
	FSCDeviceBrokeredFile(std::shared_ptr<FSCBrokeredFilesystem> filesystem,
		std::string relativePath, sint32 type)
		: m_filesystem(std::move(filesystem)), m_relativePath(std::move(relativePath)), m_type(type)
	{
	}

	~FSCDeviceBrokeredFile() override
	{
		if (m_stream)
			m_filesystem->Close(m_stream);
	}

	sint32 fscGetType() override
	{
		return m_type;
	}

	uint64 fscQueryValueU64(uint32 id) override
	{
		if (m_type != FSC_TYPE_FILE)
			return 0;
		if (id == FSC_QUERY_SIZE)
			return m_filesystem->GetFileSize(m_relativePath);
		if (id == FSC_QUERY_WRITEABLE)
			return 0;
		return 0;
	}

	uint32 fscWriteData(void*, uint32) override
	{
		return 0;
	}

	uint32 fscReadData(void* buffer, uint32 size) override
	{
		if (m_type != FSC_TYPE_FILE || !buffer || size == 0)
			return 0;
		if (!m_stream && !m_filesystem->OpenRead(m_relativePath, m_stream))
			return 0;
		const uint64 fileSize = m_filesystem->GetFileSize(m_relativePath);
		if (m_seek >= fileSize)
			return 0;
		const uint32 toRead = static_cast<uint32>((std::min)(
			static_cast<uint64>(size), fileSize - m_seek));
		const uint32 read = m_filesystem->Read(m_stream, m_seek,
			static_cast<uint8*>(buffer), toRead);
		if (read <= toRead)
			m_seek += read;
		return read <= toRead ? read : 0;
	}

	void fscSetSeek(uint64 seek) override
	{
		if (m_type == FSC_TYPE_FILE)
			m_seek = seek;
	}

	uint64 fscGetSeek() override
	{
		return m_type == FSC_TYPE_FILE ? m_seek : 0;
	}

	void fscSetFileLength(uint64) override
	{
		// The brokered source is deliberately read-only.
	}

	bool fscDirNext(FSCDirEntry* dirEntry) override
	{
		if (m_type != FSC_TYPE_DIRECTORY || !dirEntry)
			return false;
		if (!dirIterator)
		{
			dirIterator = new FSCDirIteratorState{};
			dirIterator->dirEntries = m_filesystem->GetDirectoryEntries(m_relativePath);
		}
		if (dirIterator->index < 0 ||
			static_cast<size_t>(dirIterator->index) >= dirIterator->dirEntries.size())
			return false;
		*dirEntry = dirIterator->dirEntries[dirIterator->index++];
		return true;
	}

	bool fscRewindDir() override
	{
		if (dirIterator)
			dirIterator->index = 0;
		return true;
	}

private:
	std::shared_ptr<FSCBrokeredFilesystem> m_filesystem;
	std::string m_relativePath;
	sint32 m_type{};
	uint64 m_seek{};
	void* m_stream{};
};

class FSCDeviceBrokered final : public fscDeviceC
{
public:
	FSCVirtualFile* fscDeviceOpenByPath(std::string_view path,
		FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
	{
		auto* filesystem = static_cast<std::shared_ptr<FSCBrokeredFilesystem>*>(ctx);
		if (!filesystem || !*filesystem || !fscStatus ||
			HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION) ||
			HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALLOW_CREATE) ||
			HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALWAYS_CREATE))
		{
			if (fscStatus)
				*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return nullptr;
		}

		std::string normalized;
		if (!FSCBrokeredFilesystem::NormalizePath(path, normalized))
		{
			*fscStatus = FSC_STATUS_INVALID_PATH;
			return nullptr;
		}
		if ((*filesystem)->ContainsFile(normalized))
		{
			if (!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
			{
				*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
				return nullptr;
			}
			*fscStatus = FSC_STATUS_OK;
			return new FSCDeviceBrokeredFile(*filesystem, std::move(normalized), FSC_TYPE_FILE);
		}
		if ((*filesystem)->ContainsDirectory(normalized))
		{
			if (!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
			{
				*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
				return nullptr;
			}
			*fscStatus = FSC_STATUS_OK;
			return new FSCDeviceBrokeredFile(*filesystem, std::move(normalized), FSC_TYPE_DIRECTORY);
		}
		*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
		return nullptr;
	}

	static FSCDeviceBrokered& Instance()
	{
		static FSCDeviceBrokered instance;
		return instance;
	}
};
}

FSCBrokeredFilesystem::FSCBrokeredFilesystem(std::string identity,
	OpenReadCallback openRead, ReadCallback read, CloseCallback close)
	: m_identity(std::move(identity)), m_openRead(std::move(openRead)),
	m_read(std::move(read)), m_close(std::move(close))
{
	m_entries.emplace("", Entry{true, 0, ""});
}

bool FSCBrokeredFilesystem::NormalizePath(std::string_view input, std::string& normalized)
{
	normalized.clear();
	for (const char value : input)
	{
		const char separator = value == '\\' ? '/' : value;
		if (separator == '/')
		{
			if (normalized.empty() || normalized.back() == '/')
				continue;
			normalized.push_back(separator);
			continue;
		}
		if (static_cast<unsigned char>(separator) < 0x20)
			return false;
		normalized.push_back(separator);
	}
	while (!normalized.empty() && normalized.back() == '/')
		normalized.pop_back();
	if (normalized.empty())
		return input.empty() || input == ".";
	if (normalized.front() == '/' || normalized.find(':') != std::string::npos)
		return false;
	for (size_t begin = 0; begin <= normalized.size();)
	{
		const size_t end = normalized.find('/', begin);
		const std::string_view component(normalized.data() + begin,
			(end == std::string::npos ? normalized.size() : end) - begin);
		if (component.empty() || component == "." || component == "..")
			return false;
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return true;
}

std::string FSCBrokeredFilesystem::MakeKey(std::string_view path)
{
	std::string key(path);
	std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	return key;
}

void FSCBrokeredFilesystem::AddImplicitDirectories(std::string_view normalizedPath)
{
	for (size_t separator = normalizedPath.find('/'); separator != std::string::npos;
		separator = normalizedPath.find('/', separator + 1))
	{
		const std::string parent(normalizedPath.substr(0, separator));
		m_entries.try_emplace(MakeKey(parent), Entry{true, 0, parent});
	}
}

bool FSCBrokeredFilesystem::AddEntry(std::string_view relativePath, bool isDirectory, uint64 size)
{
	std::string normalized;
	if (!NormalizePath(relativePath, normalized) || normalized.empty())
		return false;
	AddImplicitDirectories(normalized);
	const std::string key = MakeKey(normalized);
	auto it = m_entries.find(key);
	if (it != m_entries.end())
	{
		if (it->second.isDirectory != isDirectory)
			return false;
		if (!isDirectory)
			it->second.size = size;
		return true;
	}
	m_entries.emplace(key, Entry{isDirectory, isDirectory ? 0 : size, std::move(normalized)});
	return true;
}

bool FSCBrokeredFilesystem::ContainsFile(std::string_view relativePath) const
{
	std::string normalized;
	if (!NormalizePath(relativePath, normalized))
		return false;
	const auto it = m_entries.find(MakeKey(normalized));
	return it != m_entries.end() && !it->second.isDirectory;
}

bool FSCBrokeredFilesystem::ContainsDirectory(std::string_view relativePath) const
{
	std::string normalized;
	if (!NormalizePath(relativePath, normalized))
		return false;
	const auto it = m_entries.find(MakeKey(normalized));
	return it != m_entries.end() && it->second.isDirectory;
}

uint64 FSCBrokeredFilesystem::GetFileSize(std::string_view relativePath) const
{
	std::string normalized;
	if (!NormalizePath(relativePath, normalized))
		return 0;
	const auto it = m_entries.find(MakeKey(normalized));
	return it != m_entries.end() && !it->second.isDirectory ? it->second.size : 0;
}

std::vector<FSCDirEntry> FSCBrokeredFilesystem::GetDirectoryEntries(std::string_view relativePath) const
{
	std::string normalized;
	if (!NormalizePath(relativePath, normalized) || !ContainsDirectory(normalized))
		return {};
	const std::string prefix = normalized.empty() ? "" : normalized + "/";
	std::vector<FSCDirEntry> entries;
	for (const auto& [_, entry] : m_entries)
	{
		if (entry.path.empty() || entry.path.rfind(prefix, 0) != 0)
			continue;
		const std::string_view child(entry.path.data() + prefix.size(), entry.path.size() - prefix.size());
		if (child.find('/') != std::string::npos)
			continue;
		FSCDirEntry dirEntry{};
		dirEntry.isDirectory = entry.isDirectory;
		dirEntry.isFile = !entry.isDirectory;
		dirEntry.fileSize = static_cast<uint32>((std::min)(entry.size, static_cast<uint64>(UINT32_MAX)));
		std::strncpy(dirEntry.path, entry.path.data() + prefix.size(), sizeof(dirEntry.path) - 1);
		entries.emplace_back(dirEntry);
	}
	std::sort(entries.begin(), entries.end(), [](const FSCDirEntry& left, const FSCDirEntry& right) {
		return std::strcmp(left.path, right.path) < 0;
	});
	return entries;
}

std::vector<std::pair<std::string, uint64>> FSCBrokeredFilesystem::GetFiles() const
{
	std::vector<std::pair<std::string, uint64>> files;
	files.reserve(m_entries.size());
	for (const auto& [_, entry] : m_entries)
		if (!entry.isDirectory)
			files.emplace_back(entry.path, entry.size);
	return files;
}

bool FSCBrokeredFilesystem::OpenRead(std::string_view relativePath, void*& stream) const
{
	stream = nullptr;
	return m_openRead && ContainsFile(relativePath) && m_openRead(relativePath, stream) && stream;
}

uint32 FSCBrokeredFilesystem::Read(void* stream, uint64 offset, uint8* buffer, uint32 size) const
{
	return stream && m_read ? m_read(stream, offset, buffer, size) : 0;
}

void FSCBrokeredFilesystem::Close(void* stream) const
{
	if (stream && m_close)
		m_close(stream);
}

bool FSCDeviceBrokered_Mount(std::string_view mountPath,
	std::string_view destinationBaseDir,
	const std::shared_ptr<FSCBrokeredFilesystem>& filesystem, sint32 priority)
{
	if (!filesystem)
		return false;
	return fsc_mount(mountPath, destinationBaseDir, &FSCDeviceBrokered::Instance(),
		const_cast<std::shared_ptr<FSCBrokeredFilesystem>*>(&filesystem), priority) == FSC_STATUS_OK;
}
