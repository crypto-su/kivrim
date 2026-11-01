#define _POSIX_C_SOURCE 200809L
#include "kivrim.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_LIMIT ((size_t)1024 * 1024 * 1024)
#define DEFAULT_OUTPUT_LIMIT ((size_t)512 * 1024 * 1024)

static void usage(void)
{
    fprintf(stderr,
        "KIVRIM %s\n"
        "Usage: kivrim c [--fast] INPUT OUTPUT\n"
        "       kivrim d [--max-output BYTES] INPUT OUTPUT\n"
        "       kivrim info INPUT\n"
        "Existing outputs are never overwritten. Input limit: 1 GiB.\n"
        "Default decompressed output limit: 512 MiB. Files are processed in memory.\n",
        KVR_VERSION);
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    off_t end;
    if (!f) { perror(path); return NULL; }
    if (fseeko(f, 0, SEEK_END) || (end = ftello(f)) < 0 ||
        (uintmax_t)end > FILE_LIMIT || fseeko(f, 0, SEEK_SET)) {
        fprintf(stderr, "%s: cannot read file, or input exceeds 1 GiB\n", path);
        fclose(f);
        return NULL;
    }
    *size = (size_t)end;
    p = (unsigned char *)malloc(*size ? *size : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); fclose(f); return NULL; }
    if (fread(p, 1, *size, f) != *size) {
        fprintf(stderr, "%s: read failed\n", path);
        free(p); fclose(f); return NULL;
    }
    if (fclose(f)) { perror(path); free(p); return NULL; }
    return p;
}

static int write_file(const char *path, const unsigned char *p, size_t n)
{
    FILE *f = fopen(path, "wbx"); /* C11 exclusive creation, including symlinks. */
    int failed;
    if (!f) { perror(path); return 0; }
    failed = fwrite(p, 1, n, f) != n;
    if (fclose(f)) failed = 1;
    if (failed) {
        fprintf(stderr, "%s: write failed; removing incomplete output\n", path);
        (void)remove(path);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char *input, *output = NULL;
    size_t input_size, capacity = 0, output_size = 0, limit = DEFAULT_OUTPUT_LIMIT;
    kvr_mode mode = KVR_AUTO;
    kvr_status status;
    int arg = 2, compress, info, result = 1;
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("KIVRIM " KVR_VERSION); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(); return 0; }
    if (argc < 3) { usage(); return 2; }
    compress = !strcmp(argv[1], "c");
    info = !strcmp(argv[1], "info");
    if (!compress && !info && strcmp(argv[1], "d")) { usage(); return 2; }
    if (compress && arg < argc && !strcmp(argv[arg], "--fast")) {
        mode = KVR_FAST; ++arg;
    }
    if (!compress && !info && arg < argc && !strcmp(argv[arg], "--max-output")) {
        char *end;
        uintmax_t parsed;
        if (++arg == argc || argv[arg][0] < '0' || argv[arg][0] > '9') {
            usage(); return 2;
        }
        errno = 0;
        parsed = strtoull(argv[arg], &end, 10);
        if (errno || *end || parsed > SIZE_MAX) { usage(); return 2; }
        limit = (size_t)parsed;
        ++arg;
    }
    if (argc - arg != (info ? 1 : 2)) { usage(); return 2; }
    input = read_file(argv[arg], &input_size);
    if (!input) return 1;
    if (compress) {
        capacity = kvr_compress_bound(input_size);
        status = capacity ? KVR_OK : KVR_SIZE_OVERFLOW;
    } else {
        status = kvr_decompressed_size(input, input_size, &capacity);
    }
    if (status != KVR_OK) goto done;
    if (info) {
        printf("format: KVR1\nframe_bytes: %zu\ndecoded_bytes: %zu\n"
               "integrity: not checked (header only)\n", input_size, capacity);
        result = 0; goto done;
    }
    if (!compress && capacity > limit) {
        fprintf(stderr, "declared output %zu exceeds limit %zu; use --max-output BYTES\n",
                capacity, limit);
        goto done;
    }
    output = (unsigned char *)malloc(capacity ? capacity : 1);
    if (!output) { status = KVR_OUT_OF_MEMORY; goto done; }
    status = compress ? kvr_compress(input, input_size, output, capacity, &output_size, mode)
                      : kvr_decompress(input, input_size, output, capacity, &output_size);
    if (status == KVR_OK && write_file(argv[arg + 1], output, output_size)) {
        fprintf(stderr, "%zu -> %zu bytes\n", input_size, output_size);
        result = 0;
    }
done:
    if (status != KVR_OK) fprintf(stderr, "%s\n", kvr_status_string(status));
    free(input); free(output);
    return result;
}
