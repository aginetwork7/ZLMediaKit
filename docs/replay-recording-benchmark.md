the ZLM hook URLs remain on `http://127.0.0.1:8088/...`.
the same bitrate ratio as production. This command requires `ffmpeg` with
# ZLM MP4 Recording and Replay Benchmark

This benchmark measures the maximum sustainable replay concurrency while ZLM
continues to pull live RTSP streams and write MP4 recordings.

Each virtual device has two independent streams: `s0` uses the main profile and
`s1` uses the sub profile. Five hundred virtual devices therefore create 1,000
independent ZLM proxy and MP4 recording streams.

## Topology

```text
macOS: source ZLM + test_bench_push
  -> Linux ZLM under test -> MP4 storage
  -> test_bench_replay
```

Keep the source generator off the device-under-test host so source media reads
and RTP publishing do not affect its CPU, network, or storage measurements.

The examples use:

```text
macOS source ZLM: 192.168.3.24:554 and :8089
Linux ZLM under test: 192.168.2.112:554 and :8089
MP4 storage: /data/mergefs/pool
```

## 1. Start virtual sources

Build the required tools on macOS:

```bash
cd /path/to/ZLMediaKit2/build
cmake --build . --target MediaServer test_bench_push test_bench_replay -j8
```

Run a source ZLM with RTSP enabled. Use representative main and sub samples,
then publish 500 uniquely named streams for each profile:

```bash
./test_bench_push --in /media/main.mp4 --out rtsp://192.168.3.24:554/live/bench --stream-prefix main --count 500 --delay 20 --rtp 0
./test_bench_push --in /media/sub.mp4 --out rtsp://192.168.3.24:554/live/bench --stream-prefix sub --count 500 --delay 20 --rtp 0
```

This produces `bench_main_0` through `bench_main_499`, plus the matching
`bench_sub_*` streams. If camera samples are unavailable, generate repeatable
H.264 inputs with representative bitrate, GOP, frame rate, and resolution.

## 2. Create recording proxies

Run the direct ZLM seeder from the ZLM repository:

```bash
./tools/bench_seed_zlm_proxies.sh seed \
  --zlm-api http://192.168.2.112:8089 \
  --source-base rtsp://192.168.3.24:554/live/bench \
  --device-count 500 \
  --replace \
  --request-delay-ms 50 \
  --request-retries 5
```

For each device index, the script creates:

```text
live/bench/<index>/s0 <- bench_main_<index>
live/bench/<index>/s1 <- bench_sub_<index>
```

Both proxies enable RTSP output and MP4 recording. The script reads the ZLM API
secret from `conf/config.ini` and source RTSP credentials from
`/opt/media/auth/shared-config.json` by default; it does not print either.
Use `--secret-file` or `--source-auth-file` to supply alternate local paths.

Wait for at least one MP4 segment to close before creating replay URLs. Keep
the production `mp4_max_second` for the final measurement.

## 3. Collect recording metrics

Run the collector on the Linux host, outside the ZLM container. Supply the host
PID of MediaServer and the block device backing `mp4_save_path`:

```bash
./tools/record_zlm_storage_resources.sh \
  --pid MEDIA_SERVER_HOST_PID \
  --stream-count 1000 \
  --block-device nvme0n1 \
  --interval 1 \
  --log-dir ./log
```

The CSV includes MediaServer CPU/RSS, process read/write rates, and block-device
read/write throughput and IOPS. During recording-only load, sustained
`disk_write_mb_s` represents MP4 write pressure. Replay raises
`disk_read_mb_s` once reads no longer fit in page cache.

## 4. Generate replay inputs

Choose a completed `[BEGIN, END)` recording interval in Unix seconds, then
generate one replay URL for every recorded stream:

```bash
./tools/bench_seed_zlm_proxies.sh replay-list \
  --replay-base rtsp://192.168.2.112:554 \
  --device-count 500 \
  --begin BEGIN \
  --end END \
  --output ./replay-urls.txt
```

The generated list contains 1,000 URLs and lets the replay client distribute
sessions across both profiles and all devices.

## 5. Sweep replay concurrency

Start at 100 sessions and double each successful level. Hold each level for 30
minutes, then binary-search the first failing range:

```bash
./test_bench_replay \
  --in-file ./replay-urls.txt \
  --count 1000 \
  --delay 20 \
  --warmup 30 \
  --duration 1800 \
  --media-duration 3600 \
  --setup-timeout 30000 \
  --rtp 0
```

Report the highest level that has zero setup failures and runtime disconnects,
continuous MP4 recording, and no sustained CPU, memory, network, or storage
saturation. Use enough finalized MP4 segments for replay data to exceed RAM
when measuring disk-read worst case.

## Cleanup

Remove the benchmark proxies after the run:

```bash
./tools/bench_seed_zlm_proxies.sh clean \
  --zlm-api http://192.168.2.112:8089 \
  --device-count 500
```