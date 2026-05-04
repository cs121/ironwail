cmake_minimum_required(VERSION 3.7)
include(${CMAKE_CURRENT_SOURCE_DIR}/Misc/cmake/ExtractVersion.cmake)
project(ironwail VERSION ${IRONWAIL_VERSION})

cmake_policy(SET CMP0072 NEW)   #OpenGL_GL_PREFERENCE to GLVND

option(IRONWAIL_STRICT_WARNINGS "Enable stricter compiler warnings for engine code" ON)
option(IRONWAIL_MSVC_SECURE_CRT_WARNINGS "Keep MSVC secure CRT warnings enabled" ON)

if (WIN32)
	list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/Windows/cmake-modules)
endif()

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(CURL)

find_package(PkgConfig)

if (PKG_CONFIG_FOUND)
    pkg_check_modules(PC_MPG123 IMPORTED_TARGET libmpg123)
    pkg_check_modules(PC_MAD IMPORTED_TARGET mad)
    pkg_check_modules(PC_OPUSFILE IMPORTED_TARGET opusfile)
    pkg_check_modules(PC_FLAC IMPORTED_TARGET flac)
    pkg_check_modules(PC_VORBISFILE IMPORTED_TARGET vorbisfile)
    pkg_check_modules(PC_TREMOR IMPORTED_TARGET vorbisidec)
    pkg_check_modules(PC_MIKMOD IMPORTED_TARGET libmikmod)
    pkg_check_modules(PC_MODPLUG IMPORTED_TARGET libmodplug)
    pkg_check_modules(PC_XMP IMPORTED_TARGET libxmp)
endif()

if (WIN32)
	find_package(BundledCODECS)
endif()

file(GLOB_RECURSE IWAIL_SRC CONFIGURE_DEPENDS "Quake/src/*.c" "Quake/src/*.m")
file(GLOB COMMON_SRC CONFIGURE_DEPENDS "common/*.c" "common/*.h")

set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/thirdparty/bc7enc.c PROPERTIES LANGUAGE CXX)

list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/thirdparty/lodepng.c)
list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render/ref_gl_plugin.c)
list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render/ref_gl_bridge_stubs.c)
list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render/ref_vk_plugin.c)
list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render/ref_dx12_plugin.c)

if (UNIX)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/platform/pl_win.c)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/network/net_wipx.c)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/network/net_wins.c)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/network/net_win.c)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/platform/sys_sdl_win.c)
elseif (WIN32)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/platform/pl_linux.c)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/network/net_bsd.c)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/network/net_udp.c)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/platform/sys_sdl_unix.c)
    list(APPEND IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Windows/QuakeSpasm.rc)
endif()

if (PC_MPG123_FOUND)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/audio/snd_mp3.c)
elseif (PC_MAD_FOUND)
    list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/audio/snd_mpg123.c)
endif()

add_executable(ironwail ${IWAIL_SRC} ${COMMON_SRC})
target_compile_features(ironwail PRIVATE c_std_11 cxx_std_17)
target_include_directories(ironwail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/assets;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/audio;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/bot;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/client;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/core;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/gamecode;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/network;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/physics;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/platform;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/server;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/thirdparty;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/ui)
if (UNIX)
	target_link_libraries(ironwail PRIVATE m)
	set(ENABLE_USERDIRS TRUE CACHE STRING "Enable user directories")
elseif (WIN32)
	target_link_libraries(ironwail PRIVATE ws2_32 Winmm)
	if (MINGW)
		#When the GNU linker is used together with SDL2_Main,
		#it is very particular about the library ordering
		#on the link command line
		target_link_libraries(ironwail PRIVATE mingw32)
		if (SDL2_MAIN_LIBRARY)
			target_link_libraries(ironwail PRIVATE SDL2::main)
		endif()
	endif()
endif()
target_link_libraries(ironwail PRIVATE SDL2::SDL2)

if (CURL_FOUND)
	target_link_libraries(ironwail PRIVATE CURL::libcurl)
else()
	target_compile_definitions(ironwail PRIVATE WITHOUT_CURL)
endif()

if (OpenGL::OpenGL)
	target_link_libraries(ironwail PRIVATE OpenGL::OpenGL)
else()
	target_link_libraries(ironwail PRIVATE OpenGL::GL)
endif()

target_compile_definitions(ironwail PRIVATE $<$<CONFIG:Debug>:DEBUG=1> $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1> USE_SDL2 USE_CODEC_WAVE IW_RENDERER_HOST_FRONTEND=1)
if (MSVC)
	target_compile_definitions(ironwail PRIVATE _WIN32)
	if (NOT IRONWAIL_MSVC_SECURE_CRT_WARNINGS)
		target_compile_definitions(ironwail PRIVATE _CRT_SECURE_NO_WARNINGS)
	endif()
endif()

if (IRONWAIL_STRICT_WARNINGS)
	target_compile_definitions(ironwail PRIVATE IRONWAIL_ENFORCE_SAFE_CRT=1)
	if (MSVC)
		target_compile_options(ironwail PRIVATE /W4)
	else()
		target_compile_options(ironwail PRIVATE -Wall -Wextra -Wpedantic)
	endif()
endif()

if (ENABLE_USERDIRS)
	target_compile_definitions(ironwail PRIVATE DO_USERDIRS=1)
endif()

set(REF_GL_SRC
	Quake/src/render/ref_gl_plugin.c
	Quake/src/render/ref_gl_bridge_stubs.c
	Quake/src/render/gl_backend.c
	Quake/src/render/gl_backend_runtime.c
	Quake/src/render/gl_backend_resources.c
	Quake/src/render/gl_draw.c
	Quake/src/render/gl_rmain.c
	Quake/src/render/gl_rmisc.c
	Quake/src/render/gl_screen.c
	Quake/src/platform/gl_vidsdl.c
	Quake/src/render/gl_model.c
	Quake/src/render/gl_model_glb.c
	Quake/src/render/gl_mesh.c
	Quake/src/render/gl_sky.c
	Quake/src/render/gl_warp.c
	Quake/src/render/gl_texmgr.c
	Quake/src/render/gl_shaders.c
	Quake/src/render/gl_shadow.c
	Quake/src/render/gl_shadow_runtime.c
	Quake/src/render/gl_oit.c
	Quake/src/render/gl_lightgrid.c
	Quake/src/render/gl_ktx2.c
	Quake/src/render/gl_refrag.c
	Quake/src/render/gl_rlight.c
	Quake/src/render/gl_fog.c
	Quake/src/render/gl_dlight.c
	Quake/src/render/r_world.c
	Quake/src/render/r_alias.c
	Quake/src/render/r_entitylight.c
	Quake/src/render/r_part.c
	Quake/src/render/r_part_q3p.c
	Quake/src/render/r_decals.c
	Quake/src/render/r_sprite.c
	Quake/src/render/r_brush.c
	Quake/src/render/r_resources_gl.c
	Quake/src/render/r_quality.c
	Quake/src/render/r_sanitize.c
	Quake/src/render/r_ssao.c
	Quake/src/render/r_tonemap.c
	Quake/src/render/r_postfx.c
)

add_library(ref_gl MODULE ${REF_GL_SRC})
set_target_properties(ref_gl PROPERTIES PREFIX "" OUTPUT_NAME "ref_gl")
target_include_directories(ref_gl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/assets;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/audio;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/bot;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/client;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/core;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/gamecode;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/network;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/physics;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/platform;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/server;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/thirdparty;${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/ui)
target_compile_definitions(ref_gl PRIVATE RENDERER_PLUGIN_BUILD USE_SDL2)
if (MSVC)
	target_compile_options(ref_gl PRIVATE /FIref_gl_bridge.h)
else()
	target_compile_options(ref_gl PRIVATE -include ref_gl_bridge.h)
endif()
if (OpenGL::OpenGL)
	target_link_libraries(ref_gl PRIVATE OpenGL::OpenGL)
else()
	target_link_libraries(ref_gl PRIVATE OpenGL::GL)
endif()
target_link_libraries(ref_gl PRIVATE SDL2::SDL2)

if (PC_MPG123_FOUND)
	target_compile_definitions(ironwail PRIVATE USE_CODEC_MP3)
    target_link_libraries(ironwail PUBLIC PkgConfig::PC_MPG123)
elseif (PC_MAD_FOUND)
	target_compile_definitions(ironwail PRIVATE USE_CODEC_MP3)
	target_link_libraries(ironwail PUBLIC PkgConfig::PC_MAD)
endif()

if (PC_OPUSFILE_FOUND)
	target_compile_definitions(ironwail PRIVATE USE_CODEC_OPUS)
    target_link_libraries(ironwail PUBLIC PkgConfig::PC_OPUSFILE)
endif()

if (PC_FLAC_FOUND)
	target_compile_definitions(ironwail PRIVATE USE_CODEC_FLAC)
    target_link_libraries(ironwail PUBLIC PkgConfig::PC_FLAC)
endif()

if (PC_VORBISFILE_FOUND)
	target_compile_definitions(ironwail PRIVATE USE_CODEC_VORBIS)
    target_link_libraries(ironwail PUBLIC PkgConfig::PC_VORBISFILE)
elseif (PC_TREMOR_FOUND)
	target_compile_definitions(ironwail PRIVATE USE_CODEC_VORBIS VORBIS_USE_TREMOR)
    target_link_libraries(ironwail PUBLIC PkgConfig::PC_TREMOR)
endif()

if (PC_MIKMOD_FOUND)
	target_compile_definitions(ironwail PRIVATE USE_CODEC_MIKMOD)
    target_link_libraries(ironwail PUBLIC PkgConfig::PC_MIKMOD)
endif()

if (PC_MODPLUG_FOUND)
	target_compile_definitions(ironwail PRIVATE USE_CODEC_MODPLUG)
    target_link_libraries(ironwail PUBLIC PkgConfig::PC_MODPLUG)
endif()

if (PC_XMP_FOUND)
	target_compile_definitions(ironwail PRIVATE USE_CODEC_XMP)
    target_link_libraries(ironwail PUBLIC PkgConfig::PC_XMP)
endif()

if (PC_MIKMOD_FOUND OR PC_MODPLUG_FOUND OR PC_XMP_FOUND)
    target_compile_definitions(ironwail PRIVATE USE_CODEC_UMX)
endif()

if (WIN32)
	#Copy DLL dependencies to output folder
	add_custom_command(TARGET ironwail POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:ironwail> $<TARGET_FILE_DIR:ironwail>
		COMMAND_EXPAND_LISTS)

	#VORBISFILE and OPUSFILE both use the libogg-0.dll
	if (OGG_DLL AND (PC_VORBISFILE_FOUND OR PC_OPUSFILE_FOUND))
		add_custom_command(TARGET ironwail POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OGG_DLL} $<TARGET_FILE_DIR:ironwail>
			COMMAND_EXPAND_LISTS)
	endif()
	#libvorbisfile depends on libvorbis-0.dll
	if (PC_VORBISFILE_FOUND AND VORBIS_DLL)
		add_custom_command(TARGET ironwail POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VORBIS_DLL} $<TARGET_FILE_DIR:ironwail>
			COMMAND_EXPAND_LISTS)
	endif()
	#libopusfile depends on libopus-0.dll
	if (PC_OPUSFILE_FOUND AND OPUS_DLL)
		add_custom_command(TARGET ironwail POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPUS_DLL} $<TARGET_FILE_DIR:ironwail>
			COMMAND_EXPAND_LISTS)
	endif()
	#CURL wants zlib1.dll
	if (CURL_FOUND AND ZLIB1_DLL)
		add_custom_command(TARGET ironwail POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ZLIB1_DLL} $<TARGET_FILE_DIR:ironwail>
				COMMAND_EXPAND_LISTS)
	endif()

	add_custom_command(TARGET ref_gl POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ref_gl> $<TARGET_FILE_DIR:ironwail>)
endif()
