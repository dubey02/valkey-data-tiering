#include "include/recovery.h"
#include "include/log.h"
#include "include/index.h"
#include "include/serialization.h"
#include "include/util.h"
#include "include/fio.h"
#include "include/staging_buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Non-static internals reused from index.c (same pattern as snapshot_version_one.c) */
extern indexEntry *indexAddLogEntry(flashcacheIndex *index, size_t hash_bucket_idx, logEntry *log_entry);
extern size_t getIndexHash(flashcacheIndex *index, char const *key, size_t key_len);

#define FC_SUPERBLOCK_MAGIC   (0xFC0DE5B10CULL)
#define FC_SUPERBLOCK_VERSION (1u)

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t version;
    uint32_t num_databases;
    uint64_t log_size_bytes;
    uint64_t head_offset;
    uint64_t tail_offset;
    uint64_t num_items;
    uint32_t reserved;
    uint32_t crc; /* crc32c(0, bytes up to but excluding this field) */
} fcSuperblock;

/* ---------------------------------------------------------------------------
 * Superblock write (clean shutdown)
 * ---------------------------------------------------------------------------*/
int logWriteSuperblock(struct flashcacheLog *log, char const *superblock_filename) {
    flashcacheAssert(log != NULL);
    flashcacheAssert(superblock_filename != NULL);

    /* The window is only meaningful once nothing is buffered or in flight. */
    if (stagingBufferGetTotalItemSize(log->staging_buffer) != 0 ||
            !fioRequestIsEmpty(&(log->log_flush_fio_request))) {
        flashcacheLogger(FC_LL_WARNING,
                "Superblock write refused: staging buffer or log flush still busy");
        return -1;
    }

    /* O_DIRECT writes bypass the page cache but not necessarily the device
     * cache; make the log durable before persisting the window. */
    if (fsync(log->fio_context->fd) != 0) {
        flashcacheLogger(FC_LL_WARNING, "Superblock: fsync(log) failed: %d", errno);
        return -1;
    }

    fcSuperblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = FC_SUPERBLOCK_MAGIC;
    sb.version = FC_SUPERBLOCK_VERSION;
    sb.num_databases = log->num_databases;
    sb.log_size_bytes = log->log_size_bytes;
    sb.head_offset = log->head_offset;
    sb.tail_offset = log->tail_offset;
    sb.num_items = log->num_items;
    sb.crc = log->crc_function(0, (char const *)&sb, offsetof(fcSuperblock, crc));

    /* Write to a temp file then rename for atomicity. */
    char tmp_name[4096];
    int n = snprintf(tmp_name, sizeof(tmp_name), "%s.tmp", superblock_filename);
    if (n < 0 || (size_t)n >= sizeof(tmp_name)) return -1;

    int fd = open(tmp_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        flashcacheLogger(FC_LL_WARNING, "Superblock: open(%s) failed: %d", tmp_name, errno);
        return -1;
    }
    ssize_t written = write(fd, &sb, sizeof(sb));
    int rc = (written == (ssize_t)sizeof(sb) && fsync(fd) == 0) ? 0 : -1;
    close(fd);
    if (rc != 0 || rename(tmp_name, superblock_filename) != 0) {
        flashcacheLogger(FC_LL_WARNING, "Superblock: write/rename failed: %d", errno);
        unlink(tmp_name);
        return -1;
    }

    flashcacheLogger(FC_LL_NOTICE,
            "Superblock written: head=[%lu] tail=[%lu] items=[%lu] file=[%s]",
            sb.head_offset, sb.tail_offset, sb.num_items, superblock_filename);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Superblock read + validate
 * ---------------------------------------------------------------------------*/
static int readSuperblock(struct flashcacheLog *log, char const *superblock_filename,
        fcSuperblock *out) {
    int fd = open(superblock_filename, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t got = read(fd, out, sizeof(*out));
    close(fd);
    if (got != (ssize_t)sizeof(*out)) return -1;
    if (out->magic != FC_SUPERBLOCK_MAGIC || out->version != FC_SUPERBLOCK_VERSION) return -1;
    uint32_t crc = log->crc_function(0, (char const *)out, offsetof(fcSuperblock, crc));
    if (crc != out->crc) return -1;
    if (out->log_size_bytes != log->log_size_bytes ||
            out->num_databases != log->num_databases) {
        flashcacheLogger(FC_LL_WARNING,
                "Superblock config mismatch: sb log_size=[%lu] dbs=[%u] vs "
                "current log_size=[%lu] dbs=[%u]",
                out->log_size_bytes, out->num_databases,
                log->log_size_bytes, log->num_databases);
        return -1;
    }
    if (out->head_offset >= log->log_size_bytes || out->tail_offset >= log->log_size_bytes) return -1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Transient last-write-wins dedup table (keyed by dbid + full key bytes).
 * Later inserts (later in write order) overwrite earlier ones.
 * ---------------------------------------------------------------------------*/
typedef struct dedupEntry {
    struct dedupEntry *next;
    uint64_t full_hash;
    uint32_t dbid;
    uint32_t key_len;
    size_t log_offset;      /* absolute byte offset of the serialized item */
    size_t item_len;        /* total serialized length                     */
    size_t value_len;
    uint8_t value_first_byte;
    char key[];             /* key_len bytes                               */
} dedupEntry;

typedef struct {
    dedupEntry **buckets;
    size_t num_buckets;     /* power of 2 */
    size_t num_entries;
    size_t num_stale;       /* overwritten copies discarded */
    flashcache_hash_function hash_fn;
} dedupTable;

static void dedupInit(dedupTable *t, flashcache_hash_function hash_fn) {
    t->num_buckets = 1 << 16;
    t->buckets = (dedupEntry **)fcCalloc(t->num_buckets, sizeof(dedupEntry *));
    flashcacheAssert(t->buckets != NULL);
    t->num_entries = 0;
    t->num_stale = 0;
    t->hash_fn = hash_fn;
}

static void dedupGrowIfNeeded(dedupTable *t) {
    if (t->num_entries < t->num_buckets * 4) return;
    size_t new_num = t->num_buckets << 1;
    dedupEntry **nb = (dedupEntry **)fcCalloc(new_num, sizeof(dedupEntry *));
    flashcacheAssert(nb != NULL);
    for (size_t i = 0; i < t->num_buckets; i++) {
        dedupEntry *e = t->buckets[i];
        while (e) {
            dedupEntry *next = e->next;
            size_t b = e->full_hash & (new_num - 1);
            e->next = nb[b];
            nb[b] = e;
            e = next;
        }
    }
    fcFree(t->buckets);
    t->buckets = nb;
    t->num_buckets = new_num;
}

static void dedupPut(dedupTable *t, uint32_t dbid, char const *key, size_t key_len,
        size_t log_offset, size_t item_len, size_t value_len, uint8_t value_first_byte) {
    uint64_t h = t->hash_fn(key, key_len);
    size_t b = h & (t->num_buckets - 1);
    for (dedupEntry *e = t->buckets[b]; e; e = e->next) {
        if (e->full_hash == h && e->dbid == dbid && e->key_len == key_len &&
                memcmp(e->key, key, key_len) == 0) {
            /* Later copy wins (we scan in write order). */
            e->log_offset = log_offset;
            e->item_len = item_len;
            e->value_len = value_len;
            e->value_first_byte = value_first_byte;
            t->num_stale++;
            return;
        }
    }
    dedupEntry *e = (dedupEntry *)fcMalloc(sizeof(dedupEntry) + key_len);
    flashcacheAssert(e != NULL);
    e->full_hash = h;
    e->dbid = dbid;
    e->key_len = (uint32_t)key_len;
    e->log_offset = log_offset;
    e->item_len = item_len;
    e->value_len = value_len;
    e->value_first_byte = value_first_byte;
    memcpy(e->key, key, key_len);
    e->next = t->buckets[b];
    t->buckets[b] = e;
    t->num_entries++;
    dedupGrowIfNeeded(t);
}

static void dedupRelease(dedupTable *t) {
    for (size_t i = 0; i < t->num_buckets; i++) {
        dedupEntry *e = t->buckets[i];
        while (e) {
            dedupEntry *next = e->next;
            fcFree(e);
            e = next;
        }
    }
    fcFree(t->buckets);
    t->buckets = NULL;
}

/* ---------------------------------------------------------------------------
 * Sequential scanner over the active window [tail, head), write order.
 *
 * Reads page-aligned chunks with pread(2) into an aligned buffer (the log fd
 * is O_DIRECT). An item never spans the file-end boundary (FC writes a
 * FC_SKIP_SEGMENT marker and wraps instead), so a chunk never needs to stitch
 * across the wrap.
 * ---------------------------------------------------------------------------*/
#define RECOVERY_CHUNK_BYTES ((size_t)4 * 1024 * 1024)

typedef struct {
    int fd;
    char *buf;              /* page-aligned, RECOVERY_CHUNK_BYTES */
    size_t buf_base;        /* absolute log offset of buf[0]      */
    size_t buf_len;         /* valid bytes in buf                 */
    size_t log_size;
} scanReader;

/* Ensure [abs_off, abs_off+need) is in the buffer; returns pointer or NULL. */
static char *scanFetch(scanReader *r, size_t abs_off, size_t need) {
    flashcacheAssert(need <= RECOVERY_CHUNK_BYTES - (FC_PAGESIZE - 1));
    if (abs_off >= r->buf_base && abs_off + need <= r->buf_base + r->buf_len) {
        return r->buf + (abs_off - r->buf_base);
    }
    size_t base = getFloorPageAlignedOffset(abs_off);
    size_t to_read = RECOVERY_CHUNK_BYTES;
    if (base + to_read > r->log_size) to_read = r->log_size - base;
    if (abs_off + need > base + to_read) return NULL; /* would cross file end */
    ssize_t got = pread(r->fd, r->buf, to_read, (off_t)base);
    if (got < 0 || (size_t)got < (abs_off - base) + need) return NULL;
    r->buf_base = base;
    r->buf_len = (size_t)got;
    return r->buf + (abs_off - base);
}

static uint64_t recoveryNowUs(struct flashcacheLog *log) {
    return log->monotonic_clock_us();
}

int logRecoverFromLog(struct flashcacheLog *log, char const *superblock_filename,
        flashcacheRecoveryItemCallback item_cb, void *item_cb_ctx,
        flashcacheRecoveryStats *stats) {
    flashcacheAssert(log != NULL);
    flashcacheAssert(superblock_filename != NULL);
    flashcacheAssert(log->num_items == 0); /* must run before any traffic */

    flashcacheRecoveryStats local_stats;
    if (stats == NULL) stats = &local_stats;
    memset(stats, 0, sizeof(*stats));

    fcSuperblock sb;
    if (readSuperblock(log, superblock_filename, &sb) != 0) {
        flashcacheLogger(FC_LL_NOTICE,
                "Recovery: no valid superblock at [%s]; cold start", superblock_filename);
        return -1;
    }

    /* Consume the superblock immediately: whatever happens next, a future
     * boot must never reuse this window. */
    unlink(superblock_filename);

    size_t active = (sb.head_offset + sb.log_size_bytes - sb.tail_offset) % sb.log_size_bytes;
    if (active == 0 && sb.num_items > 0) active = sb.log_size_bytes; /* exactly-full log */
    if (active == 0) {
        flashcacheLogger(FC_LL_NOTICE, "Recovery: empty log, nothing to do");
        return 0;
    }

    uint64_t t0 = recoveryNowUs(log);

    scanReader reader;
    reader.fd = log->fio_context->fd;
    reader.buf = (char *)fcPosixMemalign(FC_PAGESIZE, RECOVERY_CHUNK_BYTES);
    flashcacheAssert(reader.buf != NULL);
    reader.buf_base = 0;
    reader.buf_len = 0;
    reader.log_size = sb.log_size_bytes;

    dedupTable dedup;
    dedupInit(&dedup, log->hasher.hash_function);

    int rc = 0;
    size_t off = sb.tail_offset;
    size_t remaining = active;

    while (remaining > 0) {
        char *item = scanFetch(&reader, off, FC_ITEM_HEADER_LEN);
        if (item == NULL || !validateHeaderInSerializedItem(item, log->crc_function)) {
            flashcacheLogger(FC_LL_WARNING,
                    "Recovery: bad item header at offset [%lu]; aborting scan", off);
            rc = -1;
            break;
        }
        uint32_t flag = getFlagInSerializedItem(item);
        size_t total_len = extractTotalLenFromSerializedItem(item);
        if (total_len == 0) {
            flashcacheLogger(FC_LL_WARNING,
                    "Recovery: zero-length item at offset [%lu]; aborting scan", off);
            rc = -1;
            break;
        }

        if (flag & FC_EOF_INDICATOR) {
            /* Snapshot-only marker; must not appear in the live log window. */
            flashcacheLogger(FC_LL_WARNING,
                    "Recovery: unexpected EOF marker at offset [%lu]; aborting scan", off);
            rc = -1;
            break;
        }

        if (!(flag & FC_SKIP_SEGMENT) && !(flag & FC_REPL_CMD_DELETE)) {
            /* A real kv item (bit FC_LAST_ITEM_BEFORE_NEXT_PAGE_BOUNDARY may
             * also be set — it is a padding hint, the item is still real). */
            item = scanFetch(&reader, off, total_len);
            if (item == NULL || !validateKeyInSerializedItem(item, log->crc_function)) {
                flashcacheLogger(FC_LL_WARNING,
                        "Recovery: bad item/key at offset [%lu]; aborting scan", off);
                rc = -1;
                break;
            }
            char *key = NULL, *value = NULL;
            size_t key_len = 0, value_len = 0;
            extractKeyFromSerializedItem(item, &key, &key_len);
            extractValueFromSerializedItem(item, &value, &value_len);
            uint32_t dbid = extractDbidFromSerializedItem(item);
            if (dbid >= log->num_databases || key_len == 0) {
                flashcacheLogger(FC_LL_WARNING,
                        "Recovery: bad dbid/key_len at offset [%lu]; aborting scan", off);
                rc = -1;
                break;
            }
            dedupPut(&dedup, dbid, key, key_len, off, total_len, value_len,
                     value_len > 0 ? (uint8_t)value[0] : 0);
            stats->items_scanned++;
        }

        /* Advance exactly like log_iterator.c:processReadItem. */
        size_t next = off + total_len;
        if (flag & FC_LAST_ITEM_BEFORE_NEXT_PAGE_BOUNDARY) {
            next = getCeilPageAlignedOffset(next);
        }
        next %= sb.log_size_bytes;
        size_t consumed = (next + sb.log_size_bytes - off) % sb.log_size_bytes;
        if (consumed == 0 || consumed > remaining) {
            /* consumed > remaining tolerates page-padding overshoot on the
             * final flush batch; anything else is corruption. */
            if (consumed == 0) {
                flashcacheLogger(FC_LL_WARNING,
                        "Recovery: no forward progress at offset [%lu]; aborting scan", off);
                rc = -1;
                break;
            }
            consumed = remaining;
        }
        stats->bytes_scanned += consumed;
        remaining -= consumed;
        off = next;
    }

    stats->scan_us = recoveryNowUs(log) - t0;

    if (rc != 0) {
        /* Leave the log in cold-start state: offsets reset, empty index. */
        log->head_offset = 0;
        log->tail_offset = 0;
        dedupRelease(&dedup);
        fcFree(reader.buf);
        return -1;
    }

    /* -----------------------------------------------------------------------
     * Finalize: restore the window, rebuild the index from survivors, feed
     * the engine, and drive index growth to completion so post-boot lookups
     * do not crawl giant chains.
     * ---------------------------------------------------------------------*/
    uint64_t t1 = recoveryNowUs(log);
    log->head_offset = sb.head_offset;
    log->tail_offset = sb.tail_offset;

    for (size_t i = 0; i < dedup.num_buckets; i++) {
        for (dedupEntry *e = dedup.buckets[i]; e; e = e->next) {
            flashcacheIndex *index = log->index_list[e->dbid];

            logEntry le;
            memset(&le, 0, sizeof(le));
            le.on_flash = 1;
            le.hash = computeCollisionHash(log->hasher.hash_function, e->key, e->key_len);
            size_t pages = ((e->log_offset % FC_PAGESIZE) + e->item_len + FC_PAGESIZE - 1) / FC_PAGESIZE;
            size_t additional_pages = pages - 1;
            if (additional_pages > FC_MAX_ADDITIONAL_PAGES) additional_pages = FC_MAX_ADDITIONAL_PAGES;
            le.additional_pages = additional_pages;
            le.trimmed_log_offset = trimLogOffset(e->log_offset);

            size_t bucket = getIndexHash(index, e->key, e->key_len);
            indexAddLogEntry(index, bucket, &le);

            log->num_items++;
            log->allocated_log_size_bytes += e->item_len;
            log->allocated_log_size_bytes_per_db[e->dbid] += e->item_len;

            if (item_cb != NULL) {
                item_cb(item_cb_ctx, e->dbid, e->key, e->key_len,
                        e->value_first_byte, e->value_len);
            }
        }
    }

    /* Pump incremental index growth to completion. */
    for (uint32_t d = 0; d < log->num_databases; d++) {
        while (indexGrowIfRequired(log->index_list[d])) { /* advance one batch */ }
    }

    stats->items_live = dedup.num_entries;
    stats->items_stale = dedup.num_stale;
    stats->finalize_us = recoveryNowUs(log) - t1;

    flashcacheLogger(FC_LL_NOTICE,
            "Recovery complete: scanned=[%lu] live=[%lu] stale=[%lu] bytes=[%lu] "
            "scan_us=[%lu] finalize_us=[%lu]",
            stats->items_scanned, stats->items_live, stats->items_stale,
            stats->bytes_scanned, stats->scan_us, stats->finalize_us);

    dedupRelease(&dedup);
    fcFree(reader.buf);
    return 0;
}
