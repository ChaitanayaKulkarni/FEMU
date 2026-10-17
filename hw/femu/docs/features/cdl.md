# Experimental TP4221 CDL queue capacities

FEMU exports a 64-byte Identify Controller vendor header at bytes 3072–3135:
Identify profile 2.2 with configuration log format 2.1; CDL statistics are removed. All existing
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

## No CDL statistics on the I/O path (2026-10-16)

CDL timed/miss counters, saturation flags, statistics snapshots and accounting
journals have been removed. Identify2.2 byte3090 is reserved zero. C1h is not
advertised and reading it returns Invalid Log Page. C0h is configuration-only
format2.1 with header48..63 reserved zero. Update the kernel together with FEMU.

The FTL worker pipelines modeled NAND service and keeps ready request ownership
until completion-ring delivery. It checks/releases each timed request once;
there is no accounting transaction, accounting mutex, file write, fsync, rename
or directory sync on completion. Optional per-command traces diagnose the flow.
Dispatch telemetry and fault/delay fixtures are active in qtest processes only.

Saved administrative configuration still uses SV/SEL and a configuration-only
version3 store. Older statistics-bearing stores are rejected and preserved;
use a fresh store path. Data I/O never writes that configuration file.

Default current-profile qtest `cdl-no-statistics` exercises32 cross-class BBSSD
writes/reads with exact data checks while a configuration-write failure is armed.
It requires an unchanged configuration file and no journal. Historical statistics
fixtures are outside default discovery. This gate does not claim full scheduler
conformance. The guest fio smoke independently checks256 CRC-verified reads,
explicit priority and same-CPU hint swaps through the kernel and NAND backend.

## Duration-weighted in-flight limits

The FEMU scheduler limits both total timed in-flight requests (`wfq_depth`) and
the number owned by each bound class. Limits use reciprocal configured durations,
with at least one slot per bound class. Rounding removes any over-allocation
from the largest allocation and distributes remaining slots by largest fractional
remainder, breaking ties by class ID. For depth64 and durations300/5000us the
limits are60/4. These are concurrency limits, not guaranteed throughput shares.

A class retains its reservation while any of its SQs remain bound, including
while it has no pending requests. Other bound classes cannot borrow that
reservation: issued backend work cannot be preempted on a high-priority arrival.
Consequently, low offered depth can leave slots unused and reduce throughput.
Creating more SQs for a class does not increase its allocation. Configured
classes without bound SQs reserve nothing; deleting the last SQ redistributes
its allocation and kicks pending work. Existing in-flight requests are drained,
not canceled, when adding a class shrinks an allocation.

If there are more bound classes than slots, simultaneous minimum-one reservations
are impossible. FEMU retains the global ceiling and weighted ordering with aging,
allowing every class to progress rather than permanently allocating zero slots.
This case does not provide simultaneous per-class slot protection.

Allocation runs only on SQ membership changes. The request path adds a limit
comparison to the existing class scan; no statistics, persistent I/O or kernel
changes are required. Class0 remains on the existing ordinary path.

Current qtests `cdl-low-qd-occupancy` and `cdl-slot-*` cover returning low-QD
traffic, retained/released reservations, unequal and equal durations, extreme
durations, rounding, depths1/2, four-class allocation and completion progress.
