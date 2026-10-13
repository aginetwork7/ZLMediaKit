/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

// 回放读取正在录制/被截断的 fmp4 文件的一致性测试：
// 1. 一个线程用 MP4Muxer 写 fmp4，另一个线程反复打开同一文件并读完所有帧；
// 2. 对写完的文件逐个截断前缀、并把各 moof/traf/trun 的 size 置 0 后再打开。
// 要求：打开不抛异常，读出的帧必须是完整文件帧序列的前缀，且不出现无效帧。
// Consistency test for replay reading an fmp4 file that is still being written or truncated.
// usage: test_mp4_growing <source.mp4> [work_dir] [max_frames]

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include "Common/config.h"
#include "Record/MP4Demuxer.h"
#include "Record/MP4Muxer.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

#if defined(ENABLE_MP4)

namespace {

struct FrameKey {
    int type;
    uint64_t dts;
    uint64_t pts;
    size_t size;
    uint64_t hash;
    bool operator==(const FrameKey &that) const {
        return type == that.type && dts == that.dts && pts == that.pts && size == that.size && hash == that.hash;
    }
};

struct ReadResult {
    bool has_tracks = false;
    bool threw = false;
    bool bad_frame = false;
    string error;
    vector<FrameKey> frames;
};

uint64_t fnv1a(const char *data, size_t size) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ (uint8_t)data[i]) * 1099511628211ULL;
    }
    return h;
}

ReadResult readAll(const string &path) {
    ReadResult ret;
    MP4Demuxer demuxer;
    try {
        demuxer.openMP4(path);
    } catch (const std::exception &ex) {
        ret.threw = true;
        ret.error = ex.what();
        return ret;
    }
    ret.has_tracks = !demuxer.getTracks(false).empty();
    for (;;) {
        bool key = false, eof = false;
        auto frame = demuxer.readFrame(key, eof);
        if (eof) {
            break;
        }
        if (!frame) {
            ret.bad_frame = true;
            break;
        }
        ret.frames.push_back({ (int)frame->getTrackType(), frame->dts(), frame->pts(), frame->size(), fnv1a(frame->data(), frame->size()) });
    }
    return ret;
}

bool isPrefix(const vector<FrameKey> &part, const vector<FrameKey> &full) {
    if (part.size() > full.size()) {
        return false;
    }
    for (size_t i = 0; i < part.size(); ++i) {
        if (!(part[i] == full[i])) {
            return false;
        }
    }
    return true;
}

string loadFile(const string &path) {
    ifstream in(path, ios::binary);
    return string(istreambuf_iterator<char>(in), istreambuf_iterator<char>());
}

void saveFile(const string &path, const string &data) {
    ofstream out(path, ios::binary | ios::trunc);
    out.write(data.data(), data.size());
}

uint32_t rb32(const string &data, size_t offset) {
    return ((uint8_t)data[offset] << 24) | ((uint8_t)data[offset + 1] << 16) | ((uint8_t)data[offset + 2] << 8) | (uint8_t)data[offset + 3];
}

struct Box {
    size_t offset;
    size_t size;
    string type;
};

vector<Box> listBoxes(const string &data, size_t begin, size_t end) {
    vector<Box> boxes;
    while (begin + 8 <= end) {
        size_t size = rb32(data, begin);
        if (size < 8 || begin + size > end) {
            break;
        }
        boxes.push_back({ begin, size, data.substr(begin + 4, 4) });
        begin += size;
    }
    return boxes;
}

struct Stats {
    size_t opens = 0;
    size_t not_ready = 0;
    size_t failures = 0;
};

void check(Stats &stats, const string &what, const ReadResult &ret, const vector<FrameKey> &full) {
    ++stats.opens;
    if (!ret.has_tracks && !ret.threw) {
        ++stats.not_ready;
    }
    // 尚无完整 moov 时打开失败属于"未就绪"，只有 assert 异常算失败
    // Failing to open before moov is complete means "not ready"; only an assert exception is a failure
    auto asserted = ret.threw && ret.error.find("Assertion failed") != string::npos;
    if (ret.threw && !asserted) {
        ++stats.not_ready;
    }
    if (asserted || ret.bad_frame || !isPrefix(ret.frames, full)) {
        ++stats.failures;
        if (stats.failures <= 10) {
            cout << "FAIL " << what << ": threw=" << ret.threw << " bad_frame=" << ret.bad_frame
                 << " prefix=" << isPrefix(ret.frames, full) << " frames=" << ret.frames.size() << " err=" << ret.error << endl;
        }
    }
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cout << "usage: " << argv[0] << " <source.mp4> [work_dir] [max_frames]" << endl;
        return 1;
    }
    Logger::Instance().add(std::make_shared<ConsoleChannel>("console", LError));

    string source = argv[1];
    string work_dir = argc > 2 ? argv[2] : ".";
    size_t max_frames = argc > 3 ? atoi(argv[3]) : 450;
    string growing = work_dir + "/.growing.mp4";
    string cut = work_dir + "/cut.mp4";
    // 退出时（包括提前返回）删除临时文件  Remove the work files on every exit path
    struct Cleanup {
        string a, b;
        ~Cleanup() { remove(a.data()); remove(b.data()); }
    } cleanup { growing, cut };
    remove(growing.data());

    mINI::Instance()[Record::kEnableFmp4] = 1;
    mINI::Instance()[Record::kFastStart] = 0;

    MP4Demuxer src;
    src.openMP4(source);
    auto tracks = src.getTracks(false);
    vector<Frame::Ptr> input;
    for (bool key = false, eof = false; input.size() < max_frames;) {
        auto frame = src.readFrame(key, eof);
        if (eof) {
            break;
        }
        if (frame) {
            input.push_back(Frame::getCacheAbleFrame(frame));
        }
    }
    cout << "source frames: " << input.size() << endl;

    // 1. 边写边读  Read while writing
    Stats concurrent;
    vector<ReadResult> partial_reads;
    for (int round = 0; round < 3; ++round) {
        std::atomic<bool> done { false };
        auto muxer = std::make_shared<MP4Muxer>();
        muxer->openMP4(growing);
        for (auto &track : tracks) {
            muxer->addTrack(track);
        }
        muxer->addTrackCompleted();

        std::thread reader([&]() {
            while (!done) {
                partial_reads.push_back(readAll(growing));
            }
        });
        for (size_t i = 0; i < input.size(); ++i) {
            muxer->inputFrame(input[i]);
            std::this_thread::sleep_for(std::chrono::microseconds(200 + 100 * round));
        }
        muxer->closeMP4();
        done = true;
        reader.join();
    }
    auto full = readAll(growing);
    if (full.threw || full.bad_frame || full.frames.empty()) {
        cout << "FAIL reading complete file: " << full.error << endl;
        return 1;
    }
    {
        size_t backwards = 0;
        for (size_t i = 1; i < full.frames.size(); ++i) {
            if (full.frames[i].dts < full.frames[i - 1].dts) {
                ++backwards;
                if (backwards < 4) {
                    cout << "dts goes back at frame " << i << ": " << full.frames[i - 1].dts << " -> " << full.frames[i].dts << endl;
                }
            }
        }
        cout << "complete file: frames=" << full.frames.size() << " dts_backwards=" << backwards << endl;
        if (backwards) {
            cout << "RESULT: FAIL" << endl;
            return 1;
        }
    }
    // 解析中途触发 libmov assert（tkhd version 改成 2）时 reader 不能泄漏，需配合 leaks/LSan 检查
    // A libmov assert thrown mid-parse (tkhd version set to 2) must not leak the reader; check with leaks/LSan
    {
        auto copy = loadFile(growing);
        size_t asserted = 0, opens = 200;
        for (auto &moov : listBoxes(copy, 0, copy.size())) {
            if (moov.type != "moov") {
                continue;
            }
            for (auto &trak : listBoxes(copy, moov.offset + 8, moov.offset + moov.size)) {
                if (trak.type != "trak") {
                    continue;
                }
                for (auto &tkhd : listBoxes(copy, trak.offset + 8, trak.offset + trak.size)) {
                    if (tkhd.type == "tkhd") {
                        copy[tkhd.offset + 8] = 2;
                    }
                }
            }
        }
        saveFile(cut, copy);
        for (size_t i = 0; i < opens; ++i) {
            auto ret = readAll(cut);
            asserted += ret.threw && ret.error.find("Assertion failed") != string::npos;
        }
        cout << "assert during parse: opens=" << opens << " asserted=" << asserted << endl;
        if (asserted != opens) {
            cout << "FAIL: corrupted tkhd did not trigger the assert path" << endl;
            cout << "RESULT: FAIL" << endl;
            return 1;
        }
    }
    size_t max_seen = 0;
    size_t partial_hits = 0;
    for (auto &ret : partial_reads) {
        check(concurrent, "concurrent", ret, full.frames);
        max_seen = std::max(max_seen, ret.frames.size());
        partial_hits += !ret.frames.empty() && ret.frames.size() < full.frames.size();
    }
    cout << "concurrent: opens=" << concurrent.opens << " not_ready=" << concurrent.not_ready
         << " failures=" << concurrent.failures << " partial_reads=" << partial_hits << " max_frames_seen=" << max_seen << "/" << full.frames.size() << endl;

    // 2. 截断与占位 size  Truncation and placeholder sizes
    Stats truncated;
    auto data = loadFile(growing);
    auto root = listBoxes(data, 0, data.size());

    vector<size_t> cuts;
    for (size_t pos = 0; pos < data.size(); pos += 4093) {
        cuts.push_back(pos);
    }
    for (size_t i = 0; i < root.size(); ++i) {
        auto &box = root[i];
        for (size_t pos = box.offset; pos < box.offset + std::min<size_t>(box.size, 600); ++pos) {
            cuts.push_back(pos);
        }
        cuts.push_back(box.offset + box.size - 1);
        cuts.push_back(box.offset + box.size);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    size_t last_frames = 0, shrink_failures = 0;
    for (auto pos : cuts) {
        saveFile(cut, data.substr(0, pos));
        auto ret = readAll(cut);
        check(truncated, "cut@" + to_string(pos), ret, full.frames);
        // 文件变长后可读帧数不能减少  More bytes must never yield fewer frames
        if (ret.frames.size() < last_frames) {
            ++shrink_failures;
        }
        last_frames = ret.frames.size();
    }
    saveFile(cut, data);
    if (readAll(cut).frames.size() != full.frames.size()) {
        ++shrink_failures;
    }

    // 每个 mdat 缺最后一个字节时，该分片必须被整体丢弃
    // Missing only the last mdat byte must drop that whole fragment
    size_t boundary_failures = 0;
    for (auto &box : root) {
        if (box.type != "mdat") {
            continue;
        }
        saveFile(cut, data.substr(0, box.offset + box.size));
        auto whole = readAll(cut);
        saveFile(cut, data.substr(0, box.offset + box.size - 1));
        auto missing = readAll(cut);
        if (whole.frames.size() <= missing.frames.size()) {
            ++boundary_failures;
        }
    }

    // moof 及其子 box 的 size 被置 0（写端占位）时，从该分片起全部丢弃
    // A zero placeholder size in moof/traf/trun drops that fragment and everything after it
    size_t placeholder_failures = 0;
    for (auto &box : root) {
        if (box.type != "moof") {
            continue;
        }
        vector<size_t> size_fields { box.offset };
        for (auto &traf : listBoxes(data, box.offset + 8, box.offset + box.size)) {
            if (traf.type != "traf") {
                continue;
            }
            size_fields.push_back(traf.offset);
            for (auto &child : listBoxes(data, traf.offset + 8, traf.offset + traf.size)) {
                size_fields.push_back(child.offset);
            }
        }
        saveFile(cut, data.substr(0, box.offset));
        auto expected = readAll(cut).frames.size();
        for (auto field : size_fields) {
            auto copy = data;
            memset(&copy[field], 0, 4);
            saveFile(cut, copy);
            auto ret = readAll(cut);
            check(truncated, "zero-size@" + to_string(field), ret, full.frames);
            if (ret.frames.size() != expected) {
                ++placeholder_failures;
            }
        }
    }

    cout << "truncated: opens=" << truncated.opens << " not_ready=" << truncated.not_ready << " failures=" << truncated.failures
         << " shrink_failures=" << shrink_failures << " boundary_failures=" << boundary_failures
         << " placeholder_failures=" << placeholder_failures << endl;

    // 写端第一遍写 moof 时 trun data_offset 为 0，之后才重写；读到旧值的分片必须被丢弃
    // The writer's first pass writes trun data_offset = 0 and rewrites it later; a fragment read with the stale value must be dropped
    // 规范允许 moof 与 mdat 之间有 free/skip/uuid，不能因此截断文件
    // free/skip/uuid between a moof and its mdat are legal and must not truncate the file
    size_t stale_offset_failures = 0, filler_failures = 0;
    for (auto &box : root) {
        if (box.type != "moof") {
            continue;
        }
        vector<size_t> data_offset_fields;
        for (auto &traf : listBoxes(data, box.offset + 8, box.offset + box.size)) {
            if (traf.type != "traf") {
                continue;
            }
            for (auto &child : listBoxes(data, traf.offset + 8, traf.offset + traf.size)) {
                if (child.type == "trun" && (rb32(data, child.offset + 8) & 0x000001)) {
                    data_offset_fields.push_back(child.offset + 16);
                }
            }
        }
        if (data_offset_fields.empty()) {
            continue;
        }
        saveFile(cut, data.substr(0, box.offset));
        auto expected = readAll(cut).frames.size();

        auto stale = data;
        for (auto field : data_offset_fields) {
            memset(&stale[field], 0, 4);
        }
        saveFile(cut, stale);
        auto ret = readAll(cut);
        check(truncated, "stale-data-offset@" + to_string(box.offset), ret, full.frames);
        stale_offset_failures += ret.frames.size() != expected;

        // 在 moof 后插入 8 字节 free，并把该分片的 data_offset（相对 moof）加 8
        // Insert an 8-byte free box after the moof and add 8 to that fragment's moof-relative data_offset
        auto filler = data;
        for (auto field : data_offset_fields) {
            auto v = rb32(filler, field) + 8;
            filler[field] = (char)(v >> 24); filler[field + 1] = (char)(v >> 16); filler[field + 2] = (char)(v >> 8); filler[field + 3] = (char)v;
        }
        filler.insert(box.offset + box.size, string("\x00\x00\x00\x08" "free", 8));
        saveFile(cut, filler);
        ret = readAll(cut);
        check(truncated, "filler@" + to_string(box.offset), ret, full.frames);
        filler_failures += ret.frames.size() != full.frames.size();
    }
    cout << "fragments: stale_offset_failures=" << stale_offset_failures << " filler_failures=" << filler_failures << endl;

    // 损坏的 moof/traf/trun size（声明近 2GB）配合巨大的 sample_count：不能按声明大小预分配，读到文件末尾即停止并丢弃该分片
    // Corrupted moof/traf/trun sizes (declaring ~2GB) with a huge sample_count must not preallocate by the declared size;
    // parsing stops at end of file and that fragment is dropped
    size_t huge_trun_failures = 0;
    for (auto &box : root) {
        if (box.type != "moof") {
            continue;
        }
        size_t traf_at = 0, trun_at = 0;
        for (auto &traf : listBoxes(data, box.offset + 8, box.offset + box.size)) {
            if (traf.type != "traf") {
                continue;
            }
            for (auto &child : listBoxes(data, traf.offset + 8, traf.offset + traf.size)) {
                if (child.type == "trun" && !trun_at) {
                    traf_at = traf.offset;
                    trun_at = child.offset;
                }
            }
        }
        if (!trun_at) {
            continue;
        }
        saveFile(cut, data.substr(0, box.offset));
        auto expected = readAll(cut).frames.size();
        auto huge = data;
        auto put32 = [&](size_t at, uint32_t v) {
            huge[at] = (char)(v >> 24); huge[at + 1] = (char)(v >> 16); huge[at + 2] = (char)(v >> 8); huge[at + 3] = (char)v;
        };
        put32(box.offset, 0x7FFFFFF0);
        put32(traf_at, 0x7FFFFFF0 - (uint32_t)(traf_at - box.offset));
        put32(trun_at, 0x7FFFFFF0 - (uint32_t)(trun_at - box.offset));
        put32(trun_at + 12, 0x07FFFFFF); // sample_count, ~134M entries
        saveFile(cut, huge);
        auto ret = readAll(cut);
        check(truncated, "huge-trun@" + to_string(box.offset), ret, full.frames);
        huge_trun_failures += ret.frames.size() != expected;
        break; // one fragment is enough, each case parses the whole file tail
    }
    cout << "huge trun: failures=" << huge_trun_failures << endl;

    // 非分片 MP4（moov 在前）最后一个 mdat 的 size 为 0（延伸到文件末尾）必须能正常打开
    // A plain MP4 (moov first) whose last mdat has size 0 (runs to end of file) must still open
    size_t plain_failures = 0;
    {
        mINI::Instance()[Record::kEnableFmp4] = 0;
        mINI::Instance()[Record::kFastStart] = 1;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
        auto muxer = std::make_shared<MP4Muxer>();
        muxer->openMP4(cut);
        for (auto &track : tracks) {
            muxer->addTrack(track);
        }
        muxer->addTrackCompleted();
        for (auto &frame : input) {
            muxer->inputFrame(frame);
        }
        muxer->closeMP4();
        auto plain = loadFile(cut);
        auto expected = readAll(cut);
        auto boxes = listBoxes(plain, 0, plain.size());
        auto mdat = std::find_if(boxes.begin(), boxes.end(), [](const Box &b) { return b.type == "mdat"; });
        if (mdat == boxes.end() || expected.frames.empty()) {
            cout << "FAIL plain mp4 layout" << endl;
            ++plain_failures;
        } else {
            // 去掉 mdat 之后的 box，使 mdat 成为最后一个 box 再把 size 置 0
            // Drop the boxes after the mdat so it is the last one, then zero its size
            plain.resize(mdat->offset + mdat->size);
            memset(&plain[mdat->offset], 0, 4);
            saveFile(cut, plain);
            auto ret = readAll(cut);
            if (ret.threw || ret.bad_frame || ret.frames.size() != expected.frames.size()) {
                cout << "FAIL plain mp4 with size-0 mdat: threw=" << ret.threw << " frames=" << ret.frames.size() << "/" << expected.frames.size() << endl;
                ++plain_failures;
            }
        }
    }
    cout << "plain mp4: failures=" << plain_failures << endl;

    // 边写边读必须真的读到过"部分完成"的文件，否则这一段等于没测
    // Read-while-write must actually observe a partially written file, otherwise that section tested nothing
    if (!partial_hits) {
        cout << "FAIL concurrent: no partial read observed" << endl;
    }
    auto failed = concurrent.failures + truncated.failures + shrink_failures + boundary_failures + placeholder_failures
                  + stale_offset_failures + filler_failures + huge_trun_failures + plain_failures + (partial_hits ? 0 : 1);
    cout << (failed ? "RESULT: FAIL" : "RESULT: PASS") << endl;
    return failed ? 1 : 0;
}

#else
int main() {
    cout << "ENABLE_MP4 is off" << endl;
    return 0;
}
#endif
