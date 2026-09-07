#pragma once

#include "Common/precompiled.h"

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

// Process-local path registry for read-only files whose bytes are supplied by
// an embedding host.  Loaders keep using fs::path while all actual I/O remains
// on the original storage device through random-access callbacks.
namespace VirtualFile
{
struct Source
{
	uint64 size{};
	std::function<bool(void*&)> open;
	std::function<uint32(void*, uint64, uint8*, uint32)> read;
	std::function<void(void*)> close;
};

class Stream
{
public:
	Stream(std::shared_ptr<const Source> source, void* handle)
		: m_source(std::move(source)), m_handle(handle) {}
	~Stream()
	{
		if (m_handle)
			m_source->close(m_handle);
	}

	uint64 GetSize() const { return m_source->size; }
	uint32 Read(uint64 offset, void* data, uint32 size) const
	{
		if (!data || offset >= m_source->size)
			return 0;
		const uint32 boundedSize = static_cast<uint32>((std::min)(
			static_cast<uint64>(size), m_source->size - offset));
		return m_source->read(m_handle, offset, static_cast<uint8*>(data), boundedSize);
	}

private:
	std::shared_ptr<const Source> m_source;
	void* m_handle{};
};

inline std::mutex s_mutex;
inline std::unordered_map<fs::path, std::shared_ptr<const Source>> s_sources;

inline bool Register(const fs::path& path, Source source)
{
	if (path.empty() || !source.open || !source.read || !source.close)
		return false;
	std::scoped_lock lock(s_mutex);
	s_sources[path.lexically_normal()] = std::make_shared<Source>(std::move(source));
	return true;
}

inline void Unregister(const fs::path& path)
{
	std::scoped_lock lock(s_mutex);
	s_sources.erase(path.lexically_normal());
}

inline bool Exists(const fs::path& path)
{
	std::scoped_lock lock(s_mutex);
	return s_sources.contains(path.lexically_normal());
}

inline std::unique_ptr<Stream> Open(const fs::path& path)
{
	std::shared_ptr<const Source> source;
	{
		std::scoped_lock lock(s_mutex);
		const auto it = s_sources.find(path.lexically_normal());
		if (it == s_sources.end())
			return nullptr;
		source = it->second;
	}
	void* handle{};
	if (!source->open(handle) || !handle)
		return nullptr;
	return std::make_unique<Stream>(std::move(source), handle);
}
}
