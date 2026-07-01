/**
 * C shim for FlashCache callbacks that require C variadic functions,
 * and a wrapper for flashcacheInit that avoids passing the variadic
 * logger function pointer through Rust FFI.
 *
 * Rust stable doesn't support C variadic functions, so we implement
 * the logger callback in C and expose it to Rust.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "include/flashcache.h"
#include "include/flashcache_common.h"

/* No-op logger — discards all FlashCache log messages for the POC. */
void fc_shim_noop_logger(int level, const char *fmt, ...) {
    (void)level;
    (void)fmt;
    /* In production, forward to Valkey's logging system. */
}

/* Monotonic clock returning microseconds. */
uint64_t fc_shim_monotonic_clock_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ---------------------------------------------------------------------------
 * Eviction callback — called by FlashCache GC on the IO thread.
 * Pushes eviction events to a lock-free queue consumed by the Rust ASIO layer.
 * The Rust side exposes fc_eviction_callback_rust() for us to call.
 * ---------------------------------------------------------------------------*/

/* Defined in Rust (backends/flashcache/asio.rs) */
extern void fc_eviction_callback_rust(uint32_t dbid, const char *key, size_t key_len);
extern void fc_asio_control_msg_rust(void);

/* Real eviction callback — forwards to Rust queue. */
void fc_shim_eviction_callback(void *context, uint32_t dbid, char *key, size_t key_len) {
    (void)context;
    fc_eviction_callback_rust(dbid, key, key_len);
}

/* Real ASIO control message callback — signals Rust to wake up. */
void fc_shim_asio_control_msg_callback(void *context) {
    (void)context;
    fc_asio_control_msg_rust();
}

/* Legacy no-op versions kept for reference / fallback. */
void fc_shim_noop_eviction(void *context, uint32_t dbid, char *key, size_t key_len) {
    (void)context;
    (void)dbid;
    (void)key;
    (void)key_len;
}

void fc_shim_noop_asio_control_msg(void *context) {
    (void)context;
}


/*
 * Wrapper that calls flashcacheInit with the noop logger and monotonic
 * clock from this shim. This avoids passing a C variadic function pointer
 * (flashcache_logger) through Rust FFI, which Rust stable cannot represent.
 */
flashcacheReturnCode fc_shim_init(
    const char *db_filename,
    size_t db_size_bytes,
    size_t initial_index_size_per_db,
    uint32_t num_databases,
    uint32_t max_allocated_db_size_percent,
    uint32_t max_num_in_flight_read_requests,
    uint32_t min_garbage_collection_rate,
    uint32_t evict_under_max_logsize_time_limit,
    uint8_t optimized_delete_enabled,
    int eviction_enabled,
    flashcacheEvictionDetails *eviction_details,
    flashcacheAsioControlMsgCallbackDetails *asio_control_msg_callback_details
) {
    fprintf(stderr, "FC_SHIM_INIT: db_filename=%s db_size=%zu num_db=%u max_inflight=%u\n",
            db_filename, db_size_bytes, num_databases, max_num_in_flight_read_requests);
    fflush(stderr);

    /* Use no-op callbacks for now. The eviction callback and ASIO control msg
     * will be wired up via function pointers set from Rust during module init
     * in a future iteration. */

    flashcacheReturnCode rc = flashcacheInit(
        db_filename,
        db_size_bytes,
        initial_index_size_per_db,
        num_databases,
        max_allocated_db_size_percent,
        max_num_in_flight_read_requests,
        min_garbage_collection_rate,
        evict_under_max_logsize_time_limit,
        optimized_delete_enabled,
        fc_shim_monotonic_clock_us,
        eviction_details,
        fc_shim_noop_logger,
        asio_control_msg_callback_details
    );
    if (rc == FC_OK && !eviction_enabled) {
        flashcacheConfig fc_cfg;
        fc_cfg.key = FC_CONFIG_KEY_EVICTION_ENABLED;
        fc_cfg.numeric_value = 0;
        flashcacheSetConfig(&fc_cfg);
    }
    return rc;
}


/* Print comprehensive FlashCache metrics to stderr for debugging. */
void fc_shim_print_metrics(void) {
    fprintf(stderr, "FC_METRICS: "
            "items=%zu "
            "writes=%zu "
            "reads=%zu "
            "deletes=%zu "
            "read_inflight=%zu "
            "allocated_bytes=%zu "
            "active_bytes=%zu "
            "total_bytes=%zu "
            "pending_flush_bytes=%zu "
            "evicted=%zu "
            "evicted_bytes=%zu "
            "disk_writes=%zu "
            "disk_reads=%zu "
            "disk_write_bytes=%zu "
            "disk_read_bytes=%zu "
            "gc_disk_reads=%zu "
            "gc_read_bytes=%zu "
            "gc_write_bytes=%zu "
            "gc_items_moved=%zu "
            "hash_collision_reads=%zu "
            "partial_item_reads=%zu "
            "memory_bytes=%zu "
            "index_growing=%zu "
            "retryable_disk_err=%zu\n",
            flashcacheGetCountBasedMetric(FC_NUM_ITEMS),
            flashcacheGetCountBasedMetric(FC_NUM_WRITE_REQUEST),
            flashcacheGetCountBasedMetric(FC_NUM_READ_REQUEST),
            flashcacheGetCountBasedMetric(FC_NUM_DELETE_REQUEST),
            flashcacheGetCountBasedMetric(FC_NUM_READ_IN_FLIGHT),
            flashcacheGetCountBasedMetric(FC_ALLOCATED_DB_SIZE_BYTES),
            flashcacheGetCountBasedMetric(FC_ACTIVE_DB_SIZE_BYTES),
            flashcacheGetCountBasedMetric(FC_TOTAL_DB_SIZE_BYTES),
            flashcacheGetCountBasedMetric(FC_ITEM_PENDING_FLUSH_SIZE_BYTES),
            flashcacheGetCountBasedMetric(FC_NUM_ITEMS_EVICTED),
            flashcacheGetCountBasedMetric(FC_TOTAL_EVICTED_ITEMS_SIZE_BYTES),
            flashcacheGetCountBasedMetric(FC_NUM_DISK_WRITE),
            flashcacheGetCountBasedMetric(FC_NUM_DISK_READ),
            flashcacheGetCountBasedMetric(FC_TOTAL_DISK_WRITE_BYTES),
            flashcacheGetCountBasedMetric(FC_TOTAL_DISK_READ_BYTES),
            flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_NUM_DISK_READ),
            flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_READ_BYTES),
            flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_WRITE_BYTES),
            flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED),
            flashcacheGetCountBasedMetric(FC_NUM_UNUSED_DISK_READ_HASH_COLLISION),
            flashcacheGetCountBasedMetric(FC_NUM_PARTIAL_ITEM_READ),
            flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE),
            flashcacheGetCountBasedMetric(FC_IS_INDEX_GROWING),
            flashcacheGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR));
    fflush(stderr);
}

/* Run FlashCache cron tasks (flushes staging buffer, processes AIO, GC). */
void fc_shim_run_cron_tasks(void) {
    flashcacheRunCronTasks();
}

/* Set a FlashCache config value. */
void fc_shim_set_config(int key, int64_t value) {
    flashcacheConfig config;
    config.key = (flashcacheConfigKey)key;
    config.numeric_value = value;
    flashcacheSetConfig(&config);
}

/* Force flush buffered writes to disk. */
void fc_shim_fsync_buffered_writes(void) {
    flashcacheFsyncBufferedWrites();
}

/* Return FlashCache metrics as a formatted string for INFO output.
 * Caller must free the returned string with free().
 * Returns NULL on failure.
 */
char *fc_shim_get_metrics_string(void) {
    char *buf = (char *)malloc(4096);
    if (!buf) return NULL;

    int len = snprintf(buf, 4096,
        "fc_num_items:%zu\r\n"
        "fc_num_write_request:%zu\r\n"
        "fc_num_read_request:%zu\r\n"
        "fc_num_delete_request:%zu\r\n"
        "fc_num_read_in_flight:%zu\r\n"
        "fc_allocated_db_size_bytes:%zu\r\n"
        "fc_active_db_size_bytes:%zu\r\n"
        "fc_total_db_size_bytes:%zu\r\n"
        "fc_item_pending_flush_size_bytes:%zu\r\n"
        "fc_num_items_evicted:%zu\r\n"
        "fc_total_evicted_items_size_bytes:%zu\r\n"
        "fc_num_disk_write:%zu\r\n"
        "fc_num_disk_read:%zu\r\n"
        "fc_total_disk_write_bytes:%zu\r\n"
        "fc_total_disk_read_bytes:%zu\r\n"
        "fc_gc_num_disk_read:%zu\r\n"
        "fc_gc_read_bytes:%zu\r\n"
        "fc_gc_write_bytes:%zu\r\n"
        "fc_gc_num_items_moved:%zu\r\n"
        "fc_num_unused_disk_read_hash_collision:%zu\r\n"
        "fc_num_partial_item_read:%zu\r\n"
        "fc_active_memory_bytes:%zu\r\n"
        "fc_is_index_growing:%zu\r\n"
        "fc_num_index_growth_run:%zu\r\n"
        "fc_num_retryable_disk_error:%zu\r\n"
        "fc_gc_curr_rate_bytes_per_second:%zu\r\n"
        "fc_free_allocatable_db_size_bytes:%zu\r\n",
        flashcacheGetCountBasedMetric(FC_NUM_ITEMS),
        flashcacheGetCountBasedMetric(FC_NUM_WRITE_REQUEST),
        flashcacheGetCountBasedMetric(FC_NUM_READ_REQUEST),
        flashcacheGetCountBasedMetric(FC_NUM_DELETE_REQUEST),
        flashcacheGetCountBasedMetric(FC_NUM_READ_IN_FLIGHT),
        flashcacheGetCountBasedMetric(FC_ALLOCATED_DB_SIZE_BYTES),
        flashcacheGetCountBasedMetric(FC_ACTIVE_DB_SIZE_BYTES),
        flashcacheGetCountBasedMetric(FC_TOTAL_DB_SIZE_BYTES),
        flashcacheGetCountBasedMetric(FC_ITEM_PENDING_FLUSH_SIZE_BYTES),
        flashcacheGetCountBasedMetric(FC_NUM_ITEMS_EVICTED),
        flashcacheGetCountBasedMetric(FC_TOTAL_EVICTED_ITEMS_SIZE_BYTES),
        flashcacheGetCountBasedMetric(FC_NUM_DISK_WRITE),
        flashcacheGetCountBasedMetric(FC_NUM_DISK_READ),
        flashcacheGetCountBasedMetric(FC_TOTAL_DISK_WRITE_BYTES),
        flashcacheGetCountBasedMetric(FC_TOTAL_DISK_READ_BYTES),
        flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_NUM_DISK_READ),
        flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_READ_BYTES),
        flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_WRITE_BYTES),
        flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED),
        flashcacheGetCountBasedMetric(FC_NUM_UNUSED_DISK_READ_HASH_COLLISION),
        flashcacheGetCountBasedMetric(FC_NUM_PARTIAL_ITEM_READ),
        flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE),
        flashcacheGetCountBasedMetric(FC_IS_INDEX_GROWING),
        flashcacheGetCountBasedMetric(FC_NUM_INDEX_GROWTH_RUN),
        flashcacheGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR),
        flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_CURR_RATE_BYTES_PER_SECOND),
        flashcacheGetCountBasedMetric(FC_FREE_ALLOCATABLE_DB_SIZE_BYTES));

    if (len < 0 || len >= 4096) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Concatenate two metric strings. Frees the base string and returns a new
 * malloc'd string containing base + append. Caller must free the result.
 * Returns base unchanged if append is NULL or empty.
 */
char *fc_shim_concat_metrics(char *base, const char *append) {
    if (!append || !*append) return base;
    if (!base) return NULL;

    size_t base_len = strlen(base);
    size_t append_len = strlen(append);
    char *combined = (char *)malloc(base_len + append_len + 1);
    if (!combined) return base;

    memcpy(combined, base, base_len);
    memcpy(combined + base_len, append, append_len);
    combined[base_len + append_len] = '\0';
    free(base);
    return combined;
}
