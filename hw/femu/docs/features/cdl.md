# Experimental TP4221 CDL queue capacities

FEMU exports a 64-byte Identify Controller vendor header at bytes 3072–3135:
Identify profile 2.1, separate from the unchanged CDL log format 2.0. All existing
standard Identify offsets and the 4096-byte size are preserved. This is a local
experimental assignment, not a ratified NVMe allocation.

The reference controller is initialized explicitly with:

```sh
build-femu/qemu-system-x86_64 -machine q35 -smp 4 -m 1G \
  -device femu,devsz_mb=64,femu_mode=2,vid=0x8086,did=0x5845,cdl=on,queues=52,cdl_classes=7,cdl_max_sqs=48,cdl_sqs_per_class=8,cdl_bound_classes=6,cdl_file=/tmp/cdl-reference-n7.state
```

Supply guest boot options separately. Use a controller-specific store compatible
with N=7; an existing N=256 store is incompatible and is never overwritten or
cleared to accommodate a different count.

| Advertised capability | Reference maximum | Explicit reference host use |
|---|---:|---:|
| Class IDs including bypass 0 | 7 (0–6) | 0 and 1–4 |
| Bound nonzero classes | 6 | 4 |
| SQs per nonzero class | 8 | 4 |
| Aggregate nonzero-class SQs | 48 | 16 |
| Total I/O SQs / CQs | 52 / 52 | 20 / 20 |

`queues` initializes the equal total SQ/CQ limits. The three new properties
initialize the independent class limits. Their defaults are zero: N>1 requires
an explicit valid tuple; bypass-only N=1 requires all three zero. Ordinary non-CDL
defaults remain unchanged. Inconsistent configurations fail realization without
clamping. ENABLE does not change the profile or release quota.

The five LE16 capacity fields occupy bytes 3104–3113; the reference bytes are
`34 00 34 00 30 00 08 00 06 00`. They are actual counts excluding admin. FID 07h
continues to encode zero-based counts; this controller returns its fixed full
allocation (`00330033h` for 52/52), even for a smaller request. Queue creation
history survives deletion until CLR, so late renegotiation is a sequence error.

Create SQ atomically reserves per-class, aggregate and distinct-class quota before
publication. Mapping failure unwinds it; deletion/reset releases it only after
old ownership retires. Quota overflow returns Invalid Field in Command and leaves
no SQ or lasting reference. CQs have no class selector. A configured descriptor
without an SQ consumes no quota.

The host must validate the complete identity, capacities and both logs, then
create its exact requested layout. Reporting 52 allocated queues does not mean
52 queue rings are created. The 20-pair example is four ordinary pairs and four
pairs for each of classes 1–4. Linux support and H1 are separate implementation
work; a controller qtest does not establish four-core application routing.

Validation uses `cdl-capacity-reference`, `cdl-capacity-aggregate`,
`cdl-capacity-bound`, `cdl-capacity-properties` and `cdl-discovery-matrix`, followed
by the complete CDL regression suite. The reference qtest records exact Identify
and log images, verifies the 20-pair layout, then provisions all 52 pairs to test
the advertised maximum. The local conformance contract defines T23–T26 in addition
to T1–T22.

## Timed NAND execution and durable accounting

Use `femu_mode=1` (BBSSD) for full NAND execution and performance work. The
Identify-only example above uses NoSSD as a small discovery fixture; it is not
a performance configuration. The experiment launcher in `cdl-google-exp1/run.py`
supplies the complete BBSSD geometry, kernel profile and fio workload.

The controller FTL worker issues a bounded set of requests without sleeping
through each modeled NAND latency. Executed timed requests remain owned by the
FTL pending list until their modeled completion time. The worker freezes all
ready timing results before starting durable accounting. Completion-ring
backpressure retains those owners and cannot cause a second check or count.
Delete and reset use the existing poller/FTL pause handshake to retire them.

Already-ready results share one durable transaction, with at most 64 members.
No command waits for a batch to fill. Every member receives its own contiguous
transaction ID and counter update; all members are durable before any of their
CQEs may be published. Persistence failure sets CFS and suppresses delivery.
The `femu_cdl_persist_latency` trace reports native wall time and batch size;
this measures host filesystem cost, separately from the controller timer.
Its elapsed value covers the shared transaction and must not be summed once
per member as if each member performed a separate store operation.

The main checkpoint format and NVMe wire formats are unchanged. A single
request still uses the existing 96-byte version-3 `CDL2TXN` intent. Multiple
requests use a private version-4 `CDL2BTX` envelope in the same `.txn` path:

| Offset | Bytes | Value |
|---|---:|---|
| 0 | 8 | `CDL2BTX` followed by NUL |
| 8 | 4 | LE32 version 4 |
| 12 | 4 | LE32 member count, 2 through 64 |
| 16 | 8 | LE64 first transaction ID |
| 24 | 8 | Reserved, zero |
| 32 | 32 | SHA-256 of bytes 0–31 followed by bytes 64–end |
| 64 | 96 × count | Individually checksummed version-3 member records |

Recovery validates the complete envelope and every member, including store
identity and contiguous nonwrapping transaction IDs, before changing counters.
Only members newer than the checkpoint are applied. One new checkpoint covers
the recovered suffix; replaying the same journal cannot recount its members.
Malformed evidence is preserved and controller realization fails. An older
FEMU binary cannot replay a pending version-4 envelope: use the new binary for
recovery before attempting a downgrade, and retain backups of controller state.

Run the existing fault tests normally and the BBSSD batch fault variants in a
separate discovery pass (libqos has a fixed graph stack):

```sh
python3 hw/femu/tests/cdl-conformance.py --build build-perf --output <fresh-dir> \
  --pty --batch-store-faults --match cdl-batch-fault-
```

The batch fixtures hold the first backend owner until twelve commands have
been dispatched, inject each existing persistence failure, verify CFS and no
CQEs, and inspect any published intent for all twelve members. Run
`cdl-google-exp1/batch-recovery-test.py` separately for actual process-restart
recovery of valid and malformed envelopes. These are controller checks; the
fio same-CPU hint-swap audit supplies the separate kernel routing evidence.
