#pragma once

#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>
#include "uv.h"

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace maxcso {

typedef std::function<void (uv_fs_t *req)> fs_func_cb;
typedef std::function<void (uv_work_t *req)> work_func_cb;
typedef std::function<void (uv_work_t *req, int status)> after_work_func_cb;

struct Guard {
	Guard(uv_mutex_t &mutex) : mutex_(mutex) {
		uv_mutex_lock(&mutex_);
	}
	~Guard() {
		uv_mutex_unlock(&mutex_);
	}

	uv_mutex_t &mutex_;
};

// A file that lives on the caller's side (reached through callbacks) instead of on disk. Its
// handle goes through fs_read / fs_write / fs_fstat like a real one, and the callbacks run on
// libuv's threadpool, like real file I/O does. read and write return the number of bytes
// transferred, or a negative value on error.
struct VirtualFile {
	int64_t size = -1;
	std::function<int64_t (int64_t offset, uint8_t *buffer, size_t length)> read;
	std::function<int64_t (int64_t offset, const uint8_t *buffer, size_t length)> write;
};

class UVHelper {
public:
	UVHelper() {
		if (++mutexInit_ == 1) {
			uv_mutex_init(&mutex_);
		}
	}
	~UVHelper() {
		uv_mutex_lock(&mutex_);
		if (--mutexInit_ == 0) {
			uv_mutex_unlock(&mutex_);
			uv_mutex_destroy(&mutex_);
		} else {
			uv_mutex_unlock(&mutex_);
		}
	}

	inline int fs_open(uv_loop_t *loop, uv_fs_t *req, const char *path, int flags, int mode, fs_func_cb &&cb) {
		req->data = Freeze(std::move(cb));
		return uv_fs_open(loop, req, path, flags, mode, &Dispatch);
	}

	inline int fs_close(uv_loop_t *loop, uv_fs_t *req, uv_file file, fs_func_cb &&cb) {
		req->data = Freeze(std::move(cb));
		return uv_fs_close(loop, req, file, &Dispatch);
	}

	inline int fs_read(uv_loop_t *loop, uv_fs_t *req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, fs_func_cb &&cb) {
		req->data = Freeze(std::move(cb));
		if (IsVirtual(file))
			return QueueVirtual(loop, req, file, VIRTUAL_READ, bufs, nbufs, offset);
		return uv_fs_read(loop, req, file, bufs, nbufs, offset, &Dispatch);
	}

	inline int fs_write(uv_loop_t *loop, uv_fs_t *req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, fs_func_cb &&cb) {
		req->data = Freeze(std::move(cb));
		if (IsVirtual(file))
			return QueueVirtual(loop, req, file, VIRTUAL_WRITE, bufs, nbufs, offset);
		return uv_fs_write(loop, req, file, bufs, nbufs, offset, &Dispatch);
	}

	inline int fs_fstat(uv_loop_t *loop, uv_fs_t *req, uv_file file, fs_func_cb &&cb) {
		req->data = Freeze(std::move(cb));
		if (IsVirtual(file))
			return QueueVirtual(loop, req, file, VIRTUAL_FSTAT, nullptr, 0, 0);
		return uv_fs_fstat(loop, req, file, &Dispatch);
	}

	// Handles for virtual files, far above any real descriptor. The file must outlive every
	// operation on its handle.
	static uv_file RegisterVirtual(VirtualFile *file) {
		Guard g(mutex_);
		const uv_file handle = VIRTUAL_BASE + nextVirtual_++;
		virtual_files_[handle] = file;
		return handle;
	}
	static void UnregisterVirtual(uv_file handle) {
		Guard g(mutex_);
		virtual_files_.erase(handle);
	}
	static bool IsVirtual(uv_file file) {
		return file >= VIRTUAL_BASE;
	}

	inline int queue_work(uv_loop_t *loop, uv_work_t *req, work_func_cb &&cb, after_work_func_cb &&after) {
		req->data = Freeze(std::move(cb), std::move(after));
		return uv_queue_work(loop, req, &Dispatch, &Dispatch);
	}

private:
	static const uv_file VIRTUAL_BASE = 0x40000000;

	enum VirtualKind {
		VIRTUAL_READ,
		VIRTUAL_WRITE,
		VIRTUAL_FSTAT,
	};

	struct VirtualOp {
		uv_work_t work;
		uv_fs_t *req;
		VirtualFile *file;
		VirtualKind kind;
		std::vector<uv_buf_t> bufs;
		int64_t offset;
		int64_t result;
	};

	static int QueueVirtual(uv_loop_t *loop, uv_fs_t *req, uv_file handle, VirtualKind kind, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset) {
		VirtualFile *file = nullptr;
		{
			Guard g(mutex_);
			auto it = virtual_files_.find(handle);
			if (it != virtual_files_.end())
				file = it->second;
		}
		if (!file)
			return UV_EBADF;
		VirtualOp *op = new VirtualOp();
		op->work.data = op;
		op->req = req;
		op->file = file;
		op->kind = kind;
		op->bufs.assign(bufs, bufs + nbufs);
		op->offset = offset;
		op->result = 0;
		return uv_queue_work(loop, &op->work, &VirtualWork, &VirtualDone);
	}

	static void VirtualWork(uv_work_t *work) {
		VirtualOp *op = static_cast<VirtualOp *>(work->data);
		if (op->kind == VIRTUAL_FSTAT) {
			op->result = op->file->size >= 0 ? 0 : static_cast<int64_t>(UV_EINVAL);
			return;
		}
		int64_t offset = op->offset;
		int64_t total = 0;
		for (const uv_buf_t &buf : op->bufs) {
			const size_t length = buf.len;
			int64_t done;
			if (op->kind == VIRTUAL_READ) {
				done = op->file->read ? op->file->read(offset, reinterpret_cast<uint8_t *>(buf.base), length) : static_cast<int64_t>(UV_EBADF);
			} else {
				done = op->file->write ? op->file->write(offset, reinterpret_cast<const uint8_t *>(buf.base), length) : static_cast<int64_t>(UV_EBADF);
			}
			if (done < 0) {
				op->result = done == -1 ? static_cast<int64_t>(UV_EIO) : done;
				return;
			}
			total += done;
			offset += done;
			if (static_cast<size_t>(done) < length)
				break;
		}
		op->result = total;
	}

	static void VirtualDone(uv_work_t *work, int status) {
		VirtualOp *op = static_cast<VirtualOp *>(work->data);
		uv_fs_t *req = op->req;
		// Looks like a finished fs request, so the usual uv_fs_req_cleanup is harmless.
		void *data = req->data;
		memset(req, 0, sizeof(*req));
		req->data = data;
		req->result = status < 0 ? status : static_cast<ssize_t>(op->result);
		if (op->kind == VIRTUAL_FSTAT)
			req->statbuf.st_size = static_cast<uint64_t>(op->file->size);
		delete op;
		Dispatch(req);
	}

	static void Dispatch(uv_fs_t *req) {
		fs_func_cb f = ThawFS(req->data);
		f(req);
	}

	inline static void *Freeze(fs_func_cb &&cb) {
		Guard g(mutex_);
		intptr_t ticket = ++nextTicket_;
		fs_funcs_.emplace(ticket, std::move(cb));
		return reinterpret_cast<void *>(ticket);
	}
	inline static fs_func_cb ThawFS(void *data) {
		Guard g(mutex_);
		intptr_t ticket = reinterpret_cast<intptr_t>(data);
		fs_func_cb f = std::move(fs_funcs_.at(ticket));
		fs_funcs_.erase(ticket);
		return f;
	}

	static void Dispatch(uv_work_t *req) {
		work_func_cb f = ThawWork(req->data);
		f(req);
	}
	static void Dispatch(uv_work_t *req, int status) {
		after_work_func_cb f = ThawAfterWork(req->data);
		f(req, status);
	}

	inline static void *Freeze(work_func_cb &&cb, after_work_func_cb &&after) {
		Guard g(mutex_);
		intptr_t ticket = ++nextTicket_;
		work_funcs_.emplace(ticket, std::make_pair(std::move(cb), std::move(after)));
		return reinterpret_cast<void *>(ticket);
	}
	inline static work_func_cb ThawWork(void *data) {
		Guard g(mutex_);
		intptr_t ticket = reinterpret_cast<intptr_t>(data);
		work_func_cb f = std::move(work_funcs_.at(ticket).first);
		return f;
	}
	inline static after_work_func_cb ThawAfterWork(void *data) {
		Guard g(mutex_);
		intptr_t ticket = reinterpret_cast<intptr_t>(data);
		after_work_func_cb f = std::move(work_funcs_.at(ticket).second);
		work_funcs_.erase(ticket);
		return f;
	}

	static intptr_t nextTicket_;
	static int nextVirtual_;
	static std::unordered_map<uv_file, VirtualFile *> virtual_files_;
	static int mutexInit_;
	static uv_mutex_t mutex_;
	static std::unordered_map<intptr_t, fs_func_cb> fs_funcs_;
	static std::unordered_map<intptr_t, std::pair<work_func_cb, after_work_func_cb> > work_funcs_;
};

};
