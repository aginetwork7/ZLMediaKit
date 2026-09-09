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
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

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
    atomic<uint64_t> last_success_at_ms{0};
};

struct BenchmarkContext {
    BenchmarkStats stats;
    atomic<bool> stopping{false};
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

void printBenchmarkStatus(const char *phase, const BenchmarkContext &benchmark_context, int player_count) {
    const auto succeeded = benchmark_context.stats.succeeded.load();
    const auto failed = benchmark_context.stats.failed.load();
    const auto pending = player_count - static_cast<int>(succeeded + failed);
    const auto disconnected = benchmark_context.stats.shutdown.load();
    const auto active = succeeded > disconnected ? succeeded - disconnected : 0;
    const auto average_setup_ms = succeeded == 0 ? 0 : benchmark_context.stats.setup_total_ms.load() / succeeded;

    cout << "replay benchmark: phase=" << phase
         << ", total=" << player_count
         << ", succeeded=" << succeeded
         << ", failed=" << failed
         << ", pending=" << pending
         << ", active=" << active
         << ", disconnected=" << disconnected
         << ", setup_avg_ms=" << average_setup_ms
         << ", setup_max_ms=" << benchmark_context.stats.setup_max_ms.load();
    cout << endl;
}

class CMD_main : public CMD {
public:
    CMD_main() {
        _parser.reset(new OptionParser(nullptr));

        (*_parser) << Option('l', "level", Option::ArgRequired, to_string(LWarn).data(), false,
                             "日志等级，LTrace~LError(0~4)", nullptr);
        (*_parser) << Option('t', "threads", Option::ArgRequired, "0", false,
                     "客户端事件线程数；0 自动按每 64 路客户端至少分配一个 poller", nullptr);
        (*_parser) << Option('i', "in", Option::ArgRequired, nullptr, true,
                             "回放 RTSP URL（认证信息使用标准 rtsp://user:password@host/... 形式）", nullptr);
        (*_parser) << Option('c', "count", Option::ArgRequired, "100", false,
                             "并发回放客户端数量", nullptr);
        (*_parser) << Option('d', "delay", Option::ArgRequired, "100", false,
                     "相邻客户端启动间隔，单位毫秒；用于控制爬坡速率", nullptr);
        (*_parser) << Option('D', "duration", Option::ArgRequired, "360", false,
                     "预热完成后的稳态资源采样时长，单位秒；0 表示直到 Ctrl-C", nullptr);
        (*_parser) << Option(0, "warmup", Option::ArgRequired, "30", false,
                     "所有客户端完成建连后的预热时长，单位秒", nullptr);
        (*_parser) << Option(0, "media-duration", Option::ArgRequired, "600", false,
                     "单段录像可播放时长，单位秒；用于拒绝会在采样期间播放结束的参数组合", nullptr);
        (*_parser) << Option(0, "setup-timeout", Option::ArgRequired, "30000", false,
                     "RTSP 建连和播放响应超时，单位毫秒", nullptr);
        (*_parser) << Option('T', "rtp", Option::ArgRequired, to_string((int)Rtsp::RTP_TCP).data(), false,
                             "RTSP 传输方式：tcp/udp/multicast 对应 0/1/2", nullptr);
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

    const auto requested_threads = cmd_main["threads"].as<int>();
    const auto log_level = static_cast<LogLevel>(MIN(MAX(cmd_main["level"].as<int>(), LTrace), LError));
    auto input_url = cmd_main["in"].as<string>();
    const auto player_count = cmd_main["count"].as<int>();
    const auto delay_ms = cmd_main["delay"].as<int>();
    const auto duration_sec = cmd_main["duration"].as<int>();
    const auto warmup_sec = cmd_main["warmup"].as<int>();
    const auto media_duration_sec = cmd_main["media-duration"].as<int>();
    const auto setup_timeout_ms = cmd_main["setup-timeout"].as<int>();
    const auto rtp_type = cmd_main["rtp"].as<int>();

    if (requested_threads < 0 || player_count <= 0 || delay_ms < 0 || duration_sec < 0 || warmup_sec < 0 || media_duration_sec <= 0 || setup_timeout_ms <= 0) {
        cerr << "threads、delay、duration 和 warmup 不能为负数；count、media-duration 和 setup-timeout 必须大于 0" << endl;
        return -1;
    }

    constexpr int kClientsPerPoller = 64;
    auto threads = requested_threads;
    if (threads == 0) {
        const auto required_threads = (player_count + kClientsPerPoller - 1) / kClientsPerPoller;
        threads = MAX(static_cast<int>(thread::hardware_concurrency()), required_threads);
    }

    const auto ramp_ms = static_cast<uint64_t>(player_count - 1) * delay_ms;
    const auto playback_budget_ms = ramp_ms + setup_timeout_ms + static_cast<uint64_t>(warmup_sec + duration_sec) * 1000;
    if (duration_sec > 0 && playback_budget_ms > static_cast<uint64_t>(media_duration_sec) * 1000) {
        cerr << "录像时长不足：ramp(" << ramp_ms / 1000.0
             << "s) + setup-timeout(" << setup_timeout_ms / 1000.0
             << "s) + warmup(" << warmup_sec
             << "s) + duration(" << duration_sec
             << "s) 超过 media-duration(" << media_duration_sec << "s)" << endl;
        return -1;
    }

    Logger::Instance().add(make_shared<ConsoleChannel>("ConsoleChannel", log_level));
    Logger::Instance().setWriter(make_shared<AsyncLogWriter>());
    EventPollerPool::setPoolSize(threads);
    WorkThreadPool::setPoolSize(threads);

    auto benchmark_context = make_shared<BenchmarkContext>();
    vector<MediaPlayer::Ptr> players;
    players.reserve(player_count);

    InfoL << "replay benchmark: starting " << player_count << " clients"
            << ", client_threads=" << threads
          << ", delay_ms=" << delay_ms
          << ", warmup_sec=" << warmup_sec
          << ", duration_sec=" << duration_sec
          << ", media_duration_sec=" << media_duration_sec
          << ", setup_timeout_ms=" << setup_timeout_ms
          << ", rtp_type=" << rtp_type;

    Ticker ramp_ticker;
    uint64_t next_ramp_sample_ms = 1000;
    for (int index = 0; index < player_count; ++index) {
        auto player = make_shared<MediaPlayer>();
        auto state = make_shared<PlayerState>();
        state->started_at_ms = getCurrentMillisecond();

        player->setOnCreateSocket([](const EventPoller::Ptr &poller) {
            return Socket::createSocket(poller, false);
        });
        player->setOnPlayResult([state, benchmark_context, index](const SockException &ex) {
            if (state->play_result_seen.exchange(true)) {
                return;
            }
            if (ex) {
                ++benchmark_context->stats.failed;
                WarnL << "replay benchmark: client=" << index << " play failed: " << ex;
                return;
            }

            const auto setup_ms = getCurrentMillisecond() - state->started_at_ms;
            state->playing = true;
            ++benchmark_context->stats.succeeded;
            benchmark_context->stats.setup_total_ms += setup_ms;
            updateMax(benchmark_context->stats.setup_max_ms, setup_ms);
            benchmark_context->stats.last_success_at_ms = getCurrentMillisecond();
        });
        player->setOnShutdown([state, benchmark_context, index](const SockException &ex) {
            if (!state->playing.exchange(false)) {
                return;
            }
            if (benchmark_context->stopping) {
                return;
            }
            ++benchmark_context->stats.shutdown;
            WarnL << "replay benchmark: client=" << index << " disconnected: " << ex;
        });
        (*player)[Client::kBenchmarkMode] = true;
        (*player)[Client::kWaitTrackReady] = false;
        (*player)[Client::kTimeoutMS] = setup_timeout_ms;
        (*player)[Client::kRtpType] = rtp_type;

        players.emplace_back(player);
        player->play(input_url);

        if (delay_ms > 0 && index + 1 < player_count) {
            this_thread::sleep_for(chrono::milliseconds(delay_ms));
        }
        if (ramp_ticker.elapsedTime() >= next_ramp_sample_ms) {
            printBenchmarkStatus("ramp", *benchmark_context, player_count);
            next_ramp_sample_ms += 1000;
        }
    }

    signal(SIGINT, onSignal);
    Ticker setup_ticker;
    while (!exit_flag && benchmark_context->stats.succeeded.load() + benchmark_context->stats.failed.load() < static_cast<size_t>(player_count)
           && setup_ticker.elapsedTime() < static_cast<uint64_t>(setup_timeout_ms)) {
        this_thread::sleep_for(chrono::seconds(1));
        printBenchmarkStatus("settling", *benchmark_context, player_count);
    }

    const auto settled_succeeded = benchmark_context->stats.succeeded.load();
    const auto settled_failed = benchmark_context->stats.failed.load();
    if (!exit_flag && (settled_succeeded != static_cast<size_t>(player_count) || settled_failed != 0)) {
        cerr << "replay benchmark: setup incomplete, succeeded=" << settled_succeeded
             << ", failed=" << settled_failed
             << ", expected=" << player_count << endl;
        benchmark_context->stopping = true;
        players.clear();
        return 1;
    }

    const auto last_success_at_ms = benchmark_context->stats.last_success_at_ms.load();
    const auto warmup_end_at_ms = last_success_at_ms + static_cast<uint64_t>(warmup_sec) * 1000;
    cout << "replay benchmark: all clients connected, baseline_at_ms=" << last_success_at_ms
         << ", warmup_sec=" << warmup_sec << endl;
    while (!exit_flag && getCurrentMillisecond() < warmup_end_at_ms) {
        this_thread::sleep_for(chrono::seconds(1));
        printBenchmarkStatus("warmup", *benchmark_context, player_count);
    }

    Ticker benchmark_ticker;
    while (!exit_flag && (duration_sec == 0 || benchmark_ticker.elapsedTime() < static_cast<uint64_t>(duration_sec) * 1000)) {
        this_thread::sleep_for(chrono::seconds(1));
        printBenchmarkStatus("sample", *benchmark_context, player_count);
    }

    benchmark_context->stopping = true;
    const auto succeeded = benchmark_context->stats.succeeded.load();
    const auto disconnected = benchmark_context->stats.shutdown.load();
    const auto active = succeeded > disconnected ? succeeded - disconnected : 0;
    const auto average_setup_ms = succeeded == 0 ? 0 : benchmark_context->stats.setup_total_ms.load() / succeeded;
    cout << "replay benchmark summary: total=" << player_count
         << ", succeeded=" << succeeded
            << ", failed=" << benchmark_context->stats.failed.load()
            << ", pending=" << player_count - static_cast<int>(succeeded + benchmark_context->stats.failed.load())
         << ", active=" << active
         << ", disconnected=" << disconnected
         << ", setup_avg_ms=" << average_setup_ms
                << ", setup_max_ms=" << benchmark_context->stats.setup_max_ms.load();
        cout << endl;

    players.clear();
    return benchmark_context->stats.failed.load() == 0 && succeeded == static_cast<size_t>(player_count) ? 0 : 1;
}
