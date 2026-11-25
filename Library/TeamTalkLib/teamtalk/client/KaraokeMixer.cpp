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
#include <condition_variable>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

namespace teamtalk
{

namespace
{

constexpr double kTwoPi = 6.283185307179586;

inline short ClampAdd(short base, float add)
{
    return ExtroVoiceChanger::ClampToShort(static_cast<float>(base) + add);
}

inline float SfxRand(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (static_cast<float>(s & 0x00FFFFFFu) / 8388608.0f) - 1.0f;
}

#if defined(_WIN32)
const GUID kSubFormatIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};
const GUID kSubFormatPcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};
#endif

} // namespace

KaraokeMixer::KaraokeMixer()
{
    // Allocate once, so the audio thread never waits for an allocation.
    m_ring.assign(RING_FRAMES * OUT_CHANNELS, 0);
    m_sfx_ring.assign(SFX_RING_FRAMES * OUT_CHANNELS, 0);
    m_pitch_buf_l.assign(PITCH_BUF_SIZE, 0);
    m_pitch_buf_r.assign(PITCH_BUF_SIZE, 0);
    m_soundsystem = soundsystem::GetInstance();
}

KaraokeMixer::~KaraokeMixer()
{
    StopSoundboard();
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

void KaraokeMixer::ClearSfxRing()
{
    std::lock_guard<std::mutex> g(m_sfx_lock);
    std::fill(m_sfx_ring.begin(), m_sfx_ring.end(), 0);
    m_sfx_read = 0;
    m_sfx_mon_read = 0;
    m_sfx_write = 0;
    m_sfx_pos = 0.0;
    m_sfx_mon_pos = 0.0;
}

void KaraokeMixer::ResetPitchShifter()
{
    std::lock_guard<std::mutex> g(m_pitch_lock);
    std::fill(m_pitch_buf_l.begin(), m_pitch_buf_l.end(), 0);
    std::fill(m_pitch_buf_r.begin(), m_pitch_buf_r.end(), 0);
    m_pitch_write_pos = 0;
    m_pitch_phase = 0.0f;
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

short KaraokeMixer::SfxRingAtLocked(size_t base_read, size_t idx, int channel) const
{
    size_t pos = (base_read + idx) % SFX_RING_FRAMES;
    return m_sfx_ring[pos * OUT_CHANNELS + channel];
}

float KaraokeMixer::ReadPitchInterp(const std::vector<short>& buf, float pos)
{
    const double floored = std::floor(static_cast<double>(pos));
    int index = static_cast<int>(floored) % PITCH_BUF_SIZE;
    if (index < 0)
        index += PITCH_BUF_SIZE;
    const float frac = pos - static_cast<float>(floored);
    return (static_cast<float>(buf[(index + 1) % PITCH_BUF_SIZE]) * frac) +
           ((1.0f - frac) * static_cast<float>(buf[index]));
}

bool KaraokeMixer::EnsureMonitorOpen(int outputdevid, int sndgrpid)
{
    std::lock_guard<std::mutex> g(m_monitor_lock);
    if (m_monitor_open)
        return true;

    if (!m_soundsystem)
        m_soundsystem = soundsystem::GetInstance();
    if (!m_soundsystem || outputdevid == soundsystem::SOUND_DEVICEID_INVALID ||
        outputdevid < 0 || sndgrpid == 0)
    {
        return false;
    }

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
        int const out_samples = (std::max)(1, (out_sr * 20) / 1000);
        if (m_soundsystem->OpenOutputStream(this, outputdevid, sndgrpid,
                                            out_sr, out_chans, out_samples))
        {
            if (m_soundsystem->StartStream(this))
            {
                m_monitor_open = true;
                return true;
            }
            m_soundsystem->CloseOutputStream(this);
        }
    }
    return false;
}

void KaraokeMixer::CloseMonitorIfUnused()
{
    std::lock_guard<std::mutex> g(m_monitor_lock);
    if (!m_monitor_open)
        return;
    if ((m_playing.load() && m_monitor_track) || m_sfx_playing.load())
        return;

    if (m_soundsystem)
        m_soundsystem->CloseOutputStream(this);
    m_monitor_open = false;
}

bool KaraokeMixer::Start(const ACE_TString& filename, std::string& err)
{
    return Start(filename, soundsystem::SOUND_DEVICEID_INVALID, 0, err);
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
    m_paused = false;
    m_prerolled = false;
    m_pos = 0.0;
    m_mon_prerolled = false;
    m_mon_pos = 0.0;
    m_monitor_track = true;
    m_duck_gain = 1.0f;
    m_duck_gain_milli = 1000;
    ClearRing();
    ResetPitchShifter();

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
        m_monitor_track = false;
        m_streamer.reset();
        err = "cannot open media file";
        return false;
    }

    // Open local monitor output stream so the singer hears the backing track
    // in real time through their configured speakers/headphones.
    EnsureMonitorOpen(outputdevid, sndgrpid);

    if (!m_streamer->StartStream())
    {
        m_playing = false;
        m_monitor_track = false;
        CloseMonitorIfUnused();
        m_streamer.reset();
        err = "cannot start media file";
        return false;
    }

    return true;
}

void KaraokeMixer::StopLoopbackThread()
{
    m_loopback_stop = true;
    if (m_loopback_thread.joinable())
        m_loopback_thread.join();
    m_loopback_active = false;
}

bool KaraokeMixer::StartSystemLoopback(int /*outputdevid*/, int /*sndgrpid*/, std::string& err)
{
    Stop();

#if !defined(_WIN32)
    err = "WASAPI system audio loopback is only available on Windows";
    return false;
#else
    m_finished = false;
    m_error = false;
    m_paused = false;
    m_prerolled = false;
    m_pos = 0.0;
    m_mon_prerolled = false;
    m_mon_pos = 0.0;
    // IMPORTANT: Do NOT monitor loopback locally or system audio will echo!
    m_monitor_track = false;
    m_duck_gain = 1.0f;
    m_duck_gain_milli = 1000;
    m_loopback_hold_frames = 0;
    ClearRing();
    ResetPitchShifter();

    m_loopback_stop = false;

    std::mutex init_mtx;
    std::condition_variable init_cv;
    bool init_done = false;
    bool init_ok = false;
    std::string init_err;

    m_loopback_thread = std::thread([this, &init_mtx, &init_cv, &init_done, &init_ok, &init_err]() {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool needUninit = SUCCEEDED(hrCo);

        IMMDeviceEnumerator* pEnum = nullptr;
        IMMDevice* pDevice = nullptr;
        IAudioClient* pAudioClient = nullptr;
        IAudioCaptureClient* pCapture = nullptr;
        WAVEFORMATEX* pwfx = nullptr;

        auto signalInit = [&](bool ok, const std::string& msg) {
            std::lock_guard<std::mutex> lk(init_mtx);
            init_ok = ok;
            init_err = msg;
            init_done = true;
            init_cv.notify_one();
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
        if (FAILED(hr) || !pEnum)
        {
            signalInit(false, "WASAPI: failed to create MMDeviceEnumerator");
            if (needUninit) CoUninitialize();
            return;
        }

        hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr) || !pDevice)
        {
            pEnum->Release();
            signalInit(false, "WASAPI: no default render audio endpoint");
            if (needUninit) CoUninitialize();
            return;
        }

        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&pAudioClient));
        if (FAILED(hr) || !pAudioClient)
        {
            pDevice->Release();
            pEnum->Release();
            signalInit(false, "WASAPI: failed to activate IAudioClient");
            if (needUninit) CoUninitialize();
            return;
        }

        hr = pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr) || !pwfx)
        {
            pAudioClient->Release();
            pDevice->Release();
            pEnum->Release();
            signalInit(false, "WASAPI: failed to query mix format");
            if (needUninit) CoUninitialize();
            return;
        }

        // 100 ms buffer in 100-ns units
        const REFERENCE_TIME hnsBufDur = 1000000;
        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_LOOPBACK,
                                      hnsBufDur, 0, pwfx, nullptr);
        if (FAILED(hr))
        {
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnum->Release();
            signalInit(false, "WASAPI: failed to initialize loopback client");
            if (needUninit) CoUninitialize();
            return;
        }

        hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(&pCapture));
        if (FAILED(hr) || !pCapture)
        {
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnum->Release();
            signalInit(false, "WASAPI: failed to get IAudioCaptureClient");
            if (needUninit) CoUninitialize();
            return;
        }

        bool isFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        bool isPcm16 = (pwfx->wFormatTag == WAVE_FORMAT_PCM && pwfx->wBitsPerSample == 16);
        if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22)
        {
            const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
            if (std::memcmp(&ext->SubFormat, &kSubFormatIeeeFloat, sizeof(GUID)) == 0 &&
                pwfx->wBitsPerSample == 32)
            {
                isFloat = true;
            }
            else if (std::memcmp(&ext->SubFormat, &kSubFormatPcm, sizeof(GUID)) == 0 &&
                     pwfx->wBitsPerSample == 16)
            {
                isPcm16 = true;
            }
        }

        if (!isFloat && !isPcm16)
        {
            pCapture->Release();
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnum->Release();
            signalInit(false, "WASAPI: unsupported loopback sample format");
            if (needUninit) CoUninitialize();
            return;
        }

        hr = pAudioClient->Start();
        if (FAILED(hr))
        {
            pCapture->Release();
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnum->Release();
            signalInit(false, "WASAPI: failed to start loopback capture");
            if (needUninit) CoUninitialize();
            return;
        }

        const int inRate = static_cast<int>(pwfx->nSamplesPerSec);
        const int inChans = static_cast<int>(pwfx->nChannels);

        m_loopback_active = true;
        m_playing = true;
        signalInit(true, std::string());

        // Capture & resample loop (linear interpolation to 48 kHz stereo)
        std::vector<short> srcStereo;
        std::vector<short> resampled;
        double fracPos = 0.0;
        const double step = static_cast<double>(inRate) / static_cast<double>(OUT_SAMPLERATE);

        while (!m_loopback_stop.load())
        {
            Sleep(10);

            UINT32 packetFrames = 0;
            hr = pCapture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr))
                break;

            while (packetFrames > 0 && !m_loopback_stop.load())
            {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;
                hr = pCapture->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
                if (FAILED(hr))
                    break;

                if (numFrames > 0 && !m_paused.load())
                {
                    srcStereo.resize(static_cast<size_t>(numFrames) * 2);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || pData == nullptr)
                    {
                        std::fill(srcStereo.begin(), srcStereo.end(), 0);
                    }
                    else if (isFloat)
                    {
                        const float* fp = reinterpret_cast<const float*>(pData);
                        for (UINT32 i = 0; i < numFrames; ++i)
                        {
                            float l = fp[static_cast<size_t>(i) * inChans];
                            float r = (inChans >= 2) ? fp[static_cast<size_t>(i) * inChans + 1] : l;
                            srcStereo[static_cast<size_t>(i) * 2] =
                                ExtroVoiceChanger::ClampToShort(l * 32767.0f);
                            srcStereo[static_cast<size_t>(i) * 2 + 1] =
                                ExtroVoiceChanger::ClampToShort(r * 32767.0f);
                        }
                    }
                    else
                    {
                        const short* sp = reinterpret_cast<const short*>(pData);
                        for (UINT32 i = 0; i < numFrames; ++i)
                        {
                            short l = sp[static_cast<size_t>(i) * inChans];
                            short r = (inChans >= 2) ? sp[static_cast<size_t>(i) * inChans + 1] : l;
                            srcStereo[static_cast<size_t>(i) * 2] = l;
                            srcStereo[static_cast<size_t>(i) * 2 + 1] = r;
                        }
                    }

                    if (inRate == OUT_SAMPLERATE)
                    {
                        PushTrackFrames(srcStereo.data(), static_cast<int>(numFrames), 2);
                    }
                    else
                    {
                        resampled.clear();
                        double p = fracPos;
                        while (static_cast<UINT32>(p) + 1 < numFrames)
                        {
                            const size_t i0 = static_cast<size_t>(p);
                            const float f = static_cast<float>(p - static_cast<double>(i0));
                            const float l = static_cast<float>(srcStereo[i0 * 2]) * (1.0f - f) +
                                            static_cast<float>(srcStereo[(i0 + 1) * 2]) * f;
                            const float r = static_cast<float>(srcStereo[i0 * 2 + 1]) * (1.0f - f) +
                                            static_cast<float>(srcStereo[(i0 + 1) * 2 + 1]) * f;
                            resampled.push_back(ExtroVoiceChanger::ClampToShort(l));
                            resampled.push_back(ExtroVoiceChanger::ClampToShort(r));
                            p += step;
                        }
                        fracPos = p - static_cast<double>(numFrames);
                        if (fracPos < 0.0)
                            fracPos = 0.0;
                        if (!resampled.empty())
                            PushTrackFrames(resampled.data(), static_cast<int>(resampled.size() / 2), 2);
                    }
                }

                pCapture->ReleaseBuffer(numFrames);
                hr = pCapture->GetNextPacketSize(&packetFrames);
                if (FAILED(hr))
                    break;
            }
        }

        pAudioClient->Stop();
        pCapture->Release();
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pDevice->Release();
        pEnum->Release();
        if (needUninit) CoUninitialize();
    });

    {
        std::unique_lock<std::mutex> lk(init_mtx);
        init_cv.wait(lk, [&]() { return init_done; });
    }

    if (!init_ok)
    {
        StopLoopbackThread();
        err = init_err;
        return false;
    }

    return true;
#endif
}

void KaraokeMixer::Stop()
{
    m_playing = false;
    m_paused = false;
    m_monitor_track = false;
    StopLoopbackThread();
    m_streamer.reset();
    CloseMonitorIfUnused();
    ClearRing();
    ResetPitchShifter();
    m_prerolled = false;
    m_pos = 0.0;
    m_mon_prerolled = false;
    m_mon_pos = 0.0;
    m_finished = false;
    m_error = false;
    m_duck_gain = 1.0f;
    m_duck_gain_milli = 1000;
    m_loopback_hold_frames = 0;
}

void KaraokeMixer::SetPaused(bool paused)
{
    if (!m_playing.load())
        return;

    m_paused = paused;
    if (m_streamer)
    {
        if (paused)
            m_streamer->Pause();
        else
            m_streamer->StartStream();
    }
}

void KaraokeMixer::SetVolume(int volume)
{
    m_volume = (std::max)(0, (std::min)(100, volume));
}

void KaraokeMixer::SetDucking(bool enable)
{
    m_ducking = enable;
    if (!enable)
        m_duck_gain_milli = 1000;
}

void KaraokeMixer::SetPitchSemitones(int semitones)
{
    m_pitch_semitones = (std::max)(-6, (std::min)(6, semitones));
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

void KaraokeMixer::OnSfxStatus(const MediaFileProp& /*mfp*/, MediaStreamStatus status)
{
    if (status == MEDIASTREAM_FINISHED || status == MEDIASTREAM_ERROR)
        m_sfx_finished = true;
}

void KaraokeMixer::OnAudio(const media::AudioFrame& frame)
{
    if (!m_playing.load() || m_paused.load() || frame.input_buffer == nullptr || frame.input_samples <= 0)
        return;

    const int chans = frame.inputfmt.channels > 0 ? frame.inputfmt.channels : OUT_CHANNELS;
    PushTrackFrames(frame.input_buffer, frame.input_samples, chans);
}

void KaraokeMixer::OnSfxAudio(const media::AudioFrame& frame)
{
    if (!m_sfx_playing.load() || frame.input_buffer == nullptr || frame.input_samples <= 0)
        return;

    const int chans = frame.inputfmt.channels > 0 ? frame.inputfmt.channels : OUT_CHANNELS;
    PushSfxFrames(frame.input_buffer, frame.input_samples, chans);
}

void KaraokeMixer::PushTrackFrames(const short* interleaved, int frames, int chans)
{
    if (interleaved == nullptr || frames <= 0 || chans <= 0)
        return;

    const int semis = m_pitch_semitones.load();
    const bool shiftPitch = (semis != 0);
    const float factor = shiftPitch ? static_cast<float>(std::pow(2.0, static_cast<double>(semis) / 12.0)) : 1.0f;
    const int minDelay = 2048; // ~42.6 ms at 48 kHz
    const float delta = (1.0f - factor) / static_cast<float>(minDelay);

    std::lock_guard<std::mutex> gp(m_pitch_lock);
    std::lock_guard<std::mutex> g(m_ring_lock);

    // Keep latency bounded if either consumer is inactive.
    size_t avail = m_write - m_read;
    if (avail + static_cast<size_t>(frames) > static_cast<size_t>(RING_FRAMES))
        m_read += avail + static_cast<size_t>(frames) - static_cast<size_t>(RING_FRAMES);

    size_t mon_avail = m_write - m_mon_read;
    if (mon_avail + static_cast<size_t>(frames) > static_cast<size_t>(RING_FRAMES))
        m_mon_read += mon_avail + static_cast<size_t>(frames) - static_cast<size_t>(RING_FRAMES);

    for (int i = 0; i < frames; ++i)
    {
        short l, r;
        if (chans >= 2)
        {
            l = interleaved[static_cast<size_t>(i) * chans];
            r = interleaved[static_cast<size_t>(i) * chans + 1];
        }
        else
        {
            l = r = interleaved[i];
        }

        if (shiftPitch)
        {
            m_pitch_buf_l[m_pitch_write_pos] = l;
            m_pitch_buf_r[m_pitch_write_pos] = r;

            m_pitch_phase += delta;
            while (m_pitch_phase >= 1.0f) m_pitch_phase -= 1.0f;
            while (m_pitch_phase < 0.0f) m_pitch_phase += 1.0f;

            float phase2 = m_pitch_phase + 0.5f;
            if (phase2 >= 1.0f) phase2 -= 1.0f;

            float pos1 = static_cast<float>(m_pitch_write_pos) - m_pitch_phase * static_cast<float>(minDelay);
            while (pos1 < 0.0f) pos1 += static_cast<float>(PITCH_BUF_SIZE);
            float pos2 = static_cast<float>(m_pitch_write_pos) - phase2 * static_cast<float>(minDelay);
            while (pos2 < 0.0f) pos2 += static_cast<float>(PITCH_BUF_SIZE);

            const float w1 = (1.0f - static_cast<float>(std::cos(static_cast<double>(m_pitch_phase) * kTwoPi))) * 0.5f;
            const float w2 = 1.0f - w1;

            l = ExtroVoiceChanger::ClampToShort(ReadPitchInterp(m_pitch_buf_l, pos1) * w1 +
                                                ReadPitchInterp(m_pitch_buf_l, pos2) * w2);
            r = ExtroVoiceChanger::ClampToShort(ReadPitchInterp(m_pitch_buf_r, pos1) * w1 +
                                                ReadPitchInterp(m_pitch_buf_r, pos2) * w2);

            m_pitch_write_pos = (m_pitch_write_pos + 1) % PITCH_BUF_SIZE;
        }

        size_t w = (m_write++) % RING_FRAMES;
        m_ring[w * OUT_CHANNELS] = l;
        m_ring[w * OUT_CHANNELS + 1] = r;
    }
}

void KaraokeMixer::PushSfxFrames(const short* interleaved, int frames, int chans)
{
    if (interleaved == nullptr || frames <= 0 || chans <= 0)
        return;

    std::lock_guard<std::mutex> g(m_sfx_lock);

    size_t avail = m_sfx_write - m_sfx_read;
    if (avail + static_cast<size_t>(frames) > static_cast<size_t>(SFX_RING_FRAMES))
        m_sfx_read += avail + static_cast<size_t>(frames) - static_cast<size_t>(SFX_RING_FRAMES);

    size_t mon_avail = m_sfx_write - m_sfx_mon_read;
    if (mon_avail + static_cast<size_t>(frames) > static_cast<size_t>(SFX_RING_FRAMES))
        m_sfx_mon_read += mon_avail + static_cast<size_t>(frames) - static_cast<size_t>(SFX_RING_FRAMES);

    for (int i = 0; i < frames; ++i)
    {
        short l, r;
        if (chans >= 2)
        {
            l = interleaved[static_cast<size_t>(i) * chans];
            r = interleaved[static_cast<size_t>(i) * chans + 1];
        }
        else
        {
            l = r = interleaved[i];
        }

        size_t w = (m_sfx_write++) % SFX_RING_FRAMES;
        m_sfx_ring[w * OUT_CHANNELS] = l;
        m_sfx_ring[w * OUT_CHANNELS + 1] = r;
    }
}

bool KaraokeMixer::TriggerBuiltInSfx(int sfxId, int outputdevid, int sndgrpid)
{
    if (sfxId < 0 || sfxId >= SFX_COUNT)
        return false;

    StopSoundboard();

    const int sr = OUT_SAMPLERATE;
    std::vector<short> pcm;
    uint32_t rng = 0x13579BDFu ^ static_cast<uint32_t>(sfxId * 2654435761u);

    if (sfxId == SFX_APPLAUSE)
    {
        // ~2.0s multi-layered clap bursts with stereo spread
        const int total = static_cast<int>(sr * 2.0);
        pcm.assign(static_cast<size_t>(total) * 2, 0);
        float bpL = 0.0f, bpR = 0.0f;
        float lpL = 0.0f, lpR = 0.0f;
        const int numClaps = 95;
        std::vector<float> envL(total, 0.0f), envR(total, 0.0f);
        for (int c = 0; c < numClaps; ++c)
        {
            const float tStart = (static_cast<float>(c) / static_cast<float>(numClaps)) * 1.55f +
                                 (SfxRand(rng) * 0.04f + 0.04f);
            const int s0 = (std::max)(0, (std::min)(total - 1, static_cast<int>(tStart * sr)));
            const float pan = (SfxRand(rng) + 1.0f) * 0.5f;
            const float amp = 0.45f + (SfxRand(rng) + 1.0f) * 0.25f;
            const int len = (std::min)(total - s0, static_cast<int>(sr * 0.055));
            for (int k = 0; k < len; ++k)
            {
                const float e = amp * std::exp(-static_cast<float>(k) / (0.011f * sr));
                envL[s0 + k] += e * (1.0f - pan * 0.55f);
                envR[s0 + k] += e * (0.45f + pan * 0.55f);
            }
        }
        for (int i = 0; i < total; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(sr);
            const float fade = (t < 0.08f) ? (t / 0.08f) : ((t > 1.55f) ? (std::max)(0.0f, (2.0f - t) / 0.45f) : 1.0f);
            const float nL = SfxRand(rng);
            const float nR = SfxRand(rng);
            lpL += (nL - lpL) * 0.24f;
            bpL += (lpL - bpL) * 0.06f;
            lpR += (nR - lpR) * 0.24f;
            bpR += (lpR - bpR) * 0.06f;
            const float outL = (lpL - bpL) * envL[i] * fade * 11500.0f;
            const float outR = (lpR - bpR) * envR[i] * fade * 11500.0f;
            pcm[static_cast<size_t>(i) * 2] = ExtroVoiceChanger::ClampToShort(outL);
            pcm[static_cast<size_t>(i) * 2 + 1] = ExtroVoiceChanger::ClampToShort(outR);
        }
    }
    else if (sfxId == SFX_DRUMROLL)
    {
        // Ba-dum-tss (~1.3s)
        const int total = static_cast<int>(sr * 1.30);
        pcm.assign(static_cast<size_t>(total) * 2, 0);
        double ph1 = 0.0, ph2 = 0.0;
        float hpPrev = 0.0f;
        for (int i = 0; i < total; ++i)
        {
            const double t = static_cast<double>(i) / sr;
            float sig = 0.0f;
            // "Ba" at 0.00s
            if (t >= 0.0 && t < 0.26)
            {
                const double freq = 185.0 * std::exp(-t * 4.5) + 75.0;
                ph1 += kTwoPi * freq / sr;
                const double env = std::exp(-t * 14.0);
                sig += static_cast<float>((std::sin(ph1) * 0.85 + SfxRand(rng) * 0.22 * std::exp(-t * 35.0)) * env * 22000.0);
            }
            // "Dum" at 0.22s
            if (t >= 0.22 && t < 0.55)
            {
                const double lt = t - 0.22;
                const double freq = 145.0 * std::exp(-lt * 4.2) + 60.0;
                ph2 += kTwoPi * freq / sr;
                const double env = std::exp(-lt * 12.0);
                sig += static_cast<float>((std::sin(ph2) * 0.90 + SfxRand(rng) * 0.18 * std::exp(-lt * 30.0)) * env * 24000.0);
            }
            // "Tss" cymbal at 0.48s
            if (t >= 0.48)
            {
                const double lt = t - 0.48;
                const float n = SfxRand(rng);
                const float hp = n - hpPrev;
                hpPrev = n * 0.88f;
                const float shimmer = static_cast<float>(std::sin(kTwoPi * 3840.0 * lt) * 0.18 +
                                                         std::sin(kTwoPi * 5720.0 * lt) * 0.14);
                const float env = static_cast<float>(std::exp(-lt * 5.2));
                sig += (hp * 0.65f + shimmer) * env * 18500.0f;
            }
            const short s = ExtroVoiceChanger::ClampToShort(sig);
            pcm[static_cast<size_t>(i) * 2] = s;
            pcm[static_cast<size_t>(i) * 2 + 1] = s;
        }
    }
    else if (sfxId == SFX_FANFARE)
    {
        // Bright brass triad fanfare C5 - E5 - G5 - C6 (~1.55s)
        const int total = static_cast<int>(sr * 1.55);
        pcm.assign(static_cast<size_t>(total) * 2, 0);
        struct Note { double t0, dur, freq; };
        const Note notes[4] = {
            {0.00, 0.22, 523.25},
            {0.20, 0.22, 659.25},
            {0.40, 0.22, 783.99},
            {0.62, 0.90, 1046.50}
        };
        for (int i = 0; i < total; ++i)
        {
            const double t = static_cast<double>(i) / sr;
            double mix = 0.0;
            for (int n = 0; n < 4; ++n)
            {
                if (t >= notes[n].t0 && t < notes[n].t0 + notes[n].dur)
                {
                    const double lt = t - notes[n].t0;
                    const double atk = (std::min)(1.0, lt / 0.015);
                    const double rel = (std::min)(1.0, (notes[n].dur - lt) / 0.06);
                    const double f = notes[n].freq;
                    const double brass = std::sin(kTwoPi * f * lt) * 0.55 +
                                         std::sin(kTwoPi * 2.0 * f * lt) * 0.28 +
                                         std::sin(kTwoPi * 3.0 * f * lt) * 0.14 +
                                         std::sin(kTwoPi * 4.0 * f * lt) * 0.07;
                    mix += brass * atk * rel;
                }
            }
            const short s = ExtroVoiceChanger::ClampToShort(static_cast<float>(mix * 16500.0));
            pcm[static_cast<size_t>(i) * 2] = s;
            pcm[static_cast<size_t>(i) * 2 + 1] = s;
        }
    }
    else if (sfxId == SFX_AIRHORN)
    {
        // Classic DJ Airhorn (short-short-long blasts, ~1.35s)
        const int total = static_cast<int>(sr * 1.35);
        pcm.assign(static_cast<size_t>(total) * 2, 0);
        struct Blast { double t0, dur; };
        const Blast blasts[3] = {{0.00, 0.16}, {0.20, 0.16}, {0.42, 0.88}};
        const double freqs[4] = {392.0, 466.16, 587.33, 785.5};
        for (int i = 0; i < total; ++i)
        {
            const double t = static_cast<double>(i) / sr;
            double env = 0.0;
            for (int b = 0; b < 3; ++b)
            {
                if (t >= blasts[b].t0 && t < blasts[b].t0 + blasts[b].dur)
                {
                    const double lt = t - blasts[b].t0;
                    const double a = (std::min)(1.0, lt / 0.01);
                    const double r = (std::min)(1.0, (blasts[b].dur - lt) / 0.03);
                    env = a * r;
                    break;
                }
            }
            double sig = 0.0;
            if (env > 0.0)
            {
                const double vib = 1.0 + 0.006 * std::sin(kTwoPi * 7.5 * t);
                for (int k = 0; k < 4; ++k)
                {
                    const double phase = std::fmod(t * freqs[k] * vib, 1.0);
                    sig += (2.0 * phase - 1.0) * 0.25;
                }
                sig = std::tanh(sig * 2.2) * env;
            }
            const short s = ExtroVoiceChanger::ClampToShort(static_cast<float>(sig * 17500.0));
            pcm[static_cast<size_t>(i) * 2] = s;
            pcm[static_cast<size_t>(i) * 2 + 1] = s;
        }
    }
    else if (sfxId == SFX_BELL)
    {
        // Clear bell chime (~1.2s)
        const int total = static_cast<int>(sr * 1.20);
        pcm.assign(static_cast<size_t>(total) * 2, 0);
        const double f0 = 1046.5;
        for (int i = 0; i < total; ++i)
        {
            const double t = static_cast<double>(i) / sr;
            const double atk = (std::min)(1.0, t / 0.004);
            const double sig =
                std::sin(kTwoPi * f0 * t) * std::exp(-t * 3.5) * 0.65 +
                std::sin(kTwoPi * (f0 * 2.756) * t) * std::exp(-t * 7.5) * 0.25 +
                std::sin(kTwoPi * (f0 * 5.404) * t) * std::exp(-t * 14.0) * 0.12;
            const short s = ExtroVoiceChanger::ClampToShort(static_cast<float>(sig * atk * 21000.0));
            pcm[static_cast<size_t>(i) * 2] = s;
            pcm[static_cast<size_t>(i) * 2 + 1] = s;
        }
    }
    else if (sfxId == SFX_BUZZER)
    {
        // Game-show wrong-answer buzzer (~0.70s)
        const int total = static_cast<int>(sr * 0.70);
        pcm.assign(static_cast<size_t>(total) * 2, 0);
        for (int i = 0; i < total; ++i)
        {
            const double t = static_cast<double>(i) / sr;
            const double atk = (std::min)(1.0, t / 0.01);
            const double rel = (std::min)(1.0, (0.70 - t) / 0.05);
            const double s1 = (std::fmod(t * 110.0, 1.0) < 0.5) ? 1.0 : -1.0;
            const double s2 = (std::fmod(t * 116.54, 1.0) < 0.5) ? 1.0 : -1.0;
            const double sig = std::tanh((s1 + s2) * 0.85) * atk * rel;
            const short s = ExtroVoiceChanger::ClampToShort(static_cast<float>(sig * 15500.0));
            pcm[static_cast<size_t>(i) * 2] = s;
            pcm[static_cast<size_t>(i) * 2 + 1] = s;
        }
    }

    if (pcm.empty())
        return false;

    ClearSfxRing();
    PushSfxFrames(pcm.data(), static_cast<int>(pcm.size() / 2), 2);
    m_sfx_finished = true; // buffer already contains the entire clip
    m_sfx_playing = true;
    EnsureMonitorOpen(outputdevid, sndgrpid);
    return true;
}

bool KaraokeMixer::PlaySoundboardFile(const ACE_TString& filename, int outputdevid, int sndgrpid, std::string& err)
{
    StopSoundboard();

    if (filename.empty())
    {
        err = "no file name";
        return false;
    }

    MediaStreamOutput out(media::AudioFormat(OUT_SAMPLERATE, OUT_CHANNELS),
                          OUT_SAMPLERATE / 50);
    auto streamer = MakeMediaFileStreamer(filename, out);
    if (!streamer)
    {
        err = "unsupported audio file";
        return false;
    }

    ClearSfxRing();
    m_sfx_finished = false;

    streamer->RegisterAudioCallback([this](media::AudioFrame& frame, ACE_Message_Block*) {
        OnSfxAudio(frame);
        return false;
    }, true);
    streamer->RegisterStatusCallback([this](const MediaFileProp& mfp, MediaStreamStatus status) {
        OnSfxStatus(mfp, status);
    }, true);

    m_sfx_streamer = streamer;
    m_sfx_playing = true;

    if (!m_sfx_streamer->Open())
    {
        m_sfx_playing = false;
        m_sfx_streamer.reset();
        err = "cannot open audio file";
        return false;
    }

    EnsureMonitorOpen(outputdevid, sndgrpid);

    if (!m_sfx_streamer->StartStream())
    {
        m_sfx_playing = false;
        CloseMonitorIfUnused();
        m_sfx_streamer.reset();
        err = "cannot start audio file";
        return false;
    }

    return true;
}

void KaraokeMixer::StopSoundboard()
{
    m_sfx_playing = false;
    m_sfx_finished = false;
    m_sfx_streamer.reset();
    ClearSfxRing();
    CloseMonitorIfUnused();
}

void KaraokeMixer::Mix(media::AudioFrame& ab)
{
    const bool trackActive = m_playing.load() && !m_paused.load();
    const bool sfxActive = m_sfx_playing.load();
    if (!trackActive && !sfxActive)
        return;

    const int n = ab.input_samples;
    const int chans = ab.inputfmt.channels;
    if (ab.input_buffer == nullptr || n <= 0 || chans <= 0)
        return;

    const int sr = ab.inputfmt.samplerate > 0 ? ab.inputfmt.samplerate : OUT_SAMPLERATE;
    const double step = static_cast<double>(OUT_SAMPLERATE) / static_cast<double>(sr);

    // Compute microphone RMS BEFORE mixing backing track for Auto-Ducking.
    if (trackActive && m_ducking.load())
    {
        double sumSq = 0.0;
        const int total = n * chans;
        for (int i = 0; i < total; ++i)
        {
            const double v = static_cast<double>(ab.input_buffer[i]) / 32768.0;
            sumSq += v * v;
        }
        const float micRms = static_cast<float>(std::sqrt(sumSq / static_cast<double>(total)));
        const float frameSec = static_cast<float>(n) / static_cast<float>(sr);
        const bool speaking = (micRms > 0.018f);
        const float target = speaking ? 0.48f : 1.0f;
        const float tau = speaking ? 0.025f : 0.350f;
        const float alpha = 1.0f - std::exp(-frameSec / tau);
        m_duck_gain += (target - m_duck_gain) * alpha;
        m_duck_gain_milli.store(static_cast<int>(m_duck_gain * 1000.0f), std::memory_order_relaxed);
    }
    else
    {
        m_duck_gain = 1.0f;
        m_duck_gain_milli.store(1000, std::memory_order_relaxed);
    }

    // 1. Mix backing track / WASAPI loopback
    if (trackActive)
    {
        const int vol = m_volume.load();
        // Apply 0.75 headroom at 100% volume so vocals sit on top without clipping.
        const float gain = (static_cast<float>(vol) / 100.0f) * 0.75f * m_duck_gain;

        std::lock_guard<std::mutex> g(m_ring_lock);

        size_t avail = AvailableLocked();
        const size_t preroll = m_loopback_active.load() ? static_cast<size_t>(OUT_SAMPLERATE / 25) : static_cast<size_t>(PREROLL_FRAMES);
        if (!m_prerolled)
        {
            if (avail >= preroll)
            {
                if (avail > preroll * 4)
                {
                    m_read = m_write - preroll;
                    avail = preroll;
                    m_pos = 0.0;
                }
                m_prerolled = true;
            }
        }

        if (m_prerolled)
        {
            if (avail == 0)
            {
                if (m_finished.load())
                {
                    m_playing = false;
                    m_prerolled = false;
                }
            }
            else
            {
                bool mixed = false;
                float peak = 0.0f;
                double p = m_pos;

                for (int i = 0; i < n; ++i)
                {
                    size_t i0 = static_cast<size_t>(p);
                    if (i0 + 1 >= avail)
                        break;

                    if (vol > 0)
                    {
                        double frac = p - static_cast<double>(i0);
                        int l0 = RingAtLocked(m_read, i0, 0), l1 = RingAtLocked(m_read, i0 + 1, 0);
                        int r0 = RingAtLocked(m_read, i0, 1), r1 = RingAtLocked(m_read, i0 + 1, 1);

                        float l = static_cast<float>(l0 + (l1 - l0) * frac) * gain;
                        float r = static_cast<float>(r0 + (r1 - r0) * frac) * gain;
                        peak = (std::max)(peak, (std::max)(std::fabs(l), std::fabs(r)));

                        short* out = ab.input_buffer + static_cast<size_t>(i) * chans;
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
                }
                else
                {
                    size_t consumed = static_cast<size_t>(p);
                    m_read += consumed;
                    m_pos = p - static_cast<double>(consumed);

                    if (m_loopback_active.load())
                    {
                        if (peak > 8.0f)
                            m_loopback_hold_frames = sr / 2; // 500 ms hangover
                        else if (m_loopback_hold_frames > 0)
                            m_loopback_hold_frames = (std::max)(0, m_loopback_hold_frames - n);

                        if (m_loopback_hold_frames > 0)
                            ab.force_enc = true;
                    }
                    else
                    {
                        // Backing track must be transmitted even when the singer is silent.
                        ab.force_enc = true;
                    }
                }
            }
        }
    }

    // 2. Mix Soundboard / Jingle overlay
    if (sfxActive)
    {
        std::lock_guard<std::mutex> gs(m_sfx_lock);
        const size_t avail = m_sfx_write - m_sfx_read;
        if (avail == 0)
        {
            if (m_sfx_finished.load())
            {
                const size_t monAvail = m_sfx_write - m_sfx_mon_read;
                if (!m_monitor_open || monAvail == 0)
                    m_sfx_playing = false;
            }
        }
        else
        {
            bool mixedSfx = false;
            double p = m_sfx_pos;
            const float sfxGain = 0.85f;

            for (int i = 0; i < n; ++i)
            {
                size_t i0 = static_cast<size_t>(p);
                if (i0 + 1 >= avail)
                    break;

                double frac = p - static_cast<double>(i0);
                int l0 = SfxRingAtLocked(m_sfx_read, i0, 0), l1 = SfxRingAtLocked(m_sfx_read, i0 + 1, 0);
                int r0 = SfxRingAtLocked(m_sfx_read, i0, 1), r1 = SfxRingAtLocked(m_sfx_read, i0 + 1, 1);

                float l = static_cast<float>(l0 + (l1 - l0) * frac) * sfxGain;
                float r = static_cast<float>(r0 + (r1 - r0) * frac) * sfxGain;

                short* out = ab.input_buffer + static_cast<size_t>(i) * chans;
                if (chans >= 2)
                {
                    out[0] = ClampAdd(out[0], l);
                    out[1] = ClampAdd(out[1], r);
                }
                else
                {
                    out[0] = ClampAdd(out[0], (l + r) * 0.5f);
                }

                mixedSfx = true;
                p += step;
            }

            if (mixedSfx)
            {
                size_t consumed = static_cast<size_t>(p);
                m_sfx_read += consumed;
                m_sfx_pos = p - static_cast<double>(consumed);
                ab.force_enc = true;
            }
            else if (m_sfx_finished.load())
            {
                m_sfx_read = m_sfx_write;
                const size_t monAvail = m_sfx_write - m_sfx_mon_read;
                if (!m_monitor_open || monAvail == 0)
                    m_sfx_playing = false;
            }
        }
    }
}

bool KaraokeMixer::StreamPlayerCb(const soundsystem::OutputStreamer& streamer,
                                  short* buffer, int samples)
{
    const int chans = streamer.channels > 0 ? streamer.channels : OUT_CHANNELS;
    if (buffer == nullptr || samples <= 0)
        return true;

    std::memset(buffer, 0, static_cast<size_t>(samples) * chans * sizeof(short));

    const int sr = streamer.samplerate > 0 ? streamer.samplerate : OUT_SAMPLERATE;
    const double step = static_cast<double>(OUT_SAMPLERATE) / static_cast<double>(sr);

    // 1. Local monitor for file backing track (never for WASAPI loopback)
    if (m_playing.load() && !m_paused.load() && m_monitor_track)
    {
        const int vol = m_volume.load();
        const float duck = static_cast<float>(m_duck_gain_milli.load(std::memory_order_relaxed)) / 1000.0f;
        const float gain = (static_cast<float>(vol) / 100.0f) * 0.75f * duck;

        std::lock_guard<std::mutex> g(m_ring_lock);

        const size_t avail = m_write - m_mon_read;
        if (!m_mon_prerolled)
        {
            if (avail >= static_cast<size_t>(PREROLL_FRAMES))
                m_mon_prerolled = true;
        }

        if (m_mon_prerolled)
        {
            if (avail == 0)
            {
                if (m_finished.load())
                {
                    m_playing = false;
                    m_mon_prerolled = false;
                }
            }
            else
            {
                bool mixed = false;
                double p = m_mon_pos;

                for (int i = 0; i < samples; ++i)
                {
                    size_t i0 = static_cast<size_t>(p);
                    if (i0 + 1 >= avail)
                        break;

                    if (vol > 0)
                    {
                        double frac = p - static_cast<double>(i0);
                        int l0 = RingAtLocked(m_mon_read, i0, 0), l1 = RingAtLocked(m_mon_read, i0 + 1, 0);
                        int r0 = RingAtLocked(m_mon_read, i0, 1), r1 = RingAtLocked(m_mon_read, i0 + 1, 1);

                        float l = static_cast<float>(l0 + (l1 - l0) * frac) * gain;
                        float r = static_cast<float>(r0 + (r1 - r0) * frac) * gain;

                        short* out = buffer + static_cast<size_t>(i) * chans;
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
                }
                else
                {
                    size_t consumed = static_cast<size_t>(p);
                    m_mon_read += consumed;
                    m_mon_pos = p - static_cast<double>(consumed);
                }
            }
        }
    }

    // 2. Local monitor for Soundboard / Jingle
    if (m_sfx_playing.load())
    {
        std::lock_guard<std::mutex> gs(m_sfx_lock);
        const size_t avail = m_sfx_write - m_sfx_mon_read;
        if (avail == 0)
        {
            if (m_sfx_finished.load() && (m_sfx_write - m_sfx_read) == 0)
                m_sfx_playing = false;
        }
        else
        {
            bool mixedSfx = false;
            double p = m_sfx_mon_pos;
            const float sfxGain = 0.85f;

            for (int i = 0; i < samples; ++i)
            {
                size_t i0 = static_cast<size_t>(p);
                if (i0 + 1 >= avail)
                    break;

                double frac = p - static_cast<double>(i0);
                int l0 = SfxRingAtLocked(m_sfx_mon_read, i0, 0), l1 = SfxRingAtLocked(m_sfx_mon_read, i0 + 1, 0);
                int r0 = SfxRingAtLocked(m_sfx_mon_read, i0, 1), r1 = SfxRingAtLocked(m_sfx_mon_read, i0 + 1, 1);

                float l = static_cast<float>(l0 + (l1 - l0) * frac) * sfxGain;
                float r = static_cast<float>(r0 + (r1 - r0) * frac) * sfxGain;

                short* out = buffer + static_cast<size_t>(i) * chans;
                if (chans >= 2)
                {
                    out[0] = ClampAdd(out[0], l);
                    out[1] = ClampAdd(out[1], r);
                }
                else
                {
                    out[0] = ClampAdd(out[0], (l + r) * 0.5f);
                }

                mixedSfx = true;
                p += step;
            }

            if (mixedSfx)
            {
                size_t consumed = static_cast<size_t>(p);
                m_sfx_mon_read += consumed;
                m_sfx_mon_pos = p - static_cast<double>(consumed);
            }
            else if (m_sfx_finished.load())
            {
                m_sfx_mon_read = m_sfx_write;
                if ((m_sfx_write - m_sfx_read) == 0)
                    m_sfx_playing = false;
            }
        }
    }

    return true;
}

} // namespace teamtalk
