add_library(CemuAudio
	IAudioAPI.cpp
	IAudioAPI.h
	IAudioInputAPI.cpp
	IAudioInputAPI.h
)

set_property(TARGET CemuAudio PROPERTY MSVC_RUNTIME_LIBRARY "${CEMU_MSVC_RUNTIME_LIBRARY}")

cemu_use_precompiled_header(CemuAudio)

if(WIN32 AND ENABLE_DIRECTAUDIO)
	target_sources(CemuAudio PRIVATE
		DirectSoundAPI.cpp
		DirectSoundAPI.h
	)
	target_compile_definitions(CemuAudio PRIVATE HAS_DIRECTAUDIO)
endif()

if(WIN32 AND ENABLE_XAUDIO)
	target_sources(CemuAudio PRIVATE
		XAudio2API.cpp
		XAudio2API.h
	)
	if(NOT CEMU_UWP)
		target_sources(CemuAudio PRIVATE
			XAudio27API.cpp
			XAudio27API.h
		)
		target_compile_definitions(CemuAudio PRIVATE HAS_XAUDIO27)
	endif()
	target_compile_definitions(CemuAudio PRIVATE HAS_XAUDIO)
endif()

if(ENABLE_CUBEB)
	target_sources(CemuAudio PRIVATE
		CubebAPI.cpp
		CubebAPI.h
		CubebInputAPI.cpp
		CubebInputAPI.h
	)
	#add_compile_definitions(HAS_CUBEB)
endif()

target_include_directories(CemuAudio PUBLIC "../")

target_link_libraries(CemuAudio PRIVATE
	CemuCommon
	CemuGui
)

if(ENABLE_CUBEB)
	# PUBLIC because cubeb.h/cubeb.h is included in CubebAPI.h
	target_link_libraries(CemuAudio PUBLIC cubeb::cubeb)
endif()
