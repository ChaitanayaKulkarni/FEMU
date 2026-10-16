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
