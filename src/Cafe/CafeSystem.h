#pragma once
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/TitleList/TitleId.h"

#include <memory>
#include <string_view>

enum class CosCapabilityBits : uint64;
enum class CosCapabilityGroup : uint32;
enum class CafeConsoleRegion;
class FSCBrokeredFilesystem;

namespace CafeSystem
{
	class SystemImplementation
	{
	public:
		virtual void CafeRecreateCanvas() = 0;
		virtual void CafePPCProcessExit() = 0; // emulated process exited
	};

	enum class PREPARE_STATUS_CODE
	{
		SUCCESS,
		INVALID_RPX,
		UNABLE_TO_MOUNT, // failed to mount through TitleInfo (most likely caused by an invalid or outdated path)
	};

	void Initialize();
	bool EnsureDefaultMLCFiles(const fs::path& mlc);
	void SetImplementation(SystemImplementation* impl);
    void Shutdown();

	PREPARE_STATUS_CODE PrepareForegroundTitle(TitleId titleId);
	PREPARE_STATUS_CODE PrepareForegroundTitleFromStandaloneRPX(const fs::path& path);
	PREPARE_STATUS_CODE PrepareForegroundTitleFromBrokeredStandaloneRPX(
		const std::shared_ptr<FSCBrokeredFilesystem>& filesystem,
		std::string_view executablePath);
	void LaunchForegroundTitle();
	bool IsTitleRunning();

	bool GetOverrideArgStr(std::vector<std::string>& args);
	void SetOverrideArgs(std::span<std::string> args);
	void UnsetOverrideArgs();

	TitleId GetForegroundTitleId();
	uint16 GetForegroundTitleVersion();
	uint32 GetForegroundTitleSDKVersion();
	CafeConsoleRegion GetForegroundTitleRegion();
	CafeConsoleRegion GetPlatformRegion();
	std::string GetForegroundTitleName();
	std::string GetForegroundTitleArgStr();
	uint32 GetForegroundTitleOlvAccesskey();
	CosCapabilityBits GetForegroundTitleCosCapabilities(CosCapabilityGroup group);
	std::optional<sint32> GetForegroundTitleReturnStatus(); // valid once the foreground title exited gracefully via coreinit exit

	void ShutdownTitle();

	std::string GetMlcStoragePath(TitleId titleId);
	void MlcStorageMountAllTitles();

	std::string GetInternalVirtualCodeFolder();

	uint32 GetRPXHashBase();
	uint32 GetRPXHashUpdated();

	void RequestRecreateCanvas();
	void NotifyPPCProcessExit(sint32 status);

};

extern RPLModule* applicationRPX;

extern std::atomic_bool g_isGPUInitFinished;
