#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "kivrim.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

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

#define MAX_FILE (UINT64_C(1024) * 1024 * 1024)
#define MAX_TOTAL (UINT64_C(16) * 1024 * 1024 * 1024)
#define CHUNK_MAX 32768u
#define TABLE_BITS 22u
#define TABLE_SIZE ((size_t)1 << TABLE_BITS)

typedef struct {
    char *name;
    unsigned char *data;
    size_t size;
    struct stat before;
    int fd;
} input_file;

typedef struct {
    uint64_t offset;
    uint32_t raw, stored, hash, kind, file;
} chunk_entry;

typedef struct {
    FILE *file;
    uint64_t size, pos;
} reader;

/* Incremental form of XXH32, used to verify files without retaining them in RAM. */
typedef struct {
    uint32_t a, b, c, d;
    uint64_t total;
    unsigned char tail[16];
    size_t used;
} hash_state;

static void hash_init(hash_state *s)
{
    memset(s, 0, sizeof(*s));
    s->a = UINT32_C(2654435761) + UINT32_C(2246822519);
    s->b = UINT32_C(2246822519); s->d = 0u - UINT32_C(2654435761);
}

static void hash_stripe(hash_state *s, const unsigned char *p)
{
    s->a = rotate32(s->a + read32(p) * UINT32_C(2246822519), 13) * UINT32_C(2654435761);
    s->b = rotate32(s->b + read32(p + 4) * UINT32_C(2246822519), 13) * UINT32_C(2654435761);
    s->c = rotate32(s->c + read32(p + 8) * UINT32_C(2246822519), 13) * UINT32_C(2654435761);
    s->d = rotate32(s->d + read32(p + 12) * UINT32_C(2246822519), 13) * UINT32_C(2654435761);
}

static void hash_update(hash_state *s, const unsigned char *p, size_t n)
{
    s->total += n;
    if (s->used) {
        size_t add = n < 16 - s->used ? n : 16 - s->used;
        memcpy(s->tail + s->used, p, add); s->used += add; p += add; n -= add;
        if (s->used == 16) { hash_stripe(s, s->tail); s->used = 0; }
    }
    while (n >= 16) { hash_stripe(s, p); p += 16; n -= 16; }
    if (n) { memcpy(s->tail + s->used, p, n); s->used += n; }
}

static uint32_t hash_finish(const hash_state *s)
{
    uint32_t h = s->total >= 16 ? rotate32(s->a, 1) + rotate32(s->b, 7) +
                 rotate32(s->c, 12) + rotate32(s->d, 18) : UINT32_C(374761393);
    size_t i = 0;
    h += (uint32_t)s->total;
    while (s->used - i >= 4) {
        h = rotate32(h + read32(s->tail + i) * UINT32_C(3266489917), 17) * UINT32_C(668265263);
        i += 4;
    }
    while (i < s->used) h = rotate32(h + (uint32_t)s->tail[i++] * UINT32_C(374761393), 11) * UINT32_C(2654435761);
    h ^= h >> 15; h *= UINT32_C(2246822519); h ^= h >> 13; h *= UINT32_C(3266489917);
    return h ^ (h >> 16);
}

static const char *incomplete_archive, *incomplete_directory;
static int cleanup_dirfd = -1;

static void cleanup_incomplete(void)
{
    if (incomplete_archive) (void)unlink(incomplete_archive);
    if (cleanup_dirfd >= 0) {
        DIR *dir = fdopendir(dup(cleanup_dirfd));
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir))) {
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                    (void)unlinkat(cleanup_dirfd, de->d_name, 0);
            }
            closedir(dir);
        }
        close(cleanup_dirfd);
    }
    if (incomplete_directory) (void)rmdir(incomplete_directory);
}

static void fatal(const char *message)
{
    fprintf(stderr, "kivrim-archive: %s\n", message);
    exit(1);
}

static void write_bytes(FILE *f, const void *p, size_t n)
{
    if (n && fwrite(p, 1, n, f) != n) fatal("write failed");
}

static void u32(FILE *f, uint32_t n)
{
    unsigned char p[4]; write32(p, n); write_bytes(f, p, sizeof(p));
}

static void u64(FILE *f, uint64_t n)
{
    unsigned char p[8]; write64(p, n); write_bytes(f, p, sizeof(p));
}

static void read_at(int fd, void *output, size_t size, uint64_t offset)
{
    unsigned char *p = output;
    while (size) {
        ssize_t got = pread(fd, p, size, (off_t)offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) fatal("cannot read referenced bytes");
        p += (size_t)got; size -= (size_t)got; offset += (uint64_t)got;
    }
}

static void read_part(reader *r, void *output, size_t n)
{
    if (r->pos > r->size || n > r->size - r->pos ||
        (n && fread(output, 1, n, r->file) != n)) fatal("truncated archive");
    r->pos += n;
}

/* Archive chunk records start with a canonical base-128 integer. Its low two
 * bits are the kind: 0 raw, 1 KVR1, 2 reference delta, 3 reference run.
 * For kinds 0/1 the remaining bits store raw_size - 1; kind 1 then stores
 * stored_size - 1 as another integer. Both carry a 4-byte XXH32 checksum.
 * Kind 2 stores a ZigZag delta from the previous referenced ID (initially 0
 * per file). Kind 3 stores run_length - 1 for consecutive IDs following it.
 * References use the original chunk's lengths/checksum; no payload is copied. */
static void put_var(FILE *out, uint32_t value)
{
    unsigned char encoded[5]; size_t n = 0;
    do {
        unsigned char b = (unsigned char)(value & 127u);
        value >>= 7; encoded[n++] = (unsigned char)(b | (value ? 128u : 0u));
    } while (value);
    write_bytes(out, encoded, n);
}

static uint32_t get_var(reader *r, uint32_t limit)
{
    uint32_t value = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
        if (r->pos == r->size) fatal("truncated integer");
        /* This single-threaded command owns the stream exclusively. */
        int next = getc_unlocked(r->file);
        if (next == EOF) fatal("integer read failed");
        ++r->pos;
        uint32_t bits = (uint32_t)next & 127u;
        if (shift == 28 && bits > 15) fatal("integer overflow");
        value |= bits << shift;
        if (!((unsigned)next & 128u)) {
            if ((shift && !bits) || value > limit) fatal("invalid integer encoding");
            return value;
        }
    }
    fatal("integer too long"); return 0;
}

static uint32_t ref_delta(uint32_t id, uint32_t last)
{
    int64_t delta = (int64_t)id - last;
    return delta < 0 ? (uint32_t)((-delta) * 2 - 1) : (uint32_t)(delta * 2);
}

static uint32_t ref_from_delta(uint32_t value, uint32_t last, size_t nc)
{
    int64_t delta = value & 1u ? -(int64_t)(value >> 1) - 1 : (int64_t)(value >> 1);
    int64_t id = (int64_t)last + delta;
    if (id < 0 || (uint64_t)id >= nc) fatal("invalid chunk reference");
    return (uint32_t)id;
}

static void flush_refs(FILE *out, uint32_t *last, uint32_t *pending)
{
    if (*pending) {
        put_var(out, ((*pending - 1) << 2) | 3u);
        *last += *pending; *pending = 0;
    }
}

static int same_snapshot(const struct stat *before, const struct stat *after)
{
    if (before->st_size != after->st_size) return 0;
#ifdef __APPLE__
    return before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec &&
           before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
           before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec &&
           before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
#else
    return before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
#endif
}

static int name_compare(const void *a, const void *b)
{
    return strcmp(((const input_file *)a)->name, ((const input_file *)b)->name);
}

static int valid_name(const char *name, size_t n)
{
    return n > 0 && n <= 255 && !memchr(name, '/', n) && !memchr(name, '\0', n) &&
           !(n == 1 && name[0] == '.') && !(n == 2 && !memcmp(name, "..", 2));
}

static void append_entry(chunk_entry **entries, size_t *count, size_t *capacity,
                         chunk_entry entry)
{
    if (*count >= TABLE_SIZE / 2) fatal("too many chunks");
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 4096;
        chunk_entry *p = realloc(*entries, next * sizeof(*p));
        if (!p) fatal("out of memory");
        *entries = p; *capacity = next;
    }
    (*entries)[(*count)++] = entry;
}

static void encode(const char *directory, const char *archive, kvr_mode mode)
{
    DIR *dir = opendir(directory);
    struct dirent *de;
    input_file *files = NULL;
    size_t count = 0, capacity = 0, nc = 0, cc = 0;
    uint64_t total = 0, duplicate_bytes = 0, payload_bytes = 0, refs = 0;
    uint64_t gear[256], seed = UINT64_C(0x4b495652494d0001);
    uint32_t *table = calloc(TABLE_SIZE, sizeof(*table));
    chunk_entry *chunks = NULL;
    unsigned char packed[CHUNK_MAX + 128], compared[CHUNK_MAX];
    if (!dir || !table) fatal("cannot open input or allocate index");
    for (size_t i = 0; i < 256; ++i) {
        uint64_t z = (seed += UINT64_C(0x9e3779b97f4a7c15));
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        gear[i] = z ^ (z >> 31);
    }
    errno = 0;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!valid_name(de->d_name, strlen(de->d_name))) fatal("invalid input name");
        if (count == 65535) fatal("too many files");
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            input_file *p = realloc(files, next * sizeof(*p));
            if (!p) fatal("out of memory");
            files = p; capacity = next;
        }
        input_file *f = files + count;
        memset(f, 0, sizeof(*f));
        f->name = strdup(de->d_name);
        f->fd = openat(dirfd(dir), de->d_name, O_RDONLY | O_NOFOLLOW);
        if (!f->name || f->fd < 0 || fstat(f->fd, &f->before) ||
            !S_ISREG(f->before.st_mode) || f->before.st_size < 0 ||
            (uint64_t)f->before.st_size > MAX_FILE) fatal("regular files up to 1 GiB required");
        f->size = (size_t)f->before.st_size;
        if (f->size > MAX_TOTAL - total) fatal("input exceeds 16 GiB");
        total += f->size;
        ++count;
        errno = 0;
    }
    if (errno || closedir(dir)) fatal("cannot read directory");
    if (count > 1) qsort(files, count, sizeof(*files), name_compare);
    FILE *out = fopen(archive, "wbx");
    if (!out) fatal("cannot exclusively create archive");
    incomplete_archive = archive;
    (void)setvbuf(out, NULL, _IOFBF, 1024 * 1024);
    write_bytes(out, "KVA1", 4); u32(out, 0); u32(out, (uint32_t)count); u32(out, 0);
    for (size_t fi = 0; fi < count; ++fi) {
        input_file *f = files + fi;
        off_t header = ftello(out);
        size_t name_size = strlen(f->name), pos = 0;
        uint32_t file_chunks = 0;
        uint32_t last_ref = 0, pending = 0;
        int previous_ref = 0;
        if (header < 0) fatal("cannot tell archive offset");
        if (f->size) {
            f->data = mmap(NULL, f->size, PROT_READ, MAP_PRIVATE, f->fd, 0);
            if (f->data == MAP_FAILED) fatal("cannot map input");
        }
        u32(out, (uint32_t)name_size); u32(out, 0); u64(out, f->size);
        u32(out, checksum(f->data, f->size)); write_bytes(out, f->name, name_size);
        while (pos < f->size) {
            size_t length = 0, slot, stored = 0;
            uint64_t rolling = 0;
            const unsigned char *data = f->data + pos;
            do {
                rolling = (rolling << 1) + gear[data[length++]];
            } while (length < CHUNK_MAX && length < f->size - pos &&
                     (length < 2048 || (rolling & 8191u) != 0));
            uint32_t hash = checksum(data, length);
            slot = ((size_t)hash ^ (length * 2654435761u)) & (TABLE_SIZE - 1);
            while (table[slot]) {
                chunk_entry *e = chunks + (table[slot] - 1);
                if (e->hash == hash && e->raw == length) {
                    const unsigned char *previous;
                    if (e->file == fi) previous = f->data + e->offset;
                    else {
                        read_at(files[e->file].fd, compared, length, e->offset);
                        previous = compared;
                    }
                    if (!memcmp(previous, data, length)) break;
                }
                slot = (slot + 1) & (TABLE_SIZE - 1);
            }
            if (table[slot]) {
                uint32_t id = table[slot] - 1;
                if (previous_ref && id == last_ref + pending + 1) ++pending;
                else {
                    flush_refs(out, &last_ref, &pending);
                    put_var(out, (ref_delta(id, last_ref) << 2) | 2u);
                    last_ref = id;
                }
                previous_ref = 1;
                duplicate_bytes += length; ++refs;
            } else {
                flush_refs(out, &last_ref, &pending); previous_ref = 0;
                if (kvr_compress(data, length, packed, sizeof(packed), &stored, mode) != KVR_OK)
                    fatal("compression failed");
                uint32_t kind = stored < length ? 1u : 0u;
                if (!kind) stored = length;
                put_var(out, (((uint32_t)length - 1) << 2) | kind);
                if (kind) put_var(out, (uint32_t)stored - 1);
                u32(out, hash);
                write_bytes(out, kind ? packed : data, stored);
                append_entry(&chunks, &nc, &cc, (chunk_entry){pos, (uint32_t)length, (uint32_t)stored, hash, kind, (uint32_t)fi});
                table[slot] = (uint32_t)nc;
                payload_bytes += stored;
            }
            pos += length; ++file_chunks;
        }
        flush_refs(out, &last_ref, &pending);
        off_t end = ftello(out);
        if (end < 0 || fseeko(out, header + 4, SEEK_SET)) fatal("cannot update file header");
        u32(out, file_chunks);
        if (fseeko(out, end, SEEK_SET)) fatal("cannot resume archive");
        if (f->size && munmap(f->data, f->size)) fatal("cannot unmap input");
        f->data = NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        struct stat after;
        if (fstat(files[i].fd, &after) || !same_snapshot(&files[i].before, &after)) fatal("source changed during read");
        close(files[i].fd); free(files[i].name);
    }
    off_t archive_bytes = ftello(out);
    if (archive_bytes < 0 || fclose(out)) fatal("cannot close archive");
    incomplete_archive = NULL;
    printf("{\"files\":%zu,\"raw_bytes\":%" PRIu64 ",\"archive_bytes\":%jd,\"unique_chunks\":%zu,\"references\":%" PRIu64 ",\"duplicate_bytes\":%" PRIu64 ",\"payload_bytes\":%" PRIu64 "}\n",
           count, total, (intmax_t)archive_bytes, nc, refs, duplicate_bytes, payload_bytes);
    free(table); free(chunks); free(files);
}

static void decode(const char *archive, const char *directory)
{
    int fd = open(archive, O_RDONLY | O_NOFOLLOW), output_dir;
    struct stat st;
    size_t nc = 0, cc = 0;
    uint64_t total = 0;
    unsigned char h[20], packed[CHUNK_MAX], output[CHUNK_MAX];
    chunk_entry *chunks = NULL;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 16 ||
        (uint64_t)st.st_size > MAX_TOTAL + MAX_TOTAL / 64) fatal("invalid archive file");
    FILE *in = fdopen(fd, "rb");
    if (!in) fatal("cannot open archive stream");
    (void)setvbuf(in, NULL, _IOFBF, 1024 * 1024);
    reader r = {in, (uint64_t)st.st_size, 0};
    read_part(&r, h, 16);
    if (memcmp(h, "KVA1", 4) || read32(h + 4) || read32(h + 12))
        fatal("invalid archive header");
    uint32_t files = read32(h + 8);
    if (files > 65535) fatal("file count limit exceeded");
    if (mkdir(directory, 0700)) fatal("a new output directory is required");
    incomplete_directory = directory;
    output_dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (output_dir < 0) fatal("cannot open output directory");
    cleanup_dirfd = output_dir;
    for (uint32_t fi = 0; fi < files; ++fi) {
        char name[256];
        read_part(&r, h, 20);
        uint32_t name_size = read32(h), count = read32(h + 4), file_hash = read32(h + 16);
        uint64_t length = read64(h + 8);
        if (!name_size || name_size > 255 || length > MAX_FILE || length > MAX_TOTAL - total ||
            count > length || (length && !count)) fatal("invalid file metadata");
        read_part(&r, name, name_size);
        if (!valid_name(name, name_size)) fatal("unsafe filename");
        name[name_size] = '\0';
        int outfd = openat(output_dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (outfd < 0) fatal("output file already exists or cannot be created");
        FILE *out = fdopen(outfd, "wb");
        if (!out) { close(outfd); fatal("cannot open output stream"); }
        (void)setvbuf(out, NULL, _IOFBF, 1024 * 1024);
        hash_state state;
        hash_init(&state);
        size_t written = 0;
        uint32_t last_ref = 0, pending = 0;
        int have_ref = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t kind = 0, raw = 0, stored = 0, hash = 0, id = 0;
            int reference = 0;
            if (pending) {
                id = ++last_ref; --pending; reference = 1;
            } else {
                uint32_t token = get_var(&r, (uint32_t)(TABLE_SIZE * 4) - 1);
                uint32_t value = token >> 2;
                kind = token & 3u;
                if (kind <= 1) {
                    raw = value + 1;
                    stored = kind ? get_var(&r, CHUNK_MAX - 1) + 1 : raw;
                    read_part(&r, h, 4); hash = read32(h);
                } else if (kind == 2) {
                    id = ref_from_delta(value, last_ref, nc); reference = 1;
                } else {
                    uint32_t run = value + 1;
                    if (!have_ref || last_ref >= nc || run > count - i ||
                        run >= nc - last_ref) fatal("invalid reference run");
                    id = last_ref + 1; pending = run - 1; reference = 1;
                }
            }
            chunk_entry e;
            if (reference) {
                if (id >= nc) fatal("invalid chunk reference");
                e = chunks[id];
                last_ref = id; have_ref = 1;
                read_at(fd, packed, e.stored, e.offset);
            } else {
                if (kind > 1 || !raw || raw > CHUNK_MAX || !stored || stored > CHUNK_MAX ||
                    (kind == 0 && raw != stored)) fatal("invalid chunk metadata");
                e = (chunk_entry){r.pos, raw, stored, hash, kind, 0};
                read_part(&r, packed, stored);
                append_entry(&chunks, &nc, &cc, e);
            }
            if (e.raw > length - written) fatal("chunk exceeds file size");
            if (e.kind == 0) memcpy(output, packed, e.raw);
            else {
                size_t decoded = 0;
                if (kvr_decompress(packed, e.stored, output, e.raw, &decoded) != KVR_OK || decoded != e.raw)
                    fatal("invalid compressed chunk");
            }
            if (checksum(output, e.raw) != e.hash) fatal("chunk checksum mismatch");
            hash_update(&state, output, e.raw);
            write_bytes(out, output, e.raw);
            written += e.raw;
        }
        if (written != length || hash_finish(&state) != file_hash) fatal("file checksum mismatch");
        if (fclose(out)) fatal("cannot close output file");
        total += length;
    }
    if (r.pos != r.size) fatal("trailing archive data");
    if (fclose(in) || close(output_dir)) fatal("cannot close archive or output directory");
    cleanup_dirfd = -1; incomplete_directory = NULL; free(chunks);
    printf("{\"files\":%u,\"raw_bytes\":%" PRIu64 "}\n", files, total);
}

static void usage(FILE *out)
{
    fprintf(out,
        "KIVRIM directory archive\n"
        "Usage: kivrim-archive c DIRECTORY NEW_ARCHIVE [--auto]\n"
        "       kivrim-archive d ARCHIVE NEW_DIRECTORY\n"
        "Regular files only, one directory level; no symlinks.\n"
        "Limits: 1 GiB per file, 16 GiB total, 65535 files, 2097152 unique chunks.\n"
        "Existing outputs are never overwritten. Default compression: fast.\n");
}

int main(int argc, char **argv)
{
    if (atexit(cleanup_incomplete)) fatal("cannot set up cleanup");
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(stdout); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts("KIVRIM archive " KVR_VERSION); return 0; }
    if ((argc == 4 || (argc == 5 && !strcmp(argv[4], "--auto"))) && !strcmp(argv[1], "c"))
        encode(argv[2], argv[3], argc == 5 ? KVR_AUTO : KVR_FAST);
    else if (argc == 4 && !strcmp(argv[1], "d")) decode(argv[2], argv[3]);
    else { usage(stderr); return 2; }
    return 0;
}
