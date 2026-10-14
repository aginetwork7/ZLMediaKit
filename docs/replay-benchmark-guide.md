# Replay 压测步骤

## 前提

- replay URL 的 `b.../e...` 窗口为 **10 分钟**，即 `e - b = 600` 秒。
- `conf/config.ini` 中：

```ini
[rtsp]
maxSessionCount=0
maxReplaySessionCount=0
```

## 文件和工具位置

| 用途 | 位置 |
| --- | --- |
| 压测源码 | `tests/test_bench_replay.cpp` |
| 压测二进制 | `release/darwin/Debug/test_bench_replay` |
| 资源采样脚本 | `tools/record_zlm_resources.sh` |
| MediaServer | `release/darwin/Debug/MediaServer` |

## 步骤

### 1. 构建

```bash
cd /Users/ld/E/CodePath/ZLMediaKit/build
cmake --build . --target MediaServer test_bench_replay -j8
```

### 2. 启动资源采样

先启动资源脚本，压测完成后按 `Ctrl-C` 停止脚本。

Docker 容器内执行：

```bash
./record_zlm_resources.sh \
  --pid auto \
  --stream-count 5000 \
  --network-interface eth0 \
  --interval 1
```

macOS 或 Linux 直接执行：

```bash
./record_zlm_resources.sh \
  --pid auto \
  --stream-count 5000 \
  --network-interface lo0 \
  --interval 1 \
  --log-dir ./log
```

若流量经物理网卡，将 `lo0` 换为实际接口，例如 `en0` 或 `eth0`。

### 3. 执行 5000 路 replay 压测

```bash
./test_bench_replay \
  --in 'rtsp://tinynvr:YflA3CPXxTrHmfQwLCkeUhEI6rzWxoLP@127.0.0.1:554/replay/C47905A21A3E/1/s1/b1786342006/e1786342606' \
  --count 5000 \
  --delay 50 \
  --warmup 30 \
  --duration 240 \
  --media-duration 600 \
  --setup-timeout 30000 \
  --rtp 0
```

每个 replay 请求都会生成独立 `sid_*`、reader 和 demuxer；同一个 URL 不会复用 source。

## 成功标志

先出现：

```text
replay benchmark: all clients connected, baseline_at_ms=..., warmup_sec=30
```

随后观察 `phase=sample`。成功时：

```text
succeeded=5000
failed=0
disconnected=0
```

## 成果物

资源脚本只生成一个 CSV：

```text
/opt/media/bin/log/zlm-resource-YYYYMMDD-HHMMSS.csv
```

使用 `--log-dir ./log` 时，文件保存在指定目录。

CSV 字段：

| 字段 | 含义 |
| --- | --- |
| `record_type` | `sample` 为逐秒样本；`summary` 为最后一行峰值摘要。 |
| `local_time` | 采样时间。 |
| `timestamp` | Unix 秒级时间戳。 |
| `client_type` | 固定为 `rtsp_replay`。 |
| `stream_count` | 压测目标连接数。 |
| `cpu_percent` | MediaServer 进程 CPU 使用率。 |
| `memory_mb` | MediaServer 进程 RSS。 |
| `network_io_mb_per_s` | 当前命名空间网络接口的收发总速率。 |
| `cpu_peak_percent` | 仅 `summary` 行填写，整个采样期 CPU 峰值。 |
| `memory_peak_mb` | 仅 `summary` 行填写，整个采样期 RSS 峰值。 |
| `reason` | 仅 `summary` 行填写，采样停止原因。 |
