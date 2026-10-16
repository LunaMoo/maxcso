// Minimal C API for maxcso, meant for programs that load it at runtime (dlopen / LoadLibrary),
// such as PPSSPP. All paths are UTF-8. Bump MAXCSO_CAPI_VERSION on any incompatible change.
#pragma once

#include <stddef.h>
#include <stdint.h>

// MAXCSO_CAPI_BUILD is defined when building the library itself.
#if defined(_WIN32)
#define MAXCSO_CAPI_CALL __cdecl
#if defined(MAXCSO_CAPI_BUILD)
#define MAXCSO_CAPI_EXPORT __declspec(dllexport)
#else
#define MAXCSO_CAPI_EXPORT
#endif
#else
#define MAXCSO_CAPI_CALL
#if defined(MAXCSO_CAPI_BUILD)
#define MAXCSO_CAPI_EXPORT __attribute__((visibility("default")))
#else
#define MAXCSO_CAPI_EXPORT
#endif
#endif

#define MAXCSO_CAPI_VERSION 2

#ifdef __cplusplus
extern "C" {
#endif

enum maxcso_mode {
	MAXCSO_MODE_DECOMPRESS = 0,
	MAXCSO_MODE_FAST = 1,      // Basic zlib only (like --fast).
	MAXCSO_MODE_STANDARD = 2,  // maxcso defaults (zlib + brute force + 7-zip deflate).
	MAXCSO_MODE_MAXIMUM = 3,   // Defaults + Zopfli + libdeflate (very slow, smallest output).
};

// Return values of maxcso_process / maxcso_process_flags.
enum maxcso_result {
	MAXCSO_RESULT_SUCCESS = 0,
	MAXCSO_RESULT_BAD_INPUT = 1,
	MAXCSO_RESULT_BAD_OUTPUT = 2,
	MAXCSO_RESULT_INVALID_DATA = 3,
	MAXCSO_RESULT_CANNOT_WRITE = 4,
	MAXCSO_RESULT_INVALID_OPTION = 5,
	MAXCSO_RESULT_UNKNOWN = 6,
	MAXCSO_RESULT_CANCELLED = 7,  // The progress callback asked to stop. The partial output is deleted.
};

// Called periodically from the thread that called maxcso_process. Return nonzero to cancel.
// (Version 1 of the API returned void here.)
typedef int (MAXCSO_CAPI_CALL *maxcso_progress_cb)(void *userdata, int64_t pos, int64_t total, int64_t written);

typedef int (MAXCSO_CAPI_CALL *maxcso_capi_version_fn)(void);
typedef const char *(MAXCSO_CAPI_CALL *maxcso_version_fn)(void);
typedef int (MAXCSO_CAPI_CALL *maxcso_process_fn)(const char *input, const char *output, int mode,
	maxcso_progress_cb progress, void *userdata, char *error, size_t error_size);

// Detailed settings for maxcso_process_ex. Added after API version 2 without changing it, so a
// caller probes for maxcso_options_init / maxcso_process_ex and falls back to maxcso_process.
// Always fill it with maxcso_options_init first, then change fields: new fields may be appended,
// and struct_size tells the library which ones the caller knows about.
typedef struct maxcso_options {
	uint32_t struct_size;       // sizeof(maxcso_options), set by maxcso_options_init.
	uint32_t flags;             // Raw maxcso::TaskFlags.
	uint32_t block_size;        // Power of two from 2048 to 262144, or 0xFFFFFFFF = automatic.
	uint32_t zopfli_split_max;  // Zopfli block splits per block, 0 = Zopfli's default (15).
	double precheck_percent;    // 0 = off. Otherwise skip Zopfli and 7-zip on blocks libdeflate
	                            // can't shrink below this percentage of the block size.
} maxcso_options;

typedef int (MAXCSO_CAPI_CALL *maxcso_options_init_fn)(maxcso_options *options, int mode);
typedef int (MAXCSO_CAPI_CALL *maxcso_process_ex_fn)(const char *input, const char *output, const maxcso_options *options,
	maxcso_progress_cb progress, void *userdata, char *error, size_t error_size);

// Returns MAXCSO_CAPI_VERSION.
MAXCSO_CAPI_EXPORT int MAXCSO_CAPI_CALL maxcso_capi_version(void);
// Returns the maxcso version string, e.g. "1.13.0".
MAXCSO_CAPI_EXPORT const char *MAXCSO_CAPI_CALL maxcso_version(void);

// Compresses (to CSO v1) or decompresses (CSO/ZSO/DAX to ISO) a single file. Blocking.
// error may be NULL; otherwise receives a NUL-terminated message on failure.
MAXCSO_CAPI_EXPORT int MAXCSO_CAPI_CALL maxcso_process(const char *input, const char *output, int mode,
	maxcso_progress_cb progress, void *userdata, char *error, size_t error_size);

// Same, but with raw maxcso::TaskFlags and block size (0xFFFFFFFF = automatic).
MAXCSO_CAPI_EXPORT int MAXCSO_CAPI_CALL maxcso_process_flags(const char *input, const char *output, uint32_t flags,
	uint32_t block_size, maxcso_progress_cb progress, void *userdata, char *error, size_t error_size);

// Fills options with the settings maxcso_process uses for mode. Returns MAXCSO_RESULT_SUCCESS,
// or MAXCSO_RESULT_INVALID_OPTION for an unknown mode (options are then zeroed but still valid).
MAXCSO_CAPI_EXPORT int MAXCSO_CAPI_CALL maxcso_options_init(maxcso_options *options, int mode);

// Same as maxcso_process_flags, with all the settings in options.
MAXCSO_CAPI_EXPORT int MAXCSO_CAPI_CALL maxcso_process_ex(const char *input, const char *output, const maxcso_options *options,
	maxcso_progress_cb progress, void *userdata, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
