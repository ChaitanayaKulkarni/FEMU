#!/usr/bin/env python3
"""Socket-free CDL v2 persistent-store recovery checks.

The qtest transport is unavailable in some restricted build environments.  This
harness still exercises real FEMU process initialization and crash-journal replay
by starting the controller paused, terminating it, and checking the durable
images byte for byte.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


STORE_HEADER = 96
STORE_ENTRY = 32
TXN_BYTES = 96
U64_MAX = (1 << 64) - 1
EVIDENCE_DIR: Path | None = None
BOOT_SEQUENCE = 0


def put(buf: bytearray, off: int, size: int, value: int) -> None:
    buf[off : off + size] = value.to_bytes(size, "little")


def get(buf: bytes | bytearray, off: int, size: int) -> int:
    return int.from_bytes(buf[off : off + size], "little")


def store_digest(buf: bytes | bytearray) -> bytes:
    return hashlib.sha256(buf[:64] + buf[STORE_HEADER:]).digest()


def seal_store(buf: bytearray) -> None:
    buf[64:96] = store_digest(buf)


def validate_store(path: Path, classes: int) -> bytearray:
    buf = bytearray(path.read_bytes())
    assert len(buf) == STORE_HEADER + classes * STORE_ENTRY
    assert buf[:8] == b"FEMUCDL2"
    assert get(buf, 8, 4) == 2
    assert get(buf, 12, 4) == classes
    assert get(buf, 52, 4) == classes * STORE_ENTRY
    assert any(buf[32:48])
    assert buf[64:96] == store_digest(buf)
    return buf


def write_durable(path: Path, data: bytes | bytearray) -> None:
    tmp = path.with_name(path.name + ".new")
    with tmp.open("wb") as out:
        out.write(data)
        out.flush()
        os.fsync(out.fileno())
    os.replace(tmp, path)
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_txn(path: Path, cls: int, miss: bool, txid: int) -> None:
    buf = bytearray(TXN_BYTES)
    buf[:8] = b"CDL2TXN\0"
    put(buf, 8, 4, 3)
    buf[12] = cls
    buf[13] = int(miss)
    put(buf, 16, 8, txid)
    buf[24:40] = path.read_bytes()[32:48] if path.is_file() else b"X" * 16
    put(buf, 40, 8, txid)
    put(buf, 48, 8, 1000)
    put(buf, 56, 8, 1001 if miss else 1000)
    buf[64:96] = hashlib.sha256(buf[:64]).digest()
    write_durable(path.with_name(path.name + ".txn"), buf)


def boot(qemu: Path, store: Path, classes: int = 2) -> str:
    global BOOT_SEQUENCE
    BOOT_SEQUENCE += 1
    evidence = EVIDENCE_DIR / f"boot-{BOOT_SEQUENCE:03d}" if EVIDENCE_DIR else None
    if evidence:
        evidence.mkdir()
        for suffix in ("", ".txn"):
            source = store.with_name(store.name + suffix)
            if source.is_file():
                (evidence / ("before" + suffix + ".bin")).write_bytes(source.read_bytes())
    args = [
        str(qemu),
        "-machine", "q35,accel=tcg",
        "-nodefaults",
        "-display", "none",
        "-S",
        "-device",
        ("femu,addr=04.0,devsz_mb=64,femu_mode=2,vid=0x8086,"
         f"did=0x5845,queues=8,cdl=on,cdl_classes={classes},"
         f"cdl_max_sqs={8 if classes > 1 else 0},"
         f"cdl_sqs_per_class={8 if classes > 1 else 0},"
         f"cdl_bound_classes={min(classes - 1, 8)},cdl_file={store}"),
    ]
    proc = subprocess.Popen(args, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    time.sleep(0.35)
    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        raise AssertionError(
            f"QEMU exited during CDL initialization ({proc.returncode})\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        )
    proc.terminate()
    stdout, stderr = proc.communicate(timeout=5)
    assert proc.returncode in (-15, 0)
    if evidence:
        (evidence / "command.json").write_text(json.dumps(args, indent=2) + "\n")
        (evidence / "stdout.log").write_text(stdout)
        (evidence / "stderr.log").write_text(stderr)
        for suffix in ("", ".txn"):
            source = store.with_name(store.name + suffix)
            if source.is_file():
                (evidence / ("after" + suffix + ".bin")).write_bytes(source.read_bytes())
    return stdout + stderr


def entry_offset(cls: int) -> int:
    return STORE_HEADER + cls * STORE_ENTRY


def main() -> int:
    global EVIDENCE_DIR
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path,
                        help="retain stores and each boot's input/output images")
    args = parser.parse_args()
    qemu = args.qemu.resolve()
    assert qemu.is_file(), qemu

    passed: list[str] = []
    with contextlib.ExitStack() as cleanup:
        if args.output_dir:
            root = args.output_dir.resolve()
            root.mkdir(parents=True, exist_ok=False)
            EVIDENCE_DIR = root / "boots"
            EVIDENCE_DIR.mkdir()
        else:
            root = Path(cleanup.enter_context(
                tempfile.TemporaryDirectory(prefix="femu-cdl-store-")))

        # A genuinely missing store creates one nonzero controller identity.
        store = root / "replay.state"
        boot(qemu, store)
        initial = validate_store(store, 2)
        identity = bytes(initial[32:48])
        passed.append("fresh-store")

        # Saved ENABLE and descriptor state restore before readiness and keep
        # the same controller identity.  Restoration changes effective state,
        # so it advances generation once per fresh controller initialization.
        saved_store = root / "saved.state"
        boot(qemu, saved_store)
        saved = validate_store(saved_store, 2)
        saved_identity = bytes(saved[32:48])
        put(saved, 16, 8, 41)
        put(saved, 48, 4, 3)  # saved ENABLE is present and set
        saved_off = entry_offset(1)
        saved[saved_off] = 1
        saved[saved_off + 1] = 2
        put(saved, saved_off + 4, 4, 123456)
        seal_store(saved)
        write_durable(saved_store, saved)
        boot(qemu, saved_store)
        restored = validate_store(saved_store, 2)
        assert bytes(restored[32:48]) == saved_identity
        assert get(restored, 16, 8) == 42
        assert get(restored, 48, 4) == 3
        assert restored[saved_off] == 1
        assert restored[saved_off + 1] == 2
        assert get(restored, saved_off + 4, 4) == 123456
        boot(qemu, saved_store)
        restored_again = validate_store(saved_store, 2)
        assert get(restored_again, 16, 8) == 43
        assert bytes(restored_again[32:48]) == saved_identity
        passed.append("saved-state-restart")

        # A durable completion intent is replayed once and then removed.
        write_txn(store, 1, True, 1)
        boot(qemu, store)
        replayed = validate_store(store, 2)
        off = entry_offset(1)
        assert get(replayed, 24, 8) == 1
        assert get(replayed, off + 16, 8) == 1
        assert get(replayed, off + 24, 8) == 1
        assert bytes(replayed[32:48]) == identity
        assert not store.with_name(store.name + ".txn").exists()
        boot(qemu, store)
        replayed_twice = validate_store(store, 2)
        assert get(replayed_twice, off + 16, 8) == 1
        assert get(replayed_twice, off + 24, 8) == 1
        passed.append("exact-once-replay")

        # Main-store commit with an uncleared intent is not applied twice.
        write_txn(store, 1, False, 1)
        boot(qemu, store)
        already = validate_store(store, 2)
        assert get(already, off + 16, 8) == 1
        assert get(already, off + 24, 8) == 1
        assert not store.with_name(store.name + ".txn").exists()
        passed.append("committed-intent-cleanup")

        # Replay saturates both fields and persists their flags without wrap.
        saturated_store = root / "saturation.state"
        boot(qemu, saturated_store)
        saturated = validate_store(saturated_store, 2)
        off = entry_offset(1)
        put(saturated, 24, 8, 7)
        put(saturated, off + 16, 8, U64_MAX - 1)
        put(saturated, off + 24, 8, U64_MAX - 1)
        seal_store(saturated)
        write_durable(saturated_store, saturated)
        write_txn(saturated_store, 1, True, 8)
        boot(qemu, saturated_store)
        saturated = validate_store(saturated_store, 2)
        assert saturated[off + 8] == 3
        assert get(saturated, off + 16, 8) == U64_MAX
        assert get(saturated, off + 24, 8) == U64_MAX
        boot(qemu, saturated_store)
        saturated = validate_store(saturated_store, 2)
        assert get(saturated, off + 16, 8) == U64_MAX
        assert get(saturated, off + 24, 8) == U64_MAX
        passed.append("saturating-replay")

        # Corruption never replaces an existing epoch or changes its identity.
        corrupt_store = root / "corrupt.state"
        boot(qemu, corrupt_store)
        corrupt = bytearray(corrupt_store.read_bytes())
        corrupt[64] ^= 0x80
        write_durable(corrupt_store, corrupt)
        before = corrupt_store.read_bytes()
        output = boot(qemu, corrupt_store)
        assert corrupt_store.read_bytes() == before
        assert "corrupt CDL store" in output
        passed.append("corrupt-store-preserved")

        # An existing path that cannot be decoded as a file faults the
        # controller instead of being replaced as a first-use store.
        unreadable_store = root / "unreadable.state"
        unreadable_store.mkdir()
        output = boot(qemu, unreadable_store)
        assert unreadable_store.is_dir()
        assert "cannot read CDL store" in output
        passed.append("unreadable-store-preserved")

        # An intent without its main store is a fault, never a new zero epoch.
        stale = root / "stale.state"
        write_txn(stale, 1, False, 1)
        output = boot(qemu, stale)
        assert not stale.exists()
        assert stale.with_name(stale.name + ".txn").exists()
        assert "is missing while transaction" in output
        passed.append("missing-main-with-intent")

        # A transaction-id gap is rejected without modifying either image.
        gap = root / "gap.state"
        boot(qemu, gap)
        gap_before = gap.read_bytes()
        write_txn(gap, 1, False, 2)
        txn_before = gap.with_name(gap.name + ".txn").read_bytes()
        output = boot(qemu, gap)
        assert gap.read_bytes() == gap_before
        assert gap.with_name(gap.name + ".txn").read_bytes() == txn_before
        assert "invalid CDL transaction" in output
        passed.append("transaction-gap-rejected")

        # A copied journal cannot alter another controller's counters.
        foreign = root / "foreign.state"
        boot(qemu, foreign)
        before = foreign.read_bytes()
        write_txn(store, 1, False, 2)
        foreign_txn = foreign.with_name(foreign.name + ".txn")
        write_durable(foreign_txn, store.with_name(store.name + ".txn").read_bytes())
        output = boot(qemu, foreign)
        assert "corrupt CDL transaction" in output
        assert foreign.read_bytes() == before
        assert foreign_txn.exists()
        passed.append("foreign-transaction-rejected")

        # Legacy journals lack attempt/store binding and must be preserved.
        legacy = root / "legacy.state"
        boot(qemu, legacy)
        legacy_before = legacy.read_bytes()
        old = bytearray(64)
        old[:8] = b"CDL2TXN\0"
        put(old, 8, 4, 2)
        old[12] = 1
        put(old, 16, 8, 1)
        old[32:64] = hashlib.sha256(old[:32]).digest()
        old_path = legacy.with_name(legacy.name + ".txn")
        write_durable(old_path, old)
        output = boot(qemu, legacy)
        assert "corrupt CDL transaction" in output
        assert legacy.read_bytes() == legacy_before
        assert old_path.read_bytes() == old
        passed.append("legacy-transaction-preserved")

        # Controllers backed by different paths receive different identities.
        first = root / "controller-a.state"
        second = root / "controller-b.state"
        boot(qemu, first)
        boot(qemu, second)
        a = validate_store(first, 2)
        b = validate_store(second, 2)
        assert bytes(a[32:48]) != bytes(b[32:48])
        passed.append("controller-isolation")

    for name in passed:
        print(f"PASS {name}")
    if EVIDENCE_DIR:
        (EVIDENCE_DIR.parent / "results.json").write_text(
            json.dumps({"passed": passed, "boots": BOOT_SEQUENCE}, indent=2) + "\n")
    print(f"PASS {len(passed)} CDL persistent-store cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
