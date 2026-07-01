#ifndef __FLASHCACHE_SNAPSHOT_EXPORTER_H
#define __FLASHCACHE_SNAPSHOT_EXPORTER_H

#include "include/serialization.h"
#include "include/snapshot_manager.h"
#include "include/crc.h"
#include <stdio.h>

// Used in `calculateAndSaveEncodedLen` to save values of different bit-lengths
// Same values as defined in rdb.h in ElastiCacheRedis
#define RDB_6BITLEN 0
#define RDB_14BITLEN 1
#define RDB_32BITLEN 0x80
#define RDB_64BITLEN 0x81
#define UINT32_MAX (4294967295U)

/**
 * Invokes the snapshot exporter process for FDB
 * @param source_fdb_filename: The file being read from.
 * @param target_rdb_filename: The file being written to.
 * @param metadata: Contains RDB secret, running checksum, the CRC64 checksum function callback,
 *                   and the callback to obtain the TTL and the customer DB ID for a given key.
 * @returns: Success: 0. Failure: -1 and logs.
 */
int snapshotExporterProcessFDB(const char *source_fdb_filename,
                              const char *target_rdb_filename,
                              flashcacheSnapshotExportMetadata *metadata);

/**
 * Updates the checksum based on the item. Writes the item to the target_rdb file.
 * @param target_rdb: The file being written to.
 * @param item: Item being written to the target file.
 * @param crc64_checksum: The running crc64 checksum that will be updated.
 * @param crc64_callback: The crc64 calculation callback.
 * @param dbid_and_ttl_callback: The callback to obtain the customer db id and the expiry time for a given key.
 * @return: Success: 0. Failure: -1 and logs.
 */
int snapshotExporterUpdateChecksumAndWriteItemToTargetFile(FILE *target_rdb,
                                                       char *item,
                                                       uint64_t *crc64_checksum,
                                                       crc64_checksum_callback crc64_callback,
                                                       get_customer_dbid_and_ttl_callback dbid_and_ttl_callback);
#endif
