// cso_bench: measures what each deflate method maxcso can use contributes, per block size.
//
// For every ISO it reads an evenly spread sample (1 MiB out of every N MiB), then for each block
// size compresses every block with every method separately and records the sizes. Because maxcso
// keeps the smallest result per block, any combination of methods can then be evaluated exactly
// from the recorded sizes without compressing again.
//
// Results: bench_results/<iso>/bs_<size>.csv.gz (per-block sizes) and bench_results/summary.txt.
// Nothing is uploaded anywhere; the ISOs are only read.
//
// Usage: cso_bench [--sample N] [--threads N] [--no-zopfli] [--zopfli15] [--blocks a,b,...] [dir|file.iso ...]
// With no paths, uses the current directory.

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>
#include "zopfli.h"
#include "deflate7z.h"
#include "libdeflate.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static const uint32_t CHUNK = 1 << 20;

struct Options {
	int sample = 8;
	int threads = 0;
	bool zopfli = true;
	bool zopfli15 = false;
	std::vector<uint32_t> blocks = { 32768, 65536, 16384, 131072, 262144, 8192, 4096, 2048 };
	std::vector<fs::path> inputs;
};

static uint32_t ZlibSize(z_stream *z, const uint8_t *in, uint32_t n, std::vector<uint8_t> &out) {
	deflateReset(z);
	z->next_in = (Bytef *)in;
	z->avail_in = n;
	z->next_out = out.data();
	z->avail_out = (uInt)out.size();
	int r;
	while ((r = deflate(z, Z_FINISH)) == Z_OK) {
	}
	return r == Z_STREAM_END ? (uint32_t)z->total_out : 0xFFFFFFFF;
}

// One compressor instance per worker thread.
struct Worker {
	std::string method;
	z_stream z{};
	bool zInit = false;
	libdeflate_compressor *ld = nullptr;
	Deflate7z::Context *c7 = nullptr;
	int zopfliIter = 0;
	std::vector<uint8_t> out;

	Worker(const std::string &m, uint32_t bs) : method(m), out(bs * 2 + 4096) {
		int strategy = -1;
		if (m == "zlib") strategy = Z_DEFAULT_STRATEGY;
		else if (m == "zlib_filtered") strategy = Z_FILTERED;
		else if (m == "zlib_huffman") strategy = Z_HUFFMAN_ONLY;
		else if (m == "zlib_rle") strategy = Z_RLE;
		if (strategy >= 0) {
			deflateInit2(&z, 9, Z_DEFLATED, -15, 9, strategy);
			zInit = true;
		} else if (m.rfind("libdeflate", 0) == 0) {
			ld = libdeflate_alloc_compressor(atoi(m.c_str() + 10));
		} else if (m == "7zdeflate") {
			// Same settings as maxcso's Sector.
			Deflate7z::Options o;
			Deflate7z::SetDefaults(&o);
			o.level = 9;
			o.passes = 12;
			o.fastbytes = 64;
			o.matchcycles = 32;
			o.algo = 1;
			o.useZlib = false;
			Deflate7z::Alloc(&c7, &o);
		} else if (m.rfind("zopfli", 0) == 0) {
			zopfliIter = atoi(m.c_str() + 6);
		}
	}
	~Worker() {
		if (zInit) deflateEnd(&z);
		if (ld) libdeflate_free_compressor(ld);
		if (c7) Deflate7z::Release(&c7);
	}

	uint32_t Compress(const uint8_t *in, uint32_t n) {
		if (zInit) return ZlibSize(&z, in, n, out);
		if (ld) {
			size_t r = libdeflate_deflate_compress(ld, in, n, out.data(), out.size());
			return r ? (uint32_t)r : 0xFFFFFFFF;
		}
		if (c7) {
			uint32_t r = 0;
			return Deflate7z::Deflate(c7, out.data(), (uint32_t)out.size(), in, n, &r) ? r : 0xFFFFFFFF;
		}
		if (zopfliIter) {
			// Same settings as maxcso's Sector (blocksplittinglast, numiterations).
			ZopfliOptions opt;
			ZopfliInitOptions(&opt);
			opt.blocksplittinglast = 1;
			opt.numiterations = zopfliIter;
			unsigned char *o = nullptr;
			size_t os = 0;
			ZopfliCompress(&opt, ZOPFLI_FORMAT_DEFLATE, in, n, &o, &os);
			uint32_t r = 0xFFFFFFFF;
			if (o) {
				if (os > 0) r = (uint32_t)os;
				free(o);
			}
			return r;
		}
		return 0xFFFFFFFF;
	}
};

static bool ReadSample(const fs::path &p, int sample, std::vector<uint8_t> &data, uint64_t &fullSize) {
#ifdef _WIN32
	FILE *f = _wfopen(p.wstring().c_str(), L"rb");
#else
	FILE *f = fopen(p.string().c_str(), "rb");
#endif
	if (!f) return false;
	fullSize = fs::file_size(p);
	uint64_t chunks = (fullSize + CHUNK - 1) / CHUNK;
	data.clear();
	std::vector<uint8_t> buf(CHUNK);
	for (uint64_t c = 0; c < chunks; c += sample) {
		// Offset into the stride so the start (often just headers and padding) is not overweighted.
		uint64_t pick = std::min<uint64_t>(c + (uint64_t)sample / 2, chunks - 1);
#ifdef _WIN32
		_fseeki64(f, (int64_t)(pick * CHUNK), SEEK_SET);
#else
		fseeko(f, (off_t)(pick * CHUNK), SEEK_SET);
#endif
		size_t got = fread(buf.data(), 1, CHUNK, f);
		if (got == 0) break;
		// Keep only whole 256 KiB pieces so every block size sees exactly the same bytes.
		got &= ~(size_t)(262144 - 1);
		data.insert(data.end(), buf.begin(), buf.begin() + got);
	}
	fclose(f);
	return !data.empty();
}

struct Combo {
	const char *name;
	std::vector<std::string> methods;
};

static const std::vector<Combo> &Combos() {
	static const std::vector<Combo> c = {
		{ "zlib only (current FAST)", { "zlib" } },
		{ "zlib x4 + 7z (current STANDARD)", { "zlib", "zlib_filtered", "zlib_huffman", "zlib_rle", "7zdeflate" } },
		{ "all (current MAXIMUM)", { "zlib", "zlib_filtered", "zlib_huffman", "zlib_rle", "7zdeflate", "libdeflate12", "zopfli5" } },
		{ "libdeflate12 only", { "libdeflate12" } },
		{ "libdeflate9 only", { "libdeflate9" } },
		{ "7zdeflate only", { "7zdeflate" } },
		{ "zopfli5 only", { "zopfli5" } },
		{ "libdeflate12 + 7z", { "libdeflate12", "7zdeflate" } },
		{ "libdeflate12 + zopfli5", { "libdeflate12", "zopfli5" } },
		{ "7z + zopfli5", { "7zdeflate", "zopfli5" } },
		{ "libdeflate12 + 7z + zopfli5", { "libdeflate12", "7zdeflate", "zopfli5" } },
		{ "all without zopfli", { "zlib", "zlib_filtered", "zlib_huffman", "zlib_rle", "7zdeflate", "libdeflate12" } },
		{ "zopfli15 only", { "zopfli15" } },
		{ "libdeflate12 + 7z + zopfli15", { "libdeflate12", "7zdeflate", "zopfli15" } },
	};
	return c;
}

struct BsResult {
	std::vector<std::string> methods;
	std::vector<std::vector<uint32_t>> sizes;  // [method][block]
	std::vector<uint32_t> raw;
	std::vector<double> secs;
};

// Size in a CSO v1: per block the smallest result, stored raw when not smaller; 4 bytes of index.
static uint64_t ComboSize(const BsResult &r, const std::vector<std::string> &ms, bool *ok) {
	std::vector<size_t> idx;
	for (const auto &m : ms) {
		auto it = std::find(r.methods.begin(), r.methods.end(), m);
		if (it == r.methods.end()) {
			*ok = false;
			return 0;
		}
		idx.push_back(it - r.methods.begin());
	}
	*ok = true;
	uint64_t t = 0;
	for (size_t b = 0; b < r.raw.size(); ++b) {
		uint32_t best = r.raw[b];
		for (size_t i : idx) best = std::min(best, r.sizes[i][b]);
		t += best + 4;
	}
	return t;
}

static double ComboSecs(const BsResult &r, const std::vector<std::string> &ms) {
	double s = 0;
	for (const auto &m : ms) {
		auto it = std::find(r.methods.begin(), r.methods.end(), m);
		if (it != r.methods.end()) s += r.secs[it - r.methods.begin()];
	}
	return s;
}

static void Log(FILE *summary, const char *fmt, ...) {
	char buf[1024];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	fputs(buf, stdout);
	fflush(stdout);
	if (summary) {
		fputs(buf, summary);
		fflush(summary);
	}
}

static BsResult RunBlockSize(const std::vector<uint8_t> &data, uint32_t bs, const std::vector<std::string> &methods, int threads) {
	BsResult r;
	r.methods = methods;
	size_t nblocks = data.size() / bs;
	r.raw.assign(nblocks, bs);
	for (const std::string &m : methods) {
		std::vector<uint32_t> sizes(nblocks);
		std::atomic<size_t> next{ 0 };
		std::atomic<size_t> done{ 0 };
		auto t0 = std::chrono::steady_clock::now();
		std::vector<std::thread> ts;
		for (int t = 0; t < threads; ++t) {
			ts.emplace_back([&]() {
				Worker w(m, bs);
				for (;;) {
					size_t b = next.fetch_add(1);
					if (b >= nblocks) break;
					sizes[b] = w.Compress(data.data() + b * bs, bs);
					done.fetch_add(1);
				}
			});
		}
		// Progress for the slow methods.
		double lastPrint = 0;
		while (done.load() < nblocks) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			size_t d = done.load();
			if (el > 3 && el - lastPrint > 1 && d > 0 && d < nblocks) {
				lastPrint = el;
				double eta = el / d * (nblocks - d);
				printf("\r      %-14s %5.1f%%  ~%.0f s left   ", m.c_str(), 100.0 * d / nblocks, eta);
				fflush(stdout);
			}
		}
		for (auto &t : ts) t.join();
		double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		printf("\r      %-14s %8.1f s  (%.2f MB/s)                  \n", m.c_str(), secs, data.size() / 1e6 / secs);
		r.sizes.push_back(std::move(sizes));
		r.secs.push_back(secs);
	}
	return r;
}

static void WriteCsv(const fs::path &p, const BsResult &r) {
#ifdef _WIN32
	gzFile g = gzopen_w(p.wstring().c_str(), "wb9");
#else
	gzFile g = gzopen(p.string().c_str(), "wb9");
#endif
	if (!g) return;
	gzprintf(g, "block,raw");
	for (const auto &m : r.methods) gzprintf(g, ",%s", m.c_str());
	gzprintf(g, "\n");
	for (size_t b = 0; b < r.raw.size(); ++b) {
		gzprintf(g, "%u,%u", (unsigned)b, r.raw[b]);
		for (size_t m = 0; m < r.methods.size(); ++m) gzprintf(g, ",%u", r.sizes[m][b]);
		gzprintf(g, "\n");
	}
	gzprintf(g, "#seconds");
	for (double s : r.secs) gzprintf(g, ",%.3f", s);
	gzprintf(g, "\n");
	gzclose(g);
}

static void PrintTable(FILE *summary, const char *title, const std::map<uint32_t, BsResult> &res, uint64_t rawBytes) {
	Log(summary, "\n%s\n", title);
	Log(summary, "  %-34s", "combination  \\  block size");
	for (const auto &kv : res) Log(summary, " %9uK", kv.first / 1024);
	Log(summary, "\n");
	for (const Combo &c : Combos()) {
		std::string line;
		bool any = false;
		char buf[64];
		for (const auto &kv : res) {
			bool ok;
			uint64_t s = ComboSize(kv.second, c.methods, &ok);
			if (ok) {
				snprintf(buf, sizeof(buf), " %9.3f%%", 100.0 * s / rawBytes);
				any = true;
			} else {
				snprintf(buf, sizeof(buf), " %10s", "-");
			}
			line += buf;
		}
		if (any) Log(summary, "  %-34s%s\n", c.name, line.c_str());
	}
	Log(summary, "  %-34s", "time of 'all (current MAXIMUM)' [s]");
	for (const auto &kv : res) Log(summary, " %10.1f", ComboSecs(kv.second, Combos()[2].methods));
	Log(summary, "\n  %-34s", "time of 'libdeflate12 + 7z' [s]");
	for (const auto &kv : res) Log(summary, " %10.1f", ComboSecs(kv.second, { "libdeflate12", "7zdeflate" }));
	Log(summary, "\n  %-34s", "time of 'libdeflate12' [s]");
	for (const auto &kv : res) Log(summary, " %10.1f", ComboSecs(kv.second, { "libdeflate12" }));
	Log(summary, "\n");
}

int main(int argc, char **argv) {
	Options o;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--sample" && i + 1 < argc) o.sample = std::max(1, atoi(argv[++i]));
		else if (a == "--threads" && i + 1 < argc) o.threads = atoi(argv[++i]);
		else if (a == "--no-zopfli") o.zopfli = false;
		else if (a == "--zopfli15") o.zopfli15 = true;
		else if (a == "--blocks" && i + 1 < argc) {
			o.blocks.clear();
			std::string s = argv[++i];
			size_t p = 0;
			while (p < s.size()) {
				size_t q = s.find(',', p);
				if (q == std::string::npos) q = s.size();
				o.blocks.push_back((uint32_t)atoi(s.substr(p, q - p).c_str()));
				p = q + 1;
			}
		} else o.inputs.push_back(fs::u8path(a));
	}
	if (o.threads <= 0) o.threads = std::max(1u, std::thread::hardware_concurrency());
	if (o.inputs.empty()) o.inputs.push_back(fs::current_path());

#ifdef _WIN32
	// Stay out of the way of other work on the machine.
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif

	std::vector<fs::path> isos;
	for (const auto &in : o.inputs) {
		std::error_code ec;
		if (fs::is_directory(in, ec)) {
			for (const auto &e : fs::directory_iterator(in, ec)) {
				std::string ext = e.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
				if (e.is_regular_file() && ext == ".iso") isos.push_back(e.path());
			}
		} else if (fs::is_regular_file(in, ec)) {
			isos.push_back(in);
		}
	}
	std::sort(isos.begin(), isos.end(), [](const fs::path &a, const fs::path &b) { return fs::file_size(a) < fs::file_size(b); });
	if (isos.empty()) {
		printf("No .iso files found. Put cso_bench.exe next to the ISOs or pass a folder.\n");
		return 1;
	}

	std::vector<std::string> methods = { "zlib", "zlib_filtered", "zlib_huffman", "zlib_rle", "libdeflate9", "libdeflate12", "7zdeflate" };
	if (o.zopfli) methods.push_back("zopfli5");
	if (o.zopfli15) methods.push_back("zopfli15");

	fs::path outRoot = fs::current_path() / "bench_results";
	fs::create_directories(outRoot);
	FILE *summary = nullptr;
#ifdef _WIN32
	summary = _wfopen((outRoot / "summary.txt").wstring().c_str(), L"a");
#else
	summary = fopen((outRoot / "summary.txt").string().c_str(), "a");
#endif

	Log(summary, "cso_bench: %d threads, sample 1 MiB of every %d MiB, %zu ISO(s)\n", o.threads, o.sample, isos.size());
	Log(summary, "Percentages are output size / sampled input size (lower is better).\n");

	// Totals across all ISOs, per block size, built by concatenating blocks.
	std::map<uint32_t, BsResult> overall;
	uint64_t overallRaw = 0;

	for (const fs::path &iso : isos) {
		std::vector<uint8_t> data;
		uint64_t fullSize = 0;
		std::string name = iso.filename().u8string();
		if (!ReadSample(iso, o.sample, data, fullSize)) {
			Log(summary, "\n!! Could not read %s\n", name.c_str());
			continue;
		}
		Log(summary, "\n=== %s  (%.0f MB, sampled %.0f MB) ===\n", name.c_str(), fullSize / 1e6, data.size() / 1e6);
		fs::path dir = outRoot / iso.stem();
		fs::create_directories(dir);
		std::map<uint32_t, BsResult> res;
		for (uint32_t bs : o.blocks) {
			if (bs < 2048 || bs > 262144 || (bs & (bs - 1))) continue;
			printf("    block %u\n", bs);
			res[bs] = RunBlockSize(data, bs, methods, o.threads);
			WriteCsv(dir / ("bs_" + std::to_string(bs) + ".csv.gz"), res[bs]);
			BsResult &all = overall[bs];
			if (all.methods.empty()) {
				all.methods = methods;
				all.sizes.resize(methods.size());
				all.secs.assign(methods.size(), 0.0);
			}
			for (size_t m = 0; m < methods.size(); ++m) {
				all.sizes[m].insert(all.sizes[m].end(), res[bs].sizes[m].begin(), res[bs].sizes[m].end());
				all.secs[m] += res[bs].secs[m];
			}
			all.raw.insert(all.raw.end(), res[bs].raw.begin(), res[bs].raw.end());
		}
		PrintTable(summary, ("Results for " + name).c_str(), res, data.size());
		overallRaw += data.size();
	}

	if (isos.size() > 1) PrintTable(summary, "=== ALL ISOs COMBINED ===", overall, overallRaw);
	Log(summary, "\nDone. Send back the bench_results folder (summary.txt and the .csv.gz files).\n");
	if (summary) fclose(summary);
	return 0;
}
