# Client Delta Snapshots

This document describes the minimal "Client-Delta-Snapshots" system added to Ironwail. The goal is to reduce bandwidth by sending per-client delta-compressed entity snapshots instead of full entity updates every packet. Compatibility with legacy clients is **not** required.

## Overview

The server builds a snapshot for each client and sends either:

- **FULL** snapshot (no baseline available)
- **DELTA** snapshot (changes from the last acknowledged baseline)

Clients ACK the snapshot sequence after successfully applying it. The server keeps **exactly one** baseline and uses a **stop-and-wait** approach: it will resend the pending snapshot until it receives the ACK. If the client reports a baseline mismatch, the server resets its baseline and sends a FULL snapshot.

## Message types

New protocol message types:

- `svc_snapshot_full`
- `svc_snapshot_delta`
- `clc_snapshot_ack`

## Snapshot data

Snapshot state mirrors standard Quake entity update fields:

- origin (vec3)
- angles (vec3)
- modelindex
- frame
- colormap
- skin
- effects
- alpha
- scale
- step flag (movetype step)

Entities are keyed by `entnum` and sorted by visibility (PVS) like standard entity updates.

## Packet formats

**FULL**

```
[svc_snapshot_full][uint32 seq][uint16 entityCount]
  { [uint16 entnum][entityStateFull] }*
```

**DELTA**

```
[svc_snapshot_delta][uint32 seq][uint32 baseline_seq]
  [uint16 removeCount]{ [uint16 entnum] }*
  [uint16 addCount]{ [uint16 entnum][entityStateFull] }*
  [uint16 updateCount]{ [uint16 entnum][uint32 fieldMask][changedFields...] }*
```

**ACK**

```
[clc_snapshot_ack][uint32 seq]
```

If the client receives a delta with a baseline mismatch, it still reads the packet but responds with `clc_snapshot_ack` **seq = 0** to force a full resend.

## Stop-and-wait behavior

Per client:

- The server sends a snapshot with a new `seq` only when the previous one has been ACKed.
- While awaiting ACK, the server resends the same snapshot every send tick.
- After `sv_snapshottimeout` milliseconds, the server forces resends as FULL snapshots to recover from mismatches.

## Cvars

- `sv_snapshotdelta` (default `1`): enable delta snapshots; when `0`, always send FULL snapshots.
- `sv_snapshotdebug` (default `0`): print snapshot sizes and counts.
- `sv_snapshottimeout` (default `1000`): time (ms) before forcing FULL resend of a pending snapshot.
