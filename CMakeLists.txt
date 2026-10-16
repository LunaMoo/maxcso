cmake_minimum_required(VERSION 3.16)
project(maxcso VERSION 1.13.0 LANGUAGES C CXX)

# Builds the mCSO library (C API from capi/maxcso_capi.h) and the maxcso command line tool from the
# bundled sources only, so the same build works on Windows, Linux, macOS, Android and iOS without any
# system packages.  The library is named mCSO on every platform: mCSO.dll / mCSO.so / mCSO.dylib
# (shared) or mCSO.lib / mCSO.a (static).

get_directory_property(MCSO_HAS_PARENT PARENT_DIRECTORY)
if(MCSO_HAS_PARENT)
	set(MCSO_TOPLEVEL OFF)
else()
	set(MCSO_TOPLEVEL ON)
endif()

if(ANDROID OR IOS OR CMAKE_SYSTEM_NAME MATCHES "iOS|tvOS|visionOS")
	set(MCSO_MOBILE ON)
else()
	set(MCSO_MOBILE OFF)
endif()

if(MCSO_MOBILE OR NOT MCSO_TOPLEVEL)
	set(MCSO_CLI_DEFAULT OFF)
else()
	set(MCSO_CLI_DEFAULT ON)
endif()

option(MCSO_SHARED "Build mCSO as a shared library (only the C API is exported)" ON)
option(MCSO_BUILD_CLI "Build the maxcso command line tool" ${MCSO_CLI_DEFAULT})
option(MCSO_BUILD_TESTS "Build the C API round-trip test" ${MCSO_TOPLEVEL})

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Keep the bundled libraries' symbols out of the shared library's export table.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(MSVC)
	set(MCSO_WARN_OFF /w)
	# Static CRT, like the Visual Studio solution, so mCSO.dll has no runtime dependency.
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
	set(MCSO_WARN_OFF -w)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Bundled dependencies
# ---------------------------------------------------------------------------

# libuv (static) -------------------------------------------------------------
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
# The bundled libuv asks for cmake_minimum_required(VERSION 3.4), which CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(libuv EXCLUDE_FROM_ALL)
target_compile_options(uv_a PRIVATE ${MCSO_WARN_OFF})
if(TARGET uv)
	target_compile_options(uv PRIVATE ${MCSO_WARN_OFF})
endif()

# zlib -------------------------------------------------------------------------
add_library(mcso_zlib STATIC
	zlib/adler32.c zlib/compress.c zlib/crc32.c zlib/deflate.c zlib/infback.c
	zlib/inffast.c zlib/inflate.c zlib/inftrees.c zlib/trees.c zlib/uncompr.c zlib/zutil.c)
target_include_directories(mcso_zlib PUBLIC zlib)
target_compile_definitions(mcso_zlib PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:HAVE_UNISTD_H>)
target_compile_options(mcso_zlib PRIVATE ${MCSO_WARN_OFF})

# lz4 --------------------------------------------------------------------------
add_library(mcso_lz4 STATIC lz4/lib/lz4.c lz4/lib/lz4frame.c lz4/lib/lz4hc.c lz4/lib/xxhash.c)
target_include_directories(mcso_lz4 PUBLIC lz4/lib)
target_compile_options(mcso_lz4 PRIVATE ${MCSO_WARN_OFF})

# libdeflate -------------------------------------------------------------------
add_library(mcso_libdeflate STATIC
	libdeflate/lib/adler32.c libdeflate/lib/crc32.c
	libdeflate/lib/deflate_compress.c libdeflate/lib/deflate_decompress.c
	libdeflate/lib/gzip_compress.c libdeflate/lib/gzip_decompress.c
	libdeflate/lib/zlib_compress.c libdeflate/lib/zlib_decompress.c
	libdeflate/lib/utils.c
	libdeflate/lib/arm/cpu_features.c libdeflate/lib/x86/cpu_features.c)
target_include_directories(mcso_libdeflate PUBLIC libdeflate)
target_compile_options(mcso_libdeflate PRIVATE ${MCSO_WARN_OFF})

# zopfli -----------------------------------------------------------------------
set(ZOPFLI_DIR zopfli/src/zopfli)
add_library(mcso_zopfli STATIC
	${ZOPFLI_DIR}/blocksplitter.c ${ZOPFLI_DIR}/cache.c ${ZOPFLI_DIR}/deflate.c
	${ZOPFLI_DIR}/gzip_container.c ${ZOPFLI_DIR}/hash.c ${ZOPFLI_DIR}/katajainen.c
	${ZOPFLI_DIR}/lz77.c ${ZOPFLI_DIR}/squeeze.c ${ZOPFLI_DIR}/tree.c ${ZOPFLI_DIR}/util.c
	${ZOPFLI_DIR}/zlib_container.c ${ZOPFLI_DIR}/zopfli_lib.c)
target_include_directories(mcso_zopfli PUBLIC zopfli/src)
target_compile_options(mcso_zopfli PRIVATE ${MCSO_WARN_OFF})
if(NOT MSVC)
	target_link_libraries(mcso_zopfli PRIVATE m)
endif()

# 7-zip deflate ----------------------------------------------------------------
set(SZ 7zip)
add_library(mcso_7zip STATIC
	${SZ}/CPP/7zip/Archive/Common/ParseProperties.cpp
	${SZ}/CPP/7zip/Archive/DeflateProps.cpp
	${SZ}/CPP/7zip/Common/CWrappers.cpp
	${SZ}/CPP/7zip/Common/FileStreams.cpp
	${SZ}/CPP/7zip/Common/InBuffer.cpp
	${SZ}/CPP/7zip/Common/OutBuffer.cpp
	${SZ}/CPP/7zip/Common/StreamUtils.cpp
	${SZ}/CPP/7zip/Compress/BitlDecoder.cpp
	${SZ}/CPP/7zip/Compress/DeflateDecoder.cpp
	${SZ}/CPP/7zip/Compress/DeflateEncoder.cpp
	${SZ}/CPP/7zip/Compress/LzOutWindow.cpp
	${SZ}/CPP/7zip/Compress/ZlibDecoder.cpp
	${SZ}/CPP/7zip/Compress/ZlibEncoder.cpp
	${SZ}/CPP/Common/MyString.cpp
	${SZ}/CPP/Common/StringConvert.cpp
	${SZ}/CPP/Common/StringToInt.cpp
	${SZ}/CPP/Windows/PropVariant.cpp
	${SZ}/C/Alloc.c ${SZ}/C/HuffEnc.c ${SZ}/C/LzFind.c ${SZ}/C/Sort.c
	${SZ}/deflate7z.cpp)
if(WIN32)
	target_sources(mcso_7zip PRIVATE ${SZ}/CPP/Windows/FileIO.cpp)
	target_compile_definitions(mcso_7zip PRIVATE UNICODE _UNICODE)
else()
	target_sources(mcso_7zip PRIVATE ${SZ}/CPP/NotWindows/FileIO.cpp ${SZ}/CPP/NotWindows/MyWindows.cpp)
endif()
target_include_directories(mcso_7zip PUBLIC ${SZ} PRIVATE ${SZ}/CPP ${SZ}/CPP/7zip)
target_compile_options(mcso_7zip PRIVATE ${MCSO_WARN_OFF})

# ---------------------------------------------------------------------------
# maxcso core (shared by the library and the CLI)
# ---------------------------------------------------------------------------

add_library(mcso_core STATIC
	src/buffer_pool.cpp src/checksum.cpp src/compress.cpp src/input.cpp
	src/output.cpp src/sector.cpp src/uv_helper.cpp)
target_include_directories(mcso_core PUBLIC src)
target_compile_definitions(mcso_core PUBLIC NDEBUG=1)
target_link_libraries(mcso_core PUBLIC uv_a mcso_zlib mcso_lz4 mcso_libdeflate mcso_zopfli mcso_7zip Threads::Threads)
if(MSVC)
	target_compile_options(mcso_core PRIVATE /W3 /wd4244 /wd4267)
else()
	target_compile_options(mcso_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)
endif()

# ---------------------------------------------------------------------------
# mCSO library
# ---------------------------------------------------------------------------

if(MCSO_SHARED)
	add_library(mcso SHARED capi/maxcso_capi.cpp)
	if(APPLE)
		set(MCSO_LINK_EXPORTS "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/capi/exports.txt")
	elseif(NOT WIN32)
		set(MCSO_LINK_EXPORTS "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/capi/exports.map"
			"LINKER:--no-undefined")
	endif()
	target_link_options(mcso PRIVATE ${MCSO_LINK_EXPORTS})
	set_target_properties(mcso PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/capi/exports.map")
	if(APPLE)
		set_target_properties(mcso PROPERTIES INSTALL_NAME_DIR "@rpath")
	endif()
else()
	add_library(mcso STATIC capi/maxcso_capi.cpp)
endif()
target_compile_definitions(mcso PRIVATE MAXCSO_CAPI_BUILD)
target_include_directories(mcso PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/capi> $<INSTALL_INTERFACE:include>)
target_link_libraries(mcso PRIVATE mcso_core)
# mCSO.dll / mCSO.so / mCSO.dylib / mCSO.lib / mCSO.a everywhere, without the platform's "lib" prefix.
set_target_properties(mcso PROPERTIES OUTPUT_NAME mCSO PREFIX "" IMPORT_PREFIX "")
add_library(maxcso::mCSO ALIAS mcso)

# ---------------------------------------------------------------------------
# CLI and test
# ---------------------------------------------------------------------------

if(MCSO_BUILD_CLI)
	add_executable(maxcso_cli cli/cli.cpp)
	if(WIN32)
		target_sources(maxcso_cli PRIVATE cli/winargs.cpp cli/winglob.cpp)
		target_compile_definitions(maxcso_cli PRIVATE UNICODE _UNICODE)
		target_link_libraries(maxcso_cli PRIVATE uuid)
	endif()
	target_link_libraries(maxcso_cli PRIVATE mcso_core)
	set_target_properties(maxcso_cli PROPERTIES OUTPUT_NAME maxcso)
	install(TARGETS maxcso_cli RUNTIME DESTINATION bin)
endif()

if(MCSO_BUILD_TESTS AND NOT MCSO_MOBILE)
	enable_testing()
	add_executable(mcso_capi_test tests/capi_test.c)
	target_link_libraries(mcso_capi_test PRIVATE mcso)
	set_target_properties(mcso_capi_test PROPERTIES LINKER_LANGUAGE CXX)
	add_test(NAME capi_roundtrip COMMAND mcso_capi_test ${CMAKE_CURRENT_BINARY_DIR})
endif()

install(TARGETS mcso ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES capi/maxcso_capi.h DESTINATION include)
