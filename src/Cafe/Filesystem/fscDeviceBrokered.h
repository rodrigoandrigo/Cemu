#pragma once

#include "Cafe/Filesystem/fsc.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Read-only file system whose data lives behind a host-owned broker.  It is
// intentionally independent from WinRT so the core remains usable by every
// embedded host.  The host owns the opaque root handle and services every
// read on demand.
class FSCBrokeredFilesystem final
{
public:
	using OpenReadCallback = std::function<bool(std::string_view relativePath, void*& stream)>;
	using ReadCallback = std::function<uint32(void* stream, uint64 offset, uint8* buffer, uint32 size)>;
	using CloseCallback = std::function<void(void* stream)>;

	FSCBrokeredFilesystem(std::string identity, OpenReadCallback openRead,
		ReadCallback read, CloseCallback close);

	bool AddEntry(std::string_view relativePath, bool isDirectory, uint64 size);
	bool ContainsFile(std::string_view relativePath) const;
	bool ContainsDirectory(std::string_view relativePath) const;
	uint64 GetFileSize(std::string_view relativePath) const;
	std::vector<FSCDirEntry> GetDirectoryEntries(std::string_view relativePath) const;
	bool OpenRead(std::string_view relativePath, void*& stream) const;
	uint32 Read(void* stream, uint64 offset, uint8* buffer, uint32 size) const;
	void Close(void* stream) const;
	const std::string& GetIdentity() const { return m_identity; }
	static bool NormalizePath(std::string_view input, std::string& normalized);

private:
	struct Entry
	{
		bool isDirectory{};
		uint64 size{};
		std::string path;
	};

	static std::string MakeKey(std::string_view path);
	void AddImplicitDirectories(std::string_view normalizedPath);

	std::string m_identity;
	OpenReadCallback m_openRead;
	ReadCallback m_read;
	CloseCallback m_close;
	std::unordered_map<std::string, Entry> m_entries;
};

bool FSCDeviceBrokered_Mount(std::string_view mountPath,
	std::string_view destinationBaseDir,
	const std::shared_ptr<FSCBrokeredFilesystem>& filesystem,
	sint32 priority);
