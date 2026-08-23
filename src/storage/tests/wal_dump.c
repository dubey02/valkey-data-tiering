/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* wal_dump: standalone dump/validator for the client-ack WAL.
 *
 * Implements the REPLAY-SIDE framing rules from the design doc so framing
 * behavior is testable before step 8 wires replay into boot:
 *   - STANDALONE records are valid immediately;
 *   - IN_GROUP records buffer until a GROUP_COMMIT whose group_seq,
 *     record_count, and group_crc (crc32c chained over member CRCs) match;
 *   - an unterminated or mismatched group => logical truncation at its
 *     GROUP_BEGIN; any CRC/parse break => truncation at the break.
 *
 * Build: gcc -O2 -I.. -o wal_dump wal_dump.c ../storage_wal.c -lpthread
 * Usage: wal_dump [-q] <file.wal>
 *   Prints one line per record; final summary line:
 *   SUMMARY valid_records=<n> groups_committed=<n> tombstones=<n> \
 *           truncated_at=<off|-> truncate_reason=<reason|->
 *   Exit 0 = whole file valid; 2 = valid prefix + truncated tail.
 */

#include "../storage_wal.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t rd_u32(const char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd_u64(const char *p) { uint64_t v; memcpy(&v, p, 8); return v; }

int main(int argc, char **argv) {
    int quiet = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) quiet = 1;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s [-q] <file.wal>\n", argv[0]);
        return 1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    fstat(fd, &st);
    char *data = malloc(st.st_size);
    if (read(fd, data, st.st_size) != st.st_size) { perror("read"); return 1; }
    close(fd);

    if (st.st_size < WAL_FILE_MAGIC_LEN ||
        memcmp(data, WAL_FILE_MAGIC, WAL_FILE_MAGIC_LEN) != 0) {
        fprintf(stderr, "bad magic\n");
        return 1;
    }

    size_t off = WAL_FILE_MAGIC_LEN;
    long valid_records = 0, groups_committed = 0, tombstones = 0;
    long long truncated_at = -1;
    const char *truncate_reason = "-";

    /* Pending-group state (groups are contiguous: at most one pending). */
    int in_group = 0;
    uint64_t group_seq = 0;
    size_t group_begin_off = 0;
    uint32_t group_chain_crc = 0;
    long group_members = 0, group_tombstones = 0;

#define TRUNCATE(reason, at)                                                   \
    do {                                                                       \
        truncated_at = (long long)(at);                                        \
        truncate_reason = (reason);                                            \
        goto done;                                                             \
    } while (0)

    while (off < (size_t)st.st_size) {
        if (off + WAL_RECORD_HDR_SIZE > (size_t)st.st_size)
            TRUNCATE("torn_header", in_group ? group_begin_off : off);
        const char *h = data + off;
        uint32_t crc = rd_u32(h);
        uint32_t body_len = rd_u32(h + 4);
        uint64_t lsn = rd_u64(h + 8);
        uint8_t kind = (uint8_t)h[16];
        uint8_t flags = (uint8_t)h[17];
        if (off + WAL_RECORD_HDR_SIZE + body_len > (size_t)st.st_size)
            TRUNCATE("torn_body", in_group ? group_begin_off : off);
        const char *body = h + WAL_RECORD_HDR_SIZE;
        uint32_t computed = walCrc32c(0, h + 4, WAL_RECORD_HDR_SIZE - 4);
        if (body_len) computed = walCrc32c(computed, body, body_len);
        if (computed != crc)
            TRUNCATE("crc_mismatch", in_group ? group_begin_off : off);

        switch (kind) {
        case WAL_REC_GROUP_BEGIN: {
            if (in_group) TRUNCATE("nested_group", group_begin_off);
            if (body_len != 8) TRUNCATE("bad_begin_body", off);
            in_group = 1;
            group_seq = rd_u64(body);
            group_begin_off = off;
            group_chain_crc = 0;
            group_members = 0;
            group_tombstones = 0;
            if (!quiet)
                printf("%08zx GROUP_BEGIN  lsn=%llu seq=%llu\n", off,
                       (unsigned long long)lsn, (unsigned long long)group_seq);
            break;
        }
        case WAL_REC_STANDALONE:
        case WAL_REC_IN_GROUP: {
            if ((kind == WAL_REC_IN_GROUP) != (in_group != 0))
                TRUNCATE(kind == WAL_REC_IN_GROUP ? "orphan_in_group"
                                                  : "standalone_in_group",
                         in_group ? group_begin_off : off);
            if (body_len < 12) TRUNCATE("bad_item_body", off);
            uint32_t dbid = rd_u32(body);
            uint32_t klen = rd_u32(body + 4);
            uint32_t vlen = rd_u32(body + 8);
            if (12 + (size_t)klen + vlen != body_len)
                TRUNCATE("item_len_mismatch", off);
            int tomb = (flags & WAL_RFLAG_TOMBSTONE) != 0;
            if (!quiet)
                printf("%08zx %-12s lsn=%llu db=%u klen=%u vlen=%u%s key=%.*s\n",
                       off, kind == WAL_REC_IN_GROUP ? "IN_GROUP" : "STANDALONE",
                       (unsigned long long)lsn, dbid, klen, vlen,
                       tomb ? " TOMBSTONE" : "",
                       (int)(klen > 40 ? 40 : klen), body + 12);
            if (in_group) {
                group_chain_crc = walCrc32c(group_chain_crc, &crc, sizeof(crc));
                group_members++;
                if (tomb) group_tombstones++;
            } else {
                valid_records++;
                if (tomb) tombstones++;
            }
            break;
        }
        case WAL_REC_GROUP_COMMIT: {
            if (!in_group) TRUNCATE("orphan_commit", off);
            if (body_len != 16) TRUNCATE("bad_commit_body", off);
            uint64_t cseq = rd_u64(body);
            uint32_t ccount = rd_u32(body + 8);
            uint32_t ccrc = rd_u32(body + 12);
            if (cseq != group_seq) TRUNCATE("commit_seq_mismatch", group_begin_off);
            if (ccount != (uint32_t)group_members)
                TRUNCATE("commit_count_mismatch", group_begin_off);
            if (ccrc != group_chain_crc)
                TRUNCATE("commit_crc_mismatch", group_begin_off);
            /* Group atomically valid: count its members now. */
            valid_records += group_members;
            tombstones += group_tombstones;
            groups_committed++;
            in_group = 0;
            if (!quiet)
                printf("%08zx GROUP_COMMIT lsn=%llu seq=%llu count=%u crc=ok\n",
                       off, (unsigned long long)lsn, (unsigned long long)cseq,
                       ccount);
            break;
        }
        default:
            TRUNCATE("unknown_kind", in_group ? group_begin_off : off);
        }
        off += WAL_RECORD_HDR_SIZE + body_len;
    }
    /* EOF with an open group = unterminated (crash mid-group before the
     * commit marker was durable): logical truncation at GROUP_BEGIN. */
    if (in_group) TRUNCATE("unterminated_group", group_begin_off);

done:
    printf("SUMMARY valid_records=%ld groups_committed=%ld tombstones=%ld "
           "truncated_at=%s truncate_reason=%s\n",
           valid_records, groups_committed, tombstones,
           truncated_at < 0 ? "-"
                            : ({ static char b[32];
                                 snprintf(b, sizeof(b), "%lld", truncated_at);
                                 b; }),
           truncate_reason);
    free(data);
    return truncated_at < 0 ? 0 : 2;
}
