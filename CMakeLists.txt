cmake_minimum_required(VERSION 3.21.1)

option(ENABLE_VCPKG "Enable the vcpkg package manager" ON)
option(MACOS_BUNDLE "The executable when built on macOS will be created as an application bundle" OFF)
option(ALLOW_PORTABLE "Allow Cemu to be run in portable mode" ON)
option(CEMU_UWP "Build a DLL compatible with a UWP host" OFF)
set(SDL3_UWP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SDL3-uwp" CACHE PATH
	"SDL3 source tree that provides the Windows.Gaming.Input backend for CEMU_UWP")

# Preview Windows builds can report an OS build newer than the installed SDK.
# Visual Studio generators otherwise try to target that unavailable version and
# fail before compiler detection. The host surface ABI only requires the 26100
# SDK APIs, so pin the embedded build to that installed SDK.
if(CEMU_UWP AND CMAKE_GENERATOR MATCHES "Visual Studio")
	set(CMAKE_SYSTEM_VERSION "10.0.26100.0" CACHE STRING
		"Windows SDK used by the embedded UWP-compatible DLL" FORCE)
endif()

# Do not let an MSYS2 CMake drive an MSVC configuration. CMake 4.4
# from that environment emits an invalid Ninja resource-compiler rule (RC1107)
# while building vcpkg ports such as tiff.
if(CEMU_UWP AND CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_COMMAND MATCHES "[Mm][Ss][Yy][Ss]64")
	message(FATAL_ERROR
		"Configure the MSVC build with the Visual Studio or native CMake executable, not C:/msys64/ucrt64/bin/cmake.exe.")
endif()

# used by CI script to set version:
set(EMULATOR_VERSION_MAJOR "0" CACHE STRING "")
set(EMULATOR_VERSION_MINOR "0" CACHE STRING "")
set(EMULATOR_VERSION_PATCH "0" CACHE STRING "")

set(THREADS_PREFER_PTHREAD_FLAG true) # needs to be set before vcpkg

execute_process(
		COMMAND git log --format=%h -1
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		OUTPUT_VARIABLE GIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
)
add_definitions(-DEMULATOR_HASH=${GIT_HASH})

if (ENABLE_VCPKG)
	# check if vcpkg is shallow and unshallow it if necessary
	execute_process(
	COMMAND git rev-parse --is-shallow-repository
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/dependencies/vcpkg
	OUTPUT_VARIABLE is_vcpkg_shallow
	OUTPUT_STRIP_TRAILING_WHITESPACE
    )

	if(is_vcpkg_shallow STREQUAL "true")
		message(STATUS "vcpkg is shallow. Unshallowing it now...")
		execute_process(
			COMMAND git fetch --unshallow
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dependencies/vcpkg"
			RESULT_VARIABLE result
			OUTPUT_VARIABLE output
		)
	endif()

	if(UNIX AND NOT APPLE)
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_linux;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	elseif(APPLE)
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_mac;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	else()
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_win;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	endif()
	set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake"
		CACHE STRING "Vcpkg toolchain file")
	if(CEMU_UWP)
		# The embedded DLL does not build the wx UI, libusb or the packaged SDL3
		# port. A focused manifest avoids building unsupported/unused desktop UI
		# tooling such as wxrc during configuration.
		set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vcpkg-embed" CACHE PATH "" FORCE)
	endif()
	# Set this so that all the various find_package() calls don't need an explicit
	# CONFIG option
	set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
	if (WIN32)
		if(CEMU_UWP)
			# CemuBin is hosted by an UWP app but remains a DLL rather than an
			# AppContainer executable.  Its dependency graph contains ports which
			# vcpkg explicitly does not support for the WindowsStore triplet.
			set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "" FORCE)
		else()
			set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
		endif()
	endif()
endif()

project(Cemu VERSION 2.0.0)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep Cemu and the desktop vcpkg triplet on the same static CRT. The UWP host
# itself has its own /MD runtime; it communicates through the C ABI only.
set(CEMU_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Internal static targets are linked into CemuBin, which is a shared library.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions($<$<CONFIG:Debug>:CEMU_DEBUG_ASSERT>) # if build type is debug, set CEMU_DEBUG_ASSERT

add_definitions(-DEMULATOR_VERSION_MAJOR=${EMULATOR_VERSION_MAJOR})
add_definitions(-DEMULATOR_VERSION_MINOR=${EMULATOR_VERSION_MINOR})
add_definitions(-DEMULATOR_VERSION_PATCH=${EMULATOR_VERSION_PATCH})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# enable link time optimization for release builds
if(NOT MINGW)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

if (MSVC)
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT CemuBin)
	# floating point model: precise, fiber safe optimizations
	add_compile_options(/EHsc /fp:precise)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		# Speeds up static linking (especially helpful in incremental compilation)
		if((CMAKE_LINKER MATCHES ".*lld-link.*") AND (CMAKE_AR MATCHES ".*llvm-lib.*"))
			set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY STATIC_LIBRARY_OPTIONS /llvmlibthin)
		endif()
  	else()
	  add_compile_options(/GT)
	endif()
	# enable additional optimization flags for release builds
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Oi>) # enable intrinsic functions
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Ot>) # favor speed
endif()

if(CMAKE_GENERATOR STREQUAL "Xcode")
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY XCODE_STARTUP_PROJECT CemuBin)
endif()

if (APPLE)
    enable_language(OBJC OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.4")
endif()

if (UNIX AND NOT APPLE)
	option(ENABLE_WAYLAND "Build with Wayland support" ON)
	option(ENABLE_FERAL_GAMEMODE "Enables Feral Interactive GameMode Support" ON)
	option(ENABLE_BLUEZ "Build with Bluez support" ON)
endif()

if (APPLE)
    set(ENABLE_METAL_DEFAULT ON)
	set(ENABLE_OPENGL_DEFAULT OFF)
else()
    set(ENABLE_METAL_DEFAULT OFF)
	set(ENABLE_OPENGL_DEFAULT ON)
endif()

option(ENABLE_OPENGL "Enables the OpenGL backend" ${ENABLE_OPENGL_DEFAULT})
option(ENABLE_VULKAN "Enables the Vulkan backend" ON)
option(ENABLE_METAL "Enables the Metal backend" ${ENABLE_METAL_DEFAULT})
option(ENABLE_D3D11 "Enables the native Direct3D 11 backend" OFF)
option(ENABLE_DISCORD_RPC "Enables the Discord Rich Presence feature" ON)

if (ENABLE_METAL AND NOT APPLE)
    message(FATAL_ERROR "Metal backend is only supported on Apple platforms")
endif()

# input backends
if (WIN32)
	option(ENABLE_XINPUT "Enables the usage of XInput" ON)
	option(ENABLE_DIRECTINPUT "Enables the usage of DirectInput" ON)
	if(ENABLE_DIRECTINPUT AND NOT CEMU_UWP)
		add_compile_definitions(HAS_DIRECTINPUT)
	endif()
endif()

option(ENABLE_HIDAPI "Build with HIDAPI" ON)
option(ENABLE_SDL "Enables the SDLController backend" ON)
option(ENABLE_LIBUSB "Build with libusb support" ON)

if (ENABLE_LIBUSB AND NOT CEMU_UWP)
	add_compile_definitions(HAS_LIBUSB)
endif()

# audio backends
if (WIN32)
	option(ENABLE_DIRECTAUDIO "Enables the directaudio backend" ${MSVC})
	option(ENABLE_XAUDIO "Enables the xaudio backend" ${MSVC})
endif()
option(ENABLE_CUBEB "Enabled cubeb backend" ON)

option(ENABLE_WXWIDGETS "Build with wxWidgets UI (Currently required)" ON)
option(ENABLE_EMBEDDED_WINDOW_SYSTEM "Build the host-owned window system used by the embedding API" OFF)

if(CEMU_UWP)
	if(NOT WIN32)
		message(FATAL_ERROR "CEMU_UWP requires a Windows target")
	endif()
	# These components import desktop-only APIs. UWP equivalents need dedicated
	# backends, so exclude them from the host-owned DLL for now.
	set(ENABLE_WXWIDGETS OFF CACHE BOOL "" FORCE)
	set(ENABLE_EMBEDDED_WINDOW_SYSTEM ON CACHE BOOL "" FORCE)
	set(ENABLE_DIRECTINPUT OFF CACHE BOOL "" FORCE)
	set(ENABLE_XINPUT OFF CACHE BOOL "" FORCE)
	set(ENABLE_HIDAPI OFF CACHE BOOL "" FORCE)
	set(ENABLE_LIBUSB OFF CACHE BOOL "" FORCE)
	set(ENABLE_CUBEB OFF CACHE BOOL "" FORCE)
	set(ENABLE_DIRECTAUDIO OFF CACHE BOOL "" FORCE)
	if(MSVC)
		set(ENABLE_XAUDIO ON CACHE BOOL "" FORCE)
	else()
		# MSYS2's XAudio header is the desktop/legacy variant. Keep the UCRT64
		# profile useful for source validation without reintroducing it.
		set(ENABLE_XAUDIO OFF CACHE BOOL "" FORCE)
		message(WARNING "CEMU_UWP audio requires the modern XAudio2 header from the Microsoft SDK; use MSVC for the compatible DLL.")
	endif()
	set(ENABLE_DISCORD_RPC OFF CACHE BOOL "" FORCE)
	set(ENABLE_OPENGL OFF CACHE BOOL "" FORCE)
	set(ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
	set(ENABLE_D3D11 ON CACHE BOOL "" FORCE)
	add_compile_definitions(CEMU_UWP)
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(pugixml REQUIRED)
find_package(RapidJSON REQUIRED)
find_package(Boost COMPONENTS program_options filesystem nowide REQUIRED)
find_package(libzip REQUIRED)
find_package(glslang REQUIRED)
find_package(ZLIB REQUIRED)
find_package(zstd MODULE REQUIRED) # MODULE so that zstd::zstd is available
find_package(OpenSSL COMPONENTS Crypto SSL REQUIRED)
find_package(glm REQUIRED)
find_package(fmt 12.1 REQUIRED)
find_package(PNG REQUIRED)

if(ENABLE_SDL)
	# SDL's WinRT backend owns the Windows.Gaming.Input::Gamepad integration.
	# Build it into CemuBin on the real UWP/MSVC path so no desktop SDL import
	# library can introduce XInput or DirectInput imports into the DLL.
	if(CEMU_UWP AND MSVC)
		if(NOT EXISTS "${SDL3_UWP_SOURCE_DIR}/CMakeLists.txt")
			message(FATAL_ERROR "SDL3-UWP source tree was not found: ${SDL3_UWP_SOURCE_DIR}")
		endif()

		# CemuBin is the public DLL.  Keep SDL private/static instead of adding a
		# second deployable DLL to the UWP host package.
		set(SDL_SHARED OFF CACHE BOOL "" FORCE)
		set(SDL_STATIC ON CACHE BOOL "" FORCE)
		set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
		set(SDL_TESTS OFF CACHE BOOL "" FORCE)
		set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
		# Cemu consumes SDL only for Windows.Gaming.Input. Do not build SDL's
		# independent window, audio, haptic, HID, OpenGL/Vulkan or GPU stacks.
		set(SDL_CEMU_UWP ON CACHE BOOL "" FORCE)
		set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
		set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
		set(SDL_RENDER OFF CACHE BOOL "" FORCE)
		set(SDL_GPU OFF CACHE BOOL "" FORCE)
		set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
		set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
		set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
		set(SDL_OFFSCREEN OFF CACHE BOOL "" FORCE)
		set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
		set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
		set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
		set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
		set(SDL_TRAY OFF CACHE BOOL "" FORCE)
		set(SDL_XINPUT OFF CACHE BOOL "" FORCE)
		set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
		add_subdirectory("${SDL3_UWP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/SDL3-uwp" EXCLUDE_FROM_ALL)
		# SDL's CMake project otherwise uses CMake's default (/MD).  CemuBin and
		# the x64-windows-static vcpkg graph use /MT, so mixing them leaves SDL
		# with unresolved __imp_* CRT imports at the final DLL link.
		set_property(TARGET SDL3-static PROPERTY MSVC_RUNTIME_LIBRARY "${CEMU_MSVC_RUNTIME_LIBRARY}")
	elseif(CEMU_UWP)
		# The UCRT64 build is retained for source-level validation only.  SDL3-UWP
		# uses the Microsoft WinRT SDK and is compiled by the MSVC/WindowsStore
		# configuration above.
		message(WARNING
			"CEMU_UWP under a non-MSVC compiler uses the installed SDL3 only for validation; "
			"build the compatible DLL with MSVC and SDL3_UWP_SOURCE_DIR.")
		find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)
	else()
		find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)
	endif()
	add_compile_definitions(HAS_SDL)
endif()

# glslang versions older than 11.11.0 define targets without a namespace
if (NOT TARGET glslang::SPIRV AND TARGET SPIRV)
	add_library(glslang::SPIRV ALIAS SPIRV)
endif()

if (UNIX AND NOT APPLE)
	find_package(X11 REQUIRED)
	if (ENABLE_WAYLAND)
		find_package(Wayland REQUIRED Client)
		find_package(WaylandScanner REQUIRED)
		find_package(WaylandProtocols 1.15 REQUIRED)

		ecm_add_wayland_client_protocol(WAYLAND_PROTOCOL_SRCS
			PROTOCOL "${WaylandProtocols_DATADIR}/stable/viewporter/viewporter.xml"
			BASENAME viewporter)
		add_library(CemuWaylandProtocols STATIC ${WAYLAND_PROTOCOL_SRCS})
		target_include_directories(CemuWaylandProtocols PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
		target_include_directories(CemuWaylandProtocols PRIVATE ${Wayland_INCLUDE_DIRS})
		add_compile_definitions(HAS_WAYLAND)
	endif()
	find_package(GTK3 REQUIRED)

	if(ENABLE_BLUEZ)
		find_package(bluez REQUIRED)
		set(SUPPORTS_WIIMOTE ON)
		add_compile_definitions(HAS_BLUEZ)
	endif()

endif()

if (ENABLE_VULKAN)
	include_directories("dependencies/Vulkan-Headers/include")
	add_compile_definitions(ENABLE_VULKAN)
	add_compile_definitions(VK_NO_PROTOTYPES)
endif()

if (ENABLE_D3D11)
	add_compile_definitions(ENABLE_D3D11)
endif()

if (ENABLE_OPENGL)
	find_package(OpenGL REQUIRED)
	add_compile_definitions(ENABLE_OPENGL)
endif()

if (ENABLE_METAL)
    include_directories(${CMAKE_SOURCE_DIR}/dependencies/metal-cpp)

    add_compile_definitions(ENABLE_METAL)
endif()

if (ENABLE_DISCORD_RPC)
	add_compile_definitions(ENABLE_DISCORD_RPC)
endif()

if (ENABLE_HIDAPI)
	set(SUPPORTS_WIIMOTE ON)
	add_compile_definitions(HAS_HIDAPI)
endif ()

if(UNIX AND NOT APPLE)
	if(ENABLE_FERAL_GAMEMODE)
		add_compile_definitions(ENABLE_FERAL_GAMEMODE)
		add_subdirectory(dependencies/gamemode EXCLUDE_FROM_ALL)
		target_include_directories(gamemode INTERFACE ./dependencies/gamemode/lib)
	endif()
endif()

if (ENABLE_WXWIDGETS)
	find_package(wxWidgets 3.3 REQUIRED COMPONENTS base core gl propgrid xrc)
endif()

if (ENABLE_CUBEB)
	if (NOT ENABLE_VCPKG)
	    find_package(cubeb)
	endif()
	if (NOT cubeb_FOUND)
		option(BUILD_TESTS "" OFF)
		option(BUILD_TOOLS "" OFF)
		option(BUNDLE_SPEEX "" OFF)
		set(USE_WINMM OFF CACHE BOOL "")
		add_subdirectory("dependencies/cubeb" EXCLUDE_FROM_ALL SYSTEM)
		set_property(TARGET cubeb PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		add_library(cubeb::cubeb ALIAS cubeb)
	endif()
	add_compile_definitions("HAS_CUBEB=1")
endif()

add_subdirectory("dependencies/ih264d" EXCLUDE_FROM_ALL)

if (CMAKE_OSX_ARCHITECTURES)
	set(CEMU_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
else()
	set(CEMU_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(CEMU_ARCHITECTURE MATCHES "(aarch64)|(AARCH64)|(arm64)|(ARM64)")
	add_subdirectory("dependencies/xbyak_aarch64" EXCLUDE_FROM_ALL)
endif()

find_package(ZArchive QUIET)
if (NOT ZArchive_FOUND)
	add_subdirectory("dependencies/ZArchive" EXCLUDE_FROM_ALL)
endif()

add_subdirectory(src)
