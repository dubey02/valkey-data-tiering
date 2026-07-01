#include "include/snapshot_exporter.h"
#include "include/snapshot_version_two.h"
#include "include/util.h"

#define FC_SNAPSHOT_EXPORTER_WRITE_CHUNK_SIZE_BYTES (2 * 1024 * 1024)    // Max allowed write
#define memrev64ifbe(p) ((void)(0))

// The following RDB op-codes are defined in rdb.h in the ElastiCacheRedis package
#define FC_SNAPSHOT_EXPORTER_SELECT_DB_OPCODE (254)                      // The SELECTDB Opcode is 254
#define FC_SNAPSHOT_EXPORTER_EXPIRY_OPCODE_MS (252)                      // The EXPIRE_MS Opcode is 252
#define FC_SNAPSHOT_EXPORTER_EOF_OPCODE (255)                            // The EOF Opcode is 255

extern flashcache_logger logger;

/**
 * If the buffer passed in is not a checksum, update the checksum. Loop over the
 * buffer and write it to a target_rdb file.
 * @param target_rdb: The file being written to.
 * @param buf_size: Amount of bytes from the buffer being written.
 * @param crc64_checksum: The running crc64 checksum that will be updated.
 * @param crc64_callback: The crc64 calculation callback.
 * @return: In case of success: return 0. In case of failure: return -1. Caller is 
 * responsible for logging on failures for more meaningful error messages.
 */
static int updateChecksumAndWriteToTargetRDBFile(FILE *target_rdb,
                                          void *buf,
                                          size_t buf_size,
                                          uint64_t *crc64_checksum,
                                          crc64_checksum_callback crc64_callback) {
    if (crc64_callback != NULL) {
        updateChecksumUsingCrc64(crc64_checksum, buf, buf_size, crc64_callback);
    }
    size_t max_processing_chunk = FC_SNAPSHOT_EXPORTER_WRITE_CHUNK_SIZE_BYTES;
    while (buf_size) {
        size_t bytes_to_write = (max_processing_chunk && max_processing_chunk < buf_size) ?
                                 max_processing_chunk :
                                 buf_size;
        if (fwrite(buf, 1, bytes_to_write, target_rdb) != bytes_to_write)
            return -1;
        // NO FFLUSH FOR NOW
        buf = (char *)buf + bytes_to_write;
        buf_size -= bytes_to_write;
    }
    return FC_OK;
}

/**
 * Saves the encoded length and re-calculates the checksum.
 * The first two bits in the first byte are used to hold the encoding type.
 * @param target_rdb: The file being written to.
 * @param len: The value that needs encoding.
 * @param crc64_checksum: The running crc64 checksum that will be updated.
 * @param crc64_callback: The crc64 calculation callback.
 * @return: Success: Number of bytes written. Failure: -1 and logs.
 */
static int calculateAndSaveEncodedLen(FILE *target_rdb,
                                      uint64_t len,
                                      uint64_t *crc64_checksum,
                                      crc64_checksum_callback crc64_callback) {
    unsigned char buf[2];
    size_t nwritten;

    if (len < (1 << 6)) {
        /* Save a 6 bit len */
        buf[0] = (len & 0xFF) | (RDB_6BITLEN << 6);
        if (updateChecksumAndWriteToTargetRDBFile(target_rdb, buf, 1, crc64_checksum, crc64_callback) == -1) {
            goto err;
        }
        nwritten = 1;
    } else if (len < (1 << 14)) {
        /* Save a 14 bit len */
        buf[0] = ((len >> 8) & 0xFF) | (RDB_14BITLEN << 6);
        buf[1] = len & 0xFF;
        if (updateChecksumAndWriteToTargetRDBFile(target_rdb, buf, 2, crc64_checksum, crc64_callback) == -1) {
            goto err;
        }
        nwritten = 2;
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len */
        buf[0] = RDB_32BITLEN;
        if (updateChecksumAndWriteToTargetRDBFile(target_rdb, buf, 1, crc64_checksum, crc64_callback) == -1) {
            goto err;
        }
        uint32_t len32 = htonl(len);
        if (updateChecksumAndWriteToTargetRDBFile(target_rdb, &len32, 4, crc64_checksum, crc64_callback) == -1) {
            goto err;
        }
        nwritten = 1 + 4;
    } else {
        /* Save a 64 bit len */
        buf[0] = RDB_64BITLEN;
        if (updateChecksumAndWriteToTargetRDBFile(target_rdb, buf, 1, crc64_checksum, crc64_callback) == -1) {
            goto err;
        }
        len = hostToNetworkBytes64(len);
        if (updateChecksumAndWriteToTargetRDBFile(target_rdb, &len, 8, crc64_checksum, crc64_callback) == -1) {
            goto err;
        }
        nwritten = 1 + 8;
    }
    return nwritten;
err:
    flashcacheLogger(FC_LL_WARNING, "Snapshot exporter- Unable to write encoded length to target rdb file\n", 0);
    return -1;
}

/**
 * Writes the SELECTDB OPCODE followed by the given database id to the target file. Checksum is updated.
 * @param target_rdb: The file being written to.
 * @param db_id: The database ID.
 * @param crc64_checksum: The running crc64 checksum that will be updated.
 * @return: Success: 0. Failure: -1 and logs.
 */
int writeDatabaseIdToTargetRdbFile(FILE *target_rdb,
                         uint32_t db_id,
                         uint64_t *crc64_checksum,
                         crc64_checksum_callback crc64_callback) {
    // Write the SELECTDB opcode
    unsigned char opcode = FC_SNAPSHOT_EXPORTER_SELECT_DB_OPCODE;
    if (updateChecksumAndWriteToTargetRDBFile(target_rdb, &opcode, 1, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter- Unable to write SELECTDB Opcode to target rdb file.", 0);
        return -1;
    }
    // Write the DB id
    if (calculateAndSaveEncodedLen(target_rdb, db_id, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter- Unable to write DB ID to target rdb file.", 0);
        return -1;
    }
    return FC_OK;
}

int writeExpiryToTargetRdbFile(FILE *target_rdb,
                           long long expire_time,
                           uint64_t *crc64_checksum,
                           crc64_checksum_callback crc64_callback) {
    // Write the EXPIRY opcode
    unsigned char expire_opcode = FC_SNAPSHOT_EXPORTER_EXPIRY_OPCODE_MS;
    if (updateChecksumAndWriteToTargetRDBFile(target_rdb, &expire_opcode, 1, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter - Unable to write expiry Opcode to target rdb file.", 0);
        return -1;
    }
    // Write the expiry
    // Store in little endian
    memrev64ifbe(&expire_time);
    if (updateChecksumAndWriteToTargetRDBFile(target_rdb, &expire_time, 8, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter - Unable to write expiry time to target rdb file.", 0);
        return -1;
    }
    return FC_OK;
}

int snapshotExporterUpdateChecksumAndWriteItemToTargetFile(FILE *target_rdb,
                                                       char *item,
                                                       uint64_t *crc64_checksum,
                                                       crc64_checksum_callback crc64_callback,
                                                       get_customer_dbid_and_ttl_callback dbid_and_ttl_callback) {
    flashcacheAssert(validateKeyInSerializedItem(item, flashcacheCrc32c));
    flashcacheAssert(validateValueInSerializedItem(item, flashcacheCrc32c));
    char *key, *value;
    size_t key_len, value_len;
    extractKeyFromSerializedItem(item, &key, &key_len);
    extractValueFromSerializedItem(item, &value, &value_len);
    uint32_t internal_db_id = extractDbidFromSerializedItem(item);
    int customer_db_id = 0;
    // Fetch the expiry and customer DB ID for the item
    long long expire_time = dbid_and_ttl_callback((int)(internal_db_id),
                                             (const char*)(key),
                                             (int)(key_len),
                                             &customer_db_id);
    // Write the DBID
    if (writeDatabaseIdToTargetRdbFile(target_rdb, customer_db_id, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter - Unable to write item's DB ID to target RDB file.", 0);
        return -1;
    }

    // Write the expiry data
    if (expire_time != -1 &&
        writeExpiryToTargetRdbFile(target_rdb, expire_time, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter - Unable to write item's expiry to target RDB file.", 0);
        return -1;
    }

    // Write the 1-byte flag of the value type
    if (updateChecksumAndWriteToTargetRDBFile(target_rdb, value, 1, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter - Unable to write value type flag to target rdb file.", 0);
        return -1;
    }
    // Write the key len
    if (calculateAndSaveEncodedLen(target_rdb, key_len, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter - Unable to write key length to target rdb file.", 0);
        return -1;
    }
    // Write the key
    if (updateChecksumAndWriteToTargetRDBFile(target_rdb, key, key_len, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter - Unable to write key to target rdb file.", 0);
        return -1;
    }

    // Write the value
    if (updateChecksumAndWriteToTargetRDBFile(target_rdb,
                                              value + 1,
                                              value_len - 1,
                                              crc64_checksum,
                                              crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter - Unable to write value to target rdb file.", 0);
        return -1;
    }
    return FC_OK;
}

/**
 * Write the EOF as well as the final checksum value to the target RDB file.
 * @param target_rdb: The file being written to.
 * @param crc64_checksum: The running crc64 checksum that will be updated.
 * @param crc64_callback: The crc64 calculation callback.
 * @return: Success: 0. Failure: -1 and logs.
 */
static int writeEofAndChecksumToTargetRdbFile(FILE *target_rdb,
                                              uint64_t *crc64_checksum,
                                              crc64_checksum_callback crc64_callback) {
    // Write the 1-byte EOF Opcode(255)
    unsigned char eof = FC_SNAPSHOT_EXPORTER_EOF_OPCODE;
    if (updateChecksumAndWriteToTargetRDBFile(target_rdb, &eof, 1, crc64_checksum, crc64_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter- Unable to write EOF Opcode to target rdb file.", 0);
        return -1;
    }
    // Write the 8-byte checksum value
    if (updateChecksumAndWriteToTargetRDBFile(target_rdb,
                                              crc64_checksum,
                                              8,
                                              crc64_checksum,
                                              NULL) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot exporter- Unable to write final checksum to target rdb file.", 0);
        return -1;
    }
    return FC_OK;
}

int snapshotExporterProcessFDB(const char *source_fdb_filename,
                               const char *target_rdb_filename,
                               flashcacheSnapshotExportMetadata *metadata) {
    FILE *target_rdb;
    FILE *source_fdb;
    // Open target RDB file in append mode to add flash data to it
    target_rdb = fopen(target_rdb_filename, "a");
    if (target_rdb == NULL) {
        flashcacheLogger(FC_LL_WARNING,
                         "Snapshot Exporter- Cannot open target rdb file at: %s \n",
                         target_rdb_filename);
        return -1;
    }
    // Open fdb file in read mode to read data from it
    source_fdb = fopen(source_fdb_filename, "r");
    if (source_fdb == NULL) {
        flashcacheLogger(FC_LL_WARNING,
                         "Snapshot Exporter- Cannot open source fdb file at: %s \n",
                         source_fdb_filename);
        return -1;
    }
    uint64_t crc64_checksum = metadata->target_rdb_running_checksum;
    flashcacheSnapshotSecret* rdb_secret = metadata->rdb_secret;
    crc64_checksum_callback crc64_callback = metadata->checksum_callback;
    get_customer_dbid_and_ttl_callback dbid_and_ttl_callback = metadata->dbid_and_ttl_callback;
    if (snapshotManagerInvokeProcessingForSnapshotExporter(source_fdb,
                                 target_rdb,
                                 &crc64_checksum,
                                 rdb_secret,
                                 crc64_callback,
                                 dbid_and_ttl_callback) == -1) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot Exporter for FDB has failed", 0);
        return -1;
    }

    // Write the EOF and the 8-byte checksum value
    if (writeEofAndChecksumToTargetRdbFile(target_rdb, &crc64_checksum, crc64_callback) == -1) {
        return -1;
    }
    if (fflush(target_rdb)) {
        flashcacheLogger(FC_LL_WARNING, "Snapshot Exporter- unable to flush buffer for target RDB", 0);
    }
    fclose(target_rdb);
    fclose(source_fdb);
    // If success
    return FC_OK;
}
