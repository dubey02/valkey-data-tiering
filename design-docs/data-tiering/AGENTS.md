# Data Tiering: Coding Rules

Upstream's `DEVELOPMENT_GUIDE.md`, `src/unit/README.md` and `.github/instructions/` apply first. This file adds only what data tiering needs on top.

## A. Design

### 1. Core stays storage-engine agnostic

Tenet: tiering is a plug-in. Core knows the interface, never the engine.

1. Tiering logic lives in `src/ext_*.c` and `src/storage/`. Core gets hooks only, through the tiering API, and never reads tiering state to decide something itself.
2. Core assumes nothing about any one storage engine. Whatever works for a native storage engine must work for a module storage engine and for a future engine that implements the same interface. Native code never calls module APIs.
3. Prefer a filter or callback argument over an `#ifdef` in core logic.
4. Reuse what exists, upstream or ours, before adding a second path for the same job.
5. Tiering functions share the `extStorage` prefix.

### 2. Compile-time switch

Tenet: built out means upstream. Built in means tiering.

1. `BUILD_EXT_STORAGE` is the only gate. The storage engine is chosen once at startup by config.
2. Built out, the binary keeps only tiering state fields and behaves like upstream with near-zero overhead. Everything else tiering compiles only when built in, and tests that need it skip when built out.

### 3. Storage engine contract

Tenet: one interface, every storage engine, same meaning.

1. Each op in `storage.h` is required or optional. An optional op may be `NULL`, and the valkey server checks before calling. Each op's comment states what a storage engine may assume.
2. Expiry belongs to the valkey server. A feature that needs TTLs in the storage engine adds that to the contract explicitly.
3. Every return is checked and every failure leaves the key in a valid state. Each error class is used only for what the contract says it means.
4. The mock honors the same contract as a real storage engine, even where it runs differently. A contract change updates the mock in the same PR.

### 4. Async and threads

Tenet: a key in flight belongs to the storage engine until its completion is polled.

1. Tiering IO threads never touch the keyspace. Serialize and deserialize run there, never on the main thread.
2. An entry handed to a storage engine is borrowed until its completion is polled. The main thread does not touch it, not even to read it.
3. A key in a transit state accepts no write. Block or reject, or the completion overwrites newer data.
4. A fetched value returns to the keyspace only on the main thread, and anything blocked for it is fully restored on completion.
5. All in-flight IO finishes or is cancelled before a storage engine closes.

### 5. Behavior and compatibility

Tenet: nothing changes for a user unless we decide it and write it down.

1. Any change to existing Valkey behavior, including inheriting an upstream setting, is called out in the PR and recorded in `design-docs/data-tiering/`. Design decisions and assumptions go there too.
2. `valkey.conf` and user-facing text describe what ships today.
3. Every new config says whether it is mutable or immutable, and why.
4. The current phase spills values only. Do not make later phases harder.

## B. Code quality

### 6. Correctness and invariants

Tenet: assert invariant and document them. Fix causes, not symptoms.

1. A change in one Valkey component can break another. Look for it, add tests, and keep both native and module storage engines correct.
2. Before you change a function, list every place the change must reach: callers, tests, fixtures, config. That list is the scope.
3. State invariants where the state is defined, assert them, and add tests that fail when another change breaks them.
4. Never weaken an assert or add a tolerant handler for a state that cannot happen.
5. Memory accounting stays exact across a spill and a fetch.
6. Add a state or enum value in the PR that uses it, and static-assert it fits its storage.
7. Go through upstream's accessors and helpers. Never touch a core struct's fields directly.

### 7. Naming and comments

Tenet: read like Valkey, not like an agent.

1. Comments describe behavior, briefly and specifically. No narration, no restating the code, no history, no ticket or PR references. The one exception is a `TODO` with its issue. Headers carry only what an API user needs. Internal detail goes in the `.c` file.
2. A struct gets one line saying what it is. Each member and each metric field gets one line.
3. Write "storage engine" and "valkey server" in full. Never bare "engine" or "server".
4. No `;` or `--` as punctuation in comments. No comment starts with a question word.
5. Name variables by what they hold. Single letters only for loop indices and upstream's own conventions.

### 8. Metrics and logging

Tenet: measure every layer. Keep the hot path quiet.

1. Each PR adds the metrics needed to measure its performance and run it in production. Names follow Valkey INFO patterns.
2. Logs go where they help. Nothing above `debug` on a per-key or per-command path. A `warning` means an operator must act.

### 9. Tests

Tenet: prove the result, on every storage engine, without luck.

1. List the scenarios a change can hit before writing tests. Each one is either tested or named as a known gap in the PR, with an issue if it matters. No test that adds no value.
2. Test realistic scenarios and read the data back. "It didn't crash" is not a test.
3. Core logic tests are storage-engine agnostic. Storage engine tests run on FlashCache, the mock and a module storage engine.
4. Race tests use pause points to freeze foreground and background work. Where that is not possible, a fuzzer covers the race.
5. Assert exact values only while state is frozen, bounds otherwise. Every wait has a deadline that dumps INFO and the server log.
6. Each test server gets its own storage file in the test's temp directory, removed after.
7. A test never widens a function's visibility to reach it.
8. Tiering suites run under the address and thread sanitizers.
9. TCL tests should be used for integration tests, GUNIT should be used for isolated unit testing.

## C. Process

### 10. Working a PR

Tenet: one board task, one scope, every deferral is a GitHub issue.

1. Each PR maps to a task on the [Data Tiering board](https://github.com/users/dubey02/projects/2/views/1) and targets the integration branch.
2. Deferred work gets a GitHub issue on the board. A `TODO` in code names the issue and the limit it leaves: `TODO(#N): <what is not handled>`.
3. A big feature lands as several PRs, each tested alone, with the end-to-end test in the last. Cut scope is reverted cleanly, not left half in.
4. Before review, rebase onto the integration branch and check no new helper duplicates one that landed meanwhile.
5. The PR description ends with what was skipped or not checked, and any risk the reviewer must know.
6. The repo is public. No proprietary code, internal links or secrets.
7. These rules change. If one blocks the right change, update it in the same PR and say why.

## Appendix

### A1. Deferred topics

Decided when their milestone starts.

- Compression in the tiering path.
- Flash records in RDB and replication.
- Key spilling.
- Keyspace notifications and client-tracking invalidation for spilled keys.
- Upstream sync method for the integration branch.
- Completion wake-up without waiting for the next cron tick.
- Overhead measurement method, for the benchmark doc.
