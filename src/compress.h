#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace maxcso {

static const char *VERSION = "1.13.0";

static const uint32_t DEFAULT_BLOCK_SIZE = 0xFFFFFFFF;

struct Task;

enum TaskStatus {
	TASK_INPROGRESS,
	TASK_SUCCESS,
	TASK_BAD_INPUT,
	TASK_BAD_OUTPUT,
	TASK_INVALID_DATA,
	TASK_CANNOT_WRITE,
	TASK_INVALID_OPTION,
	TASK_CANCELLED,
};

enum TaskFlags {
	TASKFLAG_DEFAULT = 0,

	// Disable certain compression algorithms.
	TASKFLAG_NO_ZLIB = 0x03,
	TASKFLAG_NO_ZLIB_DEFAULT = 0x01,
	TASKFLAG_NO_ZLIB_BRUTE = 0x02,
	TASKFLAG_NO_ZOPFLI = 0x04,
	TASKFLAG_NO_7ZIP = 0x08,
	TASKFLAG_NO_LIBDEFLATE = 0x1000,

	// Disable heuristics and compress all sectors.
	TASKFLAG_FORCE_ALL = 0x10,

	// Flags for new formats.
	TASKFLAG_FMT_ZSO = 0x20,
	TASKFLAG_FMT_CSO_2 = 0x40,
	TASKFLAG_NO_LZ4 = 0x380,
	TASKFLAG_NO_LZ4_DEFAULT = 0x80,
	TASKFLAG_NO_LZ4_HC = 0x100,
	TASKFLAG_NO_LZ4_HC_BRUTE = 0x200,

	TASKFLAG_NO_ALL = TASKFLAG_NO_ZLIB | TASKFLAG_NO_ZOPFLI | TASKFLAG_NO_7ZIP | TASKFLAG_NO_LZ4 | TASKFLAG_NO_LIBDEFLATE,

	TASKFLAG_DECOMPRESS = 0x400,
	TASKFLAG_MEASURE = 0x2000,
	TASKFLAG_FMT_DAX = 0x800,
};

typedef std::function<void (const Task *, TaskStatus status, int64_t pos, int64_t total, int64_t written)> ProgressCallback;
typedef std::function<void (const Task *, TaskStatus status, const char *reason)> ErrorCallback;

struct Task {
	std::string input;
	std::string output;
	ProgressCallback progress;
	ErrorCallback error;
	uint32_t block_size;
	uint32_t flags;
	double orig_max_cost_percent;
	double lz4_max_cost_percent;
	// Zopfli: most deflate blocks it may split each block into. 0 keeps Zopfli's default (15).
	// Larger CSO blocks compress better with more splits.
	uint32_t zopfli_split_max = 0;
	// 0 = off. Otherwise libdeflate (level 12) compresses each block first, and the slow trials
	// (Zopfli, 7-zip) are skipped for blocks it can't get below this percentage of the block size,
	// which saves their time on data that is already compressed (video, audio). Its result also
	// competes as a normal trial, unless TASKFLAG_NO_LIBDEFLATE is set.
	double precheck_percent = 0.0;
	// Optional. Checked on each progress update; returning true stops the task (reported as
	// TASK_CANCELLED through error), leaving the output incomplete.
	std::function<bool()> cancel;

	// Optional. When input_read is set, the input is read through it (input_size bytes) instead
	// of opening input; when output_write is set, the output is written through it instead of
	// opening output, after output_begin (if set) agreed to the uncompressed size by returning
	// true. Both return the bytes transferred or a negative value, and run on libuv's threadpool:
	// output_write may run for several blocks at once, at any offset.
	int64_t input_size = -1;
	std::function<int64_t (int64_t offset, uint8_t *buffer, size_t length)> input_read;
	std::function<bool (int64_t size)> output_begin;
	std::function<int64_t (int64_t offset, const uint8_t *buffer, size_t length)> output_write;
};

void Compress(const std::vector<Task> &tasks);

};
