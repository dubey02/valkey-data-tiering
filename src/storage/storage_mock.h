/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_STORAGE_MOCK_H
#define VALKEY_STORAGE_MOCK_H

#include "storage.h"

/* The in-memory reference engine, registered under the name "mock". */
const storageEngine *storageMockEngine(void);

/* Test-only. Executes up to max submitted requests, moving each to the
 * completion queue, without draining anything. poll_completions also executes
 * before it drains, so this exists for tests that observe execution and drain
 * separately. Returns the number executed. */
int storageMockRunIo(int max);

#endif /* VALKEY_STORAGE_MOCK_H */
