/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_STORAGE_MOCK_H
#define VALKEY_STORAGE_MOCK_H

#include "storage.h"

/* The in-memory reference storage engine, registered under the name "mock". */
const storageEngine *storageMockEngine(void);

/* Executes up to max submitted requests, moving each to the completion queue,
 * without draining anything. poll_completions runs the same execution before
 * it drains. Returns the number executed. */
int storageMockRunIo(int max);

#endif /* VALKEY_STORAGE_MOCK_H */
