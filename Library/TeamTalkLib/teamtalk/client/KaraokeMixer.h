/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#if !defined(KARAOKEMIXER_H)
#define KARAOKEMIXER_H

#include "avstream/MediaStreamer.h"
#include "avstream/SoundSystem.h"
#include "codec/MediaUtil.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace teamtalk
{

// Mixes a backing track (minus) into the outgoing microphone stream and
// plays it locally to the singer's output device in real time.
//
// The file is decoded and paced by MediaFileStreamer on its own thread; the
// decoded PCM is kept in a ring buffer which the capture thread drains inside
// AudioThread::ProcessAudioFrame. Mixing happens after the voice preprocessor,
// so Speex/WebRTC never see the backing track, and after the voice changer,
// so the preset never shifts the track's pitch.
class KaraokeMixer : public soundsystem::StreamPlayer
{
public:
    static constexpr int OUT_SAMPLERATE = 48000;
    static constexpr int OUT_CHANNELS = 2;
    static constexpr int RING_FRAMES = OUT_SAMPLERATE * 4;     // 4 seconds of audio
    static constexpr int PREROLL_FRAMES = OUT_SAMPLERATE / 10; // 100 ms

    KaraokeMixer();
    ~KaraokeMixer() override;

    // Returns false and fills 'err' when the file cannot be opened.
    bool Start(const ACE_TString& filename, std::string& err);
    bool Start(const ACE_TString& filename, int outputdevid, int sndgrpid, std::string& err);
    void Stop();

    bool IsPlaying() const { return m_playing; }
    bool HasError() const { return m_error; }

    // 0..100, applied to the backing track only.
    void SetVolume(int volume);
    int GetVolume() const { return m_volume.load(); }

    // Called from the audio thread. Adds the backing track to 'ab.input_buffer'
    // and forces the frame to be encoded while the track is audible.
    void Mix(media::AudioFrame& ab);

    // soundsystem::StreamPlayer callback for local speaker/headphone monitoring.
    bool StreamPlayerCb(const soundsystem::OutputStreamer& streamer,
                        short* buffer, int samples) override;

private:
    void OnAudio(const media::AudioFrame& frame);
    void OnStatus(const MediaFileProp& mfp, MediaStreamStatus status);
    void ClearRing();

    size_t AvailableLocked() const;
    short RingAtLocked(size_t base_read, size_t idx, int channel) const;

    std::shared_ptr<MediaFileStreamer> m_streamer;
    soundsystem::soundsystem_t m_soundsystem;
    bool m_monitor_open = false;

    mutable std::mutex m_ring_lock;
    std::vector<short> m_ring;
    size_t m_read = 0;     // outgoing mix frame counter, monotonic
    size_t m_mon_read = 0; // local monitor frame counter, monotonic
    size_t m_write = 0;

    std::atomic<int> m_volume{ 75 };
    std::atomic<bool> m_playing{ false };
    std::atomic<bool> m_error{ false };
    std::atomic<bool> m_finished{ false };

    bool m_prerolled = false;     // outgoing mix consumed for the first time
    double m_pos = 0.0;           // outgoing mix fractional read position
    bool m_mon_prerolled = false; // local monitor consumed for the first time
    double m_mon_pos = 0.0;       // local monitor fractional read position
};

} // namespace teamtalk

#endif
