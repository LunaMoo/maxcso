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

	remove(iso_path);
	free(iso);
	return failed;
}
