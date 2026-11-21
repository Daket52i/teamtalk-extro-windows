/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "KaraokeMixer.h"
#include "VoiceChanger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace teamtalk
{

namespace
{

inline short ClampAdd(short mic, float track)
{
    return ExtroVoiceChanger::ClampToShort(static_cast<float>(mic) + track);
}

} // namespace

KaraokeMixer::KaraokeMixer()
{
    // Allocate once, so the audio thread never waits for an allocation.
    m_ring.assign(RING_FRAMES * OUT_CHANNELS, 0);
    m_soundsystem = soundsystem::GetInstance();
}

KaraokeMixer::~KaraokeMixer()
{
    Stop();
}

void KaraokeMixer::ClearRing()
{
    std::lock_guard<std::mutex> g(m_ring_lock);
    std::fill(m_ring.begin(), m_ring.end(), 0);
    m_read = 0;
    m_mon_read = 0;
    m_write = 0;
}

size_t KaraokeMixer::AvailableLocked() const
{
    return m_write - m_read;
}

short KaraokeMixer::RingAtLocked(size_t base_read, size_t idx, int channel) const
{
    size_t pos = (base_read + idx) % RING_FRAMES;
    return m_ring[pos * OUT_CHANNELS + channel];
}

bool KaraokeMixer::Start(const ACE_TString& filename, std::string& err)
{
    return Start(filename, SOUNDDEVICE_IGNORE_ID, 0, err);
}

bool KaraokeMixer::Start(const ACE_TString& filename, int outputdevid, int sndgrpid, std::string& err)
{
    Stop();

    if (filename.empty())
    {
        err = "no file name";
        return false;
    }

    // Ask the decoder for 48 kHz stereo frames of 20 ms. FFmpeg inside
    // libavstream performs the resampling and channel conversion.
    MediaStreamOutput out(media::AudioFormat(OUT_SAMPLERATE, OUT_CHANNELS),
                          OUT_SAMPLERATE / 50);
    auto streamer = MakeMediaFileStreamer(filename, out);
    if (!streamer)
    {
        err = "unsupported media file";
        return false;
    }

    m_finished = false;
    m_error = false;
    m_prerolled = false;
    m_pos = 0.0;
    m_mon_prerolled = false;
    m_mon_pos = 0.0;
    ClearRing();

    streamer->RegisterAudioCallback([this](media::AudioFrame& frame, ACE_Message_Block*) {
        OnAudio(frame);
        return false; // let the streamer release the message block
    }, true);
    streamer->RegisterStatusCallback([this](const MediaFileProp& mfp, MediaStreamStatus status) {
        OnStatus(mfp, status);
    }, true);

    m_streamer = streamer;
    m_playing = true;

    if (!m_streamer->Open())
    {
        m_playing = false;
        m_streamer.reset();
        err = "cannot open media file";
        return false;
    }

    // Open local monitor output stream so the singer hears the backing track
    // in real time through their configured speakers/headphones.
    if (!m_soundsystem)
        m_soundsystem = soundsystem::GetInstance();
    if (m_soundsystem && outputdevid != SOUNDDEVICE_IGNORE_ID && sndgrpid != 0)
    {
        soundsystem::DeviceInfo out_dev;
        if (m_soundsystem->GetDevice(outputdevid, out_dev) && out_dev.default_samplerate > 0)
        {
            int out_chans = OUT_CHANNELS;
            int out_sr = OUT_SAMPLERATE;
            if (!out_dev.SupportsOutputFormat(out_chans, out_sr))
            {
                out_chans = out_dev.GetSupportedOutputChannels(OUT_CHANNELS);
                out_sr = out_dev.default_samplerate;
            }
            int const out_samples = std::max(1, (out_sr * 20) / 1000);
            if (m_soundsystem->OpenOutputStream(this, outputdevid, sndgrpid,
                                                out_sr, out_chans, out_samples))
            {
                if (m_soundsystem->StartStream(this))
                    m_monitor_open = true;
                else
                    m_soundsystem->CloseOutputStream(this);
            }
        }
    }

    if (!m_streamer->StartStream())
    {
        m_playing = false;
        if (m_monitor_open && m_soundsystem)
        {
            m_soundsystem->CloseOutputStream(this);
            m_monitor_open = false;
        }
        m_streamer.reset();
        err = "cannot start media file";
        return false;
    }

    return true;
}

void KaraokeMixer::Stop()
{
    m_playing = false;
    if (m_monitor_open && m_soundsystem)
    {
        m_soundsystem->CloseOutputStream(this);
        m_monitor_open = false;
    }
    m_streamer.reset();
    ClearRing();
    m_prerolled = false;
    m_pos = 0.0;
    m_mon_prerolled = false;
    m_mon_pos = 0.0;
    m_finished = false;
    m_error = false;
}

void KaraokeMixer::SetVolume(int volume)
{
    m_volume = std::max(0, std::min(100, volume));
}

void KaraokeMixer::OnStatus(const MediaFileProp& /*mfp*/, MediaStreamStatus status)
{
    if (status == MEDIASTREAM_FINISHED)
        m_finished = true;
    else if (status == MEDIASTREAM_ERROR)
    {
        m_error = true;
        m_playing = false;
    }
}

void KaraokeMixer::OnAudio(const media::AudioFrame& frame)
{
    if (!m_playing.load() || frame.input_buffer == nullptr || frame.input_samples <= 0)
        return;

    const int chans = frame.inputfmt.channels > 0 ? frame.inputfmt.channels : OUT_CHANNELS;
    const int frames = frame.input_samples;

    std::lock_guard<std::mutex> g(m_ring_lock);

    // Keep latency bounded if either consumer is inactive.
    size_t avail = m_write - m_read;
    if (avail + (size_t)frames > (size_t)RING_FRAMES)
        m_read += avail + (size_t)frames - (size_t)RING_FRAMES;

    size_t mon_avail = m_write - m_mon_read;
    if (mon_avail + (size_t)frames > (size_t)RING_FRAMES)
        m_mon_read += mon_avail + (size_t)frames - (size_t)RING_FRAMES;

    for (int i = 0; i < frames; ++i)
    {
        short l, r;
        if (chans >= 2)
        {
            l = frame.input_buffer[(size_t)i * chans];
            r = frame.input_buffer[(size_t)i * chans + 1];
        }
        else
            l = r = frame.input_buffer[i];

        size_t w = (m_write++) % RING_FRAMES;
        m_ring[w * OUT_CHANNELS] = l;
        m_ring[w * OUT_CHANNELS + 1] = r;
    }
}

void KaraokeMixer::Mix(media::AudioFrame& ab)
{
    if (!m_playing.load())
        return;

    const int n = ab.input_samples;
    const int chans = ab.inputfmt.channels;
    if (ab.input_buffer == nullptr || n <= 0 || chans <= 0)
        return;

    const int sr = ab.inputfmt.samplerate > 0 ? ab.inputfmt.samplerate : OUT_SAMPLERATE;
    const int vol = m_volume.load();
    // Apply 0.75 headroom at 100% volume so vocals can sit on top without clipping.
    const float gain = (static_cast<float>(vol) / 100.0f) * 0.75f;

    const double step = (double)OUT_SAMPLERATE / (double)sr;

    std::lock_guard<std::mutex> g(m_ring_lock);

    size_t avail = AvailableLocked();
    if (!m_prerolled)
    {
        // Wait for a small buffer so the first frames do not stutter.
        if (avail < (size_t)PREROLL_FRAMES)
            return;
        // If karaoke was started while not in a channel, snap to live position.
        if (avail > (size_t)(PREROLL_FRAMES * 4))
        {
            m_read = m_write - (size_t)PREROLL_FRAMES;
            avail = (size_t)PREROLL_FRAMES;
            m_pos = 0.0;
        }
        m_prerolled = true;
    }

    if (avail == 0)
    {
        if (m_finished.load())
        {
            m_playing = false;
            m_prerolled = false;
        }
        return;
    }

    bool mixed = false;
    double p = m_pos;

    for (int i = 0; i < n; ++i)
    {
        size_t i0 = (size_t)p;
        if (i0 + 1 >= avail)
            break;

        if (vol > 0)
        {
            double frac = p - (double)i0;
            int l0 = RingAtLocked(m_read, i0, 0), l1 = RingAtLocked(m_read, i0 + 1, 0);
            int r0 = RingAtLocked(m_read, i0, 1), r1 = RingAtLocked(m_read, i0 + 1, 1);

            float l = static_cast<float>(l0 + (l1 - l0) * frac) * gain;
            float r = static_cast<float>(r0 + (r1 - r0) * frac) * gain;

            short* out = ab.input_buffer + (size_t)i * chans;
            if (chans >= 2)
            {
                out[0] = ClampAdd(out[0], l);
                out[1] = ClampAdd(out[1], r);
            }
            else
            {
                out[0] = ClampAdd(out[0], (l + r) * 0.5f);
            }
        }

        mixed = true;
        p += step;
    }

    if (!mixed)
    {
        if (m_finished.load())
        {
            m_playing = false;
            m_prerolled = false;
        }
        return;
    }

    size_t consumed = (size_t)p;
    m_read += consumed;
    m_pos = p - (double)consumed;

    // The backing track must be transmitted even when the singer is silent.
    ab.force_enc = true;
}

bool KaraokeMixer::StreamPlayerCb(const soundsystem::OutputStreamer& streamer,
                                  short* buffer, int samples)
{
    const int chans = streamer.channels > 0 ? streamer.channels : OUT_CHANNELS;
    if (buffer == nullptr || samples <= 0)
        return true;

    std::memset(buffer, 0, (size_t)samples * chans * sizeof(short));

    if (!m_playing.load())
        return true;

    const int sr = streamer.samplerate > 0 ? streamer.samplerate : OUT_SAMPLERATE;
    const int vol = m_volume.load();
    const float gain = (static_cast<float>(vol) / 100.0f) * 0.75f;
    const double step = (double)OUT_SAMPLERATE / (double)sr;

    std::lock_guard<std::mutex> g(m_ring_lock);

    const size_t avail = m_write - m_mon_read;
    if (!m_mon_prerolled)
    {
        if (avail < (size_t)PREROLL_FRAMES)
            return true;
        m_mon_prerolled = true;
    }

    if (avail == 0)
    {
        if (m_finished.load())
        {
            m_playing = false;
            m_mon_prerolled = false;
        }
        return true;
    }

    bool mixed = false;
    double p = m_mon_pos;

    for (int i = 0; i < samples; ++i)
    {
        size_t i0 = (size_t)p;
        if (i0 + 1 >= avail)
            break;

        if (vol > 0)
        {
            double frac = p - (double)i0;
            int l0 = RingAtLocked(m_mon_read, i0, 0), l1 = RingAtLocked(m_mon_read, i0 + 1, 0);
            int r0 = RingAtLocked(m_mon_read, i0, 1), r1 = RingAtLocked(m_mon_read, i0 + 1, 1);

            float l = static_cast<float>(l0 + (l1 - l0) * frac) * gain;
            float r = static_cast<float>(r0 + (r1 - r0) * frac) * gain;

            short* out = buffer + (size_t)i * chans;
            if (chans >= 2)
            {
                out[0] = ExtroVoiceChanger::ClampToShort(l);
                out[1] = ExtroVoiceChanger::ClampToShort(r);
            }
            else
            {
                out[0] = ExtroVoiceChanger::ClampToShort((l + r) * 0.5f);
            }
        }

        mixed = true;
        p += step;
    }

    if (!mixed)
    {
        if (m_finished.load())
        {
            m_playing = false;
            m_mon_prerolled = false;
        }
        return true;
    }

    size_t consumed = (size_t)p;
    m_mon_read += consumed;
    m_mon_pos = p - (double)consumed;
    return true;
}

} // namespace teamtalk
