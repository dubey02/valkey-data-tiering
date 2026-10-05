#### FlashCache stress test

Writes and reads a synthetic workload directly against libflashcache, without a
valkey-server, and prints throughput, GC and latency statistics until killed.

1. Build (from the repository root; builds the library and the stress test, needs a C++17 compiler and libaio-dev)
```
make BUILD_EXT_STORAGE=yes
```

2. Run
```
deps/flashcache/build/app/FlashCacheApp <db_filename> <dbsize_GiB> <max_num_items> <mean_value_sizes> <value_distribution_type> <number_of_large_items> <reading_preference> <tps> <num_ops_per_stat_emission> <read_to_write_ratio> <snapshot_version> <snapshot_save_type>
```

Example against a raw device:
```
sudo chmod 777 /dev/nvme1n1
deps/flashcache/build/app/FlashCacheApp /dev/nvme1n1 120 80000000 500:1,2000:1 normal_dist 20 prefer_old_item 0 2000000 1 2 bgsave
```

Example against a file:
```
truncate -s 1G /tmp/fc.db
deps/flashcache/build/app/FlashCacheApp /tmp/fc.db 1 20000 500:1,2000:1 normal_dist 0 prefer_old_item 0 20000 1 2 bgsave
```

3. Argument reference (also printed when run with no arguments)
```
<db_filename>: The name of the DB file
<dbsize_GiB>: The size of DB in GiB
<max_num_items>: The maximum number of items in DB
<mean_value_sizes>:<weight>: Comma separated mean value sizes with the value weight, e.g. 500:1,200:8,10000:1
<value_distribution_type>: normal_dist or constant_dist
<number_of_large_items>: Number of items of size 100 MiB
<reading_preference>: prefer_old_item or prefer_new_item or prefer_middle_item
<tps>: expected read/write tps (0 = unlimited)
<num_ops_per_stat_emission>: Number of ops before emitting stats
<read_to_write_ratio>: Number of read operations per write operation
<snapshot_version>: The version of snapshot algorithm to use, 1 or 2
<snapshot_save_type>: bgsave or forkless
```
