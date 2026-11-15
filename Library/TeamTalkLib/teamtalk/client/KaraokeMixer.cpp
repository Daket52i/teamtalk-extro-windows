/*
 * Copyright (C) 2026, TeamTalk Extro
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "KaraokeMixer.h"

#include <algorithm>
#include <cmath>

namespace teamtalk
{

namespace
{

inline short ClampAdd(short mic, int track)
{
    int v = (int)mic + track;
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (short)v;
}

} // namespace

KaraokeMixer::KaraokeMixer()
{
    // Allocate once, so the audio thread never waits for an allocation.
    m_ring.assign(RING_FRAMES * OUT_CHANNELS, 0);
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
    m_write = 0;
}

size_t KaraokeMixer::AvailableLocked() const
{
    return m_write - m_read;
}

short KaraokeMixer::RingAtLocked(size_t idx, int channel) const
{
    size_t pos = (m_read + idx) % RING_FRAMES;
    return m_ring[pos * OUT_CHANNELS + channel];
}

bool KaraokeMixer::Start(const ACE_TString& filename, std::string& err)
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

    if (!m_streamer->StartStream())
    {
        m_playing = false;
        m_streamer.reset();
        err = "cannot start media file";
        return false;
    }

    return true;
}

void KaraokeMixer::Stop()
{
    m_playing = false;
    m_streamer.reset();
    ClearRing();
    m_prerolled = false;
    m_pos = 0.0;
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

    // Keep latency bounded: drop the oldest audio instead of overflowing.
    size_t avail = AvailableLocked();
    if (avail + (size_t)frames > (size_t)RING_FRAMES)
        m_read += avail + (size_t)frames - (size_t)RING_FRAMES;

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
    if (vol <= 0)
        return;

    const double step = (double)OUT_SAMPLERATE / (double)sr;

    std::lock_guard<std::mutex> g(m_ring_lock);

    const size_t avail = AvailableLocked();
    if (!m_prerolled)
    {
        // Wait for a small buffer so the first frames do not stutter.
        if (avail < (size_t)PREROLL_FRAMES)
            return;
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

        double frac = p - (double)i0;
        int l0 = RingAtLocked(i0, 0), l1 = RingAtLocked(i0 + 1, 0);
        int r0 = RingAtLocked(i0, 1), r1 = RingAtLocked(i0 + 1, 1);

        int l = (int)(l0 + (l1 - l0) * frac);
        int r = (int)(r0 + (r1 - r0) * frac);
        l = l * vol / 100;
        r = r * vol / 100;

        short* out = ab.input_buffer + (size_t)i * chans;
        if (chans >= 2)
        {
            out[0] = ClampAdd(out[0], l);
            out[1] = ClampAdd(out[1], r);
        }
        else
            out[0] = ClampAdd(out[0], (l + r) / 2);

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

} // namespace teamtalk
