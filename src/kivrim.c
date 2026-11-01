#include "kivrim.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HASH_BITS 14u
#define HASH_SIZE (1u << HASH_BITS)
#define MIN_MATCH 4u

typedef struct {
    uint32_t table[HASH_SIZE];
    unsigned char plain[KVR_BLOCK_SIZE];
    unsigned char delta[KVR_BLOCK_SIZE];
    unsigned char candidate[KVR_BLOCK_SIZE];
} workspace;

static uint32_t read32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write32(unsigned char *p, uint32_t n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
    p[2] = (unsigned char)(n >> 16);
    p[3] = (unsigned char)(n >> 24);
}

static uint64_t read64(const unsigned char *p)
{
    return (uint64_t)read32(p) | ((uint64_t)read32(p + 4) << 32);
}

static void write64(unsigned char *p, uint64_t n)
{
    write32(p, (uint32_t)n);
    write32(p + 4, (uint32_t)(n >> 32));
}

static uint32_t rotate32(uint32_t n, unsigned int r)
{
    return (n << r) | (n >> (32u - r));
}

/* Independent implementation of the published XXH32 algorithm, seed zero.
 * Algorithm by Yann Collet.
 * This is an accidental-corruption checksum, not authentication. */
static uint32_t checksum(const unsigned char *p, size_t n)
{
    const uint32_t p1 = UINT32_C(2654435761), p2 = UINT32_C(2246822519);
    const uint32_t p3 = UINT32_C(3266489917), p4 = UINT32_C(668265263);
    const uint32_t p5 = UINT32_C(374761393);
    size_t i = 0;
    uint32_t h;
    if (n >= 16) {
        uint32_t a = p1 + p2, b = p2, c = 0, d = 0u - p1;
        do {
            a = rotate32(a + read32(p + i) * p2, 13) * p1;
            b = rotate32(b + read32(p + i + 4) * p2, 13) * p1;
            c = rotate32(c + read32(p + i + 8) * p2, 13) * p1;
            d = rotate32(d + read32(p + i + 12) * p2, 13) * p1;
            i += 16;
        } while (n - i >= 16);
        h = rotate32(a, 1) + rotate32(b, 7) + rotate32(c, 12) + rotate32(d, 18);
    } else {
        h = p5;
    }
    h += (uint32_t)n;
    while (n - i >= 4) {
        h = rotate32(h + read32(p + i) * p3, 17) * p4;
        i += 4;
    }
    while (i < n) h = rotate32(h + (uint32_t)p[i++] * p5, 11) * p1;
    h ^= h >> 15;
    h *= p2;
    h ^= h >> 13;
    h *= p3;
    return h ^ (h >> 16);
}

static uint32_t hash4(const unsigned char *p)
{
    return (read32(p) * UINT32_C(2654435761)) >> (32u - HASH_BITS);
}

static size_t match_length(const unsigned char *a, const unsigned char *b, size_t n)
{
    size_t i = MIN_MATCH;
    while (n - i >= 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8);
        memcpy(&y, b + i, 8);
        if (x != y) break;
        i += 8;
    }
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

static int put_varint(unsigned char *dst, size_t cap, size_t *pos, size_t n)
{
    do {
        unsigned char b = (unsigned char)(n & 127u);
        n >>= 7;
        if (*pos == cap) return 0;
        dst[(*pos)++] = (unsigned char)(b | (n ? 128u : 0u));
    } while (n);
    return 1;
}

/* All wire varints are canonical and bounded by one block (three bytes). */
static int get_varint(const unsigned char *src, size_t n, size_t *pos, size_t *v)
{
    size_t value = 0;
    unsigned int shift;
    for (shift = 0; shift <= 14; shift += 7) {
        unsigned char b;
        if (*pos == n) return 0;
        b = src[(*pos)++];
        value |= (size_t)(b & 127u) << shift;
        if (!(b & 128u)) {
            if ((shift && !(b & 127u)) || value > KVR_BLOCK_SIZE) return 0;
            *v = value;
            return 1;
        }
    }
    return 0;
}

static int emit_sequence(unsigned char *dst, size_t cap, size_t *op,
                         const unsigned char *literals, size_t ll,
                         size_t ml, size_t distance, size_t *last_distance)
{
    size_t m = ml ? ml - MIN_MATCH : 0;
    if (*op == cap) return 0;
    dst[(*op)++] = (unsigned char)(((ll < 15 ? ll : 15) << 4) | (m < 15 ? m : 15));
    if (ll >= 15 && !put_varint(dst, cap, op, ll - 15)) return 0;
    if (ll > cap - *op) return 0;
    memcpy(dst + *op, literals, ll);
    *op += ll;
    if (!ml) return 1;
    if (!put_varint(dst, cap, op, distance == *last_distance ? 0 : distance)) return 0;
    *last_distance = distance;
    return m < 15 || put_varint(dst, cap, op, m - 15);
}

/* Returns zero if it cannot beat raw storage. No speculative reads/writes. */
static size_t lz_encode(const unsigned char *src, size_t n, unsigned char *dst,
                        uint32_t *table)
{
    size_t pos = 0, anchor = 0, op = 0, last = 0;
    memset(table, 0, HASH_SIZE * sizeof(*table));
    while (n - pos >= MIN_MATCH) {
        uint32_t h = hash4(src + pos), entry = table[h];
        size_t best = 0, distance = 0;
        table[h] = (uint32_t)(pos + 1);
        if (last && pos >= last && read32(src + pos) == read32(src + pos - last)) {
            best = match_length(src + pos, src + pos - last, n - pos);
            distance = last;
        }
        if (entry) {
            size_t ref = (size_t)entry - 1;
            if (pos - ref != distance && read32(src + ref) == read32(src + pos)) {
                size_t length = match_length(src + pos, src + ref, n - pos);
                if (length > best) {
                    best = length;
                    distance = pos - ref;
                }
            }
        }
        if (!best) {
            ++pos;
            continue;
        }
        if (!emit_sequence(dst, n, &op, src + anchor, pos - anchor,
                           best, distance, &last)) return 0;
        pos += best;
        anchor = pos;
        if (pos >= 2 && n - pos >= 2)
            table[hash4(src + pos - 2)] = (uint32_t)(pos - 1);
        if (pos && n - pos >= 3)
            table[hash4(src + pos - 1)] = (uint32_t)pos;
    }
    if (anchor < n && !emit_sequence(dst, n, &op, src + anchor, n - anchor, 0, 0, &last))
        return 0;
    return op < n ? op : 0;
}

static int lz_decode(const unsigned char *src, size_t n, unsigned char *dst, size_t cap)
{
    size_t ip = 0, op = 0, last = 0;
    while (ip < n) {
        unsigned char token = src[ip++];
        size_t ll = token >> 4, ml = (token & 15u) + MIN_MATCH, ext, distance;
        if (ll == 15) {
            if (!get_varint(src, n, &ip, &ext)) return 0;
            ll += ext;
        }
        if (ll > n - ip || ll > cap - op) return 0;
        memcpy(dst + op, src + ip, ll);
        ip += ll;
        op += ll;
        if (op == cap) return ip == n && (token & 15u) == 0;
        if (!get_varint(src, n, &ip, &distance)) return 0;
        if (!distance) distance = last;
        if (!distance || distance > op || distance >= KVR_BLOCK_SIZE) return 0;
        last = distance;
        if ((token & 15u) == 15) {
            if (!get_varint(src, n, &ip, &ext)) return 0;
            ml += ext;
        }
        if (ml > cap - op) return 0;
        if (distance == 1) {
            memset(dst + op, dst[op - 1], ml);
        } else if (ml <= distance) {
            memcpy(dst + op, dst + op - distance, ml);
        } else {
            size_t copied = distance;
            memcpy(dst + op, dst + op - distance, copied);
            while (copied < ml) {
                size_t chunk = ml - copied < copied ? ml - copied : copied;
                memcpy(dst + op + copied, dst + op, chunk);
                copied += chunk;
            }
        }
        op += ml;
        if (op == cap) return ip == n;
    }
    return op == cap;
}

static void delta_encode(const unsigned char *src, unsigned char *dst, size_t n, size_t stride)
{
    size_t i;
    memcpy(dst, src, stride);
    for (i = stride; i < n; ++i) dst[i] = (unsigned char)(src[i] - src[i - stride]);
}

static void delta_decode(unsigned char *p, size_t n, size_t stride)
{
    size_t i;
    for (i = stride; i < n; ++i) p[i] = (unsigned char)(p[i] + p[i - stride]);
}

/* A cheap, deliberately testable hypothesis: the middle 2 KiB predicts the
 * useful stride. Full-block comparison always prevents a size regression. */
static unsigned int choose_delta(const unsigned char *src, size_t n, workspace *w)
{
    const size_t sample = 2048;
    const unsigned char *p = src + (n - sample) / 2;
    size_t baseline = lz_encode(p, sample, w->candidate, w->table), best;
    unsigned int mode, selected = 0;
    if (!baseline) baseline = sample;
    best = baseline;
    for (mode = 2; mode <= 5; ++mode) {
        size_t stride = (size_t)1 << (mode - 2);
        size_t encoded;
        delta_encode(p, w->delta, sample, stride);
        encoded = lz_encode(w->delta, sample, w->candidate, w->table);
        if (encoded && encoded < best) {
            best = encoded;
            selected = mode;
        }
    }
    return best + 16 < baseline - baseline / 8 ? selected : 0;
}

size_t kvr_compress_bound(size_t n)
{
    size_t blocks = n / KVR_BLOCK_SIZE + (n % KVR_BLOCK_SIZE != 0);
    if (n > SIZE_MAX - KVR_HEADER_SIZE ||
        blocks > (SIZE_MAX - KVR_HEADER_SIZE - n) / KVR_BLOCK_HEADER_SIZE) return 0;
    return KVR_HEADER_SIZE + n + blocks * KVR_BLOCK_HEADER_SIZE;
}

kvr_status kvr_compress(const void *input, size_t n, void *output, size_t cap,
                        size_t *written, kvr_mode mode)
{
    const unsigned char empty = 0;
    const unsigned char *src = input ? (const unsigned char *)input : &empty;
    unsigned char *dst = (unsigned char *)output;
    size_t bound = kvr_compress_bound(n), ip = 0, op = KVR_HEADER_SIZE;
    workspace *w;
    if (!written) return KVR_INVALID_ARGUMENT;
    *written = 0;
    if ((!input && n) || (!output && cap) || (mode != KVR_FAST && mode != KVR_AUTO))
        return KVR_INVALID_ARGUMENT;
    if (!bound) return KVR_SIZE_OVERFLOW;
    if (cap < bound) return KVR_BUFFER_TOO_SMALL;
    w = n ? (workspace *)malloc(sizeof(*w)) : NULL;
    if (n && !w) return KVR_OUT_OF_MEMORY;
    memcpy(dst, "KVR1", 4);
    dst[4] = 1; dst[5] = 0; dst[6] = 16; dst[7] = 0;
    write64(dst + 8, (uint64_t)n);
    write32(dst + 16, checksum(src, n));
    write32(dst + 20, 0);
    while (ip < n) {
        size_t block = n - ip < KVR_BLOCK_SIZE ? n - ip : KVR_BLOCK_SIZE;
        size_t encoded = lz_encode(src + ip, block, w->plain, w->table);
        const unsigned char *payload = encoded ? w->plain : src + ip;
        size_t stored = encoded ? encoded : block;
        unsigned int block_mode = encoded ? 1u : 0u;
        if (mode == KVR_AUTO && block >= 2048 && stored > block / 32) {
            unsigned int selected = choose_delta(src + ip, block, w);
            if (selected) {
                size_t candidate;
                delta_encode(src + ip, w->delta, block, (size_t)1 << (selected - 2));
                candidate = lz_encode(w->delta, block, w->candidate, w->table);
                if (candidate && candidate < stored) {
                    payload = w->candidate;
                    stored = candidate;
                    block_mode = selected;
                }
            }
        }
        write32(dst + op, (uint32_t)block);
        write32(dst + op + 4, (uint32_t)stored);
        dst[op + 8] = (unsigned char)block_mode;
        op += KVR_BLOCK_HEADER_SIZE;
        memcpy(dst + op, payload, stored);
        op += stored;
        ip += block;
    }
    free(w);
    *written = op;
    return KVR_OK;
}

kvr_status kvr_decompressed_size(const void *input, size_t n, size_t *size)
{
    const unsigned char *src = (const unsigned char *)input;
    uint64_t decoded;
    if (!size) return KVR_INVALID_ARGUMENT;
    *size = 0;
    if (!input && n) return KVR_INVALID_ARGUMENT;
    if (n < KVR_HEADER_SIZE) return KVR_BAD_FRAME;
    if (memcmp(src, "KVR1", 4) || src[4] != 1 || src[5] || src[6] != 16 ||
        src[7] || read32(src + 20)) return KVR_BAD_FRAME;
    decoded = read64(src + 8);
    if (decoded > SIZE_MAX) return KVR_SIZE_OVERFLOW;
    *size = (size_t)decoded;
    return KVR_OK;
}

kvr_status kvr_decompress(const void *input, size_t n, void *output, size_t cap,
                          size_t *written)
{
    unsigned char empty = 0;
    const unsigned char *src = (const unsigned char *)input;
    unsigned char *dst = output ? (unsigned char *)output : &empty;
    size_t expected, ip = KVR_HEADER_SIZE, op = 0;
    kvr_status status;
    if (!written) return KVR_INVALID_ARGUMENT;
    *written = 0;
    if ((!input && n) || (!output && cap)) return KVR_INVALID_ARGUMENT;
    status = kvr_decompressed_size(input, n, &expected);
    if (status != KVR_OK) return status;
    if (expected > cap) return KVR_BUFFER_TOO_SMALL;
    while (op < expected) {
        size_t block, stored, remaining = expected - op;
        unsigned int mode;
        if (n - ip < KVR_BLOCK_HEADER_SIZE) return KVR_BAD_FRAME;
        block = read32(src + ip);
        stored = read32(src + ip + 4);
        mode = src[ip + 8];
        ip += KVR_BLOCK_HEADER_SIZE;
        if (block != (remaining < KVR_BLOCK_SIZE ? remaining : KVR_BLOCK_SIZE) ||
            !stored || stored > n - ip || mode > 5) return KVR_BAD_FRAME;
        if (mode == 0) {
            if (stored != block) return KVR_BAD_FRAME;
            memcpy(dst + op, src + ip, block);
        } else {
            if (stored >= block || !lz_decode(src + ip, stored, dst + op, block))
                return KVR_BAD_FRAME;
            if (mode >= 2) delta_decode(dst + op, block, (size_t)1 << (mode - 2));
        }
        ip += stored;
        op += block;
    }
    if (ip != n) return KVR_BAD_FRAME;
    if (checksum(dst, expected) != read32(src + 16)) return KVR_CHECKSUM_MISMATCH;
    *written = expected;
    return KVR_OK;
}

const char *kvr_status_string(kvr_status status)
{
    switch (status) {
    case KVR_OK: return "success";
    case KVR_INVALID_ARGUMENT: return "invalid argument";
    case KVR_BUFFER_TOO_SMALL: return "buffer too small";
    case KVR_BAD_FRAME: return "invalid or truncated frame";
    case KVR_CHECKSUM_MISMATCH: return "checksum mismatch";
    case KVR_OUT_OF_MEMORY: return "out of memory";
    case KVR_SIZE_OVERFLOW: return "size overflow";
    default: return "unknown status";
    }
}
