# ZLM MP4 Recording I/O Capacity Benchmark Results

Results of a seven-level concurrency ramp measuring how many simultaneous MP4
recording streams `ainvr51` sustains, and which resource binds first.

Methodology follows `docs/replay-recording-benchmark.md`, with one deliberate
deviation: the load generator runs on the device under test and publishes over
loopback. Keeping it off-host, as that document prescribes, capped the run at
the network link instead of the disk (see [Discarded first attempt](#discarded-first-attempt)).

Run date: 2026-09-15 to 2026-09-16.

## Summary

| Constraint | Limit | Basis |
| --- | --- | --- |
| **NIC `eno1` (1 GbE)** | **~700 streams / ~350 cameras** | Extrapolated. Binds in production. |
| Disk write | 1920 streams / ~960 cameras | Measured. 295 MB/s ceiling. |
| CPU | ~7000 streams | Extrapolated. 3.04 of 12 cores at 1920 streams. |

Recording throughput exceeds network ingest capacity by roughly 3x. Raising the
real camera count requires a 10 GbE NIC first; until then no storage-side
optimisation is reachable.

Stream counts assume a 1:1 mix of main and sub profiles averaging 1.28 Mbps per
stream. One dual-stream camera equals two streams. All counts scale inversely
with bitrate.

## Environment

| Item | Value |
| --- | --- |
| Host | `ainvr51`, aarch64 Jetson, 12 cores, 61 GB RAM, Ubuntu |
| ZLMediaKit | docker container `zlmediakit`, host PID of `MediaServer` sampled per run |
| Recording path | `/data/mergefs/pool` (mergerfs/FUSE over ext4 on `sda1`) |
| Recording device | WD_BLACK P40 Game Drive, 931.5 GB, USB 3.2 Gen 2 (10 Gbps link) |
| Idle device | Samsung 970 EVO Plus 1TB NVMe, mounted at `/data` |
| NIC | `eno1`, 1000 Mb/s |
| Co-resident load | edge-ai, yolo, lpr, mobileclip2 containers; 2 live camera streams |

Relevant ZLM configuration: `mp4_max_second=300`, `record.fileBufSize=65536`,
`general.mergeWriteMS=0`, `protocol.enable_mp4=0` (recording enabled per stream
through `startRecord`).

## Method

- **Bitrate.** Main profile 1920x1080@25, GOP 50, H.264 High, 2.05 Mbps. Sub
  profile 640x360@25, GOP 50, H.264 Main, 515 kbps. Generated with `ffmpeg`;
  no camera samples were available.
- **Source media in `/dev/shm`.** Holding the samples in tmpfs keeps the
  generator from reading the recording device. `sda` read throughput measured 0
  for the whole run, confirming isolation.
- **Load on the device under test.** `test_bench_push` publishes to
  `rtsp://127.0.0.1:554/live/...` over loopback, bypassing the NIC so the disk
  ceiling is reachable.
- **Recording.** `startRecord` (`type=1`) called per stream.
- **Sampling.** `/proc/diskstats` for `sda` throughput, IOPS, utilisation and
  write latency; `/proc/<pid>/stat` for process CPU; `/proc/meminfo` for dirty
  pages; ZLM `getMediaList` for per-stream health.

## Ramp results

| Streams | sda write | Util | Write lat | IOPS | ZLM CPU | ZLM RSS | Recording | Verdict |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| 60 | 13.3 MB/s | - | - | 22 | 10% | 295 MB | 60 / 60 | linear |
| 120 | 19.3 MB/s | - | - | 31 | 19% | 308 MB | 120 / 120 | linear |
| 240 | 37.8 MB/s | - | - | 63 | 37% | 460 MB | 240 / 240 | linear |
| 480 | 72.2 MB/s | - | - | 154 | 79% | 857 MB | 480 / 480 | linear |
| 960 | 144.4 MB/s | 39.6% | 0.91 ms | 378 | 170% | 1.7 GB | 960 / 960 | linear |
| **1920** | **295.7 MB/s** | **98.4%** | 1.18 ms | 798 | 304% | 8.2 GB | 1920 / 1920 | **sustainable ceiling** |
| 2400 | 220-289 MB/s | 88-98% | 1.2 ms | 659-818 | 331% | 9.4 GB | 1696-2025 | degraded |
| 2880 | 262-286 MB/s | 96-98% | 1.16 ms | 814-956 | 359% | 8.6 GB | 1886-2159 | degraded |

CPU is a single-core percentage against 12 cores.

From 60 to 960 streams the curve tracks ideal linear scaling at a steady
0.150 MB/s per stream. At 1920 streams throughput reaches 295.7 MB/s with the
device 98% utilised and every stream still recording.

The 1920-stream level was resampled twice to confirm it is sustainable rather
than a transient: T+1min at 294.7 MB/s with 8178 MB RSS, T+4min at 295.7 MB/s
with 8179 MB RSS. Resident memory did not grow and dirty pages held near 350 MB.

## Failure mode

Past 1920 streams the server does not fail loudly. It degrades:

- **Throughput stops rising and falls back.** 2400 streams wrote 220-289 MB/s
  and 2880 streams wrote 262 MB/s, neither exceeding the 295.7 MB/s reached at
  1920. Added streams produce no additional written bytes.
- **Buffers accumulate.** `MediaServer` RSS climbed from 8.2 GB to 9.4 GB as
  data it could not write backed up in memory.
- **Recording count oscillates.** Streams actually in the recording state
  fluctuated between 1696 and 2159 while publishers began to churn and reconnect.

One observation during the run is a measurement artefact, not a server limit:
roughly 400 streams appeared unable to start recording at the 2400 level. This
was the collection script's `curl -m 8` timing out against a saturated server;
calling `startRecord` individually returned success. The throughput plateau is
independent of it.

## Storage findings

`/dev/sda` reports `rotational=1`, but this is a false signal from the USB
bridge: the WD_BLACK P40 Game Drive is an SSD, and the USB link negotiated
10 Gbps. The measured 798 IOPS at 1.18 ms latency corroborates this - a
mechanical drive at that concurrency would show tens of milliseconds.

Sequential 8 GB write capability, measured with the benchmark load stopped:

| Path | Throughput |
| --- | ---: |
| NVMe `/data` (idle) | 770 MB/s |
| Via mergerfs (recording path) | 451 MB/s |
| Direct to underlying ext4 | 382 MB/s |
| Benchmark result, 1920 concurrent streams | 295.7 MB/s |

Two conclusions:

- **mergerfs/FUSE is not a penalty.** Writing through it measured faster than
  writing to the underlying ext4 directly (451 vs 382 MB/s, a caching effect).
- **The gap is concurrency, not media.** 295.7 MB/s is 65-78% of raw sequential
  throughput. The shortfall comes from 1920 files being appended simultaneously,
  which breaks the sequential stream into interleaved writes.

One suspect setting: `sda` has a block-layer queue depth of `nr_requests=2`,
against 1023 on the NVMe and a typical default of 64-256. This may be limiting
merge efficiency under highly concurrent interleaved writes.

## Recommendations

1. **Move to a 10 GbE NIC.** The only change that raises the real camera count.
   The 1 GbE link caps the box at ~700 streams / 350 cameras, leaving the disk's
   1920-stream capability unreachable.
2. **Raise the `sda` queue depth.** Zero cost and immediately testable:
   `echo 256 > /sys/block/sda/queue/nr_requests`, then re-run the 1920-stream
   level and check whether throughput passes 295 MB/s.
3. **Move the recording pool to NVMe, or pool multiple disks under mergerfs.**
   The internal Samsung 970 EVO Plus writes at twice the current recording
   device; scaling the concurrent-write ratio suggests a ceiling near 3800
   streams. It has only 249 GB free, so space must be freed first. Worth doing
   only after the NIC upgrade.

## Discarded first attempt

The first run followed the reference topology with the source generator on a
separate macOS host. The link between the two hosts saturated at roughly
150-200 Mbps at 100 devices / 200 streams:

- Main-profile streams arrived at 0.92 Mbps each against 2.01 Mbps sent by the
  source.
- 22 of 200 proxies failed to establish, biased almost entirely toward the
  1080p profile.
- `aliveSecond` dropped to 1 on surviving streams, indicating continuous
  reconnection.

Source-host CPU stayed at 21.9% and the pushers at 7%, ruling out compute. The
data from this phase is invalid and was discarded; load generation moved onto
the device under test.

## Limitations

- **Generator co-resident with the server.** The pushers consumed 0.67 cores at
  1920 streams. Production has no such overhead, so real CPU headroom is larger
  than measured.
- **The NIC limit is extrapolated.** Traffic ran over loopback. The ~700-stream
  figure is derived from 1 GbE bandwidth and mean per-stream bitrate, not measured.
- **Not an isolated environment.** Inference containers and two live camera
  streams ran throughout.
- **Levels held 1-5 minutes.** The 1920-stream level was resampled at T+4min to
  confirm memory stability, but no multi-hour soak was run. Slow leaks are out
  of scope.
- **Results are bitrate-bound.** Every stream count assumes 1.28 Mbps average.

## Cleanup

Benchmark load is stopped and no benchmark streams remain registered in ZLM.
Remaining manual steps:

```bash
sudo rm -rf /data/mergefs/pool/record/live/bmain_* \
            /data/mergefs/pool/record/live/bsub_*
rm -rf /dev/shm/bench
```

- The pool held 466 GB of benchmark recordings (404 GB free) at the end of the run.
- `MediaServer` RSS remained at 9.3 GB after the load stopped; peak buffers are
  not released. Restart the `zlmediakit` container.
- The `on_record_mp4` hook posts to TinyNVR at `127.0.0.1:8088`, so its database
  accumulated a large number of `bmain_*` and `bsub_*` recording rows that need
  pruning.
