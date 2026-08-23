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
extern int indexGrowIfRequired(flashcacheIndex *index);
extern size_t indexTableSize(flashcacheIndex *index);

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
 * Head journal (see recovery.h). io-thread only; file-static state is fine.
 *
 * File layout: fcHeadjFileHeader at offset 0, then packed 40-byte
 * fcHeadjRecord entries appended per staging-buffer flush completion.
 * ---------------------------------------------------------------------------*/
#define FC_HEADJ_MAGIC          (0xFC4EADull)
#define FC_HEADJ_VERSION        (1u)
#define FC_HEADJ_REC_MAGIC      (0x4EAD4ECDu)

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t version;
    uint32_t num_databases;
    uint64_t log_size_bytes;
    uint32_t reserved;
    uint32_t crc; /* crc32c over bytes up to but excluding this field */
} fcHeadjFileHeader;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint64_t head_offset;
    uint64_t tail_offset;
    uint64_t num_items;   /* disambiguates head==tail: 0 = empty, >0 = full */
    uint32_t reserved;
    uint32_t crc; /* crc32c over bytes up to but excluding this field */
} fcHeadjRecord;

static struct {
    int configured;
    int fd;              /* O_APPEND for record writes */
    uint32_t seq;
    char path[4096];
} fc_headj = { 0, -1, 0, {0} };

void logSetFastBootDurability(struct flashcacheLog *log, int enabled) {
    flashcacheAssert(log != NULL);
    log->fast_boot_durability = enabled ? 1 : 0;
}

int logHeadJournalConfigure(char const *headj_filename) {
    flashcacheAssert(headj_filename != NULL);
    int n = snprintf(fc_headj.path, sizeof(fc_headj.path), "%s", headj_filename);
    if (n < 0 || (size_t)n >= sizeof(fc_headj.path)) return -1;
    if (fc_headj.fd >= 0) close(fc_headj.fd);
    /* No O_TRUNC: boot must be able to read the previous run's records. */
    fc_headj.fd = open(fc_headj.path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fc_headj.fd < 0) {
        flashcacheLogger(FC_LL_WARNING, "Head journal: open(%s) failed: %d",
                fc_headj.path, errno);
        fc_headj.configured = 0;
        return -1;
    }
    fc_headj.configured = 1;
    fc_headj.seq = 0;
    return 0;
}

static void headjWriteRecord(struct flashcacheLog *log) {
    fcHeadjRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = FC_HEADJ_REC_MAGIC;
    rec.seq = ++fc_headj.seq;
    rec.head_offset = log->head_offset;
    rec.tail_offset = log->tail_offset;
    rec.num_items = log->num_items;
    rec.crc = log->crc_function(0, (char const *)&rec, offsetof(fcHeadjRecord, crc));
    ssize_t w = write(fc_headj.fd, &rec, sizeof(rec)); /* O_APPEND */
    if (w != (ssize_t)sizeof(rec) || fdatasync(fc_headj.fd) != 0) {
        flashcacheLogger(FC_LL_WARNING, "Head journal: record write failed: %d", errno);
    }
}

void logHeadJournalAppend(struct flashcacheLog *log) {
    if (!fc_headj.configured || fc_headj.fd < 0) return;
    /* The O_DIRECT flush bypassed the page cache but not necessarily the
     * device volatile cache; the record must never claim durability that
     * does not exist yet. */
    if (fsync(log->fio_context->fd) != 0) {
        flashcacheLogger(FC_LL_WARNING, "Head journal: fsync(log) failed: %d; "
                "skipping record", errno);
        return;
    }
    headjWriteRecord(log);
}

void logHeadJournalReset(struct flashcacheLog *log) {
    if (!fc_headj.configured || fc_headj.fd < 0) return;
    if (ftruncate(fc_headj.fd, 0) != 0) {
        flashcacheLogger(FC_LL_WARNING, "Head journal: truncate failed: %d", errno);
        return;
    }
    fc_headj.seq = 0;
    fcHeadjFileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FC_HEADJ_MAGIC;
    hdr.version = FC_HEADJ_VERSION;
    hdr.num_databases = log->num_databases;
    hdr.log_size_bytes = log->log_size_bytes;
    hdr.crc = log->crc_function(0, (char const *)&hdr, offsetof(fcHeadjFileHeader, crc));
    ssize_t w = write(fc_headj.fd, &hdr, sizeof(hdr)); /* O_APPEND, file empty */
    if (w != (ssize_t)sizeof(hdr)) {
        flashcacheLogger(FC_LL_WARNING, "Head journal: header write failed: %d", errno);
        return;
    }
    /* One record for the current window so a crash immediately after this
     * point still recovers (idempotent replay of the same window). Durability
     * of the log bytes was established by whoever produced this window
     * (recovery scan read them; a flush fsync'd them). */
    headjWriteRecord(log);
    if (fdatasync(fc_headj.fd) != 0) {
        flashcacheLogger(FC_LL_WARNING, "Head journal: fdatasync failed: %d", errno);
    }
}

/* num_items of the last valid record read by logHeadJournalReadLast, for
 * head==tail disambiguation (0 = empty window, >0 = exactly-full log). */
static uint64_t fc_headj_last_num_items = 0;

int logHeadJournalReadLast(struct flashcacheLog *log, uint64_t *head_offset,
        uint64_t *tail_offset) {
    flashcacheAssert(head_offset != NULL && tail_offset != NULL);
    if (!fc_headj.configured || fc_headj.fd < 0) return -1;

    fcHeadjFileHeader hdr;
    ssize_t got = pread(fc_headj.fd, &hdr, sizeof(hdr), 0);
    if (got != (ssize_t)sizeof(hdr)) return -1;
    if (hdr.magic != FC_HEADJ_MAGIC || hdr.version != FC_HEADJ_VERSION) return -1;
    if (hdr.crc != log->crc_function(0, (char const *)&hdr, offsetof(fcHeadjFileHeader, crc))) return -1;
    if (hdr.log_size_bytes != log->log_size_bytes ||
            hdr.num_databases != log->num_databases) {
        flashcacheLogger(FC_LL_WARNING,
                "Head journal config mismatch: journal log_size=[%lu] dbs=[%u] "
                "vs current log_size=[%lu] dbs=[%u]",
                hdr.log_size_bytes, hdr.num_databases,
                log->log_size_bytes, log->num_databases);
        return -1;
    }

    int found = 0;
    uint64_t best_items = 0;
    off_t off = (off_t)sizeof(hdr);
    fcHeadjRecord rec;
    while (pread(fc_headj.fd, &rec, sizeof(rec), off) == (ssize_t)sizeof(rec)) {
        off += (off_t)sizeof(rec);
        if (rec.magic != FC_HEADJ_REC_MAGIC) continue;
        if (rec.crc != log->crc_function(0, (char const *)&rec, offsetof(fcHeadjRecord, crc))) {
            continue; /* torn/partial append: skip, keep the previous best */
        }
        if (rec.head_offset >= log->log_size_bytes ||
                rec.tail_offset >= log->log_size_bytes) {
            continue;
        }
        *head_offset = rec.head_offset;
        *tail_offset = rec.tail_offset;
        best_items = rec.num_items;
        if (rec.seq > fc_headj.seq) fc_headj.seq = rec.seq;
        found = 1;
    }
    if (found) {
        fc_headj_last_num_items = best_items;
    }
    return found ? 0 : -1;
}

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

/* Delete tombstone replay: remove the key's entry if present. Returns 1 when
 * an entry was removed (the tombstone killed a live copy in the window). A
 * miss is normal: the insert may predate the window's tail (already GC'd). */
static int dedupDelete(dedupTable *t, uint32_t dbid, char const *key, size_t key_len) {
    uint64_t h = t->hash_fn(key, key_len);
    size_t b = h & (t->num_buckets - 1);
    dedupEntry **pp = &t->buckets[b];
    while (*pp) {
        dedupEntry *e = *pp;
        if (e->full_hash == h && e->dbid == dbid && e->key_len == key_len &&
                memcmp(e->key, key, key_len) == 0) {
            *pp = e->next;
            fcFree(e);
            t->num_entries--;
            t->num_stale++;
            return 1;
        }
        pp = &e->next;
    }
    return 0;
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
    int clean_shutdown = 1;
    if (readSuperblock(log, superblock_filename, &sb) == 0) {
        /* Consume the superblock immediately: whatever happens next, a future
         * boot must never reuse this window. */
        unlink(superblock_filename);
    } else {
        /* Crash path (fast-boot durability step 2): no clean-shutdown
         * superblock; take the durable window from the head journal. Every
         * byte in [tail, head) named by the last valid record was flushed
         * O_DIRECT and fsync'd before the record was written. */
        uint64_t jh = 0, jt = 0;
        if (logHeadJournalReadLast(log, &jh, &jt) != 0) {
            flashcacheLogger(FC_LL_NOTICE,
                    "Recovery: no valid superblock at [%s] and no usable head "
                    "journal; cold start", superblock_filename);
            return -1;
        }
        clean_shutdown = 0;
        memset(&sb, 0, sizeof(sb));
        sb.log_size_bytes = log->log_size_bytes;
        sb.num_databases = log->num_databases;
        sb.head_offset = jh;
        sb.tail_offset = jt;
        sb.num_items = fc_headj_last_num_items;
        unlink(superblock_filename); /* stale leftovers, if any */
        flashcacheLogger(FC_LL_NOTICE,
                "Recovery: crash recovery via head journal: head=[%lu] tail=[%lu]",
                jh, jt);
    }

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

        if (flag & FC_REPL_CMD_DELETE) {
            /* Delete tombstone (fast-boot durability step 1): remove the
             * key's live copy from the dedup table. Scan order == write
             * order, so any re-insert after the delete re-adds it. */
            item = scanFetch(&reader, off, total_len);
            if (item == NULL || !validateKeyInSerializedItem(item, log->crc_function)) {
                flashcacheLogger(FC_LL_WARNING,
                        "Recovery: bad tombstone at offset [%lu]; aborting scan", off);
                rc = -1;
                break;
            }
            char *key = NULL;
            size_t key_len = 0;
            extractKeyFromSerializedItem(item, &key, &key_len);
            uint32_t dbid = extractDbidFromSerializedItem(item);
            if (dbid >= log->num_databases || key_len == 0) {
                flashcacheLogger(FC_LL_WARNING,
                        "Recovery: bad tombstone dbid/key_len at offset [%lu]; aborting scan", off);
                rc = -1;
                break;
            }
            if (dedupDelete(&dedup, dbid, key, key_len)) {
                stats->tombstones_applied++;
            }
        } else if (!(flag & FC_SKIP_SEGMENT)) {
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
            "Recovery complete (%s): scanned=[%lu] live=[%lu] stale=[%lu] "
            "tombstones_applied=[%lu] bytes=[%lu] scan_us=[%lu] finalize_us=[%lu]",
            clean_shutdown ? "clean superblock" : "head journal",
            stats->items_scanned, stats->items_live, stats->items_stale,
            stats->tombstones_applied, stats->bytes_scanned,
            stats->scan_us, stats->finalize_us);

    dedupRelease(&dedup);
    fcFree(reader.buf);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Index reflection: serialize/restore the in-memory index (see recovery.h).
 *
 * File layout (little-endian, packed):
 *   fcIndexFileHeader
 *   per db: fcIndexFileDbHeader, then for each non-empty bucket:
 *     u64 bucket_idx, u32 chain_len, chain_len x logEntry (8B each)
 *   per db terminated by bucket_idx == UINT64_MAX
 *   u32 crc32c over all preceding bytes
 * ---------------------------------------------------------------------------*/
#include "include/hash.h"

#define FC_INDEXFILE_MAGIC   (0xFC1DECF11E5ULL)
#define FC_INDEXFILE_VERSION (1u)

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t version;
    uint32_t num_databases;
    uint64_t log_size_bytes;
    uint8_t  hash_seed[FLASHCACHE_HASHER_SEED_SIZE];
    uint64_t allocated_log_size_bytes;
    uint64_t total_num_items;
} fcIndexFileHeader;

typedef struct __attribute__((packed)) {
    uint32_t base_size_bits;
    uint32_t collision_bits_used;
    uint64_t num_items;
    uint64_t allocated_bytes;
} fcIndexFileDbHeader;

typedef struct {
    FILE *fp;
    uint32_t crc;
    flashcache_crc_function crc_fn;
    int failed;
} idxWriter;

static void idxWrite(idxWriter *w, void const *buf, size_t len) {
    if (w->failed) return;
    if (fwrite(buf, 1, len, w->fp) != len) { w->failed = 1; return; }
    w->crc = w->crc_fn(w->crc, (char const *)buf, len);
}

int logWriteIndexFile(struct flashcacheLog *log, char const *index_filename) {
    flashcacheAssert(log != NULL);
    flashcacheAssert(index_filename != NULL);

    if (stagingBufferGetTotalItemSize(log->staging_buffer) != 0 ||
            !fioRequestIsEmpty(&(log->log_flush_fio_request))) {
        flashcacheLogger(FC_LL_WARNING,
                "Index file write refused: staging buffer or log flush still busy");
        return -1;
    }

    /* Bucket indices must reflect one stable geometry: drive any in-progress
     * (or newly needed) incremental growth to completion. */
    for (uint32_t d = 0; d < log->num_databases; d++) {
        while (indexGrowIfRequired(log->index_list[d])) { /* advance */ }
    }

    char tmp_name[4096];
    int n = snprintf(tmp_name, sizeof(tmp_name), "%s.tmp", index_filename);
    if (n < 0 || (size_t)n >= sizeof(tmp_name)) return -1;

    FILE *fp = fopen(tmp_name, "wb");
    if (fp == NULL) {
        flashcacheLogger(FC_LL_WARNING, "Index file: fopen(%s) failed: %d", tmp_name, errno);
        return -1;
    }
    idxWriter w = { .fp = fp, .crc = 0, .crc_fn = log->crc_function, .failed = 0 };

    fcIndexFileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FC_INDEXFILE_MAGIC;
    hdr.version = FC_INDEXFILE_VERSION;
    hdr.num_databases = log->num_databases;
    hdr.log_size_bytes = log->log_size_bytes;
    log->hasher.get_seed(hdr.hash_seed);
    hdr.allocated_log_size_bytes = log->allocated_log_size_bytes;
    hdr.total_num_items = log->num_items;
    idxWrite(&w, &hdr, sizeof(hdr));

    for (uint32_t d = 0; d < log->num_databases; d++) {
        flashcacheIndex *index = log->index_list[d];
        fcIndexFileDbHeader dbh;
        memset(&dbh, 0, sizeof(dbh));
        dbh.base_size_bits = (uint32_t)index->base_size_bits;
        dbh.collision_bits_used = (uint32_t)index->collision_bits_used;
        dbh.num_items = index->num_items;
        dbh.allocated_bytes = log->allocated_log_size_bytes_per_db[d];
        idxWrite(&w, &dbh, sizeof(dbh));

        size_t table_size = indexTableSize(index);
        for (size_t b = 0; b < table_size; b++) {
            indexEntry *e = index->table[b];
            if (e == NULL) continue;
            uint32_t chain_len = 0;
            for (indexEntry *it = e; it; it = it->next) chain_len++;
            uint64_t bucket_idx = (uint64_t)b;
            idxWrite(&w, &bucket_idx, sizeof(bucket_idx));
            idxWrite(&w, &chain_len, sizeof(chain_len));
            for (indexEntry *it = e; it; it = it->next) {
                /* All entries are on-flash at clean shutdown (staging drained). */
                flashcacheAssert(it->item_entry.log_entry.on_flash);
                idxWrite(&w, &(it->item_entry.log_entry), sizeof(logEntry));
            }
        }
        uint64_t terminator = UINT64_MAX;
        idxWrite(&w, &terminator, sizeof(terminator));
    }

    uint32_t crc = w.crc;
    if (!w.failed && fwrite(&crc, 1, sizeof(crc), fp) != sizeof(crc)) w.failed = 1;
    int rc = (!w.failed && fflush(fp) == 0 && fsync(fileno(fp)) == 0) ? 0 : -1;
    fclose(fp);
    if (rc != 0 || rename(tmp_name, index_filename) != 0) {
        flashcacheLogger(FC_LL_WARNING, "Index file: write/rename failed: %d", errno);
        unlink(tmp_name);
        return -1;
    }
    flashcacheLogger(FC_LL_NOTICE, "Index file written: items=[%lu] file=[%s]",
            log->num_items, index_filename);
    return 0;
}

/* Restore one db's exact bucket geometry on an EMPTY index. */
static void indexRestoreGeometry(flashcacheIndex *index, uint32_t base_size_bits,
        uint32_t collision_bits_used) {
    flashcacheAssert(index->num_items == 0);
    size_t new_size = (size_t)1 << (base_size_bits + collision_bits_used);
    fcFree(index->table);
    index->table = (indexEntry **)fcCalloc(new_size, sizeof(indexEntry *));
    flashcacheAssert(index->table != NULL);
    index->base_size_bits = base_size_bits;
    index->collision_bits_used = collision_bits_used;
    index->num_hash_bucket_used = 0;
}

int logRecoverFromIndexFile(struct flashcacheLog *log,
        char const *superblock_filename, char const *index_filename,
        flashcacheRecoveryCountsCallback counts_cb, void *counts_cb_ctx) {
    flashcacheAssert(log != NULL);
    flashcacheAssert(log->num_items == 0); /* must run before any traffic */

    fcSuperblock sb;
    if (readSuperblock(log, superblock_filename, &sb) != 0) {
        flashcacheLogger(FC_LL_NOTICE,
                "Index recovery: no valid superblock at [%s]", superblock_filename);
        return -1;
    }

    /* Read the whole index file, then consume BOTH sidecars immediately —
     * whatever happens next, a future boot must never reuse this state. */
    FILE *fp = fopen(index_filename, "rb");
    if (fp == NULL) return -1;
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsz < (long)(sizeof(fcIndexFileHeader) + sizeof(uint32_t))) { fclose(fp); return -1; }
    char *buf = (char *)fcMalloc((size_t)fsz);
    flashcacheAssert(buf != NULL);
    size_t got = fread(buf, 1, (size_t)fsz, fp);
    fclose(fp);
    unlink(index_filename);
    unlink(superblock_filename);
    if (got != (size_t)fsz) { fcFree(buf); return -1; }

    /* Validate */
    uint32_t stored_crc;
    memcpy(&stored_crc, buf + fsz - sizeof(uint32_t), sizeof(uint32_t));
    uint32_t crc = log->crc_function(0, buf, (size_t)fsz - sizeof(uint32_t));
    fcIndexFileHeader hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (crc != stored_crc || hdr.magic != FC_INDEXFILE_MAGIC ||
            hdr.version != FC_INDEXFILE_VERSION ||
            hdr.num_databases != log->num_databases ||
            hdr.log_size_bytes != log->log_size_bytes) {
        flashcacheLogger(FC_LL_WARNING, "Index recovery: invalid index file [%s]", index_filename);
        fcFree(buf);
        return -1;
    }

    uint64_t t0 = log->monotonic_clock_us();

    /* Bucket indices and collision hashes were computed under the previous
     * process's SipHash seed — restore it before anything hashes. */
    log->hasher.init(hdr.hash_seed);

    log->head_offset = sb.head_offset;
    log->tail_offset = sb.tail_offset;
    log->allocated_log_size_bytes = hdr.allocated_log_size_bytes;

    char const *p = buf + sizeof(hdr);
    char const *end = buf + fsz - sizeof(uint32_t);
    int rc = 0;
    for (uint32_t d = 0; d < log->num_databases && rc == 0; d++) {
        if (p + sizeof(fcIndexFileDbHeader) > end) { rc = -1; break; }
        fcIndexFileDbHeader dbh;
        memcpy(&dbh, p, sizeof(dbh));
        p += sizeof(dbh);

        flashcacheIndex *index = log->index_list[d];
        indexRestoreGeometry(index, dbh.base_size_bits, dbh.collision_bits_used);
        log->allocated_log_size_bytes_per_db[d] = dbh.allocated_bytes;
        size_t table_size = indexTableSize(index);

        for (;;) {
            if (p + sizeof(uint64_t) > end) { rc = -1; break; }
            uint64_t bucket_idx;
            memcpy(&bucket_idx, p, sizeof(bucket_idx));
            p += sizeof(bucket_idx);
            if (bucket_idx == UINT64_MAX) break; /* db terminator */
            uint32_t chain_len;
            if (p + sizeof(chain_len) > end || bucket_idx >= table_size) { rc = -1; break; }
            memcpy(&chain_len, p, sizeof(chain_len));
            p += sizeof(chain_len);
            if (chain_len == 0 || p + (size_t)chain_len * sizeof(logEntry) > end) { rc = -1; break; }
            for (uint32_t i = 0; i < chain_len; i++) {
                logEntry le;
                memcpy(&le, p, sizeof(le));
                p += sizeof(le);
                indexAddLogEntry(index, (size_t)bucket_idx, &le);
            }
        }
        if (rc == 0) {
            if (index->num_items != dbh.num_items) {
                flashcacheLogger(FC_LL_WARNING,
                        "Index recovery: db [%u] item count mismatch [%lu] vs [%lu]",
                        d, index->num_items, (unsigned long)dbh.num_items);
                rc = -1;
                break;
            }
            log->num_items += dbh.num_items;
            if (counts_cb != NULL && dbh.num_items > 0) {
                counts_cb(counts_cb_ctx, d, (size_t)dbh.num_items);
            }
        }
    }
    fcFree(buf);

    if (rc != 0) {
        /* Corrupt mid-restore: too risky to serve — reset to cold start. */
        flashcacheLogger(FC_LL_WARNING, "Index recovery: parse failed; cold start");
        for (uint32_t d = 0; d < log->num_databases; d++) {
            /* leak-free reset: free chains inserted so far */
            flashcacheIndex *index = log->index_list[d];
            size_t ts = indexTableSize(index);
            for (size_t b = 0; b < ts; b++) {
                indexEntry *e = index->table[b];
                while (e) { indexEntry *nx = e->next; fcFree(e); e = nx; }
                index->table[b] = NULL;
            }
            index->num_items = 0;
            index->num_hash_bucket_used = 0;
            log->allocated_log_size_bytes_per_db[d] = 0;
        }
        log->num_items = 0;
        log->allocated_log_size_bytes = 0;
        log->head_offset = 0;
        log->tail_offset = 0;
        return -1;
    }

    flashcacheLogger(FC_LL_NOTICE,
            "Index recovery complete: items=[%lu] restore_us=[%lu]",
            log->num_items, log->monotonic_clock_us() - t0);
    return 0;
}
