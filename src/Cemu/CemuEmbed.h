#pragma once

// Stable C ABI for application hosts. It deliberately contains no C++ or WinRT
// types, so C++/WinRT, C#, and other hosts can consume the same DLL surface.
#include <stdint.h>

#if defined(_WIN32)
#if defined(CEMU_EMBED_BUILD)
#define CEMU_EMBED_API __declspec(dllexport)
#else
#define CEMU_EMBED_API __declspec(dllimport)
#endif
#define CEMU_EMBED_CALL __cdecl
#else
#define CEMU_EMBED_API __attribute__((visibility("default")))
#define CEMU_EMBED_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CEMU_EMBED_ABI_VERSION 1u
#define CEMU_EMBED_BROKERED_STORAGE_VERSION 2u
#define CEMU_EMBED_D3D11_SURFACE_VERSION 1u
typedef struct CemuEmbedInstance CemuEmbedInstance;

typedef enum CemuEmbedResult { CEMU_EMBED_OK, CEMU_EMBED_INVALID_ARGUMENT, CEMU_EMBED_INVALID_STATE, CEMU_EMBED_BUSY, CEMU_EMBED_INITIALIZATION_FAILED, CEMU_EMBED_LAUNCH_FAILED, CEMU_EMBED_STORAGE_FAILED } CemuEmbedResult;
typedef enum CemuEmbedState { CEMU_EMBED_STATE_CREATED, CEMU_EMBED_STATE_INITIALIZING, CEMU_EMBED_STATE_READY, CEMU_EMBED_STATE_STOPPING, CEMU_EMBED_STATE_STOPPED, CEMU_EMBED_STATE_FAILED } CemuEmbedState;

typedef void (CEMU_EMBED_CALL *CemuEmbedLogCallback)(void* user_data, const char* category_utf8, const char* message_utf8);
typedef void (CEMU_EMBED_CALL *CemuEmbedErrorCallback)(void* user_data, CemuEmbedResult result, const char* message_utf8);
typedef void (CEMU_EMBED_CALL *CemuEmbedStateCallback)(void* user_data, CemuEmbedState state);

typedef struct CemuEmbedCallbacks {
	uint32_t struct_size;
	void* user_data;
	CemuEmbedLogCallback log;
	CemuEmbedErrorCallback error;
	CemuEmbedStateCallback state_changed;
} CemuEmbedCallbacks;

typedef struct CemuEmbedConfig {
	uint32_t struct_size;
	uint32_t abi_version;
	// UTF-8 absolute paths. For UWP, supply LocalFolder or its subdirectories.
	const char* executable_path_utf8;
	const char* user_data_path_utf8;
	const char* config_path_utf8;
	const char* cache_path_utf8;
	const char* data_path_utf8;
} CemuEmbedConfig;

typedef struct CemuEmbedSurface {
	uint32_t struct_size;
	// window remains available for input/window-system integration.
	// canvas points to CemuEmbedD3D11Surface for the D3D11 backend.
	void* window;
	void* canvas;
	int32_t width;
	int32_t height;
	double dpi_scale;
} CemuEmbedSurface;

// Host-owned Direct3D 11 presentation objects. All pointers are COM interface
// pointers and remain owned by the host. The embedded renderer takes its own
// references while active.
typedef struct CemuEmbedD3D11Surface {
	uint32_t struct_size;
	uint32_t abi_version;
	void* device;             // ID3D11Device
	void* immediate_context;  // ID3D11DeviceContext
	void* swap_chain;         // IDXGISwapChain
	void* render_target_view; // ID3D11RenderTargetView
} CemuEmbedD3D11Surface;

// A brokered folder is deliberately represented by host-owned opaque handles.
// This keeps the DLL usable from C, C#, and C++/WinRT without tying its ABI to
// a particular Windows Runtime projection. A UWP host may use StorageFolder
// as folder_handle and StorageFile as each file_handle.
typedef enum CemuEmbedBrokeredEntryType {
	CEMU_EMBED_BROKERED_FILE = 0,
	CEMU_EMBED_BROKERED_DIRECTORY = 1
} CemuEmbedBrokeredEntryType;

typedef CemuEmbedResult (CEMU_EMBED_CALL *CemuEmbedBrokeredEntryCallback)(
	void* callback_user_data, const char* relative_path_utf8,
	CemuEmbedBrokeredEntryType type, uint64_t size, void* file_handle);

// Enumerate every item below folder_handle, recursively. file_handle is used
// only for FILE entries and remains valid until the entry callback returns.
typedef CemuEmbedResult (CEMU_EMBED_CALL *CemuEmbedBrokeredEnumerateCallback)(
	void* user_data, void* folder_handle, CemuEmbedBrokeredEntryCallback entry_callback,
	void* entry_callback_user_data);
typedef CemuEmbedResult (CEMU_EMBED_CALL *CemuEmbedBrokeredOpenReadCallback)(
	void* user_data, void* file_handle, void** stream_handle);
typedef CemuEmbedResult (CEMU_EMBED_CALL *CemuEmbedBrokeredReadCallback)(
	void* user_data, void* stream_handle, uint64_t offset, uint8_t* buffer,
	uint32_t buffer_size, uint32_t* bytes_read);
typedef void (CEMU_EMBED_CALL *CemuEmbedBrokeredCloseCallback)(void* user_data, void* stream_handle);
typedef void (CEMU_EMBED_CALL *CemuEmbedBrokeredProgressCallback)(
	void* user_data, uint64_t bytes_copied, uint64_t total_bytes,
	const char* relative_path_utf8);

typedef struct CemuEmbedBrokeredStorage {
	uint32_t struct_size;
	uint32_t abi_version;
	void* user_data;
	CemuEmbedBrokeredEnumerateCallback enumerate_recursive;
	CemuEmbedBrokeredOpenReadCallback open_read;
	CemuEmbedBrokeredReadCallback read;
	CemuEmbedBrokeredCloseCallback close;
	// Optional in version 2. Called periodically on the thread performing the
	// synchronous staging operation.
	CemuEmbedBrokeredProgressCallback progress;
} CemuEmbedBrokeredStorage;

CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_Create(const CemuEmbedConfig* config, const CemuEmbedCallbacks* callbacks, CemuEmbedInstance** instance);
// Call on the host UI thread before InitializeAsync and whenever the host
// surface size changes while the instance is initializing or ready.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetSurface(CemuEmbedInstance* instance, const CemuEmbedSurface* surface);
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_InitializeAsync(CemuEmbedInstance* instance);
// Launches a Wii U title directory containing the meta, code and content folders.
// Call only after the instance reaches CEMU_EMBED_STATE_READY.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_LaunchGame(CemuEmbedInstance* instance, const char* game_path_utf8);
// Stages a StorageFolder-backed title into Cemu's cache through the supplied
// broker, then launches it. No unrestricted filesystem capability is needed
// for the selected folder. The broker is called synchronously on this thread.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_LaunchGameFromBrokeredFolder(
	CemuEmbedInstance* instance, void* folder_handle, const CemuEmbedBrokeredStorage* storage);
// Non-blocking: call this from the host dispatcher. It never creates wxEntry.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_Pump(CemuEmbedInstance* instance);
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_RequestStop(CemuEmbedInstance* instance);
CEMU_EMBED_API CemuEmbedState CEMU_EMBED_CALL CemuEmbed_GetState(const CemuEmbedInstance* instance);
// The Cemu runtime currently has process-wide singletons, so only one instance
// may be created during a process lifetime.
CEMU_EMBED_API void CEMU_EMBED_CALL CemuEmbed_Destroy(CemuEmbedInstance* instance);

#ifdef __cplusplus
}
#endif
