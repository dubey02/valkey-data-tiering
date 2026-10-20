/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Registers the reference mock storage engine through the module path.
 *
 * The storage engine logic lives in src/storage/storage_mock.c, compiled into
 * this module. OnLoad takes that one storage engine, renames the copy to
 * "module_mock", and registers it. The valkey server's built-in "mock" is a
 * separate thing. */

#include "storage/storage.h"
#include "storage/storage_mock.h"
#include "valkeymodule.h"

/* The registered storage engine outlives OnLoad, so it is static. */
static storageEngine module_mock_engine;

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
  VALKEYMODULE_NOT_USED(argv);
  VALKEYMODULE_NOT_USED(argc);

  if (ValkeyModule_Init(ctx, "storage_mock_module", 1, VALKEYMODULE_APIVER_1) ==
      VALKEYMODULE_ERR) {
    return VALKEYMODULE_ERR;
  }

  module_mock_engine = *storageMockEngine();
  module_mock_engine.name = "module_mock";

  if (ValkeyModule_RegisterStorageEngine(ctx, &module_mock_engine) ==
      VALKEYMODULE_ERR) {
    return VALKEYMODULE_ERR;
  }
  return VALKEYMODULE_OK;
}
