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
#include <thread>
#include <vector>

namespace teamtalk
{

// Mixes a backing track (minus) or Windows WASAPI system audio loopback plus
// soundboard/jingle effects into the outgoing microphone stream and plays
// local monitoring (for files/SFX) to the singer's output device in real time.
//
// Mixing happens after the voice preprocessor (so Speex/WebRTC never see the
// backing track) and after the voice changer (so voice presets never affect
// the backing track or soundboard).
class KaraokeMixer : public soundsystem::StreamPlayer
{
public:
    static constexpr int OUT_SAMPLERATE = 48000;
    static constexpr int OUT_CHANNELS = 2;
    static constexpr int RING_FRAMES = OUT_SAMPLERATE * 4;     // 4 seconds
    static constexpr int SFX_RING_FRAMES = OUT_SAMPLERATE * 4; // 4 seconds
    static constexpr int PREROLL_FRAMES = OUT_SAMPLERATE / 10; // 100 ms
    static constexpr int PITCH_BUF_SIZE = 8192;

    enum BuiltInSfx : int
    {
        SFX_APPLAUSE = 0,
        SFX_DRUMROLL = 1,
        SFX_FANFARE = 2,
        SFX_AIRHORN = 3,
        SFX_BELL = 4,
        SFX_BUZZER = 5,
        SFX_COUNT = 6
    };

    KaraokeMixer();
    ~KaraokeMixer() override;

    // File backing track
    bool Start(const ACE_TString& filename, std::string& err);
    bool Start(const ACE_TString& filename, int outputdevid, int sndgrpid, std::string& err);

    // Windows WASAPI system/browser audio loopback capture (no local monitor echo)
    bool StartSystemLoopback(int outputdevid, int sndgrpid, std::string& err);
    bool IsLoopbackActive() const { return m_loopback_active.load(); }

    void Stop();

    bool IsPlaying() const { return m_playing.load(); }
    bool IsSoundboardPlaying() const { return m_sfx_playing.load(); }
    // True when either karaoke/loopback OR soundboard is actively producing audio
    bool IsOutputActive() const { return (m_playing.load() && !m_paused.load()) || m_sfx_playing.load(); }
    bool HasError() const { return m_error.load(); }

    // Pause / Resume backing track or loopback
    void SetPaused(bool paused);
    bool IsPaused() const { return m_paused.load(); }

    // 0..100, applied to the backing track / loopback only.
    void SetVolume(int volume);
    int GetVolume() const { return m_volume.load(); }

    // Auto-Ducking: attenuates backing track smoothly while singer speaks/sings
    void SetDucking(bool enable);
    bool IsDucking() const { return m_ducking.load(); }

    // Key / Pitch Shift (-6..+6 semitones) for backing track / loopback
    void SetPitchSemitones(int semitones);
    int GetPitchSemitones() const { return m_pitch_semitones.load(); }

    // Soundboard / Jingle Machine (overlays on voice + local monitor)
    bool TriggerBuiltInSfx(int sfxId, int outputdevid, int sndgrpid);
    bool PlaySoundboardFile(const ACE_TString& filename, int outputdevid, int sndgrpid, std::string& err);
    void StopSoundboard();

    // Called from the audio thread. Adds the backing track and/or soundboard
    // to 'ab.input_buffer' and sets ab.force_enc while audible.
    void Mix(media::AudioFrame& ab);

    // soundsystem::StreamPlayer callback for local speaker/headphone monitoring.
    bool StreamPlayerCb(const soundsystem::OutputStreamer& streamer,
                        short* buffer, int samples) override;

private:
    void OnAudio(const media::AudioFrame& frame);
    void OnStatus(const MediaFileProp& mfp, MediaStreamStatus status);
    void OnSfxAudio(const media::AudioFrame& frame);
    void OnSfxStatus(const MediaFileProp& mfp, MediaStreamStatus status);

    void PushTrackFrames(const short* interleaved, int frames, int chans);
    void PushSfxFrames(const short* interleaved, int frames, int chans);
    void ClearRing();
    void ClearSfxRing();
    void ResetPitchShifter();

    bool EnsureMonitorOpen(int outputdevid, int sndgrpid);
    void CloseMonitorIfUnused();
    void StopLoopbackThread();

    size_t AvailableLocked() const;
    short RingAtLocked(size_t base_read, size_t idx, int channel) const;
    short SfxRingAtLocked(size_t base_read, size_t idx, int channel) const;
    static float ReadPitchInterp(const std::vector<short>& buf, float pos);

    std::shared_ptr<MediaFileStreamer> m_streamer;
    std::shared_ptr<MediaFileStreamer> m_sfx_streamer;
    soundsystem::soundsystem_t m_soundsystem;
    std::mutex m_monitor_lock;
    std::atomic<bool> m_monitor_open{ false };
    std::atomic<bool> m_monitor_track{ false }; // false for WASAPI loopback so system audio doesn't echo

    // WASAPI loopback capture thread (Windows)
    std::thread m_loopback_thread;
    std::atomic<bool> m_loopback_stop{ false };
    std::atomic<bool> m_loopback_active{ false };

    // Main track ring buffer
    mutable std::mutex m_ring_lock;
    std::vector<short> m_ring;
    size_t m_read = 0;     // outgoing mix frame counter, monotonic
    size_t m_mon_read = 0; // local monitor frame counter, monotonic
    size_t m_write = 0;

    // Backing track pitch shifter state (runs in producer thread under m_pitch_lock)
    std::mutex m_pitch_lock;
    std::vector<short> m_pitch_buf_l;
    std::vector<short> m_pitch_buf_r;
    int m_pitch_write_pos = 0;
    float m_pitch_phase = 0.0f;

    // Soundboard / Jingle ring buffer
    mutable std::mutex m_sfx_lock;
    std::vector<short> m_sfx_ring;
    size_t m_sfx_read = 0;
    size_t m_sfx_mon_read = 0;
    size_t m_sfx_write = 0;
    double m_sfx_pos = 0.0;
    double m_sfx_mon_pos = 0.0;

    std::atomic<int> m_volume{ 75 };
    std::atomic<bool> m_playing{ false };
    std::atomic<bool> m_paused{ false };
    std::atomic<bool> m_ducking{ false };
    std::atomic<int> m_pitch_semitones{ 0 };
    std::atomic<int> m_duck_gain_milli{ 1000 }; // synced for local monitor
    std::atomic<bool> m_error{ false };
    std::atomic<bool> m_finished{ false };

    std::atomic<bool> m_sfx_playing{ false };
    std::atomic<bool> m_sfx_finished{ false };

    // Audio-thread state
    float m_duck_gain = 1.0f;
    bool m_prerolled = false;     // outgoing mix consumed for the first time
    double m_pos = 0.0;           // outgoing mix fractional read position
    bool m_mon_prerolled = false; // local monitor consumed for the first time
    double m_mon_pos = 0.0;       // local monitor fractional read position
    int m_loopback_hold_frames = 0;
};

} // namespace teamtalk

#endif
