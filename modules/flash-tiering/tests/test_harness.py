"""Test orchestrator for the key-spilling module.

Manages valkey-server lifecycle with the key-spilling module loaded,
coordinates test execution from test_cases.py, reports results, and
cleans up.

Supports two module backends:
  - rocksdb    : key-spilling module with RocksDB backend (default)
  - flashcache : stub — module backend not yet implemented
  - all        : run all available backends
"""
import os
import signal
import subprocess
import sys
import time
import shutil

import redis

from test_cases import TestResult, ALL_TESTS

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
REDIS_PORT = 6399
MAXMEMORY = "32mb"
DB_PATH = "/tmp/ks-test/rocksdb"
NUM_DATABASES = 16
MAX_IN_FLIGHT_READS = 64

STARTUP_TIMEOUT = 30   # seconds
SHUTDOWN_TIMEOUT = 10  # seconds


class TestHarness:
    """Orchestrates valkey-server lifecycle and test execution."""

    def __init__(self):
        # Resolve key-spilling root from this file's location (key-spilling/tests/../)
        self.ks_root = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..")
        )
        # valkey-server binary
        self.server_binary = os.path.join(
            self.ks_root, "src", "valkey", "src", "valkey-server",
        )
        # key-spilling module shared object
        self.module_so = os.path.join(
            self.ks_root, "src", "key-spilling-module", "target", "release",
            "libkey_spilling_module.so",
        )
        self.server_process = None
        self.server_log_path = os.path.join(DB_PATH, "valkey.log")

    # ------------------------------------------------------------------
    # Server lifecycle
    # ------------------------------------------------------------------
    def _start_server(self, module_name: str) -> bool:
        """Start valkey-server with the key-spilling module loaded.

        Returns True if the server starts and responds to PING.
        """
        # Ensure data directory exists
        os.makedirs(DB_PATH, exist_ok=True)

        cmd = [
            self.server_binary,
            "--port", str(REDIS_PORT),
            "--maxmemory", MAXMEMORY,
            "--maxmemory-policy", "allkeys-lru",
            "--logfile", self.server_log_path,
            "--daemonize", "no",
            "--save", "",
            "--ext-data-enabled", "yes",
        ]

        if module_name == "rocksdb":
            # Load the key-spilling module with RocksDB backend
            if not os.path.isfile(self.module_so):
                print(f"ERROR: Module .so not found at {self.module_so}",
                      file=sys.stderr)
                print("  Build it first: cd key-spilling/src/key-spilling-module "
                      "&& cargo build --release", file=sys.stderr)
                return False

            cmd.extend([
                "--loadmodule", self.module_so,
                f"backend=rocksdb",
                f"db_path={DB_PATH}",
                f"num_databases={NUM_DATABASES}",
                f"max_in_flight_reads={MAX_IN_FLIGHT_READS}",
            ])
        elif module_name == "flashcache":
            # Stub — flashcache backend not yet implemented
            print(f"  [flashcache] Backend not yet implemented — skipping.",
                  file=sys.stderr)
            return False

        try:
            self.server_process = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except FileNotFoundError:
            print(f"ERROR: valkey-server not found at {self.server_binary}",
                  file=sys.stderr)
            return False

        return self._wait_for_server()

    def _wait_for_server(self) -> bool:
        """Poll PING until the server responds or we hit STARTUP_TIMEOUT."""
        deadline = time.time() + STARTUP_TIMEOUT
        while time.time() < deadline:
            try:
                client = redis.Redis(host="localhost", port=REDIS_PORT,
                                     socket_connect_timeout=2)
                if client.ping():
                    client.close()
                    return True
                client.close()
            except (redis.ConnectionError, redis.TimeoutError,
                    ConnectionRefusedError, OSError):
                pass
            time.sleep(0.5)

        # Timeout — print server log for diagnosis
        print("ERROR: valkey-server did not respond to PING within "
              f"{STARTUP_TIMEOUT}s", file=sys.stderr)
        log = self._get_server_log()
        if log:
            print("--- server log ---", file=sys.stderr)
            print(log, file=sys.stderr)
            print("--- end server log ---", file=sys.stderr)
        return False

    def _stop_server(self) -> None:
        """Send SHUTDOWN NOSAVE, wait, SIGKILL fallback, cleanup."""
        if self.server_process is None:
            return

        # Phase 1: graceful shutdown via Redis command
        try:
            client = redis.Redis(host="localhost", port=REDIS_PORT,
                                 socket_connect_timeout=2)
            client.shutdown(nosave=True)
            client.close()
        except (redis.ConnectionError, redis.TimeoutError,
                ConnectionRefusedError, OSError):
            pass  # server may already be down

        # Phase 2: wait for process exit
        try:
            self.server_process.wait(timeout=SHUTDOWN_TIMEOUT)
        except subprocess.TimeoutExpired:
            # Phase 3: force kill
            try:
                self.server_process.kill()
                self.server_process.wait(timeout=5)
            except Exception:
                pass

        self.server_process = None

    def _cleanup(self) -> None:
        """Remove the test data directory."""
        base_dir = os.path.dirname(DB_PATH)  # /tmp/ks-test
        if os.path.exists(base_dir):
            shutil.rmtree(base_dir, ignore_errors=True)

    def _get_server_log(self) -> str:
        """Read and return the server log file contents."""
        try:
            with open(self.server_log_path, "r") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    # ------------------------------------------------------------------
    # Test execution
    # ------------------------------------------------------------------
    def _run_module_tests(self, module_name: str) -> list:
        """Start server, run all test cases, stop server. Returns list of TestResult."""
        results = []

        started = self._start_server(module_name)
        if not started:
            log = self._get_server_log()
            for test_fn in ALL_TESTS:
                results.append(TestResult(
                    test_fn.__name__, False,
                    f"Server startup failed. Log: {log[:500]}"
                ))
            self._stop_server()
            self._cleanup()
            return results

        client = redis.Redis(host="localhost", port=REDIS_PORT,
                             socket_connect_timeout=5,
                             socket_timeout=30)
        server_crashed = False

        try:
            for test_fn in ALL_TESTS:
                if server_crashed:
                    results.append(TestResult(
                        test_fn.__name__, False, "Skipped: server crashed"
                    ))
                    continue

                try:
                    result = test_fn(client)
                    results.append(result)
                    self._print_result(result)
                except (redis.ConnectionError, ConnectionRefusedError) as exc:
                    server_crashed = True
                    results.append(TestResult(
                        test_fn.__name__, False,
                        f"Server connection lost: {exc}"
                    ))
                    self._print_result(results[-1])
                    print(f"WARNING: Server crashed during {test_fn.__name__}. "
                          f"Remaining tests will be skipped.", file=sys.stderr)
                except Exception as exc:
                    results.append(TestResult(
                        test_fn.__name__, False, f"Unexpected error: {exc}"
                    ))
                    self._print_result(results[-1])
        finally:
            client.close()
            self._stop_server()
            self._cleanup()

        return results

    # ------------------------------------------------------------------
    # Output formatting
    # ------------------------------------------------------------------
    @staticmethod
    def _print_result(result: TestResult) -> None:
        """Print PASS/FAIL line for a single test."""
        if result.passed:
            print(f"  PASS: {result.name}")
        else:
            detail = f" - {result.detail}" if result.detail else ""
            print(f"  FAIL: {result.name}{detail}")

    @staticmethod
    def _print_module_summary(module_name: str, results: list) -> None:
        """Print summary for one module."""
        total = len(results)
        passed = sum(1 for r in results if r.passed)
        failed = total - passed
        print(f"--- {module_name}: {total} tests, {passed} passed, "
              f"{failed} failed ---")

    @staticmethod
    def _print_combined_summary(all_results: dict) -> None:
        """Print combined summary across all modules."""
        print()
        print("=== COMBINED SUMMARY ===")
        total_failures = 0
        for module_name, results in all_results.items():
            passed = sum(1 for r in results if r.passed)
            total = len(results)
            failed = total - passed
            total_failures += failed
            print(f"  {module_name}: {passed}/{total} passed")
        if total_failures == 0:
            print("RESULT: PASS")
        else:
            print(f"RESULT: FAIL ({total_failures} failure(s) total)")

    # ------------------------------------------------------------------
    # Main entry point
    # ------------------------------------------------------------------
    def run(self, module_selection: str) -> bool:
        """Run tests for selected module(s). Returns True if all pass."""
        if module_selection == "all":
            modules = ["rocksdb", "flashcache"]
        elif module_selection in ("rocksdb", "flashcache"):
            modules = [module_selection]
        else:
            print(f"ERROR: Unknown module selection: {module_selection}",
                  file=sys.stderr)
            return False

        all_results = {}
        all_passed = True

        for module_name in modules:
            if module_name == "flashcache":
                print(f"\n[{module_name}] Backend not yet implemented — skipping.")
                print(f"  Once available, re-run with --module {module_name}")
                all_results[module_name] = []
                continue

            print(f"\n[{module_name}] Running tests...")
            results = self._run_module_tests(module_name)
            all_results[module_name] = results
            self._print_module_summary(module_name, results)

            if any(not r.passed for r in results):
                all_passed = False

        # Combined summary when multiple modules tested
        if len(all_results) > 1:
            self._print_combined_summary(all_results)

        return all_passed
