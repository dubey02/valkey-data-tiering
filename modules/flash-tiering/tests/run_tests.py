#!/usr/bin/env python3
"""CLI entry point for the key-spilling test harness.

Usage:
    python run_tests.py --module rocksdb      # test RocksDB backend (default)
    python run_tests.py --module flashcache   # (stub) test FlashCache backend
    python run_tests.py --module all          # test all available backends
"""
import argparse
import sys

from test_harness import TestHarness

VALID_MODULES = ("rocksdb", "flashcache", "all")


def main():
    parser = argparse.ArgumentParser(
        description="Key-spilling module test harness"
    )
    parser.add_argument(
        "--module",
        choices=VALID_MODULES,
        default="rocksdb",
        help="Module backend to test (default: rocksdb)",
    )
    args = parser.parse_args()

    harness = TestHarness()
    success = harness.run(args.module)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
