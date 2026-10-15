#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <uv.h>
#include "maxcso_capi.h"
#include "../src/compress.h"

namespace {

void EnsureThreadpoolSize() {
	static std::once_flag once;
	std::call_once(once, [] {
		// libuv reads this once, when its threadpool starts. Respect a user override.
		if (getenv("UV_THREADPOOL_SIZE") != nullptr)
			return;
		unsigned int n = std::thread::hardware_concurrency();
		if (n == 0)
			n = 4;
		std::string value = std::to_string(n);
#ifdef _WIN32
		_putenv_s("UV_THREADPOOL_SIZE", value.c_str());
#else
		setenv("UV_THREADPOOL_SIZE", value.c_str(), 0);
#endif
	});
}

uint32_t FlagsForMode(int mode, bool *ok) {
	*ok = true;
	// CSO v1 base, same as the CLI: no lz4, zopfli or libdeflate unless asked for.
	const uint32_t base = maxcso::TASKFLAG_NO_ZOPFLI | maxcso::TASKFLAG_NO_LIBDEFLATE | maxcso::TASKFLAG_NO_LZ4;
	switch (mode) {
	case MAXCSO_MODE_DECOMPRESS:
		return base | maxcso::TASKFLAG_DECOMPRESS;
	case MAXCSO_MODE_FAST:
		return base | maxcso::TASKFLAG_NO_ZLIB_BRUTE | maxcso::TASKFLAG_NO_7ZIP;
	case MAXCSO_MODE_STANDARD:
		return base;
	case MAXCSO_MODE_MAXIMUM:
		return base & ~(uint32_t)(maxcso::TASKFLAG_NO_ZOPFLI | maxcso::TASKFLAG_NO_LIBDEFLATE);
	default:
		*ok = false;
		return 0;
	}
}

int ToResult(maxcso::TaskStatus status) {
	switch (status) {
	case maxcso::TASK_SUCCESS: return MAXCSO_RESULT_SUCCESS;
	case maxcso::TASK_BAD_INPUT: return MAXCSO_RESULT_BAD_INPUT;
	case maxcso::TASK_BAD_OUTPUT: return MAXCSO_RESULT_BAD_OUTPUT;
	case maxcso::TASK_INVALID_DATA: return MAXCSO_RESULT_INVALID_DATA;
	case maxcso::TASK_CANNOT_WRITE: return MAXCSO_RESULT_CANNOT_WRITE;
	case maxcso::TASK_INVALID_OPTION: return MAXCSO_RESULT_INVALID_OPTION;
	case maxcso::TASK_CANCELLED: return MAXCSO_RESULT_CANCELLED;
	default: return MAXCSO_RESULT_UNKNOWN;
	}
}

// UTF-8 path delete that works on Windows too.
void RemoveOutput(const char *path) {
	uv_loop_t loop;
	if (uv_loop_init(&loop) != 0)
		return;
	uv_fs_t req;
	uv_fs_unlink(&loop, &req, path, nullptr);
	uv_fs_req_cleanup(&req);
	uv_loop_close(&loop);
}

void CopyError(char *error, size_t error_size, const char *msg) {
	if (!error || error_size == 0)
		return;
	if (!msg)
		msg = "";
	strncpy(error, msg, error_size - 1);
	error[error_size - 1] = '\0';
}

}  // namespace

extern "C" {

int MAXCSO_CAPI_CALL maxcso_capi_version(void) {
	return MAXCSO_CAPI_VERSION;
}

const char *MAXCSO_CAPI_CALL maxcso_version(void) {
	return maxcso::VERSION;
}

int MAXCSO_CAPI_CALL maxcso_process_flags(const char *input, const char *output, uint32_t flags,
	uint32_t block_size, maxcso_progress_cb progress, void *userdata, char *error, size_t error_size) {
	CopyError(error, error_size, "");
	if (!input || !output || !*input || !*output) {
		CopyError(error, error_size, "Missing input or output path");
		return MAXCSO_RESULT_INVALID_OPTION;
	}

	EnsureThreadpoolSize();

	bool finished = false;
	maxcso::TaskStatus finalStatus = maxcso::TASK_INPROGRESS;
	std::string reason;

	maxcso::Task task;
	task.input = input;
	task.output = output;
	task.block_size = block_size;
	task.flags = flags;
	task.orig_max_cost_percent = 0.0;
	task.lz4_max_cost_percent = 0.0;
	bool cancelRequested = false;
	task.progress = [&](const maxcso::Task *, maxcso::TaskStatus status, int64_t pos, int64_t total, int64_t written) {
		if (status == maxcso::TASK_INPROGRESS) {
			if (progress && progress(userdata, pos, total, written) != 0)
				cancelRequested = true;
		} else {
			finished = true;
			finalStatus = status;
		}
	};
	task.cancel = [&]() {
		return cancelRequested;
	};
	task.error = [&](const maxcso::Task *, maxcso::TaskStatus status, const char *msg) {
		finished = true;
		finalStatus = status;
		reason = msg ? msg : "";
	};

	std::vector<maxcso::Task> tasks;
	tasks.push_back(std::move(task));
	maxcso::Compress(tasks);

	if (!finished) {
		CopyError(error, error_size, "Task did not complete");
		return MAXCSO_RESULT_UNKNOWN;
	}
	int result = ToResult(finalStatus);
	if (result != MAXCSO_RESULT_SUCCESS)
		CopyError(error, error_size, reason.empty() ? "Unknown error" : reason.c_str());
	if (result == MAXCSO_RESULT_CANCELLED)
		RemoveOutput(output);
	return result;
}

int MAXCSO_CAPI_CALL maxcso_process(const char *input, const char *output, int mode,
	maxcso_progress_cb progress, void *userdata, char *error, size_t error_size) {
	bool ok = false;
	uint32_t flags = FlagsForMode(mode, &ok);
	if (!ok) {
		CopyError(error, error_size, "Unknown mode");
		return MAXCSO_RESULT_INVALID_OPTION;
	}
	return maxcso_process_flags(input, output, flags, maxcso::DEFAULT_BLOCK_SIZE, progress, userdata, error, error_size);
}

}  // extern "C"
