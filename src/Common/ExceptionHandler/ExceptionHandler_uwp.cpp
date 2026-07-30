#include "ExceptionHandler.h"
#include <Windows.h>
#include <cstdio>

namespace
{
	LONG WINAPI LogUwpUnhandledException(EXCEPTION_POINTERS* exceptionInfo)
	{
		static LONG reported = 0;
		if (InterlockedExchange(&reported, 1) != 0 ||
			!exceptionInfo || !exceptionInfo->ExceptionRecord)
			return EXCEPTION_CONTINUE_SEARCH;

		const auto* record = exceptionInfo->ExceptionRecord;
		const auto moduleBase = reinterpret_cast<uintptr_t>(
			GetModuleHandleW(L"Cemu_release.dll"));
		const auto exceptionAddress =
			reinterpret_cast<uintptr_t>(record->ExceptionAddress);
		const auto relativeAddress = moduleBase && exceptionAddress >= moduleBase
			? exceptionAddress - moduleBase : uintptr_t{};
		const ULONG_PTR accessAddress =
			record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
			record->NumberParameters >= 2
			? record->ExceptionInformation[1] : ULONG_PTR{};

		wchar_t message[1024]{};
#if defined(_M_X64)
		const auto* context = exceptionInfo->ContextRecord;
		swprintf_s(message,
			L"[Cemu/UWP crash] code=0x%08lX address=0x%p "
			L"Cemu_release.dll+0x%llX access=0x%llX "
			L"RIP=0x%llX RSP=0x%llX RAX=0x%llX RBX=0x%llX "
			L"RCX=0x%llX RDX=0x%llX RSI=0x%llX RDI=0x%llX\n",
			record->ExceptionCode, record->ExceptionAddress,
			static_cast<unsigned long long>(relativeAddress),
			static_cast<unsigned long long>(accessAddress),
			context ? context->Rip : 0,
			context ? context->Rsp : 0,
			context ? context->Rax : 0,
			context ? context->Rbx : 0,
			context ? context->Rcx : 0,
			context ? context->Rdx : 0,
			context ? context->Rsi : 0,
			context ? context->Rdi : 0);
#else
		swprintf_s(message,
			L"[Cemu/UWP crash] code=0x%08lX address=0x%p "
			L"Cemu_release.dll+0x%llX access=0x%llX\n",
			record->ExceptionCode, record->ExceptionAddress,
			static_cast<unsigned long long>(relativeAddress),
			static_cast<unsigned long long>(accessAddress));
#endif
		OutputDebugStringW(message);
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

// UWP doesn't support the desktop dbghelp/minidump pipeline, but the
// application-family unhandled-exception filter can still report a stable DLL
// RVA and CPU registers. This makes optimized UWP crashes actionable even when
// Visual Studio could not load a PDB.
void ExceptionHandler_Init()
{
	SetUnhandledExceptionFilter(LogUwpUnhandledException);
}
