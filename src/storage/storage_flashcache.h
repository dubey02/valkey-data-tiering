/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_STORAGE_FLASHCACHE_H
#define VALKEY_STORAGE_FLASHCACHE_H

#include "storage.h"

/* The FlashCache storage engine, selected by the name "flashcache". */
const storageEngine *storageFlashCacheEngine(void);

#endif /* VALKEY_STORAGE_FLASHCACHE_H */
