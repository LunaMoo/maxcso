// cso_full: full-ISO test of MAXIMUM preset candidates at one block size (default 256K), no 7z.
//
// For every ISO, reads the whole image once in 64 MiB chunks and runs four real passes on each chunk:
//   A  zopfli5, blocksplittingmax 15 (maxcso's settings)
//   B  zopfli5, blocksplittingmax 20
//   C  libdeflate12 on every block; zopfli as in A only where libdeflate12 got below the threshold
//   D  the same pre-check, with zopfli as in B
// Each pass keeps the smallest result per block (raw when not smaller), like maxcso, and its
// wall-clock time is measured on its own. Per-block sizes and times are saved too, so other
// thresholds can be evaluated afterwards without running again.
//
// Results: bench_full/summary.txt and bench_full/<iso>.csv.gz. The ISOs are only read.
//
// Usage: cso_full [--threads N] [--threshold 0.98] [--block 262144] [dir|file.iso ...]

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>
#include "zopfli.h"
#include "libdeflate.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static const int NUM_PASSES = 4;
static const char *PASS_NAMES[NUM_PASSES] = {
	"A  zopfli5, splits 15 (no pre-check)",
	"B  zopfli5, splits 20 (no pre-check)",
	"C  pre-check + zopfli5, splits 15",
	"D  pre-check + zopfli5, splits 20",
};

struct BlockInfo {
	uint32_t raw = 0;
	uint32_t ld12 = 0;     // libdeflate12 size (from the pre-check passes)
	uint32_t z15 = 0;      // zopfli5 splits 15 size
	uint32_t z20 = 0;      // zopfli5 splits 20 size
	uint32_t ld12_us = 0;  // per-block CPU time, microseconds
	uint32_t z15_us = 0;
	uint32_t z20_us = 0;
	uint8_t skipped = 0;
};

static uint32_t Zopfli(const uint8_t *in, uint32_t n, int splits) {
	ZopfliOptions opt;
	ZopfliInitOptions(&opt);
	opt.blocksplittinglast = 1;  // As maxcso.
	opt.numiterations = 5;
	opt.blocksplittingmax = splits;
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

static uint32_t Micros(Clock::time_point a, Clock::time_point b) {
	return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
}

static FILE *g_summary = nullptr;
static void Log(const char *fmt, ...) {
	char buf[2048];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	fputs(buf, stdout);
	fflush(stdout);
	if (g_summary) {
		fputs(buf, g_summary);
		fflush(g_summary);
	}
}

static FILE *OpenRead(const fs::path &p) {
#ifdef _WIN32
	return _wfopen(p.wstring().c_str(), L"rb");
#else
	return fopen(p.string().c_str(), "rb");
#endif
}

struct Totals {
	uint64_t size[NUM_PASSES] = {};
	double secs[NUM_PASSES] = {};
	uint64_t ld12Only = 0;  // libdeflate12 alone, as a reference
	uint64_t blocks = 0, skipped = 0, input = 0;
};

static void PrintTotals(const char *title, const Totals &t) {
	Log("\n%s  (input %.1f MB, %llu blocks, pre-check skipped %llu = %.1f%%)\n", title, t.input / 1e6,
		(unsigned long long)t.blocks, (unsigned long long)t.skipped, t.blocks ? 100.0 * t.skipped / t.blocks : 0.0);
	Log("  %-40s %15s %10s %10s %10s %10s\n", "pass", "output bytes", "of input", "vs A", "time (s)", "speed vs A");
	for (int p = 0; p < NUM_PASSES; ++p) {
		Log("  %-40s %15llu %9.3f%% %+9.3f%% %10.1f %9.2fx\n", PASS_NAMES[p], (unsigned long long)t.size[p],
			100.0 * t.size[p] / t.input, 100.0 * ((double)t.size[p] / t.size[0] - 1.0), t.secs[p], t.secs[0] / t.secs[p]);
	}
	Log("  %-40s %15llu %9.3f%% %+9.3f%% %10s\n", "(reference) libdeflate12 only", (unsigned long long)t.ld12Only,
		100.0 * t.ld12Only / t.input, 100.0 * ((double)t.ld12Only / t.size[0] - 1.0), "-");
}

int main(int argc, char **argv) {
	int threads = 0;
	double threshold = 0.98;
	uint32_t bs = 262144;
	std::vector<fs::path> inputs;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
		else if (a == "--threshold" && i + 1 < argc) threshold = atof(argv[++i]);
		else if (a == "--block" && i + 1 < argc) bs = (uint32_t)atoi(argv[++i]);
		else inputs.push_back(fs::u8path(a));
	}
	if (threads <= 0) threads = std::max(1u, std::thread::hardware_concurrency());
	if (bs < 2048 || bs > 262144 || (bs & (bs - 1))) {
		printf("Block size must be a power of two from 2048 to 262144.\n");
		return 1;
	}
	if (inputs.empty()) inputs.push_back(fs::current_path());
#ifdef _WIN32
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif

	std::vector<fs::path> isos;
	for (const auto &in : inputs) {
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
		printf("No .iso files found. Put cso_full.exe next to the ISOs or pass a folder.\n");
		return 1;
	}

	fs::path outRoot = fs::current_path() / "bench_full";
	fs::create_directories(outRoot);
#ifdef _WIN32
	g_summary = _wfopen((outRoot / "summary.txt").wstring().c_str(), L"a");
#else
	g_summary = fopen((outRoot / "summary.txt").string().c_str(), "a");
#endif
	Log("cso_full: block %u, %d threads, pre-check skips zopfli when libdeflate12 >= %.0f%% of raw, %zu ISO(s)\n",
		bs, threads, threshold * 100, isos.size());
	Log("Sizes are CSO v1 bytes: smallest result per block (raw if not smaller) + 4 index bytes per block.\n");

	uint64_t totalInput = 0;
	for (const auto &p : isos) totalInput += fs::file_size(p);
	uint64_t doneInput = 0;
	auto runStart = Clock::now();

	const uint32_t CHUNK_BLOCKS = std::max<uint32_t>(1, (64u << 20) / bs);
	Totals all;
	for (const fs::path &iso : isos) {
		std::string name = iso.filename().u8string();
		uint64_t size = fs::file_size(iso);
		FILE *f = OpenRead(iso);
		if (!f) {
			Log("\n!! Could not read %s\n", name.c_str());
			continue;
		}
		Log("\n=== %s (%.0f MB) ===\n", name.c_str(), size / 1e6);
		uint64_t nblocks = (size + bs - 1) / bs;
		std::vector<BlockInfo> info(nblocks);
		std::vector<uint8_t> chunk((size_t)CHUNK_BLOCKS * bs);
		Totals t;
		t.input = size;
		t.blocks = nblocks;

		for (uint64_t first = 0; first < nblocks; first += CHUNK_BLOCKS) {
			uint32_t count = (uint32_t)std::min<uint64_t>(CHUNK_BLOCKS, nblocks - first);
			size_t got = fread(chunk.data(), 1, (size_t)count * bs, f);
			if (got < (size_t)count * bs) memset(chunk.data() + got, 0, (size_t)count * bs - got);
			for (uint32_t b = 0; b < count; ++b) {
				uint64_t start = (first + b) * bs;
				info[first + b].raw = (uint32_t)std::min<uint64_t>(bs, size - start);
			}

			for (int pass = 0; pass < NUM_PASSES; ++pass) {
				const bool precheck = pass >= 2;
				const int splits = (pass == 0 || pass == 2) ? 15 : 20;
				std::atomic<uint32_t> next{ 0 };
				auto t0 = Clock::now();
				std::vector<std::thread> ts;
				for (int th = 0; th < threads; ++th) {
					ts.emplace_back([&]() {
						libdeflate_compressor *ld = precheck ? libdeflate_alloc_compressor(12) : nullptr;
						std::vector<uint8_t> out(bs * 2 + 4096);
						for (;;) {
							uint32_t b = next.fetch_add(1);
							if (b >= count) break;
							BlockInfo &bi = info[first + b];
							const uint8_t *in = chunk.data() + (size_t)b * bs;
							uint32_t best = bi.raw;
							bool skip = false;
							if (precheck) {
								auto a = Clock::now();
								size_t r = libdeflate_deflate_compress(ld, in, bi.raw, out.data(), out.size());
								uint32_t ldSize = r ? (uint32_t)r : 0xFFFFFFFF;
								bi.ld12_us = Micros(a, Clock::now());
								bi.ld12 = ldSize;
								best = std::min(best, ldSize);
								skip = (double)ldSize >= threshold * bi.raw;
								bi.skipped = skip ? 1 : 0;
							}
							if (!skip) {
								auto a = Clock::now();
								uint32_t z = Zopfli(in, bi.raw, splits);
								uint32_t us = Micros(a, Clock::now());
								if (!precheck) {
									if (splits == 15) {
										bi.z15 = z;
										bi.z15_us = us;
									} else {
										bi.z20 = z;
										bi.z20_us = us;
									}
								}
								best = std::min(best, z);
							}
							(void)best;  // Pass sizes are summed after the ISO from the per-block results.
						}
						if (ld) libdeflate_free_compressor(ld);
					});
				}
				for (auto &th : ts) th.join();
				t.secs[pass] += std::chrono::duration<double>(Clock::now() - t0).count();
			}

			doneInput += (uint64_t)count * bs;
			double el = std::chrono::duration<double>(Clock::now() - runStart).count();
			double frac = std::min(1.0, (double)doneInput / totalInput);
			printf("\r  %5.1f%% of %s, all ISOs %5.1f%%, ~%.0f min left    ", 100.0 * (first + count) / nblocks, name.c_str(),
				100.0 * frac, frac > 0 ? el / frac * (1 - frac) / 60 : 0.0);
			fflush(stdout);
		}
		fclose(f);
		printf("\n");

		// Sizes per pass from the per-block results (zopfli output is deterministic).
		for (const BlockInfo &bi : info) {
			uint32_t a = std::min(bi.raw, bi.z15), b = std::min(bi.raw, bi.z20);
			uint32_t ldBest = std::min(bi.raw, bi.ld12);
			t.size[0] += a + 4;
			t.size[1] += b + 4;
			t.size[2] += (bi.skipped ? ldBest : std::min(ldBest, bi.z15)) + 4;
			t.size[3] += (bi.skipped ? ldBest : std::min(ldBest, bi.z20)) + 4;
			t.ld12Only += ldBest + 4;
			t.skipped += bi.skipped;
		}
		PrintTotals(("Results for " + name).c_str(), t);
		for (int p = 0; p < NUM_PASSES; ++p) {
			all.size[p] += t.size[p];
			all.secs[p] += t.secs[p];
		}
		all.ld12Only += t.ld12Only;
		all.blocks += t.blocks;
		all.skipped += t.skipped;
		all.input += t.input;

		fs::path csv = outRoot / (iso.stem().u8string() + ".csv.gz");
#ifdef _WIN32
		gzFile g = gzopen_w(csv.wstring().c_str(), "wb9");
#else
		gzFile g = gzopen(csv.string().c_str(), "wb9");
#endif
		if (g) {
			gzprintf(g, "block,raw,libdeflate12,zopfli5_s15,zopfli5_s20,libdeflate12_us,zopfli5_s15_us,zopfli5_s20_us,skipped\n");
			for (size_t b = 0; b < info.size(); ++b) {
				const BlockInfo &bi = info[b];
				gzprintf(g, "%u,%u,%u,%u,%u,%u,%u,%u,%u\n", (unsigned)b, bi.raw, bi.ld12, bi.z15, bi.z20, bi.ld12_us, bi.z15_us, bi.z20_us, bi.skipped);
			}
			gzprintf(g, "#pass_seconds,%.3f,%.3f,%.3f,%.3f\n", t.secs[0], t.secs[1], t.secs[2], t.secs[3]);
			gzclose(g);
		}
	}
	if (isos.size() > 1) PrintTotals("=== ALL ISOs COMBINED ===", all);
	Log("\nDone. The bench_full folder holds summary.txt and one small .csv.gz per ISO.\n");
	if (g_summary) fclose(g_summary);
	return 0;
}
