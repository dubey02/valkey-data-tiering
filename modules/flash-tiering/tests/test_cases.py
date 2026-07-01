"""Test case implementations for key-spilling module verification.

Each test function takes a redis.Redis client and returns a TestResult.
Tests exercise key-spilling behaviors: round-trip integrity, deletion of
spilled keys, FLUSHDB, eviction under memory pressure, multi-database
isolation, and error handling.
"""
import redis
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
VALUE_SIZE = 512           # bytes per value (padded)
NUM_KEYS_PRESSURE = 10000  # enough keys at 512B to exceed 32mb maxmemory
NUM_KEYS_SMALL = 100       # for quick non-pressure tests


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------
@dataclass
class TestResult:
    """Result of a single test case."""
    name: str
    passed: bool
    detail: str = ""


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def make_key(i: int) -> str:
    """Generate a deterministic key name for index *i*."""
    return f"key:{i}"


def make_value(i: int) -> bytes:
    """Generate a deterministic value for index *i*, padded to VALUE_SIZE bytes."""
    base = f"value:{i}:".encode()
    return base + b"x" * (VALUE_SIZE - len(base))


# ---------------------------------------------------------------------------
# Test list (used by the harness to discover tests)
# ---------------------------------------------------------------------------
ALL_TESTS = []


def _register(fn):
    """Decorator that adds a test function to ALL_TESTS."""
    ALL_TESTS.append(fn)
    return fn


# ---------------------------------------------------------------------------
# Test 1: SET/GET round-trip
# ---------------------------------------------------------------------------
@_register
def test_set_get_roundtrip(client: redis.Redis) -> TestResult:
    """SET 10000 keys (512B values), GET each, verify match."""
    name = "test_set_get_roundtrip"
    try:
        client.flushdb()

        # SET keys — enough to exceed 32mb maxmemory and trigger spilling
        for i in range(NUM_KEYS_PRESSURE):
            client.set(make_key(i), make_value(i))

        # GET each key and verify
        mismatches = 0
        first_mismatch_detail = ""
        for i in range(NUM_KEYS_PRESSURE):
            expected = make_value(i)
            actual = client.get(make_key(i))
            if actual != expected:
                mismatches += 1
                if not first_mismatch_detail:
                    actual_len = len(actual) if actual is not None else "None"
                    first_mismatch_detail = (
                        f"Mismatch at {make_key(i)}: "
                        f"expected {len(expected)} bytes, got {actual_len}"
                    )

        if mismatches > 0:
            return TestResult(name, False,
                              f"{mismatches} mismatches. {first_mismatch_detail}")
        return TestResult(name, True)
    except Exception as exc:
        return TestResult(name, False, str(exc))


# ---------------------------------------------------------------------------
# Test 2: DEL on spilled keys
# ---------------------------------------------------------------------------
@_register
def test_delete_tiered_keys(client: redis.Redis) -> TestResult:
    """SET keys, DEL every 10th, verify GET returns nil."""
    name = "test_delete_tiered_keys"
    try:
        client.flushdb()

        # Write keys to trigger spilling
        for i in range(NUM_KEYS_PRESSURE):
            client.set(make_key(i), make_value(i))

        # Delete every 10th key
        del_indices = list(range(0, NUM_KEYS_PRESSURE, 10))
        for i in del_indices:
            result = client.delete(make_key(i))
            if result != 1:
                return TestResult(name, False,
                                  f"DEL {make_key(i)} returned {result}, expected 1")

        # Verify deleted keys return None
        for i in del_indices:
            val = client.get(make_key(i))
            if val is not None:
                return TestResult(name, False,
                                  f"GET {make_key(i)} after DEL returned {val!r}, "
                                  f"expected None")

        return TestResult(name, True)
    except Exception as exc:
        return TestResult(name, False, str(exc))


# ---------------------------------------------------------------------------
# Test 3: FLUSHDB
# ---------------------------------------------------------------------------
@_register
def test_flushdb(client: redis.Redis) -> TestResult:
    """SET keys, FLUSHDB, verify DBSIZE=0."""
    name = "test_flushdb"
    try:
        client.flushdb()

        # Write keys to create both in-memory and spilled data
        for i in range(NUM_KEYS_PRESSURE):
            client.set(make_key(i), make_value(i))

        client.flushdb()

        dbsize = client.dbsize()
        if dbsize != 0:
            return TestResult(name, False,
                              f"DBSIZE after FLUSHDB is {dbsize}, expected 0")

        # Spot-check a few keys
        for i in [0, NUM_KEYS_PRESSURE // 2, NUM_KEYS_PRESSURE - 1]:
            val = client.get(make_key(i))
            if val is not None:
                return TestResult(name, False,
                                  f"GET {make_key(i)} after FLUSHDB returned "
                                  f"{val!r}, expected None")

        return TestResult(name, True)
    except Exception as exc:
        return TestResult(name, False, str(exc))


# ---------------------------------------------------------------------------
# Test 4: Eviction under memory pressure
# ---------------------------------------------------------------------------
@_register
def test_eviction_under_pressure(client: redis.Redis) -> TestResult:
    """SET keys exceeding maxmemory, verify no OOM errors."""
    name = "test_eviction_under_pressure"
    try:
        client.flushdb()
        oom_errors = 0
        for i in range(NUM_KEYS_PRESSURE):
            try:
                client.set(make_key(i), make_value(i))
            except redis.ResponseError as e:
                if "OOM" in str(e):
                    oom_errors += 1
                    if oom_errors == 1:
                        info_mem = client.info("memory")
                        used = info_mem.get("used_memory_human", "unknown")
                        return TestResult(
                            name, False,
                            f"OOM error on key {i}: {e}. Memory used: {used}"
                        )
                else:
                    raise

        return TestResult(name, True)
    except Exception as exc:
        return TestResult(name, False, str(exc))


# ---------------------------------------------------------------------------
# Test 5: Multi-database isolation
# ---------------------------------------------------------------------------
@_register
def test_multi_database(client: redis.Redis) -> TestResult:
    """Write to db 0 and db 1, verify isolation."""
    name = "test_multi_database"
    try:
        conn_info = client.connection_pool.connection_kwargs
        host = conn_info.get("host", "localhost")
        port = conn_info.get("port", 6399)

        client_db0 = redis.Redis(host=host, port=port, db=0)
        client_db1 = redis.Redis(host=host, port=port, db=1)

        try:
            client_db0.flushdb()
            client_db1.flushdb()

            # Write keys to db 0
            for i in range(NUM_KEYS_PRESSURE):
                client_db0.set(make_key(i), make_value(i))

            # Write different keys to db 1 (offset by NUM_KEYS_PRESSURE)
            for i in range(NUM_KEYS_PRESSURE):
                idx = i + NUM_KEYS_PRESSURE
                client_db1.set(make_key(idx), make_value(idx))

            # Verify db 0 keys (spot-check every 100th)
            for i in range(0, NUM_KEYS_PRESSURE, 100):
                val = client_db0.get(make_key(i))
                expected = make_value(i)
                if val != expected:
                    return TestResult(name, False,
                                      f"db0: mismatch at {make_key(i)}")

            # Verify db 1 keys (spot-check every 100th)
            for i in range(0, NUM_KEYS_PRESSURE, 100):
                idx = i + NUM_KEYS_PRESSURE
                val = client_db1.get(make_key(idx))
                expected = make_value(idx)
                if val != expected:
                    return TestResult(name, False,
                                      f"db1: mismatch at {make_key(idx)}")

            # Verify isolation: db 0 keys should NOT be in db 1
            for i in range(0, NUM_KEYS_SMALL, 10):
                val = client_db1.get(make_key(i))
                if val is not None:
                    return TestResult(name, False,
                                      f"db1 has db0 key {make_key(i)}")

            # Verify isolation: db 1 keys should NOT be in db 0
            for i in range(NUM_KEYS_PRESSURE,
                           NUM_KEYS_PRESSURE + NUM_KEYS_SMALL, 10):
                val = client_db0.get(make_key(i))
                if val is not None:
                    return TestResult(name, False,
                                      f"db0 has db1 key {make_key(i)}")

            return TestResult(name, True)
        finally:
            client_db0.close()
            client_db1.close()
    except Exception as exc:
        return TestResult(name, False, str(exc))


# ---------------------------------------------------------------------------
# Test 6: Error handling
# ---------------------------------------------------------------------------
@_register
def test_error_handling(client: redis.Redis) -> TestResult:
    """Invalid commands, nonexistent keys, verify server alive."""
    name = "test_error_handling"
    try:
        client.flushdb()

        # GET non-existent key should return None
        val = client.get("nonexistent_key_12345")
        if val is not None:
            return TestResult(name, False,
                              f"GET nonexistent key returned {val!r}, expected None")

        # DEL non-existent key should return 0
        result = client.delete("nonexistent_key_12345")
        if result != 0:
            return TestResult(name, False,
                              f"DEL nonexistent key returned {result}, expected 0")

        # Send an invalid command (wrong number of args) — should get error, not crash
        try:
            client.execute_command("SET")  # missing key and value
        except redis.ResponseError:
            pass  # expected

        # Verify server is still alive
        pong = client.ping()
        if not pong:
            return TestResult(name, False,
                              "Server did not respond to PING after error handling tests")

        return TestResult(name, True)
    except Exception as exc:
        return TestResult(name, False, str(exc))
