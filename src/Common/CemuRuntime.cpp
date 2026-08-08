#include "CemuRuntime.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace CemuRuntime
{
namespace
{
std::atomic_bool s_embeddingMode{false};
std::atomic_bool s_outOfMemory{false};
std::mutex s_fatalErrorMutex;
std::string s_fatalError;
}

void SetEmbeddingMode(bool enabled) { s_embeddingMode.store(enabled, std::memory_order_release); }
bool IsEmbeddingMode() { return s_embeddingMode.load(std::memory_order_acquire); }
void ClearFatalError()
{
	s_outOfMemory.store(false, std::memory_order_release);
	std::lock_guard lock(s_fatalErrorMutex);
	s_fatalError.clear();
}
void RecordFatalError(std::string message) { std::lock_guard lock(s_fatalErrorMutex); s_fatalError = std::move(message); }
bool HasFatalError() { std::lock_guard lock(s_fatalErrorMutex); return !s_fatalError.empty(); }
std::string GetFatalError() { std::lock_guard lock(s_fatalErrorMutex); return s_fatalError; }
void RecordOutOfMemory() noexcept { s_outOfMemory.store(true, std::memory_order_release); }
bool HasOutOfMemory() noexcept { return s_outOfMemory.load(std::memory_order_acquire); }

[[noreturn]] void RaiseFatalError(std::string message, int desktopExitCode)
{
	if (IsEmbeddingMode())
	{
		RecordFatalError(message);
		throw FatalError(message);
	}
	std::exit(desktopExitCode);
}
}
