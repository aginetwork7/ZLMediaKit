/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_MEDIASINK_H
#define ZLMEDIAKIT_MEDIASINK_H

#include <mutex>
#include <memory>
#include "Util/TimeTicker.h"
#include "Extension/Frame.h"
#include "Extension/Track.h"

namespace mediakit{

class TrackListener {
public:
    virtual ~TrackListener() = default;

    /**
     * 添加track，内部会调用Track的clone方法
     * 只会克隆sps pps这些信息 ，而不会克隆Delegate相关关系
     * @param track
     * Add track, internally calls the clone method of Track
     * Only clones sps pps information, not the Delegate relationship
     * @param track
     
     * [AUTO-TRANSLATED:ba6faf58]
     */
    virtual bool addTrack(const Track::Ptr & track) = 0;

    /**
     * 添加track完毕
     * Track added
     
     * [AUTO-TRANSLATED:dc70ddea]
     */
    virtual void addTrackCompleted() {};

    /**
     * 重置track
     * Reset track
     
     * [AUTO-TRANSLATED:95dc0b4f]
     */
    virtual void resetTracks() {};
};

class MediaSinkInterface : public FrameWriterInterface, public TrackListener {
public:
    using Ptr = std::shared_ptr<MediaSinkInterface>;
};

/**
 * aac静音音频添加器
 * AAC mute audio adder
 
 * [AUTO-TRANSLATED:aa154f71]
 */
class MuteAudioMaker : public FrameDispatcher {
public:
    using Ptr = std::shared_ptr<MuteAudioMaker>;
    bool inputFrame(const Frame::Ptr &frame) override;

private:
    int _track_index = -1;
    uint64_t _audio_idx = 0;
};

/**
 * 该类的作用是等待Track ready()返回true也就是就绪后再通知派生类进行下一步的操作
 * 目的是输入Frame前由Track截取处理下，以便获取有效的信息（譬如sps pps aa_cfg）
 * The role of this class is to wait for Track ready() to return true, that is, ready, and then notify the derived class to perform the next operation.
 * The purpose is to intercept and process the input Frame by Track before inputting the Frame, so as to obtain valid information (such as sps pps aa_cfg)
 
 * [AUTO-TRANSLATED:9e4f096b]
 */
class MediaSink : public MediaSinkInterface, public TrackSource{
public:
    using Ptr = std::shared_ptr<MediaSink>;
    /**
     * 输入frame
     * @param frame
     * Input frame
     * @param frame
     
     * [AUTO-TRANSLATED:7aaa5bba]
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * 添加track，内部会调用Track的clone方法
     * 只会克隆sps pps这些信息 ，而不会克隆Delegate相关关系
     * @param track
     * Add track, internally calls the clone method of Track
     * Only clones sps pps information, not the Delegate relationship
     * @param track
     
     * [AUTO-TRANSLATED:ba6faf58]
     */
    bool addTrack(const Track::Ptr & track) override;

    /**
     * 添加Track完毕，如果是单Track，会最多等待3秒才会触发onAllTrackReady
     * 这样会增加生成流的延时，如果添加了音视频双Track，那么可以不调用此方法
     * 否则为了降低流注册延时，请手动调用此方法
     * Track added, if it is a single Track, it will wait for a maximum of 3 seconds before triggering onAllTrackReady
     * This will increase the delay in generating the stream. If you add both audio and video tracks, you can skip this method.
     * Otherwise, to reduce the stream registration delay, please call this method manually.
     
     * [AUTO-TRANSLATED:580b6163]
     */
    void addTrackCompleted() override;

    /**
     * 设置最大track数，取值范围>=1；该方法与addTrackCompleted类型；
     * 在设置单track时，可以加快媒体注册速度
     * Set the maximum number of tracks, the value range is >=1; this method is of the addTrackCompleted type;
     * When setting a single track, it can speed up media registration
     
     * [AUTO-TRANSLATED:cd521c6f]
     */
    void setMaxTrackCount(size_t i);

    /**
     * 重置track
     * Reset track
     
     * [AUTO-TRANSLATED:95dc0b4f]
     */
    void resetTracks() override;

    /**
     * 获取所有Track
     * @param trackReady 是否获取已经准备好的Track
     * Get all Tracks
     * @param trackReady Whether to get the ready Track
     
     * [AUTO-TRANSLATED:32032e47]
     */
    std::vector<Track::Ptr> getTracks(bool trackReady = true) const override;

    /**
     * 判断是否已经触发onAllTrackReady事件
     * Determine whether the onAllTrackReady event has been triggered
     
     * [AUTO-TRANSLATED:fb8b4c71]
     */
    bool isAllTrackReady() const;

    /**
     * 设置是否开启音频
     * Set whether to enable audio
     
     * [AUTO-TRANSLATED:0e9a3ef0]
     */
    void enableAudio(bool flag);

    /**
     * 设置单音频
     * Set single audio
     
     * [AUTO-TRANSLATED:48fc734a]
     */
    void setOnlyAudio();

    /**
     * 设置是否开启添加静音音频
     * Set whether to enable adding mute audio
     
     * [AUTO-TRANSLATED:49efef10]
     */
    void enableMuteAudio(bool flag);

    /**
     * 是否有视频track
     * Whether there is a video track
     
     * [AUTO-TRANSLATED:4c4d651d]
     */
    bool haveVideo() const;

protected:
    /**
     * 某track已经准备好，其ready()状态返回true，
     * 此时代表可以获取其例如sps pps等相关信息了
     * @param track
     * A certain track is ready, its ready() status returns true,
     * This means that you can get its related information such as sps pps
     * @param track
     
     * [AUTO-TRANSLATED:720dedc1]
     */
    virtual bool onTrackReady(const Track::Ptr & track) { return false; };

    /**
     * 所有Track已经准备好，
     * All Tracks are ready,
     
     * [AUTO-TRANSLATED:c54d02e2]
     */
    virtual void onAllTrackReady() {};

    /**
     * 某Track输出frame，在onAllTrackReady触发后才会调用此方法
     * @param frame
     * A certain Track outputs a frame, this method will be called only after onAllTrackReady is triggered
     * @param frame
     
     * [AUTO-TRANSLATED:debbd247]
     */
    virtual bool onTrackFrame(const Frame::Ptr &frame) { return false; };

    /**
     * 日志里标识这个 sink 的名字（例如 app/stream），默认为空
     * Name identifying this sink in logs (e.g. app/stream), empty by default
     */
    virtual std::string sinkName() const { return ""; }

private:
    /**
     * 触发onAllTrackReady事件
     * Trigger the onAllTrackReady event
     
     * [AUTO-TRANSLATED:068fdb61]
     */
    void emitAllTrackReady();

    /**
     * 检查track是否准备完毕
     * Check if the track is ready
     
     * [AUTO-TRANSLATED:12e7c3e6]
     */
    void checkTrackIfReady();
    void onAllTrackReady_l();
    /**
     * 记录音频时间戳是否前进；就绪后时间戳停住时限频告警
     * Track whether the audio timestamp advances; warn (rate limited) when it stalls after all tracks are ready
     */
    void checkAudioStamp(const Frame::Ptr &frame);
    /**
     * 就绪前音频时间戳一直不前进时移除音频 track，返回是否移除
     * Remove the audio track when its timestamp never advanced before all tracks are ready; returns whether removed
     */
    bool dropFrozenAudio(const Track::Ptr &track);
    /**
     * 添加aac静音轨道
     * Add AAC mute track
     
     * [AUTO-TRANSLATED:9ba052b5]
     */
    bool addMuteAudioTrack();

private:
    bool _audio_add = false;
    bool _have_video = false;
    bool _enable_audio = true;
    bool _only_audio = false;
    bool _add_mute_audio = true;
    bool _all_track_ready = false;
    size_t _max_track_size = 2;

    toolkit::Ticker _ticker;
    MuteAudioMaker::Ptr _mute_audio_maker;

    std::unordered_map<int, toolkit::List<Frame::Ptr> > _frame_unread;
    std::unordered_map<int, std::function<void()> > _track_ready_callback;
    std::unordered_map<int, std::pair<Track::Ptr, bool/*got frame*/> > _track_map;

    // 音频时间戳检查：有的摄像头音频 RTP 时间戳恒为 0（RTCP SR 里 rtp_stamp 也是 0），
    // 这样的音频播放器放不出声音，录像里时间戳全是 0 还会拖住回放定位
    // Audio stamp check: some cameras send audio RTP timestamps that are always 0 (rtp_stamp 0 in RTCP SR as well);
    // players cannot play such audio, and all-zero stamps in recordings break replay positioning
    struct AudioStamp {
        uint64_t last_dts = 0;
        size_t frames = 0;   // 收到的音频帧数 / audio frames received
        size_t stalled = 0;  // 时间戳连续未前进的帧数 / consecutive frames whose stamp did not advance
        bool advanced = false;
        toolkit::Ticker first_frame;  // 从第一帧起计时 / since the first frame
        toolkit::Ticker warn;         // 告警限频 / warning rate limit
        bool warned = false;
    };
    std::unordered_map<int, AudioStamp> _audio_stamp;
};


class MediaSinkDelegate : public MediaSink {
public:
    /**
     * 设置track监听器
     * Set track listener
     
     * [AUTO-TRANSLATED:cedc97d7]
     */
    void setTrackListener(TrackListener *listener);

protected:
    void resetTracks() override;
    bool onTrackReady(const Track::Ptr & track) override;
    void onAllTrackReady() override;

private:
    TrackListener *_listener = nullptr;
};

class Demuxer : protected TrackListener, public TrackSource {
public:
    void setTrackListener(TrackListener *listener, bool wait_track_ready = false);
    std::vector<Track::Ptr> getTracks(bool trackReady = true) const override;

protected:
    bool addTrack(const Track::Ptr &track) override;
    void addTrackCompleted() override;
    void resetTracks() override;

private:
    MediaSink::Ptr _sink;
    TrackListener *_listener = nullptr;
    std::vector<Track::Ptr> _origin_track;
};

}//namespace mediakit

#endif //ZLMEDIAKIT_MEDIASINK_H
