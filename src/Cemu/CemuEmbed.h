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
#define CEMU_EMBED_BROKERED_STORAGE_VERSION 3u
#define CEMU_EMBED_D3D11_SURFACE_VERSION 1u
#define CEMU_EMBED_LIBRARY_VERSION 2u
#define CEMU_EMBED_ACCOUNT_VERSION 1u
#define CEMU_EMBED_GAMEPAD_VERSION 1u
#define CEMU_EMBED_DIMENSIONS_VERSION 1u
#define CEMU_EMBED_SETTINGS_VERSION 1u
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

// Host-fed Xbox/Windows.Gaming.Input state. Buttons use the SDL gamepad
// layout: South/A=bit 0, East/B=1, West/X=2, North/Y=3, View=4, Menu=6,
// thumbsticks=7/8, shoulders=9/10 and D-pad Up/Down/Left/Right=11..14.
// Stick axes are [-1, 1], triggers are [0, 1].
typedef struct CemuEmbedGamepadState {
	uint32_t struct_size;
	uint32_t abi_version;
	int32_t connected;
	uint32_t buttons;
	float left_x;
	float left_y;
	float right_x;
	float right_y;
	float left_trigger;
	float right_trigger;
} CemuEmbedGamepadState;

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
// Optional direct-copy fast path in version 3. The host may use its brokered
// StorageFile handle to let the platform storage service copy directly into
// the app-owned destination. Returning anything other than OK makes Cemu use
// the open/read fallback for that file.
typedef CemuEmbedResult (CEMU_EMBED_CALL *CemuEmbedBrokeredCopyFileCallback)(
	void* user_data, void* file_handle, const char* destination_path_utf8);

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
	// Optional in version 3. Avoids routing the full file through ABI buffers.
	CemuEmbedBrokeredCopyFileCallback copy_file;
} CemuEmbedBrokeredStorage;

typedef enum CemuEmbedInstallType {
	CEMU_EMBED_INSTALL_AUTO = 0,
	CEMU_EMBED_INSTALL_BASE_GAME = 1,
	CEMU_EMBED_INSTALL_UPDATE = 2,
	CEMU_EMBED_INSTALL_DLC = 3
} CemuEmbedInstallType;

// Strings in this structure remain valid only for the duration of the
// enumeration callback.
typedef struct CemuEmbedInstalledTitle {
	uint32_t struct_size;
	uint32_t abi_version;
	uint64_t title_id;
	uint16_t base_version;
	uint16_t effective_version;
	uint16_t update_version;
	uint16_t dlc_version;
	uint32_t dlc_count;
	uint32_t region;
	const char* name_utf8;
	const char* region_utf8;
	uint32_t compatible_graphic_pack_count;
	uint32_t enabled_graphic_pack_count;
} CemuEmbedInstalledTitle;

typedef CemuEmbedResult (CEMU_EMBED_CALL *CemuEmbedInstalledTitleCallback)(
	void* user_data, const CemuEmbedInstalledTitle* title);

typedef struct CemuEmbedActiveAccount {
	uint32_t struct_size;
	uint32_t abi_version;
	uint32_t persistent_id;
	int32_t online_enabled;
	char mii_name_utf8[64];
	char account_id_utf8[64];
} CemuEmbedActiveAccount;

// User-facing global settings supported by embedded hosts. Integer enum values
// match Cemu's settings.xml representation. Paths, accounts, graphic packs and
// controller profiles have dedicated host APIs and are intentionally excluded.
typedef struct CemuEmbedSettings {
	uint32_t struct_size;
	uint32_t abi_version;
	int32_t cpu_mode;
	int32_t console_language;
	int32_t vsync;
	int32_t gx2drawdone_sync;
	int32_t async_compile;
	int32_t render_upside_down;
	int32_t play_boot_sound;
	int32_t disable_screensaver;
	int32_t override_gamma;
	float override_gamma_value;
	float display_gamma;
	int32_t upscale_filter;
	int32_t downscale_filter;
	int32_t fullscreen_scaling;
	int32_t overlay_position;
	int32_t overlay_text_scale;
	int32_t overlay_fps;
	int32_t overlay_drawcalls;
	int32_t overlay_cpu_usage;
	int32_t overlay_cpu_per_core;
	int32_t overlay_ram_usage;
	int32_t overlay_vram_usage;
	int32_t notification_position;
	int32_t notification_text_scale;
	int32_t notification_controller_profiles;
	int32_t notification_controller_battery;
	int32_t notification_shader_compiling;
	int32_t notification_friends;
	int32_t audio_api;
	int32_t audio_delay;
	int32_t tv_channels;
	int32_t pad_channels;
	int32_t input_channels;
	int32_t tv_volume;
	int32_t pad_volume;
	int32_t input_volume;
	int32_t portal_volume;
	int32_t emulate_skylander_portal;
	int32_t emulate_infinity_base;
	int32_t emulate_dimensions_toypad;
} CemuEmbedSettings;

typedef enum CemuEmbedDimensionsFigureType {
	CEMU_EMBED_DIMENSIONS_CHARACTER = 0,
	CEMU_EMBED_DIMENSIONS_VEHICLE_OR_GADGET = 1
} CemuEmbedDimensionsFigureType;

// Strings remain valid only for the duration of the enumeration callback.
typedef struct CemuEmbedDimensionsFigure {
	uint32_t struct_size;
	uint32_t abi_version;
	uint32_t id;
	CemuEmbedDimensionsFigureType type;
	const char* name_utf8;
} CemuEmbedDimensionsFigure;

typedef CemuEmbedResult (CEMU_EMBED_CALL *CemuEmbedDimensionsFigureCallback)(
	void* user_data, const CemuEmbedDimensionsFigure* figure);

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
// Installs an extracted base game, update or DLC into Cemu's persistent MLC.
// The selected folder must contain code, content and meta. expected_type may
// be AUTO to accept the type declared by app.xml. The operation is synchronous
// and should be invoked outside the host UI dispatcher.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_InstallTitleFromBrokeredFolder(
	CemuEmbedInstance* instance, void* folder_handle,
	const CemuEmbedBrokeredStorage* storage, CemuEmbedInstallType expected_type,
	uint64_t* installed_base_title_id);
// Re-scans the configured MLC and enumerates installed, runnable base games.
// Update and DLC information is folded into the matching base-game record.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_EnumerateInstalledTitles(
	CemuEmbedInstance* instance, CemuEmbedInstalledTitleCallback callback,
	void* user_data);
// Launches a base title from the persistent library. Cemu resolves and mounts
// the highest installed update and matching DLC through CafeTitleList.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_LaunchInstalledTitle(
	CemuEmbedInstance* instance, uint64_t base_title_id);
// Imports Cemu-format graphic packs (rules.txt plus patches/shaders) from a
// brokered graphicPacks folder into the persistent user-data directory.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_InstallGraphicPacksFromBrokeredFolder(
	CemuEmbedInstance* instance, void* folder_handle,
	const CemuEmbedBrokeredStorage* storage, uint32_t* imported_pack_count);
// Enables or disables every loaded graphic pack compatible with a base title.
// Preset defaults declared by each pack are preserved in settings.xml.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetGraphicPacksEnabledForTitle(
	CemuEmbedInstance* instance, uint64_t base_title_id, int32_t enabled,
	uint32_t* affected_pack_count);
// Applies the host-safe automatic policy for a title: compatibility
// Workarounds are enabled, while executable Mods and Cheats are disabled.
// Graphics packs and every other category retain the user's saved state.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_ApplySafeGraphicPackPolicyForTitle(
	CemuEmbedInstance* instance, uint64_t base_title_id,
	uint32_t* affected_pack_count);
// Creates player one's Wii U GamePad profile. On a UWP/Xbox host, the host's
// Windows.Gaming.Input snapshot takes precedence and replaces a stale SDL
// profile copied from a desktop session. Desktop builds use the first SDL
// gamepad, preferring an Xbox device.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_EnsureDefaultGamepadProfile(
	CemuEmbedInstance* instance, int32_t* profile_ready);
// Publishes the latest host-owned gamepad state. This has no WinRT objects in
// its ABI and is safe to call from the XAML/Windows.Gaming.Input thread.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetHostGamepadState(
	CemuEmbedInstance* instance, const CemuEmbedGamepadState* state);
// Publishes a host-owned virtual mouse in physical surface pixels. While it is
// enabled, the UWP SDL path reserves A, L/R and the left stick for the mouse.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetVirtualMouse(
	CemuEmbedInstance* instance, int32_t x, int32_t y,
	int32_t left_down, int32_t enabled);
// Shows or hides Cemu's native performance overlay. The embedded preset uses
// the top-right corner and reports FPS, draw calls, CPU, RAM and VRAM.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetPerformanceMetrics(
	CemuEmbedInstance* instance, int32_t enabled);
// Reads or persists every scalar, user-facing global setting exposed to an
// embedded host. Renderer/backend selection remains host-owned on Xbox.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_GetSettings(
	CemuEmbedInstance* instance, CemuEmbedSettings* settings);
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_SetSettings(
	CemuEmbedInstance* instance, const CemuEmbedSettings* settings);
// Enables the native LEGO Dimensions USB/HID Toy Pad before initialization.
// Embedded hosts must call this while the instance is still in CREATED state.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_EnableDimensionsToypad(
	CemuEmbedInstance* instance, int32_t enabled);
// Enumerates Cemu's native character, vehicle and gadget catalog.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_EnumerateDimensionsFigures(
	CemuEmbedInstance* instance, CemuEmbedDimensionsFigureCallback callback,
	void* user_data);
// Creates (or reopens) a persistent native tag and places it in one of the
// seven physical Toy Pad positions. slot is in the range 0..6.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_PlaceDimensionsFigure(
	CemuEmbedInstance* instance, uint32_t figure_id, uint8_t slot);
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_RemoveDimensionsFigure(
	CemuEmbedInstance* instance, uint8_t slot);
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_MoveDimensionsFigure(
	CemuEmbedInstance* instance, uint8_t source_slot, uint8_t destination_slot);
// Replaces the user-data keys.txt and immediately reloads the decryption-key
// cache. Call while no title is running. valid_key_count may be null.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_ImportKeys(
	CemuEmbedInstance* instance, const uint8_t* data, uint32_t data_size,
	uint32_t* valid_key_count);
// Non-blocking: call this from the host dispatcher. It never creates wxEntry.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_Pump(CemuEmbedInstance* instance);
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_RequestStop(CemuEmbedInstance* instance);
CEMU_EMBED_API CemuEmbedState CEMU_EMBED_CALL CemuEmbed_GetState(const CemuEmbedInstance* instance);
// Returns the account actually selected by Cemu. If settings reference a
// missing account, this reports the valid fallback account used by IOSU/ACT.
CEMU_EMBED_API CemuEmbedResult CEMU_EMBED_CALL CemuEmbed_GetActiveAccount(
	CemuEmbedInstance* instance, CemuEmbedActiveAccount* account);
// The Cemu runtime currently has process-wide singletons, so only one instance
// may be created during a process lifetime.
CEMU_EMBED_API void CEMU_EMBED_CALL CemuEmbed_Destroy(CemuEmbedInstance* instance);

#ifdef __cplusplus
}
#endif
