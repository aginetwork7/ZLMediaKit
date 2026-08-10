/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "Common/config.h"
#include "Player/PlayerProxy.h"
#include "Rtsp/UDPServer.h"
#include "Thread/WorkThreadPool.h"
#include "Util/CMD.h"
#include "Util/TimeTicker.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace {

volatile sig_atomic_t exit_flag = 0;

void onSignal(int) {
    exit_flag = 1;
}

struct BenchmarkStats {
    atomic<size_t> succeeded{0};
    atomic<size_t> failed{0};
    atomic<size_t> shutdown{0};
    atomic<uint64_t> setup_total_ms{0};
    atomic<uint64_t> setup_max_ms{0};
};

struct PlayerState {
    uint64_t started_at_ms;
    atomic<bool> play_result_seen{false};
    atomic<bool> playing{false};
};

void updateMax(atomic<uint64_t> &maximum, uint64_t value) {
    auto current = maximum.load();
    while (current < value && !maximum.compare_exchange_weak(current, value)) {
    }
}

struct ProcessSnapshot {
    uint64_t cpu_time_ns;
    uint64_t rss_bytes;
};

bool readProcessSnapshot(int process_id, ProcessSnapshot &snapshot) {
#if defined(__APPLE__)
    rusage_info_v2 usage = {};
    if (proc_pid_rusage(process_id, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t *>(&usage)) != 0) {
        return false;
    }
    snapshot.cpu_time_ns = usage.ri_user_time + usage.ri_system_time;
    snapshot.rss_bytes = usage.ri_resident_size;
    return true;
#elif defined(__linux__)
    ifstream stat_file("/proc/" + to_string(process_id) + "/stat");
    string stat_line;
    if (!getline(stat_file, stat_line)) {
        return false;
    }

    const auto process_name_end = stat_line.rfind(')');
    if (process_name_end == string::npos || process_name_end + 2 >= stat_line.size()) {
        return false;
    }

    istringstream stat_fields(stat_line.substr(process_name_end + 2));
    string field;
    uint64_t user_ticks = 0;
    uint64_t system_ticks = 0;
    for (int field_index = 0; stat_fields >> field; ++field_index) {
        if (field_index == 11) {
            user_ticks = stoull(field);
        } else if (field_index == 12) {
            system_ticks = stoull(field);
            break;
        }
    }

    ifstream statm_file("/proc/" + to_string(process_id) + "/statm");
    uint64_t virtual_pages = 0;
    uint64_t resident_pages = 0;
    if (!(statm_file >> virtual_pages >> resident_pages)) {
        return false;
    }

    const auto ticks_per_second = sysconf(_SC_CLK_TCK);
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (ticks_per_second <= 0 || page_size <= 0) {
        return false;
    }
    snapshot.cpu_time_ns = (user_ticks + system_ticks) * 1000000000ULL / static_cast<uint64_t>(ticks_per_second);
    snapshot.rss_bytes = resident_pages * static_cast<uint64_t>(page_size);
    return true;
#else
    (void)process_id;
    (void)snapshot;
    return false;
#endif
}

class ProcessSampler {
public:
    explicit ProcessSampler(int process_id) : _process_id(process_id) {}

    bool sample(double &cpu_percent, uint64_t &rss_bytes) {
        ProcessSnapshot current;
        if (!readProcessSnapshot(_process_id, current)) {
            return false;
        }

        const auto now_ms = getCurrentMillisecond();
        if (_has_previous && now_ms - _previous_at_ms < 100 && _has_sample) {
            cpu_percent = _last_cpu_percent;
            rss_bytes = _last_rss_bytes;
            return true;
        }

        cpu_percent = 0;
        if (_has_previous && now_ms > _previous_at_ms && current.cpu_time_ns >= _previous.cpu_time_ns) {
            const auto elapsed_ns = (now_ms - _previous_at_ms) * 1000000ULL;
            cpu_percent = static_cast<double>(current.cpu_time_ns - _previous.cpu_time_ns) * 100.0 / elapsed_ns;
        }
        _previous = current;
        _previous_at_ms = now_ms;
        _has_previous = true;
        _last_cpu_percent = cpu_percent;
        _last_rss_bytes = current.rss_bytes;
        _has_sample = true;
        rss_bytes = current.rss_bytes;
        return true;
    }

private:
    int _process_id;
    bool _has_previous = false;
    uint64_t _previous_at_ms = 0;
    ProcessSnapshot _previous = {};
    bool _has_sample = false;
    double _last_cpu_percent = 0;
    uint64_t _last_rss_bytes = 0;
};

void printProcessMetrics(ProcessSampler *sampler) {
    if (!sampler) {
        return;
    }

    double cpu_percent = 0;
    uint64_t rss_bytes = 0;
    if (!sampler->sample(cpu_percent, rss_bytes)) {
        cout << ", zlm_metrics=unavailable";
        return;
    }
    cout << ", zlm_cpu_pct=" << cpu_percent
         << ", zlm_rss_mb=" << rss_bytes / 1024 / 1024;
}

class CMD_main : public CMD {
public:
    CMD_main() {
        _parser.reset(new OptionParser(nullptr));

        (*_parser) << Option('l', "level", Option::ArgRequired, to_string(LDebug).data(), false,
                             "日志等级，LTrace~LError(0~4)", nullptr);
        (*_parser) << Option('t', "threads", Option::ArgRequired,
                             to_string(thread::hardware_concurrency()).data(), false,
                             "事件线程数", nullptr);
        (*_parser) << Option('i', "in", Option::ArgRequired, nullptr, true,
                             "回放 RTSP URL（认证信息使用标准 rtsp://user:password@host/... 形式）", nullptr);
        (*_parser) << Option('c', "count", Option::ArgRequired, "100", false,
                             "并发回放客户端数量", nullptr);
        (*_parser) << Option('d', "delay", Option::ArgRequired, "10", false,
                             "相邻客户端启动间隔，单位毫秒", nullptr);
        (*_parser) << Option('D', "duration", Option::ArgRequired, "60", false,
                             "所有客户端启动完成后的保持时长，单位秒；0 表示直到 Ctrl-C", nullptr);
        (*_parser) << Option('T', "rtp", Option::ArgRequired, to_string((int)Rtsp::RTP_TCP).data(), false,
                             "RTSP 传输方式：tcp/udp/multicast 对应 0/1/2", nullptr);
        (*_parser) << Option(0, "zlm-pid", Option::ArgRequired, "0", false,
                     "本机 ZLM 进程 PID；每秒输出其 CPU 占用和 RSS 内存，0 表示不采集", nullptr);
    }

    const char *description() const override {
        return "RTSP replay 并发回放压测";
    }
};

} // namespace

int main(int argc, char *argv[]) {
    CMD_main cmd_main;
    try {
        cmd_main(argc, argv);
    } catch (ExitException &) {
        return 0;
    } catch (const exception &ex) {
        cerr << ex.what() << endl;
        return -1;
    }

    const auto threads = cmd_main["threads"].as<int>();
    const auto log_level = static_cast<LogLevel>(MIN(MAX(cmd_main["level"].as<int>(), LTrace), LError));
    const auto input_url = cmd_main["in"].as<string>();
    const auto player_count = cmd_main["count"].as<int>();
    const auto delay_ms = cmd_main["delay"].as<int>();
    const auto duration_sec = cmd_main["duration"].as<int>();
    const auto rtp_type = cmd_main["rtp"].as<int>();
    const auto zlm_pid = cmd_main["zlm-pid"].as<int>();

    if (player_count <= 0 || delay_ms < 0 || duration_sec < 0 || zlm_pid < 0) {
        cerr << "count 必须大于 0，delay、duration 和 zlm-pid 不能为负数" << endl;
        return -1;
    }

    Logger::Instance().add(make_shared<ConsoleChannel>("ConsoleChannel", log_level));
    Logger::Instance().setWriter(make_shared<AsyncLogWriter>());
    EventPollerPool::setPoolSize(threads);
    WorkThreadPool::setPoolSize(threads);

    BenchmarkStats stats;
    atomic<bool> benchmark_stopping{false};
    vector<MediaPlayer::Ptr> players;
    players.reserve(player_count);
    unique_ptr<ProcessSampler> process_sampler;
    if (zlm_pid > 0) {
        process_sampler.reset(new ProcessSampler(zlm_pid));
    }

    InfoL << "replay benchmark: starting " << player_count << " clients"
          << ", delay_ms=" << delay_ms
          << ", duration_sec=" << duration_sec
          << ", rtp_type=" << rtp_type;

    for (int index = 0; index < player_count; ++index) {
        auto player = make_shared<MediaPlayer>();
        auto state = make_shared<PlayerState>();
        state->started_at_ms = getCurrentMillisecond();

        player->setOnCreateSocket([](const EventPoller::Ptr &poller) {
            return Socket::createSocket(poller, false);
        });
        player->setOnPlayResult([state, &stats, index](const SockException &ex) {
            if (state->play_result_seen.exchange(true)) {
                return;
            }
            if (ex) {
                ++stats.failed;
                WarnL << "replay benchmark: client=" << index << " play failed: " << ex;
                return;
            }

            const auto setup_ms = getCurrentMillisecond() - state->started_at_ms;
            state->playing = true;
            ++stats.succeeded;
            stats.setup_total_ms += setup_ms;
            updateMax(stats.setup_max_ms, setup_ms);
        });
        player->setOnShutdown([state, &stats, &benchmark_stopping, index](const SockException &ex) {
            if (!state->playing.exchange(false)) {
                return;
            }
            if (benchmark_stopping) {
                return;
            }
            ++stats.shutdown;
            WarnL << "replay benchmark: client=" << index << " disconnected: " << ex;
        });
        (*player)[Client::kBenchmarkMode] = true;
        (*player)[Client::kWaitTrackReady] = false;
        (*player)[Client::kRtpType] = rtp_type;

        players.emplace_back(player);
        player->play(input_url);

        if (delay_ms > 0) {
            this_thread::sleep_for(chrono::milliseconds(delay_ms));
        }
    }

    signal(SIGINT, onSignal);
    if (process_sampler) {
        double cpu_percent = 0;
        uint64_t rss_bytes = 0;
        if (!process_sampler->sample(cpu_percent, rss_bytes)) {
            cerr << "无法采集 zlm-pid=" << zlm_pid << "，请确认 ZLM 与 benchmark 在同一台主机且当前用户有读取该进程的权限" << endl;
        }
    }
    Ticker benchmark_ticker;
    while (!exit_flag && (duration_sec == 0 || benchmark_ticker.elapsedTime() < static_cast<uint64_t>(duration_sec) * 1000)) {
        this_thread::sleep_for(chrono::seconds(1));
        const auto succeeded = stats.succeeded.load();
        const auto failed = stats.failed.load();
        const auto pending = player_count - static_cast<int>(succeeded + failed);
        const auto average_setup_ms = succeeded == 0 ? 0 : stats.setup_total_ms.load() / succeeded;
        cout << "replay benchmark: total=" << player_count
             << ", succeeded=" << succeeded
             << ", failed=" << failed
             << ", pending=" << pending
             << ", disconnected=" << stats.shutdown.load()
             << ", setup_avg_ms=" << average_setup_ms
               << ", setup_max_ms=" << stats.setup_max_ms.load();
           printProcessMetrics(process_sampler.get());
           cout << endl;
    }

    benchmark_stopping = true;
    const auto succeeded = stats.succeeded.load();
    const auto average_setup_ms = succeeded == 0 ? 0 : stats.setup_total_ms.load() / succeeded;
    cout << "replay benchmark summary: total=" << player_count
         << ", succeeded=" << succeeded
         << ", failed=" << stats.failed.load()
         << ", pending=" << player_count - static_cast<int>(succeeded + stats.failed.load())
         << ", disconnected=" << stats.shutdown.load()
         << ", setup_avg_ms=" << average_setup_ms
            << ", setup_max_ms=" << stats.setup_max_ms.load();
        printProcessMetrics(process_sampler.get());
        cout << endl;

    players.clear();
    return stats.failed.load() == 0 && succeeded == static_cast<size_t>(player_count) ? 0 : 1;
}