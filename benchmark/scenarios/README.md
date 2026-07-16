# Scenarios

Full documentation of all scenarios, configs, and the dispatch hierarchy lives in
[`../SCENARIOS.md`](../SCENARIOS.md).

## Adding a New Scenario

1. Create `scenarios/<name>/run.sh` — self-contained; source `../lib.sh`, read the config, run the workload, write `output.txt`. Use `mixed-rw/run.sh` as a template.
2. Create `scenarios/<name>/configs/default.env` (and any variants).
3. Document in `../SCENARIOS.md`.
