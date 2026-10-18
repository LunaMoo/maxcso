/* Round trip through the mCSO C API: ISO -> CSO (fast and standard) -> ISO, compared byte for byte.
 * Usage: mcso_capi_test <scratch directory> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "maxcso_capi.h"

#define ISO_SIZE (2048 * 600)

static int MAXCSO_CAPI_CALL progress(void *userdata, int64_t pos, int64_t total, int64_t written) {
	(void)pos; (void)total; (void)written;
	++*(int *)userdata;
	return 0;
}

static unsigned char *read_file(const char *path, long *size) {
	FILE *f = fopen(path, "rb");
	unsigned char *data;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (unsigned char *)malloc(*size > 0 ? *size : 1);
	if (fread(data, 1, *size, f) != (size_t)*size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

static int round_trip(const char *dir, const unsigned char *iso, int mode, const char *name) {
	char iso_path[1024], cso_path[1024], out_path[1024], error[256] = "";
	long cso_size = 0, out_size = 0;
	unsigned char *cso, *out;
	int calls = 0, rc;

	snprintf(iso_path, sizeof(iso_path), "%s/mcso_test.iso", dir);
	snprintf(cso_path, sizeof(cso_path), "%s/mcso_test_%s.cso", dir, name);
	snprintf(out_path, sizeof(out_path), "%s/mcso_test_%s.iso", dir, name);

	rc = maxcso_process(iso_path, cso_path, mode, progress, &calls, error, sizeof(error));
	if (rc != MAXCSO_RESULT_SUCCESS) {
		fprintf(stderr, "%s: compress failed (%d): %s\n", name, rc, error);
		return 1;
	}
	rc = maxcso_process(cso_path, out_path, MAXCSO_MODE_DECOMPRESS, progress, &calls, error, sizeof(error));
	if (rc != MAXCSO_RESULT_SUCCESS) {
		fprintf(stderr, "%s: decompress failed (%d): %s\n", name, rc, error);
		return 1;
	}
	cso = read_file(cso_path, &cso_size);
	out = read_file(out_path, &out_size);
	if (!cso || !out || out_size != ISO_SIZE || memcmp(out, iso, ISO_SIZE) != 0) {
		fprintf(stderr, "%s: round trip mismatch (iso %ld bytes)\n", name, out_size);
		return 1;
	}
	if (cso_size <= 24 || cso_size >= ISO_SIZE || memcmp(cso, "CISO", 4) != 0) {
		fprintf(stderr, "%s: unexpected CSO (%ld bytes)\n", name, cso_size);
		return 1;
	}
	printf("%s: %d -> %ld bytes, %d progress calls\n", name, ISO_SIZE, cso_size, calls);
	free(cso);
	free(out);
	remove(cso_path);
	remove(out_path);
	return 0;
}

struct memory {
	const unsigned char *in;
	unsigned char *out;
	uint64_t size;
	uint64_t end; /* highest offset written */
	int refuse;
	int slack;    /* begin allocates twice the size (compressed output can outgrow the input a bit) */
};

static int64_t MAXCSO_CAPI_CALL mem_read(void *user, uint64_t offset, void *buffer, size_t length) {
	struct memory *m = (struct memory *)user;
	if (offset >= m->size)
		return 0;
	if (length > m->size - offset)
		length = (size_t)(m->size - offset);
	memcpy(buffer, m->in + offset, length);
	return (int64_t)length;
}

static int MAXCSO_CAPI_CALL mem_begin(void *user, uint64_t size) {
	struct memory *m = (struct memory *)user;
	if (m->refuse)
		return 1;
	m->size = m->slack ? size * 2 : size;
	m->out = (unsigned char *)calloc((size_t)m->size, 1);
	return m->out ? 0 : 1;
}

static int64_t MAXCSO_CAPI_CALL mem_write(void *user, uint64_t offset, const void *buffer, size_t length) {
	struct memory *m = (struct memory *)user;
	if (!m->out || offset > m->size || length > m->size - offset)
		return -1;
	memcpy(m->out + offset, buffer, length);
	if (offset + length > m->end)
		m->end = offset + length; /* the test's writes come from several threads, but only read after */
	return (int64_t)length;
}

static int MAXCSO_CAPI_CALL cancel_progress(void *userdata, int64_t pos, int64_t total, int64_t written) {
	(void)userdata; (void)pos; (void)total; (void)written;
	return 1;
}

/* ISO in memory -> CSO file must match ISO file -> CSO file; that CSO back into memory must be the ISO. */
static int memory_round_trip(const char *dir, const unsigned char *iso) {
	char iso_path[1024], file_cso[1024], mem_cso[1024], error[256] = "";
	long file_size = 0, mem_size = 0;
	unsigned char *a, *b;
	struct memory m;
	maxcso_reader reader;
	maxcso_writer writer;
	maxcso_options opts;
	int rc, i, failed = 0;

	snprintf(iso_path, sizeof(iso_path), "%s/mcso_test.iso", dir);
	snprintf(file_cso, sizeof(file_cso), "%s/mcso_test_file.cso", dir);
	snprintf(mem_cso, sizeof(mem_cso), "%s/mcso_test_mem.cso", dir);
	maxcso_options_init(&opts, MAXCSO_MODE_FAST);

	memset(&m, 0, sizeof(m));
	m.in = iso;
	m.size = ISO_SIZE;
	reader.user = &m;
	reader.size = ISO_SIZE;
	reader.read_at = mem_read;
	rc = maxcso_process_ex(iso_path, file_cso, &opts, NULL, NULL, error, sizeof(error));
	if (rc == MAXCSO_RESULT_SUCCESS)
		rc = maxcso_process_reader(&reader, mem_cso, &opts, NULL, NULL, error, sizeof(error));
	if (rc != MAXCSO_RESULT_SUCCESS) {
		fprintf(stderr, "memory: compress failed (%d): %s\n", rc, error);
		return 1;
	}
	a = read_file(file_cso, &file_size);
	b = read_file(mem_cso, &mem_size);
	if (!a || !b || file_size != mem_size || memcmp(a, b, file_size) != 0) {
		fprintf(stderr, "memory: CSO from memory differs (%ld vs %ld bytes)\n", mem_size, file_size);
		failed = 1;
	}
	free(b);

	/* ISO (file, then memory) -> CSO in memory: must match the CSO file too. */
	writer.user = &m;
	writer.begin = mem_begin;
	writer.write_at = mem_write;
	for (i = 0; i < 2 && a; ++i) {
		struct memory src;
		memset(&src, 0, sizeof(src));
		src.in = iso;
		src.size = ISO_SIZE;
		reader.user = &src;
		memset(&m, 0, sizeof(m));
		m.slack = 1;
		rc = maxcso_process_io(i == 0 ? iso_path : NULL, &reader, &writer, &opts, NULL, NULL, error, sizeof(error));
		if (rc != MAXCSO_RESULT_SUCCESS || m.end != (uint64_t)file_size || memcmp(m.out, a, file_size) != 0) {
			fprintf(stderr, "memory: CSO into memory (%s) differs (%d, %llu vs %ld bytes): %s\n", i == 0 ? "from file" : "from memory", rc,
				(unsigned long long)m.end, file_size, error);
			failed = 1;
		}
		free(m.out);
	}
	reader.user = &m;
	if (maxcso_process_io(NULL, NULL, &writer, &opts, NULL, NULL, error, sizeof(error)) != MAXCSO_RESULT_INVALID_OPTION) {
		fprintf(stderr, "memory: maxcso_process_io took no input\n");
		failed = 1;
	}
	free(a);

	memset(&m, 0, sizeof(m));
	writer.user = &m;
	writer.begin = mem_begin;
	writer.write_at = mem_write;
	rc = maxcso_decompress_writer(mem_cso, &writer, NULL, NULL, error, sizeof(error));
	if (rc != MAXCSO_RESULT_SUCCESS || m.size != ISO_SIZE || memcmp(m.out, iso, ISO_SIZE) != 0) {
		fprintf(stderr, "memory: decompress failed (%d, %llu bytes): %s\n", rc, (unsigned long long)m.size, error);
		failed = 1;
	}
	free(m.out);

	memset(&m, 0, sizeof(m));
	m.refuse = 1;
	if (maxcso_decompress_writer(mem_cso, &writer, NULL, NULL, error, sizeof(error)) != MAXCSO_RESULT_BAD_OUTPUT) {
		fprintf(stderr, "memory: begin couldn't refuse\n");
		failed = 1;
	}
	memset(&m, 0, sizeof(m));
	if (maxcso_decompress_writer(mem_cso, &writer, cancel_progress, NULL, error, sizeof(error)) != MAXCSO_RESULT_CANCELLED) {
		fprintf(stderr, "memory: cancel didn't stop it\n");
		failed = 1;
	}
	free(m.out);

	if (!failed)
		printf("memory: ok\n");
	remove(file_cso);
	remove(mem_cso);
	return failed;
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : ".";
	char iso_path[1024];
	unsigned char *iso = (unsigned char *)malloc(ISO_SIZE);
	unsigned int seed = 12345;
	maxcso_options opts;
	FILE *f;
	int i, failed = 0;

	if (maxcso_capi_version() != MAXCSO_CAPI_VERSION) {
		fprintf(stderr, "API version %d, header %d\n", maxcso_capi_version(), MAXCSO_CAPI_VERSION);
		return 1;
	}
	printf("maxcso %s\n", maxcso_version());
	if (maxcso_options_init(&opts, MAXCSO_MODE_STANDARD) != MAXCSO_RESULT_SUCCESS || opts.struct_size != sizeof(opts)) {
		fprintf(stderr, "maxcso_options_init failed\n");
		return 1;
	}

	/* Mixed content: text-like runs, zeros and incompressible noise. */
	for (i = 0; i < ISO_SIZE; ++i) {
		int sector = i / 2048;
		seed = seed * 1103515245u + 12345u;
		if (sector % 3 == 0)
			iso[i] = (unsigned char)("PSP GAME DATA "[i % 14]);
		else if (sector % 3 == 1)
			iso[i] = 0;
		else
			iso[i] = (unsigned char)(seed >> 16);
	}
	snprintf(iso_path, sizeof(iso_path), "%s/mcso_test.iso", dir);
	f = fopen(iso_path, "wb");
	if (!f || fwrite(iso, 1, ISO_SIZE, f) != ISO_SIZE) {
		fprintf(stderr, "cannot write %s\n", iso_path);
		return 1;
	}
	fclose(f);

	failed |= round_trip(dir, iso, MAXCSO_MODE_FAST, "fast");
	failed |= round_trip(dir, iso, MAXCSO_MODE_STANDARD, "standard");
	failed |= memory_round_trip(dir, iso);

	remove(iso_path);
	free(iso);
	return failed;
}
