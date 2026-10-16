// img_bench: full-ISO test of CHD (CHDmodern) and EBOOT.PBP (iPBP) compression, through the same
// libraries PPSSPP loads (CHDm.dll, iPBP.dll and mCSO.dll next to this exe, or in the current folder).
// CSO sets (all at 256K): A zopfli5, B zopfli5 splits 20, C pre-check 98% + A, D pre-check 98% + B, L libdeflate12.
//
// For every ISO and every setting it: compresses the whole ISO (timed), records the output size,
// decompresses it back to an ISO (timed) and compares it byte for byte with the original, and for
// CHD also times random sector reads like an emulator does (each read from a different hunk).
// Temporary files go to bench_lib\tmp and are deleted right after each step.
//
// Results: bench_lib/summary.txt and bench_lib/results.csv. The ISOs are only read.
//
// Usage: img_bench [--hunks 2048,4096,16384,65536] [--codecs default,lzma,zstd] [--no-chd] [--no-pbp] [--no-cso] [--cso-sets B,D,L]
//                  [--no-verify] [--reads 4000] [--threads N] [dir|file.iso ...]

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <zlib.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---- Library loading -------------------------------------------------------------------------

#ifdef _WIN32
typedef HMODULE LibHandle;
static LibHandle LoadLib(const fs::path &p) { return LoadLibraryW(p.wstring().c_str()); }
static void *Sym(LibHandle h, const char *n) { return (void *)GetProcAddress(h, n); }
static const char *CHD_LIB = "CHDm.dll";
static const char *PBP_LIB = "iPBP.dll";
static const char *CSO_LIB = "mCSO.dll";
#else
typedef void *LibHandle;
static LibHandle LoadLib(const fs::path &p) { return dlopen(p.string().c_str(), RTLD_NOW); }
static void *Sym(LibHandle h, const char *n) { return dlsym(h, n); }
#ifdef __APPLE__
static const char *CHD_LIB = "CHDm.dylib";
static const char *PBP_LIB = "iPBP.dylib";
static const char *CSO_LIB = "mCSO.dylib";
#else
static const char *CHD_LIB = "CHDm.so";
static const char *PBP_LIB = "iPBP.so";
static const char *CSO_LIB = "mCSO.so";
#endif
#endif

// CHDmodern (include/chdmodern/chdmodern.h)
struct chdm_file;
struct chdm_info {
	uint32_t version, hunk_bytes, hunk_count, unit_bytes;
	uint64_t unit_count, logical_bytes;
	uint32_t compression[4];
	int compressed, has_parent, parent_missing;
	int kind;
	uint8_t sha1[20], raw_sha1[20], parent_sha1[20];
};
typedef int (*chdm_progress_func)(void *user, double progress);
struct ChdApi {
	const char *(*version)(void) = nullptr;
	const char *(*last_error)(void) = nullptr;
	int (*create_dvd)(const char *, const char *, uint32_t, const uint32_t[4], chdm_progress_func, void *) = nullptr;
	int (*extract_raw)(const char *, const char *, chdm_progress_func, void *) = nullptr;
	int (*open)(const char *, int, chdm_file *, chdm_file **) = nullptr;
	void (*close)(chdm_file *) = nullptr;
	int (*get_info)(chdm_file *, chdm_info *) = nullptr;
	int (*read_units)(chdm_file *, uint64_t, void *, uint32_t) = nullptr;
	bool ok = false;
} chd;

// iPBP (include/ipbp/ipbp.h)
struct ipbp_job;
typedef void (*ipbp_progress_fn)(void *user, uint64_t done, uint64_t total);
struct ipbp_iso2pbp_options {
	int compress;
	const char *content_id;
	const uint8_t *version_key;
	const uint8_t *startdat_png;
	size_t startdat_png_size;
	unsigned threads;
};
struct ipbp_pbp2iso_options {
	int verify;
	int trim;
	unsigned threads;
};
struct PbpApi {
	const char *(*version)(void) = nullptr;
	ipbp_job *(*job_create)(void) = nullptr;
	void (*job_destroy)(ipbp_job *) = nullptr;
	void (*job_set_progress)(ipbp_job *, ipbp_progress_fn, void *) = nullptr;
	const char *(*job_error)(const ipbp_job *) = nullptr;
	void (*iso2pbp_init)(ipbp_iso2pbp_options *) = nullptr;
	void (*pbp2iso_init)(ipbp_pbp2iso_options *) = nullptr;
	int (*iso_to_pbp)(ipbp_job *, const char *, const char *, const ipbp_iso2pbp_options *) = nullptr;
	int (*pbp_to_iso)(ipbp_job *, const char *, const char *, const ipbp_pbp2iso_options *) = nullptr;
	bool ok = false;
} pbp;

// mCSO (capi/maxcso_capi.h): maxcso_options_init / maxcso_process_ex are needed for these tests.
struct maxcso_options {
	uint32_t struct_size;
	uint32_t flags;
	uint32_t block_size;
	uint32_t zopfli_split_max;
	double precheck_percent;
};
typedef int (*maxcso_progress_cb)(void *userdata, int64_t pos, int64_t total, int64_t written);
struct CsoApi {
	const char *(*version)(void) = nullptr;
	int (*capi_version)(void) = nullptr;
	int (*options_init)(maxcso_options *, int) = nullptr;
	int (*process_ex)(const char *, const char *, const maxcso_options *, maxcso_progress_cb, void *, char *, size_t) = nullptr;
	int (*process)(const char *, const char *, int, maxcso_progress_cb, void *, char *, size_t) = nullptr;
	bool ok = false;
} cso;
// maxcso::TaskFlags
static const uint32_t CSO_NO_ZLIB = 0x03, CSO_NO_ZOPFLI = 0x04, CSO_NO_7ZIP = 0x08, CSO_NO_LZ4 = 0x380, CSO_NO_LIBDEFLATE = 0x1000;
struct CsoSet {
	const char *key, *name;
	uint32_t flags, splits;
	double precheck;
};
static const CsoSet CSO_SETS[] = {
	{ "A", "zopfli5 256K", CSO_NO_ZLIB | CSO_NO_7ZIP | CSO_NO_LZ4 | CSO_NO_LIBDEFLATE, 0, 0.0 },
	{ "B", "zopfli5 splits20 256K", CSO_NO_ZLIB | CSO_NO_7ZIP | CSO_NO_LZ4 | CSO_NO_LIBDEFLATE, 20, 0.0 },
	{ "C", "precheck98 + zopfli5 256K", CSO_NO_ZLIB | CSO_NO_7ZIP | CSO_NO_LZ4, 0, 98.0 },
	{ "D", "precheck98 + zopfli5 splits20 256K", CSO_NO_ZLIB | CSO_NO_7ZIP | CSO_NO_LZ4, 20, 98.0 },
	{ "L", "libdeflate12 256K", CSO_NO_ZLIB | CSO_NO_7ZIP | CSO_NO_LZ4 | CSO_NO_ZOPFLI, 0, 0.0 },
};

template <typename T> static bool Get(LibHandle h, const char *name, T &fn) {
	fn = (T)Sym(h, name);
	if (!fn) printf("  missing export %s\n", name);
	return fn != nullptr;
}

static LibHandle FindLib(const char *name, const fs::path &exeDir) {
	for (const fs::path &dir : { exeDir, fs::current_path() }) {
		fs::path p = dir / name;
		std::error_code ec;
		if (fs::exists(p, ec)) {
			LibHandle h = LoadLib(p);
			if (h) return h;
			printf("Found %s but could not load it.\n", p.u8string().c_str());
		}
	}
	return nullptr;
}

// ---- Helpers ------------------------------------------------------------------------------------

static FILE *g_summary = nullptr;
static FILE *g_csv = nullptr;
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

static FILE *OpenFile(const fs::path &p, const char *mode) {
#ifdef _WIN32
	std::wstring m(mode, mode + strlen(mode));
	return _wfopen(p.wstring().c_str(), m.c_str());
#else
	return fopen(p.string().c_str(), mode);
#endif
}

static double Secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

// The decompressed image must equal the original; anything past the original's end must be zero
// (PBP rounds the image up to whole 16-sector blocks).
static std::string CompareImages(const fs::path &orig, const fs::path &back) {
	FILE *a = OpenFile(orig, "rb"), *b = OpenFile(back, "rb");
	if (!a || !b) {
		if (a) fclose(a);
		if (b) fclose(b);
		return "could not open for compare";
	}
	std::vector<uint8_t> ba(8 << 20), bb(8 << 20);
	std::string result = "OK";
	uint64_t pos = 0;
	for (;;) {
		size_t ra = fread(ba.data(), 1, ba.size(), a);
		size_t rb = fread(bb.data(), 1, ra ? ra : bb.size(), b);
		if (ra == 0) {
			// Original ended; the rest of the copy must be zero padding.
			while (rb > 0) {
				for (size_t i = 0; i < rb; ++i)
					if (bb[i] != 0) {
						result = "extra non-zero data at end";
						goto done;
					}
				rb = fread(bb.data(), 1, bb.size(), b);
			}
			break;
		}
		if (rb < ra) {
			result = "decompressed image is shorter";
			break;
		}
		if (memcmp(ba.data(), bb.data(), ra) != 0) {
			for (size_t i = 0; i < ra; ++i)
				if (ba[i] != bb[i]) {
					char m[96];
					snprintf(m, sizeof(m), "MISMATCH at byte %llu", (unsigned long long)(pos + i));
					result = m;
					break;
				}
			break;
		}
		pos += ra;
	}
done:
	fclose(a);
	fclose(b);
	return result;
}

struct Progress {
	std::string label;
	double last = -1;
};
static void ShowProgress(Progress *p, double f) {
	if (f - p->last >= 0.002 || f >= 1.0) {
		p->last = f;
		printf("\r    %-34s %5.1f%%   ", p->label.c_str(), f * 100);
		fflush(stdout);
	}
}
static int ChdProgress(void *user, double f) {
	ShowProgress((Progress *)user, f);
	return 0;
}
static void PbpProgress(void *user, uint64_t done, uint64_t total) { ShowProgress((Progress *)user, total ? (double)done / total : 0); }

static uint32_t Tag(const char *s) { return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3]; }

struct Result {
	std::string format, setting;
	uint64_t input = 0, output = 0;
	double compSecs = 0, decompSecs = 0, readMs = -1;
	std::string verify = "-";
	bool failed = false;
	std::string error;
};

static void Report(const std::string &game, const Result &r) {
	if (r.failed) {
		Log("    %-42s FAILED: %s\n", (r.format + " " + r.setting).c_str(), r.error.c_str());
	} else {
		char rd[32] = "-";
		if (r.readMs >= 0) snprintf(rd, sizeof(rd), "%.3f", r.readMs);
		Log("    %-42s %14llu %8.3f%% %9.1f %8.1f %9.1f %9s  %s\n", (r.format + " " + r.setting).c_str(), (unsigned long long)r.output,
			100.0 * r.output / r.input, r.compSecs, r.input / 1e6 / r.compSecs, r.decompSecs, rd, r.verify.c_str());
	}
	if (g_csv) {
		fprintf(g_csv, "\"%s\",%s,\"%s\",%llu,%llu,%.3f,%.3f,%.4f,\"%s\"\n", game.c_str(), r.format.c_str(), r.setting.c_str(),
			(unsigned long long)r.input, (unsigned long long)r.output, r.compSecs, r.decompSecs, r.readMs,
			r.failed ? ("FAILED: " + r.error).c_str() : r.verify.c_str());
		fflush(g_csv);
	}
}

// Random sector reads, each from a hunk not read before in this test (so every read decompresses).
// Random block reads from a CSO v1 file, decompressing the whole block each time like PPSSPP.
static double CsoRandomReadMs(const fs::path &file, int reads) {
	FILE *f = OpenFile(file, "rb");
	if (!f) return -1;
	uint8_t hdr[24];
	if (fread(hdr, 1, 24, f) != 24 || memcmp(hdr, "CISO", 4) != 0) {
		fclose(f);
		return -1;
	}
	uint64_t total;
	uint32_t bs;
	memcpy(&total, hdr + 8, 8);
	memcpy(&bs, hdr + 16, 4);
	int align = hdr[21];
	uint32_t blocks = (uint32_t)((total + bs - 1) / bs);
	std::vector<uint32_t> index(blocks + 1);
	if (fread(index.data(), 4, blocks + 1, f) != blocks + 1) {
		fclose(f);
		return -1;
	}
	std::vector<uint32_t> order(blocks);
	for (uint32_t i = 0; i < blocks; ++i) order[i] = i;
	std::mt19937 rng(12345);
	std::shuffle(order.begin(), order.end(), rng);
	int n = std::min<int>(reads, (int)blocks);
	std::vector<uint8_t> in(bs + (1u << align) + 64), out(bs);
	auto t0 = Clock::now();
	for (int i = 0; i < n; ++i) {
		uint32_t b = order[i];
		bool plain = (index[b] & 0x80000000) != 0;
		uint64_t pos = (uint64_t)(index[b] & 0x7FFFFFFF) << align;
		uint64_t end = (uint64_t)(index[b + 1] & 0x7FFFFFFF) << align;
		size_t len = (size_t)std::min<uint64_t>(end - pos, in.size());
#ifdef _WIN32
		_fseeki64(f, (int64_t)pos, SEEK_SET);
#else
		fseeko(f, (off_t)pos, SEEK_SET);
#endif
		size_t got = fread(in.data(), 1, len, f);
		if (plain || len >= bs) continue;
		z_stream z{};
		inflateInit2(&z, -15);
		z.next_in = in.data();
		z.avail_in = (uInt)got;
		z.next_out = out.data();
		z.avail_out = bs;
		inflate(&z, Z_FINISH);
		inflateEnd(&z);
	}
	double ms = Secs(t0) * 1000.0 / std::max(1, n);
	fclose(f);
	return ms;
}

static int CsoProgress(void *user, int64_t pos, int64_t total, int64_t) {
	ShowProgress((Progress *)user, total ? (double)pos / total : 0);
	return 0;
}

static double ChdRandomReadMs(const fs::path &file, int reads) {
	chdm_file *f = nullptr;
	if (chd.open(file.u8string().c_str(), 0, nullptr, &f) != 0 || !f) return -1;
	chdm_info info{};
	chd.get_info(f, &info);
	uint32_t perHunk = info.hunk_bytes / std::max<uint32_t>(1, info.unit_bytes);
	std::vector<uint32_t> hunks(info.hunk_count);
	for (uint32_t i = 0; i < info.hunk_count; ++i) hunks[i] = i;
	std::mt19937 rng(12345);
	std::shuffle(hunks.begin(), hunks.end(), rng);
	int n = std::min<int>(reads, (int)hunks.size());
	std::vector<uint8_t> buf(info.unit_bytes);
	auto t0 = Clock::now();
	for (int i = 0; i < n; ++i) {
		uint64_t unit = (uint64_t)hunks[i] * perHunk + rng() % perHunk;
		if (unit >= info.unit_count) unit = info.unit_count - 1;
		chd.read_units(f, unit, buf.data(), 1);
	}
	double ms = Secs(t0) * 1000.0 / std::max(1, n);
	chd.close(f);
	return ms;
}

static void Remove(const fs::path &p) {
	std::error_code ec;
	fs::remove(p, ec);
}

// ---- Main ---------------------------------------------------------------------------------------

static std::vector<std::string> Split(const std::string &s) {
	std::vector<std::string> out;
	size_t p = 0;
	while (p <= s.size()) {
		size_t q = s.find(',', p);
		if (q == std::string::npos) q = s.size();
		if (q > p) out.push_back(s.substr(p, q - p));
		p = q + 1;
	}
	return out;
}

int main(int argc, char **argv) {
	std::vector<uint32_t> hunks = { 2048, 4096, 16384, 65536 };
	std::vector<std::string> codecs = { "default", "lzma", "zstd" };
	bool doChd = true, doPbp = true, doCso = true, verify = true;
	std::string csoSets = "B,D,L";
	int reads = 4000;
	unsigned threads = 0;
	std::vector<fs::path> inputs;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--hunks" && i + 1 < argc) {
			hunks.clear();
			for (auto &s : Split(argv[++i])) hunks.push_back((uint32_t)atoi(s.c_str()));
		} else if (a == "--codecs" && i + 1 < argc) codecs = Split(argv[++i]);
		else if (a == "--no-chd") doChd = false;
		else if (a == "--no-pbp") doPbp = false;
		else if (a == "--no-cso") doCso = false;
		else if (a == "--cso-sets" && i + 1 < argc) csoSets = argv[++i];
		else if (a == "--no-verify") verify = false;
		else if (a == "--reads" && i + 1 < argc) reads = atoi(argv[++i]);
		else if (a == "--threads" && i + 1 < argc) threads = (unsigned)atoi(argv[++i]);
		else inputs.push_back(fs::u8path(a));
	}
	if (inputs.empty()) inputs.push_back(fs::current_path());
#ifdef _WIN32
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	fs::path exeDir = fs::path(exePath).parent_path();
#else
	fs::path exeDir = fs::absolute(fs::path(argv[0])).parent_path();
#endif

	if (doChd) {
		LibHandle h = FindLib(CHD_LIB, exeDir);
		if (!h) printf("%s not found next to img_bench or in the current folder: skipping CHD.\n", CHD_LIB);
		else {
			bool ok = Get(h, "chdm_last_error_message", chd.last_error) && Get(h, "chdm_create_dvd", chd.create_dvd) &&
				Get(h, "chdm_extract_raw", chd.extract_raw) && Get(h, "chdm_open", chd.open) && Get(h, "chdm_close", chd.close) &&
				Get(h, "chdm_get_info", chd.get_info) && Get(h, "chdm_read_units", chd.read_units);
			chd.version = (const char *(*)(void))Sym(h, "chdm_version");
			chd.ok = ok;
		}
	}
	if (doPbp) {
		LibHandle h = FindLib(PBP_LIB, exeDir);
		if (!h) printf("%s not found next to img_bench or in the current folder: skipping PBP.\n", PBP_LIB);
		else {
			pbp.ok = Get(h, "ipbp_version", pbp.version) && Get(h, "ipbp_job_create", pbp.job_create) &&
				Get(h, "ipbp_job_destroy", pbp.job_destroy) && Get(h, "ipbp_job_set_progress", pbp.job_set_progress) &&
				Get(h, "ipbp_job_error", pbp.job_error) && Get(h, "ipbp_iso2pbp_options_init", pbp.iso2pbp_init) &&
				Get(h, "ipbp_pbp2iso_options_init", pbp.pbp2iso_init) && Get(h, "ipbp_iso_to_pbp", pbp.iso_to_pbp) &&
				Get(h, "ipbp_pbp_to_iso", pbp.pbp_to_iso);
		}
	}
	if (doCso) {
		LibHandle h = FindLib(CSO_LIB, exeDir);
		if (!h) printf("%s not found next to img_bench or in the current folder: skipping CSO.\n", CSO_LIB);
		else {
			bool ok = Get(h, "maxcso_version", cso.version) && Get(h, "maxcso_capi_version", cso.capi_version) &&
				Get(h, "maxcso_process", cso.process);
			if (ok && !(Get(h, "maxcso_options_init", cso.options_init) && Get(h, "maxcso_process_ex", cso.process_ex)))
				printf("This mCSO build has no maxcso_process_ex: build it from the feature/zopfli-splits-precheck branch. Skipping CSO.\n");
			else
				cso.ok = ok;
		}
	}
	if (!chd.ok && !pbp.ok && !cso.ok) {
		printf("Neither library could be used. Put CHDm.dll and/or iPBP.dll next to img_bench.exe.\n");
		return 1;
	}

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
		printf("No .iso files found.\n");
		return 1;
	}

	fs::path outRoot = fs::current_path() / "bench_lib";
	fs::path tmp = outRoot / "tmp";
	fs::create_directories(tmp);
	g_summary = OpenFile(outRoot / "summary.txt", "a");
	bool newCsv = !fs::exists(outRoot / "results.csv");
	g_csv = OpenFile(outRoot / "results.csv", "a");
	if (g_csv && newCsv) fprintf(g_csv, "game,format,setting,input_bytes,output_bytes,compress_s,decompress_s,random_read_ms,verify\n");

	uint64_t largest = 0;
	for (const auto &p : isos) largest = std::max<uint64_t>(largest, fs::file_size(p));
	std::error_code ec;
	auto sp = fs::space(tmp, ec);
	if (!ec && sp.available < largest * 2 + (256u << 20))
		Log("WARNING: only %.1f GB free next to the ISOs; up to %.1f GB is needed for temporary files.\n", sp.available / 1e9, (largest * 2) / 1e9);

	Log("img_bench: CSO %s (%s, sets %s)\n", cso.ok ? "on" : "off", cso.ok ? cso.version() : "-", csoSets.c_str());
	Log("img_bench: CHD %s (%s), PBP %s (%s), verify %s, %d random reads\n", chd.ok ? "on" : "off",
		chd.ok && chd.version ? chd.version() : "-", pbp.ok ? "on" : "off", pbp.ok ? pbp.version() : "-", verify ? "on" : "off", reads);
	Log("Columns: output bytes, of ISO, compress s, compress MB/s, decompress-to-ISO s, ms per random read of a new block or hunk (CHD, CSO), verify\n");

	for (const fs::path &iso : isos) {
		std::string game = iso.stem().u8string();
		uint64_t size = fs::file_size(iso);
		Log("\n=== %s (%.0f MB) ===\n", iso.filename().u8string().c_str(), size / 1e6);
		const std::string isoU8 = iso.u8string();
		fs::path back = tmp / "roundtrip.iso";

		if (chd.ok) {
			for (const std::string &codec : codecs) {
				uint32_t list[4] = { 0, 0, 0, 0 };
				const uint32_t *codecPtr = nullptr;
				if (codec == "default") codecPtr = nullptr;
				else {
					// "lzma+zstd" style lists are allowed.
					std::vector<std::string> parts;
					size_t p = 0;
					while (p <= codec.size()) {
						size_t q = codec.find('+', p);
						if (q == std::string::npos) q = codec.size();
						if (q > p) parts.push_back(codec.substr(p, q - p));
						p = q + 1;
					}
					for (size_t k = 0; k < parts.size() && k < 4; ++k) {
						std::string c = parts[k];
						c.resize(4, ' ');
						list[k] = Tag(c.c_str());
					}
					codecPtr = list;
				}
				for (uint32_t hb : hunks) {
					Result r;
					r.format = "CHD";
					char s[64];
					snprintf(s, sizeof(s), "%s hunk %u", codec.c_str(), hb);
					r.setting = s;
					r.input = size;
					fs::path out = tmp / "test.chd";
					Remove(out);
					Progress pr{ "CHD " + r.setting };
					auto t0 = Clock::now();
					int rc = chd.create_dvd(isoU8.c_str(), out.u8string().c_str(), hb, codecPtr, &ChdProgress, &pr);
					r.compSecs = Secs(t0);
					printf("\r%60s\r", "");
					if (rc != 0) {
						r.failed = true;
						r.error = chd.last_error();
					} else {
						r.output = fs::file_size(out);
						r.readMs = ChdRandomReadMs(out, reads);
						if (verify) {
							Remove(back);
							Progress pd{ "  decompressing" };
							t0 = Clock::now();
							rc = chd.extract_raw(out.u8string().c_str(), back.u8string().c_str(), &ChdProgress, &pd);
							r.decompSecs = Secs(t0);
							printf("\r%60s\r", "");
							r.verify = rc == 0 ? CompareImages(iso, back) : std::string("extract failed: ") + chd.last_error();
							Remove(back);
						}
					}
					Remove(out);
					Report(game, r);
				}
			}
		}

		if (cso.ok) {
			std::vector<std::string> want = Split(csoSets);
			for (const CsoSet &set : CSO_SETS) {
				if (std::find(want.begin(), want.end(), std::string(set.key)) == want.end()) continue;
				Result r;
				r.format = "CSO";
				r.setting = std::string(set.key) + " " + set.name;
				r.input = size;
				fs::path out = tmp / "test.cso";
				Remove(out);
				maxcso_options o;
				cso.options_init(&o, 2);
				o.flags = set.flags;
				o.block_size = 262144;
				o.zopfli_split_max = set.splits;
				o.precheck_percent = set.precheck;
				char err[512] = "";
				Progress pr{ "CSO " + r.setting };
				auto t0 = Clock::now();
				int rc = cso.process_ex(isoU8.c_str(), out.u8string().c_str(), &o, &CsoProgress, &pr, err, sizeof(err));
				r.compSecs = Secs(t0);
				printf("\r%70s\r", "");
				if (rc != 0) {
					r.failed = true;
					r.error = err;
				} else {
					r.output = fs::file_size(out);
					r.readMs = CsoRandomReadMs(out, reads);
					if (verify) {
						Remove(back);
						Progress pd{ "  decompressing" };
						t0 = Clock::now();
						rc = cso.process(out.u8string().c_str(), back.u8string().c_str(), 0, &CsoProgress, &pd, err, sizeof(err));
						r.decompSecs = Secs(t0);
						printf("\r%70s\r", "");
						r.verify = rc == 0 ? CompareImages(iso, back) : std::string("decompress failed: ") + err;
						Remove(back);
					}
				}
				Remove(out);
				Report(game, r);
			}
		}

		if (pbp.ok) {
			uint64_t lzrcSize = 0;
			for (int compress : { 1, 2, 0 }) {
				Result r;
				r.format = "PBP";
				r.setting = compress == 2 ? "LZRC max" : compress ? "LZRC" : "uncompressed";
				r.input = size;
				fs::path out = tmp / "EBOOT.PBP";
				Remove(out);
				ipbp_job *job = pbp.job_create();
				Progress pr{ "PBP " + r.setting };
				pbp.job_set_progress(job, &PbpProgress, &pr);
				ipbp_iso2pbp_options o;
				pbp.iso2pbp_init(&o);
				o.compress = compress;
				o.threads = threads;
				auto t0 = Clock::now();
				int rc = pbp.iso_to_pbp(job, isoU8.c_str(), out.u8string().c_str(), &o);
				r.compSecs = Secs(t0);
				printf("\r%60s\r", "");
				if (rc != 0) {
					r.failed = true;
					r.error = pbp.job_error(job);
				} else {
					r.output = fs::file_size(out);
					if (verify) {
						Remove(back);
						ipbp_pbp2iso_options d;
						pbp.pbp2iso_init(&d);
						d.verify = 1;
						d.threads = threads;
						Progress pd{ "  decompressing" };
						pbp.job_set_progress(job, &PbpProgress, &pd);
						t0 = Clock::now();
						rc = pbp.pbp_to_iso(job, out.u8string().c_str(), back.u8string().c_str(), &d);
						r.decompSecs = Secs(t0);
						printf("\r%60s\r", "");
						r.verify = rc == 0 ? CompareImages(iso, back) : std::string("extract failed: ") + pbp.job_error(job);
						Remove(back);
					}
				}
				pbp.job_destroy(job);
				Remove(out);
				if (compress == 1) lzrcSize = r.output;
				// An iPBP.dll older than the max mode treats 2 as plain LZRC.
				if (compress == 2 && !r.failed && r.output == lzrcSize) r.verify += " (same as LZRC: iPBP.dll without max mode?)";
				Report(game, r);
			}
		}
	}
	Log("\nDone. Send back bench_lib (summary.txt and results.csv).\n");
	if (g_summary) fclose(g_summary);
	if (g_csv) fclose(g_csv);
	std::error_code e2;
	fs::remove(tmp, e2);
	return 0;
}
