#pragma once

#include <stdexcept>
#include <string>

namespace CemuRuntime
{
	class FatalError final : public std::runtime_error
	{
	public:
		explicit FatalError(const std::string& message) : std::runtime_error(message) {}
	};

	void SetEmbeddingMode(bool enabled);
	bool IsEmbeddingMode();
	void ClearFatalError();
	void RecordFatalError(std::string message);
	bool HasFatalError();
	std::string GetFatalError();
	void RecordOutOfMemory() noexcept;
	bool HasOutOfMemory() noexcept;
	[[noreturn]] void RaiseFatalError(std::string message, int desktopExitCode = -1);
}
