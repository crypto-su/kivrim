#ifndef KIVRIM_H
#define KIVRIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVR_VERSION "0.1.0"
#define KVR_BLOCK_SIZE 65536u
#define KVR_HEADER_SIZE 24u
#define KVR_BLOCK_HEADER_SIZE 9u

typedef enum {
    KVR_OK = 0,
    KVR_INVALID_ARGUMENT,
    KVR_BUFFER_TOO_SMALL,
    KVR_BAD_FRAME,
    KVR_CHECKSUM_MISMATCH,
    KVR_OUT_OF_MEMORY,
    KVR_SIZE_OVERFLOW
} kvr_status;

typedef enum {
    KVR_FAST = 0, /* LZ with a repeated-distance code. */
    KVR_AUTO = 1  /* Also test a sampled, reversible strided delta transform. */
} kvr_mode;

/* Returns zero on overflow. The bound includes framing and raw fallback. */
size_t kvr_compress_bound(size_t src_size);

/* Buffers must not overlap. A NULL buffer is valid only when its size is zero.
 * *written is zero on failure; destination contents are unspecified on failure.
 * Compression allocates a fixed-size workspace; decompression allocates none.
 * No global mutable state: independent calls may run concurrently. */
kvr_status kvr_compress(const void *src, size_t src_size, void *dst,
                        size_t dst_capacity, size_t *written, kvr_mode mode);
kvr_status kvr_decompress(const void *src, size_t src_size, void *dst,
                          size_t dst_capacity, size_t *written);

/* Reads UNTRUSTED size metadata, validates the header only. Apply an application
 * resource limit before allocating. This does not validate blocks or checksum. */
kvr_status kvr_decompressed_size(const void *src, size_t src_size, size_t *size);
const char *kvr_status_string(kvr_status status);

#ifdef __cplusplus
}
#endif
#endif
