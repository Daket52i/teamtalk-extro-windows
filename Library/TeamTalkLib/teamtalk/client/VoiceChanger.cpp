/*
 * TeamTalk Extro voice changer (24 modes: classic + Auto-Tune + Studio Rack).
 *
 * Modes 0..12 preserve the exact DSP coefficients and tuning tables of the
 * Android reference; modes 13..23 add real-time F0 Auto-Tune (Pop & Hard),
 * Studio Voice Rack, Concert Hall, Demon, Helium, Female, Male,
 * Walkie-Talkie, Whisper/ASMR, and Broadcast DJ.
 */

#include "VoiceChanger.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace
{

const int kCombTuningsL[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
const int kAllpassTuningsL[4] = {556, 441, 341, 225};
constexpr double kTwoPi = 6.283185307179586;

std::atomic<int> g_vc_mode{ teamtalk::OFF };
std::atomic<int> g_vc_echo{ 1 };
std::atomic<int> g_vc_echo_level{ 35 };
std::atomic<int> g_vc_reverb{ 1 };
std::atomic<int> g_vc_reverb_level{ 50 };
std::atomic<int> g_vc_room_size{ 60 };
std::atomic<int> g_vc_autotune_scale{ teamtalk::AUTOTUNE_SCALE_CHROMATIC };

inline float NextWhiteNoise(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    // Map to [-1.0, 1.0]
    return (static_cast<float>(state & 0x00FFFFFFu) / 8388608.0f) - 1.0f;
}

} // namespace

namespace teamtalk
{

ExtroVoiceChanger::ExtroVoiceChanger()
    : m_mode(OFF)
    , m_echo_enabled(1)
    , m_echo_level(35)
    , m_reverb_enabled(1)
    , m_reverb_level(50)
    , m_reverb_room_size(60)
    , m_autotune_scale(AUTOTUNE_SCALE_CHROMATIC)
    , m_pitch_write_pos(0)
    , m_pitch_phase(0.0f)
    , m_at_write_pos(0)
    , m_at_filled(0)
    , m_at_ratio(1.0f)
    , m_at_target_ratio(1.0f)
    , m_echo_index(0)
    , m_echo_filter_l(0.0f)
    , m_echo_filter_r(0.0f)
    , m_chorus_index(0)
    , m_lfo_phase(0.0)
    , m_monster_phase(0.0)
    , m_robot_phase(0.0)
    , m_filter_x1_l(0.0)
    , m_filter_x1_r(0.0)
    , m_filter_x2_l(0.0)
    , m_filter_x2_r(0.0)
    , m_filter_y1_l(0.0)
    , m_filter_y1_r(0.0)
    , m_filter_y2_l(0.0)
    , m_filter_y2_r(0.0)
    , m_eq_lp_l(0.0f)
    , m_eq_lp_r(0.0f)
    , m_eq_hp_l(0.0f)
    , m_eq_hp_r(0.0f)
    , m_gate_env(0.0f)
    , m_comp_env(0.0f)
    , m_whisper_env(0.0f)
    , m_whisper_noise_lp(0.0f)
    , m_prng_state(0x9E3779B9u)
{
    m_pitch_buf_l.assign(kPitchBufferSize, static_cast<short>(0));
    m_pitch_buf_r.assign(kPitchBufferSize, static_cast<short>(0));

    m_at_buf.assign(kAutoTuneBufSize, 0.0f);
    m_at_win.assign(kAutoTuneBufSize, 0.0f);

    m_echo_buf_l.assign(kMaxEchoSamples, 0.0f);
    m_echo_buf_r.assign(kMaxEchoSamples, 0.0f);

    m_comb_buf_l.assign(kNumCombs * kMaxCombSize, 0.0f);
    m_comb_buf_r.assign(kNumCombs * kMaxCombSize, 0.0f);
    m_comb_index_l.assign(kNumCombs, 0);
    m_comb_index_r.assign(kNumCombs, 0);
    m_comb_filter_l.assign(kNumCombs, 0.0f);
    m_comb_filter_r.assign(kNumCombs, 0.0f);

    m_allpass_buf_l.assign(kNumAllpass * kMaxAllpassSize, 0.0f);
    m_allpass_buf_r.assign(kNumAllpass * kMaxAllpassSize, 0.0f);
    m_allpass_index_l.assign(kNumAllpass, 0);
    m_allpass_index_r.assign(kNumAllpass, 0);

    m_chorus_buf_l.assign(kChorusMaxSamples, 0.0f);
    m_chorus_buf_r.assign(kChorusMaxSamples, 0.0f);

    m_scratch_l.assign(kMaxProcessSamples, static_cast<short>(0));
    m_scratch_r.assign(kMaxProcessSamples, static_cast<short>(0));
}

void ExtroVoiceChanger::SetMode(int mode)
{
    if (mode < static_cast<int>(OFF) || mode > static_cast<int>(VOICE_MODE_MAX))
        mode = OFF;

    m_mode.store(mode, std::memory_order_relaxed);
    g_vc_mode.store(mode, std::memory_order_relaxed);
    Reset();
}

int ExtroVoiceChanger::GetMode() const
{
    return m_mode.load(std::memory_order_relaxed);
}

void ExtroVoiceChanger::SetEchoReverb(bool echo, int echoLevel, bool reverb,
                                      int reverbLevel, int roomSize)
{
    const int e = echo ? 1 : 0;
    const int el = (std::max)(0, (std::min)(100, echoLevel));
    const int r = reverb ? 1 : 0;
    const int rl = (std::max)(0, (std::min)(100, reverbLevel));
    const int rs = (std::max)(0, (std::min)(100, roomSize));

    m_echo_enabled.store(e, std::memory_order_relaxed);
    m_echo_level.store(el, std::memory_order_relaxed);
    m_reverb_enabled.store(r, std::memory_order_relaxed);
    m_reverb_level.store(rl, std::memory_order_relaxed);
    m_reverb_room_size.store(rs, std::memory_order_relaxed);

    g_vc_echo.store(e, std::memory_order_relaxed);
    g_vc_echo_level.store(el, std::memory_order_relaxed);
    g_vc_reverb.store(r, std::memory_order_relaxed);
    g_vc_reverb_level.store(rl, std::memory_order_relaxed);
    g_vc_room_size.store(rs, std::memory_order_relaxed);
}

void ExtroVoiceChanger::SetAutoTuneScale(int scale)
{
    if (scale < static_cast<int>(AUTOTUNE_SCALE_CHROMATIC) ||
        scale > static_cast<int>(AUTOTUNE_SCALE_MINOR))
    {
        scale = AUTOTUNE_SCALE_CHROMATIC;
    }
    m_autotune_scale.store(scale, std::memory_order_relaxed);
    g_vc_autotune_scale.store(scale, std::memory_order_relaxed);
}

int ExtroVoiceChanger::GetAutoTuneScale() const
{
    return m_autotune_scale.load(std::memory_order_relaxed);
}

void ExtroVoiceChanger::SetGlobalSettings(const VoiceChangerSettings& settings)
{
    int mode = settings.mode;
    if (mode < static_cast<int>(OFF) || mode > static_cast<int>(VOICE_MODE_MAX))
        mode = OFF;
    int scale = settings.autotune_scale;
    if (scale < static_cast<int>(AUTOTUNE_SCALE_CHROMATIC) ||
        scale > static_cast<int>(AUTOTUNE_SCALE_MINOR))
    {
        scale = AUTOTUNE_SCALE_CHROMATIC;
    }
    g_vc_mode.store(mode, std::memory_order_relaxed);
    g_vc_echo.store(settings.echo ? 1 : 0, std::memory_order_relaxed);
    g_vc_echo_level.store((std::max)(0, (std::min)(100, settings.echo_level)), std::memory_order_relaxed);
    g_vc_reverb.store(settings.reverb ? 1 : 0, std::memory_order_relaxed);
    g_vc_reverb_level.store((std::max)(0, (std::min)(100, settings.reverb_level)), std::memory_order_relaxed);
    g_vc_room_size.store((std::max)(0, (std::min)(100, settings.room_size)), std::memory_order_relaxed);
    g_vc_autotune_scale.store(scale, std::memory_order_relaxed);
}

VoiceChangerSettings ExtroVoiceChanger::GetGlobalSettings()
{
    VoiceChangerSettings s;
    s.mode = g_vc_mode.load(std::memory_order_relaxed);
    s.echo = g_vc_echo.load(std::memory_order_relaxed) != 0;
    s.echo_level = g_vc_echo_level.load(std::memory_order_relaxed);
    s.reverb = g_vc_reverb.load(std::memory_order_relaxed) != 0;
    s.reverb_level = g_vc_reverb_level.load(std::memory_order_relaxed);
    s.room_size = g_vc_room_size.load(std::memory_order_relaxed);
    s.autotune_scale = g_vc_autotune_scale.load(std::memory_order_relaxed);
    return s;
}

short ExtroVoiceChanger::ClampToShort(float f)
{
    if (f > 28000.0f)
    {
        const float excess = f - 28000.0f;
        f = (excess / (2.1276595E-4f * excess + 1.0f)) + 28000.0f;
        return static_cast<short>(f + 0.5f);
    }

    if (f < -28000.0f)
    {
        const float excess = (-f) - 28000.0f;
        f = -((excess / (2.1276595E-4f * excess + 1.0f)) + 28000.0f);
        return static_cast<short>(f - 0.5f);
    }

    return static_cast<short>(f >= 0.0f ? f + 0.5f : f - 0.5f);
}

double ExtroVoiceChanger::SoftSaturate(double d)
{
    if (d > 1.0)
        return 0.85;
    if (d < -1.0)
        return -0.85;

    return d - (((d * d) * d) / 4.0);
}

int ExtroVoiceChanger::JavaRound(float v)
{
    // Java Math.round(float) == (int)Math.floor(v + 0.5f).
    return static_cast<int>(std::floor(v + 0.5f));
}

float ExtroVoiceChanger::ReadInterpolated(const short* buf, float pos)
{
    const double floored = std::floor(static_cast<double>(pos));
    int index = static_cast<int>(floored) % kPitchBufferSize;
    if (index < 0)
        index += kPitchBufferSize;

    const float frac = pos - static_cast<float>(floored);
    return (static_cast<float>(buf[(index + 1) % kPitchBufferSize]) * frac) +
           ((1.0f - frac) * static_cast<float>(buf[index]));
}

void ExtroVoiceChanger::ApplyPitchShift(short* left, short* right,
                                        int numSamples, float factor,
                                        int samplerate)
{
    const int minDelay =
        (std::min)(2048, (std::max)(512, (samplerate * 42) / 1000));
    const float delta = (1.0f - factor) / static_cast<float>(minDelay);

    for (int i = 0; i < numSamples; ++i)
    {
        const int writePos = m_pitch_write_pos;
        m_pitch_buf_l[writePos] = left[i];
        if (right != nullptr)
            m_pitch_buf_r[writePos] = right[i];

        m_pitch_phase += delta;
        while (m_pitch_phase >= 1.0f)
            m_pitch_phase -= 1.0f;
        while (m_pitch_phase < 0.0f)
            m_pitch_phase += 1.0f;

        float phase2 = m_pitch_phase + 0.5f;
        if (phase2 >= 1.0f)
            phase2 -= 1.0f;

        const float delay1 = m_pitch_phase * static_cast<float>(minDelay);
        const float delay2 = phase2 * static_cast<float>(minDelay);

        float pos1 = std::fmod(static_cast<float>(m_pitch_write_pos) - delay1 +
                                   static_cast<float>(kPitchBufferSize),
                               static_cast<float>(kPitchBufferSize));
        while (pos1 < 0.0f)
            pos1 += static_cast<float>(kPitchBufferSize);

        float pos2 = std::fmod(static_cast<float>(m_pitch_write_pos) - delay2 +
                                   static_cast<float>(kPitchBufferSize),
                               static_cast<float>(kPitchBufferSize));
        while (pos2 < 0.0f)
            pos2 += static_cast<float>(kPitchBufferSize);

        const float read1 = ReadInterpolated(m_pitch_buf_l.data(), pos1);
        const float read2 = ReadInterpolated(m_pitch_buf_l.data(), pos2);
        const float cosine =
            (1.0f - static_cast<float>(
                        std::cos(static_cast<double>(m_pitch_phase) * kTwoPi))) *
            0.5f;
        const float inv = 1.0f - cosine;

        left[i] = ClampToShort((read2 * inv) + (read1 * cosine));

        if (right != nullptr)
        {
            right[i] = ClampToShort(
                (ReadInterpolated(m_pitch_buf_r.data(), pos2) * inv) +
                (ReadInterpolated(m_pitch_buf_r.data(), pos1) * cosine));
        }

        m_pitch_write_pos = (m_pitch_write_pos + 1) % kPitchBufferSize;
    }
}

void ExtroVoiceChanger::ApplyAlienEffect(short* left, short* right,
                                         int numSamples, int samplerate)
{
    ApplyPitchShift(left, right, numSamples, 1.15f, samplerate);

    const double delta = 75.39822368615503 / samplerate;
    for (int i = 0; i < numSamples; ++i)
    {
        const double gain =
            ((std::sin(m_lfo_phase) * 0.5) + 0.5) * 0.65 + 0.35;
        m_lfo_phase += delta;
        if (m_lfo_phase > kTwoPi)
            m_lfo_phase -= kTwoPi;

        left[i] = ClampToShort(static_cast<float>(left[i] * gain));
        if (right != nullptr)
            right[i] = ClampToShort(static_cast<float>(right[i] * gain));
    }
}

void ExtroVoiceChanger::ApplyChorusMix(short* left, short* right,
                                       int numSamples, int samplerate,
                                       float wetMix)
{
    const double delta = 5.340707511102648 / samplerate;
    const float sampleRate = static_cast<float>(samplerate);
    const int delay = JavaRound(0.02f * sampleRate);
    const int sweep = JavaRound(sampleRate * 0.006f);
    const float dryMix = 1.0f - wetMix;

    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = static_cast<float>(left[i]);
        const float inR = right != nullptr ? static_cast<float>(right[i]) : inL;

        const int idx = m_chorus_index;
        m_chorus_buf_l[idx] = inL;
        m_chorus_buf_r[idx] = inR;

        const float inner = static_cast<float>(
            delay + (std::sin(m_lfo_phase) * static_cast<double>(sweep)));
        float pos = std::fmod(static_cast<float>(m_chorus_index) - inner +
                                  2048.0f,
                              2048.0f);
        if (pos < 0.0f)
            pos += 2048.0f;

        const int i7 = static_cast<int>(pos);
        const int i8 = (i7 + 1) % 2048;
        const float frac = pos - static_cast<float>(i7);
        const float inv = 1.0f - frac;

        const float outL = (m_chorus_buf_l[i8] * frac) +
                           (m_chorus_buf_l[i7] * inv);
        const float outR = (m_chorus_buf_r[i8] * frac) +
                           (m_chorus_buf_r[i7] * inv);

        left[i] = ClampToShort((outL * wetMix) + (inL * dryMix));
        if (right != nullptr)
            right[i] = ClampToShort((outR * wetMix) + (inR * dryMix));

        m_chorus_index = (m_chorus_index + 1) % 2048;
        m_lfo_phase += delta;
        if (m_lfo_phase > kTwoPi)
            m_lfo_phase -= kTwoPi;
    }
}

void ExtroVoiceChanger::ApplyChorusEffect(short* left, short* right,
                                          int numSamples, int samplerate)
{
    ApplyChorusMix(left, right, numSamples, samplerate, 0.4f);
}

void ExtroVoiceChanger::ApplyEchoAndReverbEffect(short* left, short* right,
                                                 int numSamples,
                                                 int samplerate)
{
    int echoLevel = m_echo_level.load(std::memory_order_relaxed);
    int reverbLevel = m_reverb_level.load(std::memory_order_relaxed);
    int roomSize = m_reverb_room_size.load(std::memory_order_relaxed);

    bool echoOn =
        m_echo_enabled.load(std::memory_order_relaxed) != 0 && echoLevel > 0;
    bool reverbOn =
        m_reverb_enabled.load(std::memory_order_relaxed) != 0 &&
        reverbLevel > 0;

    // When the user explicitly selects the ECHO preset mode, guarantee an
    // audible echo + reverb effect even if the caller passed zeroed flags.
    if (!echoOn && !reverbOn)
    {
        if (m_mode.load(std::memory_order_relaxed) == ECHO)
        {
            echoOn = true;
            reverbOn = true;
            if (echoLevel <= 0)
                echoLevel = 35;
            if (reverbLevel <= 0)
                reverbLevel = 50;
            if (roomSize <= 0)
                roomSize = 60;
        }
        else
        {
            return;
        }
    }

    ApplyEchoAndReverbCore(left, right, numSamples, samplerate,
                           echoOn, echoLevel, reverbOn, reverbLevel, roomSize);
}

void ExtroVoiceChanger::ApplyEchoAndReverbCore(short* left, short* right,
                                               int numSamples, int samplerate,
                                               bool echoOn, int echoLevel,
                                               bool reverbOn, int reverbLevel,
                                               int roomSize)
{
    if (!echoOn && !reverbOn)
        return;

    const bool stereo = right != nullptr;

    const float reverbMixLevel = static_cast<float>(reverbLevel) / 100.0f;
    const float reverbMix = reverbMixLevel * reverbMixLevel;
    const float room = static_cast<float>(roomSize) / 100.0f;
    const float roomScale = (0.55f * room) + 0.72f;
    const float combFeedback =
        (static_cast<float>(std::pow(static_cast<double>(room), 1.2)) *
         0.27f) +
        0.7f;
    const float rateScale =
        (static_cast<float>(samplerate) / 44100.0f) * roomScale;

    int combTuningsL[kNumCombs];
    int combTuningsR[kNumCombs];
    for (int c = 0; c < kNumCombs; ++c)
    {
        combTuningsL[c] = (std::min)(
            3199, (std::max)(16, JavaRound(kCombTuningsL[c] * rateScale)));
        combTuningsR[c] = (std::min)(
            3199, (std::max)(16,
                           JavaRound((kCombTuningsL[c] + kStereoSpread) *
                                     rateScale)));
    }

    int allpassTuningsL[kNumAllpass];
    int allpassTuningsR[kNumAllpass];
    for (int a = 0; a < kNumAllpass; ++a)
    {
        allpassTuningsL[a] = (std::min)(
            1023, (std::max)(8, JavaRound(kAllpassTuningsL[a] * rateScale)));
        allpassTuningsR[a] = (std::min)(
            1023, (std::max)(8,
                           JavaRound((kAllpassTuningsL[a] + kStereoSpread) *
                                     rateScale)));
    }

    const int echoDelaySamples =
        (std::min)(47999, (samplerate * 190) / 1000);
    const float echoFeedback =
        ((static_cast<float>(echoLevel) / 100.0f) * 0.45f) + 0.1f;
    const float echoGain = static_cast<float>(std::pow(
        static_cast<double>(static_cast<float>(echoLevel) / 100.0f), 1.5));
    const float dryGain =
        (std::max)(0.2f, (1.0f - (reverbOn ? reverbMix * 0.35f : 0.0f)) -
                           (echoOn ? echoGain * 0.35f : 0.0f));

    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = static_cast<float>(left[i]);
        const float inR = stereo ? static_cast<float>(right[i]) : inL;

        float revOutL = 0.0f;
        float revOutR = 0.0f;

        if (reverbOn)
        {
            float combSumL = 0.0f;
            float combSumR = 0.0f;

            for (int c = 0; c < kNumCombs; ++c)
            {
                const int idxL = m_comb_index_l[c];
                const float outL = m_comb_buf_l[c * kMaxCombSize + idxL];
                const float filteredL =
                    (m_comb_filter_l[c] * 0.36f) + (outL * 0.64f);
                m_comb_filter_l[c] = filteredL;
                m_comb_buf_l[c * kMaxCombSize + idxL] =
                    (filteredL * combFeedback) + inL;

                int nextL = idxL + 1;
                if (nextL >= combTuningsL[c])
                    nextL = 0;
                m_comb_index_l[c] = nextL;
                combSumL += outL;

                if (stereo)
                {
                    const int idxR = m_comb_index_r[c];
                    const float outR = m_comb_buf_r[c * kMaxCombSize + idxR];
                    const float filteredR =
                        (m_comb_filter_r[c] * 0.36f) + (outR * 0.64f);
                    m_comb_filter_r[c] = filteredR;
                    m_comb_buf_r[c * kMaxCombSize + idxR] =
                        (filteredR * combFeedback) + inR;

                    int nextR = idxR + 1;
                    if (nextR >= combTuningsR[c])
                        nextR = 0;
                    m_comb_index_r[c] = nextR;
                    combSumR += outR;
                }
            }

            revOutL = combSumL * 0.125f;
            if (std::fabs(revOutL) < 1.0E-15f)
                revOutL = 0.0f;

            if (stereo)
            {
                combSumR *= 0.125f;
                if (std::fabs(combSumR) < 1.0E-15f)
                    combSumR = 0.0f;
            }
            revOutR = combSumR;

            for (int a = 0; a < kNumAllpass; ++a)
            {
                const int idxL = m_allpass_index_l[a];
                const float bufValL =
                    m_allpass_buf_l[a * kMaxAllpassSize + idxL];
                m_allpass_buf_l[a * kMaxAllpassSize + idxL] =
                    (bufValL * 0.5f) + revOutL;
                const float outL = (-revOutL) + bufValL;

                int nextL = idxL + 1;
                if (nextL >= allpassTuningsL[a])
                    nextL = 0;
                m_allpass_index_l[a] = nextL;
                revOutL = outL;

                if (stereo)
                {
                    const int idxR = m_allpass_index_r[a];
                    const float bufValR =
                        m_allpass_buf_r[a * kMaxAllpassSize + idxR];
                    m_allpass_buf_r[a * kMaxAllpassSize + idxR] =
                        (bufValR * 0.5f) + revOutR;
                    const float outR = (-revOutR) + bufValR;

                    int nextR = idxR + 1;
                    if (nextR >= allpassTuningsR[a])
                        nextR = 0;
                    m_allpass_index_r[a] = nextR;
                    revOutR = outR;
                }
            }
        }

        float echoOutL = 0.0f;
        float echoOutR = 0.0f;

        if (echoOn)
        {
            int readIdx = m_echo_index - echoDelaySamples;
            if (readIdx < 0)
                readIdx += kMaxEchoSamples;
            if (readIdx < 0 || readIdx >= kMaxEchoSamples)
                readIdx = 0;

            m_echo_filter_l =
                (m_echo_buf_l[readIdx] * 0.65f) + (m_echo_filter_l * 0.35f);
            echoOutL = m_echo_filter_l;
            m_echo_buf_l[m_echo_index] = (echoOutL * echoFeedback) + inL;

            if (stereo)
            {
                m_echo_filter_r = (m_echo_buf_r[readIdx] * 0.65f) +
                                  (m_echo_filter_r * 0.35f);
                echoOutR = m_echo_filter_r;
                m_echo_buf_r[m_echo_index] = (echoOutR * echoFeedback) + inR;
            }

            ++m_echo_index;
            if (m_echo_index >= kMaxEchoSamples)
                m_echo_index = 0;
        }

        left[i] = ClampToShort((inL * dryGain) +
                               (reverbOn ? revOutL * reverbMix : 0.0f) +
                               (echoOn ? echoOutL * echoGain : 0.0f));

        if (stereo)
        {
            right[i] = ClampToShort((inR * dryGain) +
                                    (reverbOn ? revOutR * reverbMix : 0.0f) +
                                    (echoOn ? echoOutR * echoGain : 0.0f));
        }
    }
}

void ExtroVoiceChanger::ApplyGhostEffect(short* left, short* right,
                                         int numSamples, int samplerate,
                                         float pitchFactor)
{
    ApplyPitchShift(left, right, numSamples, pitchFactor, samplerate);

    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = static_cast<float>(left[i]);
        const float inR = right != nullptr ? static_cast<float>(right[i]) : inL;

        const int idxL = m_allpass_index_l[0] % 400;
        const float bufValL = m_allpass_buf_l[idxL];
        m_allpass_buf_l[idxL] = (bufValL * 0.6f) + inL;
        m_allpass_index_l[0] = (idxL + 1) % 400;

        left[i] = ClampToShort((((-inL) + bufValL) * 0.45f) + (inL * 0.55f));

        if (right != nullptr)
        {
            const int idxR = m_allpass_index_r[0] % 423;
            const float bufValR = m_allpass_buf_r[idxR];
            m_allpass_buf_r[idxR] = (0.6f * bufValR) + inR;
            m_allpass_index_r[0] = (idxR + 1) % 423;

            right[i] =
                ClampToShort((((-inR) + bufValR) * 0.45f) + (inR * 0.55f));
        }
    }
}

void ExtroVoiceChanger::ApplyMegaphoneEffect(short* left, short* right,
                                             int numSamples, int samplerate)
{
    (void)samplerate;

    for (int i = 0; i < numSamples; ++i)
    {
        const double inL = left[i] / 32768.0;
        const double outL = ((m_filter_y2_l + inL) - m_filter_x2_l) * 0.55;
        m_filter_y2_l = outL;
        m_filter_x2_l = inL;
        left[i] = ClampToShort(static_cast<float>(
            SoftSaturate(outL * 1.35) * 0.88 * 32767.0));

        if (right != nullptr)
        {
            const double inR = right[i] / 32768.0;
            const double outR = ((m_filter_y2_r + inR) - m_filter_x2_r) * 0.55;
            m_filter_y2_r = outR;
            m_filter_x2_r = inR;
            right[i] = ClampToShort(static_cast<float>(
                SoftSaturate(outR * 1.35) * 0.88 * 32767.0));
        }
    }
}

void ExtroVoiceChanger::ApplyMonsterEffect(short* left, short* right,
                                           int numSamples, int samplerate)
{
    ApplyPitchShift(left, right, numSamples, 0.6f, samplerate);

    const double delta = 28.274333882308138 / samplerate;
    for (int i = 0; i < numSamples; ++i)
    {
        const double gain = (std::sin(m_monster_phase) * 0.18) + 0.82;
        m_monster_phase += delta;
        if (m_monster_phase > kTwoPi)
            m_monster_phase -= kTwoPi;

        left[i] = ClampToShort(static_cast<float>(left[i] * gain));
        if (right != nullptr)
            right[i] = ClampToShort(static_cast<float>(right[i] * gain));
    }
}

void ExtroVoiceChanger::ApplyRadioEffect(short* left, short* right,
                                         int numSamples, int samplerate)
{
    (void)samplerate;

    for (int i = 0; i < numSamples; ++i)
    {
        const double inL = left[i] / 32768.0;
        const double outL = ((m_filter_y1_l + inL) - m_filter_x1_l) * 0.7;
        m_filter_y1_l = outL;
        m_filter_x1_l = inL;
        left[i] = ClampToShort(static_cast<float>(
            SoftSaturate(outL * 1.25) * 0.9 * 32767.0));

        if (right != nullptr)
        {
            const double inR = right[i] / 32768.0;
            const double outR = ((m_filter_y1_r + inR) - m_filter_x1_r) * 0.7;
            m_filter_y1_r = outR;
            m_filter_x1_r = inR;
            right[i] = ClampToShort(static_cast<float>(
                SoftSaturate(outR * 1.25) * 0.9 * 32767.0));
        }
    }
}

void ExtroVoiceChanger::ApplyRobotEffect(short* left, short* right,
                                         int numSamples, int samplerate)
{
    const double delta = 364.424747816416 / samplerate;
    for (int i = 0; i < numSamples; ++i)
    {
        const double sine = std::sin(m_robot_phase);
        m_robot_phase += delta;
        if (m_robot_phase > kTwoPi)
            m_robot_phase -= kTwoPi;

        const double factor = (sine * 0.7) + 0.3;
        left[i] = ClampToShort(static_cast<float>(left[i] * factor));
        if (right != nullptr)
            right[i] = ClampToShort(static_cast<float>(right[i] * factor));
    }
}

void ExtroVoiceChanger::ApplyTelephoneEffect(short* left, short* right,
                                             int numSamples, int samplerate)
{
    (void)samplerate;

    for (int i = 0; i < numSamples; ++i)
    {
        const double inL = left[i] / 32768.0;
        const double outL = ((m_filter_y1_l + inL) - m_filter_x1_l) * 0.8;
        m_filter_y1_l = outL;
        m_filter_x1_l = inL;
        left[i] = ClampToShort(static_cast<float>(
            SoftSaturate(outL * 1.3) * 0.85 * 32767.0));

        if (right != nullptr)
        {
            const double inR = right[i] / 32768.0;
            const double outR = ((m_filter_y1_r + inR) - m_filter_x1_r) * 0.8;
            m_filter_y1_r = outR;
            m_filter_x1_r = inR;
            right[i] = ClampToShort(static_cast<float>(
                SoftSaturate(outR * 1.3) * 0.85 * 32767.0));
        }
    }
}

void ExtroVoiceChanger::ApplyUnderwaterEffect(short* left, short* right,
                                              int numSamples, int samplerate)
{
    const double invRate = 1.0 / static_cast<double>(samplerate);
    const double alpha = invRate / (2.448537586029159E-4 + invRate);
    const double delta = 5.654866776461628 / static_cast<double>(samplerate);

    for (int i = 0; i < numSamples; ++i)
    {
        m_filter_y1_l = ((left[i] - m_filter_y1_l) * alpha) + m_filter_y1_l;
        const double gain = (std::sin(m_lfo_phase) * 0.15) + 0.85;
        left[i] = ClampToShort(static_cast<float>(m_filter_y1_l * gain));

        if (right != nullptr)
        {
            m_filter_y1_r =
                ((right[i] - m_filter_y1_r) * alpha) + m_filter_y1_r;
            right[i] = ClampToShort(static_cast<float>(m_filter_y1_r * gain));
        }

        m_lfo_phase += delta;
        if (m_lfo_phase > kTwoPi)
            m_lfo_phase -= kTwoPi;
    }
}

// --- v5.31.0 Enhanced Voice Changer DSP (Modes 13..23) ---------------------

float ExtroVoiceChanger::QuantizeMidiToScale(float midi, int scale) const
{
    if (scale == AUTOTUNE_SCALE_CHROMATIC)
        return std::floor(midi + 0.5f);

    static const int kMajorNotes[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int kMinorNotes[7] = {0, 2, 3, 5, 7, 8, 10};
    const int* notes = (scale == AUTOTUNE_SCALE_MAJOR) ? kMajorNotes : kMinorNotes;

    const int baseOctave = static_cast<int>(std::floor(midi / 12.0f));
    float bestNote = std::floor(midi + 0.5f);
    float bestDist = 100.0f;

    for (int oct = baseOctave - 1; oct <= baseOctave + 1; ++oct)
    {
        for (int k = 0; k < 7; ++k)
        {
            const float candidate = static_cast<float>(oct * 12 + notes[k]);
            const float dist = std::fabs(candidate - midi);
            if (dist < bestDist)
            {
                bestDist = dist;
                bestNote = candidate;
            }
        }
    }
    return bestNote;
}

float ExtroVoiceChanger::EstimatePitchHz(int samplerate)
{
    const int minLag = (std::max)(16, samplerate / 700);
    const int maxLag = (std::min)(kAutoTuneBufSize / 2 - 4, samplerate / 80);
    const int corrLen = (std::min)(kAutoTuneBufSize - maxLag - 2,
                                   (std::max)(256, samplerate / 75));
    const int needed = corrLen + maxLag + 2;
    if (m_at_filled < needed || maxLag <= minLag)
        return 0.0f;

    // Linearize the most recent 'needed' samples from circular buffer.
    int start = m_at_write_pos - needed;
    if (start < 0)
        start += kAutoTuneBufSize;

    float mean = 0.0f;
    for (int i = 0; i < needed; ++i)
    {
        const float s = m_at_buf[(start + i) % kAutoTuneBufSize];
        m_at_win[i] = s;
        mean += s;
    }
    mean /= static_cast<float>(needed);

    float energy0 = 0.0f;
    for (int i = 0; i < needed; ++i)
    {
        m_at_win[i] -= mean;
        if (i < corrLen)
            energy0 += m_at_win[i] * m_at_win[i];
    }

    const float rms = std::sqrt(energy0 / static_cast<float>(corrLen));
    if (rms < 0.015f)
        return 0.0f;

    // Coarse normalized autocorrelation search (step 2 for low CPU).
    float maxCorr = 0.0f;
    int bestLag = 0;
    const float* x = m_at_win.data();

    for (int lag = minLag; lag <= maxLag; lag += 2)
    {
        float dot = 0.0f;
        float e0 = 0.0f;
        float e1 = 0.0f;
        for (int i = 0; i < corrLen; i += 2)
        {
            const float a = x[i];
            const float b = x[i + lag];
            dot += a * b;
            e0 += a * a;
            e1 += b * b;
        }
        const float denom = std::sqrt(e0 * e1) + 1.0E-9f;
        const float corr = dot / denom;
        if (corr > maxCorr)
        {
            maxCorr = corr;
            bestLag = lag;
        }
    }

    if (maxCorr < 0.45f || bestLag <= 0)
        return 0.0f;

    // Anti-subharmonic check: if half-lag has strong correlation, prefer fundamental.
    const int halfLag = bestLag / 2;
    if (halfLag >= minLag)
    {
        float dot = 0.0f, e0 = 0.0f, e1 = 0.0f;
        for (int i = 0; i < corrLen; i += 2)
        {
            const float a = x[i];
            const float b = x[i + halfLag];
            dot += a * b;
            e0 += a * a;
            e1 += b * b;
        }
        const float halfCorr = dot / (std::sqrt(e0 * e1) + 1.0E-9f);
        if (halfCorr >= maxCorr * 0.90f && halfCorr >= 0.50f)
            bestLag = halfLag;
    }

    // Fine search +/- 2 samples + parabolic interpolation.
    const int lo = (std::max)(minLag, bestLag - 2);
    const int hi = (std::min)(maxLag, bestLag + 2);
    float fineCorr[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float bestFine = -1.0f;
    int bestFineIdx = 0;
    int fineCount = hi - lo + 1;
    for (int idx = 0; idx < fineCount && idx < 5; ++idx)
    {
        const int lag = lo + idx;
        float dot = 0.0f, e0 = 0.0f, e1 = 0.0f;
        for (int i = 0; i < corrLen; ++i)
        {
            const float a = x[i];
            const float b = x[i + lag];
            dot += a * b;
            e0 += a * a;
            e1 += b * b;
        }
        const float c = dot / (std::sqrt(e0 * e1) + 1.0E-9f);
        fineCorr[idx] = c;
        if (c > bestFine)
        {
            bestFine = c;
            bestFineIdx = idx;
        }
    }

    if (bestFine < 0.45f)
        return 0.0f;

    float refinedLag = static_cast<float>(lo + bestFineIdx);
    if (bestFineIdx > 0 && bestFineIdx + 1 < fineCount)
    {
        const float y0 = fineCorr[bestFineIdx - 1];
        const float y1 = fineCorr[bestFineIdx];
        const float y2 = fineCorr[bestFineIdx + 1];
        const float denom = 2.0f * (y0 - 2.0f * y1 + y2);
        if (std::fabs(denom) > 1.0E-6f)
        {
            const float offset = (y0 - y2) / denom;
            if (offset > -0.5f && offset < 0.5f)
                refinedLag += offset;
        }
    }

    const float f0 = static_cast<float>(samplerate) / refinedLag;
    if (f0 < 80.0f || f0 > 700.0f)
        return 0.0f;
    return f0;
}

void ExtroVoiceChanger::ApplyAutoTuneEffect(short* left, short* right,
                                            int numSamples, int samplerate,
                                            bool hardSnap)
{
    // Push mono signal into Auto-Tune analysis ring buffer.
    for (int i = 0; i < numSamples; ++i)
    {
        float s = static_cast<float>(left[i]) / 32768.0f;
        if (right != nullptr)
            s = 0.5f * (s + static_cast<float>(right[i]) / 32768.0f);
        m_at_buf[m_at_write_pos] = s;
        m_at_write_pos = (m_at_write_pos + 1) % kAutoTuneBufSize;
        if (m_at_filled < kAutoTuneBufSize)
            ++m_at_filled;
    }

    const float f0 = EstimatePitchHz(samplerate);
    if (f0 > 0.0f)
    {
        const float midi =
            69.0f + 12.0f * static_cast<float>(std::log2(static_cast<double>(f0) / 440.0));
        const int scale = m_autotune_scale.load(std::memory_order_relaxed);
        const float targetMidi = QuantizeMidiToScale(midi, scale);
        const float targetHz =
            440.0f * static_cast<float>(std::pow(2.0, static_cast<double>(targetMidi - 69.0f) / 12.0));
        float ratio = targetHz / f0;
        ratio = (std::max)(0.72f, (std::min)(1.38f, ratio));
        m_at_target_ratio = ratio;
    }
    else
    {
        // Gently relax toward 1.0 on unvoiced/consonant/silent frames.
        m_at_target_ratio = 1.0f + (m_at_target_ratio - 1.0f) * 0.7f;
    }

    const float glide = hardSnap ? 0.75f : 0.22f;
    m_at_ratio += (m_at_target_ratio - m_at_ratio) * glide;

    if (std::fabs(m_at_ratio - 1.0f) > 0.002f)
        ApplyPitchShift(left, right, numSamples, m_at_ratio, samplerate);

    if (hardSnap)
    {
        // Signature crisp shimmer on Auto-Tune Hard (T-Pain)
        ApplyChorusMix(left, right, numSamples, samplerate, 0.22f);
    }
}

void ExtroVoiceChanger::ApplyShelfTilt(short* left, short* right,
                                       int numSamples, int samplerate,
                                       float lowBoost, float highBoost)
{
    const float sr = static_cast<float>((std::max)(8000, samplerate));
    const float lpAlpha = 1.0f - std::exp(-static_cast<float>(kTwoPi) * 220.0f / sr);
    const float hpAlpha = 1.0f - std::exp(-static_cast<float>(kTwoPi) * 3200.0f / sr);

    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = static_cast<float>(left[i]);
        m_eq_lp_l += (inL - m_eq_lp_l) * lpAlpha;
        m_eq_hp_l += (inL - m_eq_hp_l) * hpAlpha;
        const float highsL = inL - m_eq_hp_l;
        left[i] = ClampToShort(inL + m_eq_lp_l * lowBoost + highsL * highBoost);

        if (right != nullptr)
        {
            const float inR = static_cast<float>(right[i]);
            m_eq_lp_r += (inR - m_eq_lp_r) * lpAlpha;
            m_eq_hp_r += (inR - m_eq_hp_r) * hpAlpha;
            const float highsR = inR - m_eq_hp_r;
            right[i] = ClampToShort(inR + m_eq_lp_r * lowBoost + highsR * highBoost);
        }
    }
}

void ExtroVoiceChanger::ApplyStudioEffect(short* left, short* right,
                                          int numSamples, int samplerate,
                                          bool broadcastDj)
{
    const float sr = static_cast<float>((std::max)(8000, samplerate));

    // Noise gate parameters
    const float gateThresh = broadcastDj ? 0.0065f : 0.0055f;
    const float gateAtk = 1.0f - std::exp(-1.0f / (0.002f * sr));
    const float gateRel = 1.0f - std::exp(-1.0f / (0.085f * sr));

    // RMS compressor parameters
    const float compThresh = broadcastDj ? 0.20f : 0.28f;
    const float compRatio = broadcastDj ? 5.0f : 3.2f;
    const float makeupGain = broadcastDj ? 1.50f : 1.25f;
    const float compAtk = 1.0f - std::exp(-1.0f / (0.006f * sr));
    const float compRel = 1.0f - std::exp(-1.0f / (0.120f * sr));

    // Warm Bass / Presence EQ coefficients
    const float lpAlpha = 1.0f - std::exp(-static_cast<float>(kTwoPi) * (broadcastDj ? 170.0f : 200.0f) / sr);
    const float hpAlpha = 1.0f - std::exp(-static_cast<float>(kTwoPi) * (broadcastDj ? 3800.0f : 3400.0f) / sr);
    const float lowBoost = broadcastDj ? 0.42f : 0.26f;
    const float highBoost = broadcastDj ? 0.36f : 0.24f;

    for (int i = 0; i < numSamples; ++i)
    {
        float inL = static_cast<float>(left[i]) / 32768.0f;
        float inR = right != nullptr ? static_cast<float>(right[i]) / 32768.0f : inL;
        const float level = (std::max)(std::fabs(inL), std::fabs(inR));

        // 1. Noise gate
        const float gateTarget = (level >= gateThresh) ? 1.0f : 0.08f;
        const float gateCoeff = (gateTarget > m_gate_env) ? gateAtk : gateRel;
        m_gate_env += (gateTarget - m_gate_env) * gateCoeff;

        inL *= m_gate_env;
        inR *= m_gate_env;

        // 2. Soft RMS-style compressor
        const float compCoeff = (level > m_comp_env) ? compAtk : compRel;
        m_comp_env += (level - m_comp_env) * compCoeff;

        float compGain = makeupGain;
        if (m_comp_env > compThresh)
        {
            const float over = m_comp_env / compThresh;
            const float desired = std::pow(over, 1.0f / compRatio);
            compGain *= (desired / over);
        }

        inL *= compGain;
        inR *= compGain;

        // 3. Warm Bass + Presence EQ
        m_eq_lp_l += (inL - m_eq_lp_l) * lpAlpha;
        m_eq_hp_l += (inL - m_eq_hp_l) * hpAlpha;
        float outL = inL + m_eq_lp_l * lowBoost + (inL - m_eq_hp_l) * highBoost;
        if (broadcastDj)
            outL = static_cast<float>(SoftSaturate(static_cast<double>(outL) * 1.12));
        left[i] = ClampToShort(outL * 32767.0f);

        if (right != nullptr)
        {
            m_eq_lp_r += (inR - m_eq_lp_r) * lpAlpha;
            m_eq_hp_r += (inR - m_eq_hp_r) * hpAlpha;
            float outR = inR + m_eq_lp_r * lowBoost + (inR - m_eq_hp_r) * highBoost;
            if (broadcastDj)
                outR = static_cast<float>(SoftSaturate(static_cast<double>(outR) * 1.12));
            right[i] = ClampToShort(outR * 32767.0f);
        }
    }
}

void ExtroVoiceChanger::ApplyConcertHallEffect(short* left, short* right,
                                               int numSamples, int samplerate)
{
    ApplyStudioEffect(left, right, numSamples, samplerate, false);
    ApplyEchoAndReverbCore(left, right, numSamples, samplerate,
                           true, 18, true, 68, 88);
}

void ExtroVoiceChanger::ApplyDemonEffect(short* left, short* right,
                                         int numSamples, int samplerate)
{
    ApplyPitchShift(left, right, numSamples, 0.56f, samplerate);

    const double delta = (kTwoPi * 22.0) / static_cast<double>((std::max)(8000, samplerate));
    for (int i = 0; i < numSamples; ++i)
    {
        const double gain = (std::sin(m_monster_phase) * 0.24) + 0.76;
        m_monster_phase += delta;
        if (m_monster_phase > kTwoPi)
            m_monster_phase -= kTwoPi;

        left[i] = ClampToShort(static_cast<float>(left[i] * gain));
        if (right != nullptr)
            right[i] = ClampToShort(static_cast<float>(right[i] * gain));
    }

    ApplyEchoAndReverbCore(left, right, numSamples, samplerate,
                           false, 0, true, 52, 78);
    ApplyShelfTilt(left, right, numSamples, samplerate, 0.35f, -0.25f);
}

void ExtroVoiceChanger::ApplyHeliumEffect(short* left, short* right,
                                          int numSamples, int samplerate)
{
    ApplyPitchShift(left, right, numSamples, 1.65f, samplerate);
    ApplyShelfTilt(left, right, numSamples, samplerate, -0.15f, 0.32f);
}

void ExtroVoiceChanger::ApplyFemaleEffect(short* left, short* right,
                                          int numSamples, int samplerate)
{
    ApplyPitchShift(left, right, numSamples, 1.24f, samplerate);
    ApplyShelfTilt(left, right, numSamples, samplerate, -0.08f, 0.24f);
}

void ExtroVoiceChanger::ApplyMaleEffect(short* left, short* right,
                                        int numSamples, int samplerate)
{
    ApplyPitchShift(left, right, numSamples, 0.82f, samplerate);
    ApplyShelfTilt(left, right, numSamples, samplerate, 0.38f, -0.08f);
}

void ExtroVoiceChanger::ApplyWalkieTalkieEffect(short* left, short* right,
                                                int numSamples, int samplerate)
{
    const double sr = static_cast<double>((std::max)(8000, samplerate));
    const double hpAlpha = 1.0 - std::exp(-kTwoPi * 400.0 / sr);
    const double lpAlpha = 1.0 - std::exp(-kTwoPi * 2800.0 / sr);

    for (int i = 0; i < numSamples; ++i)
    {
        const double inL = left[i] / 32768.0;
        m_filter_x1_l += (inL - m_filter_x1_l) * hpAlpha;
        const double hpL = inL - m_filter_x1_l;
        m_filter_y1_l += (hpL - m_filter_y1_l) * lpAlpha;
        const double drivenL = SoftSaturate(m_filter_y1_l * 2.15) * 0.90;
        left[i] = ClampToShort(static_cast<float>(drivenL * 32767.0));

        if (right != nullptr)
        {
            const double inR = right[i] / 32768.0;
            m_filter_x1_r += (inR - m_filter_x1_r) * hpAlpha;
            const double hpR = inR - m_filter_x1_r;
            m_filter_y1_r += (hpR - m_filter_y1_r) * lpAlpha;
            const double drivenR = SoftSaturate(m_filter_y1_r * 2.15) * 0.90;
            right[i] = ClampToShort(static_cast<float>(drivenR * 32767.0));
        }
    }
}

void ExtroVoiceChanger::ApplyWhisperEffect(short* left, short* right,
                                           int numSamples, int samplerate)
{
    const float sr = static_cast<float>((std::max)(8000, samplerate));
    const float hpAlpha = 1.0f - std::exp(-static_cast<float>(kTwoPi) * 950.0f / sr);
    const float noiseLpAlpha = 1.0f - std::exp(-static_cast<float>(kTwoPi) * 4200.0f / sr);
    const float envAtk = 1.0f - std::exp(-1.0f / (0.005f * sr));
    const float envRel = 1.0f - std::exp(-1.0f / (0.045f * sr));

    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = static_cast<float>(left[i]) / 32768.0f;
        const float inR = right != nullptr ? static_cast<float>(right[i]) / 32768.0f : inL;
        const float level = (std::max)(std::fabs(inL), std::fabs(inR));

        const float coeff = (level > m_whisper_env) ? envAtk : envRel;
        m_whisper_env += (level - m_whisper_env) * coeff;

        // Filtered breathy noise modulated by vocal envelope
        const float rawNoise = NextWhiteNoise(m_prng_state);
        m_whisper_noise_lp += (rawNoise - m_whisper_noise_lp) * noiseLpAlpha;
        const float breath = m_whisper_noise_lp * m_whisper_env * 1.35f;

        m_eq_hp_l += (inL - m_eq_hp_l) * hpAlpha;
        const float hpL = inL - m_eq_hp_l;
        left[i] = ClampToShort((breath * 0.65f + hpL * 0.45f) * 32767.0f);

        if (right != nullptr)
        {
            const float rawNoiseR = NextWhiteNoise(m_prng_state);
            const float breathR = ((m_whisper_noise_lp * 0.7f) + (rawNoiseR * 0.3f)) * m_whisper_env * 1.35f;
            m_eq_hp_r += (inR - m_eq_hp_r) * hpAlpha;
            const float hpR = inR - m_eq_hp_r;
            right[i] = ClampToShort((breathR * 0.65f + hpR * 0.45f) * 32767.0f);
        }
    }
}

void ExtroVoiceChanger::Process(short* interleaved, int samplesPerChannel,
                                int samplerate, int channels)
{
    if (interleaved == nullptr || samplesPerChannel <= 0 || samplerate <= 0 ||
        channels <= 0)
    {
        return;
    }

    const int mode = m_mode.load(std::memory_order_relaxed);
    if (mode == OFF)
        return;

    // Cheap try-lock: the audio thread only skips a block while the UI
    // thread is inside Reset(). It never waits for the mutex.
    std::unique_lock<std::mutex> lock(m_reset_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;

    const bool stereo = channels > 1;

    // The Java Ghost effect derives its pitch factor and advances the LFO
    // once per process() call, so do the same here rather than once per
    // chunk.
    float ghostFactor = 1.0f;
    if (mode == GHOST)
    {
        ghostFactor =
            static_cast<float>(std::sin(m_lfo_phase)) * 0.1f + 0.95f;
        m_lfo_phase += 7.5398223686155035 / samplerate;
        if (m_lfo_phase > kTwoPi)
            m_lfo_phase -= kTwoPi;
    }

    const std::ptrdiff_t stride = channels;
    int processed = 0;
    while (processed < samplesPerChannel)
    {
        const int count =
            (std::min)(samplesPerChannel - processed, kMaxProcessSamples);
        short* src = interleaved + static_cast<std::ptrdiff_t>(processed) * stride;

        for (int i = 0; i < count; ++i)
        {
            m_scratch_l[i] = src[0];
            if (stereo)
                m_scratch_r[i] = src[1];
            src += stride;
        }

        short* left = m_scratch_l.data();
        short* right = stereo ? m_scratch_r.data() : nullptr;

        switch (mode)
        {
        case ECHO:
            ApplyEchoAndReverbEffect(left, right, count, samplerate);
            break;
        case DEEP:
            ApplyPitchShift(left, right, count, 0.74f, samplerate);
            break;
        case CHIPMUNK:
            ApplyPitchShift(left, right, count, 1.44f, samplerate);
            break;
        case ROBOT:
            ApplyRobotEffect(left, right, count, samplerate);
            break;
        case RADIO:
            ApplyRadioEffect(left, right, count, samplerate);
            break;
        case MEGAPHONE:
            ApplyMegaphoneEffect(left, right, count, samplerate);
            break;
        case MONSTER:
            ApplyMonsterEffect(left, right, count, samplerate);
            break;
        case ALIEN:
            ApplyAlienEffect(left, right, count, samplerate);
            break;
        case UNDERWATER:
            ApplyUnderwaterEffect(left, right, count, samplerate);
            break;
        case CHORUS:
            ApplyChorusEffect(left, right, count, samplerate);
            break;
        case TELEPHONE:
            ApplyTelephoneEffect(left, right, count, samplerate);
            break;
        case GHOST:
            ApplyGhostEffect(left, right, count, samplerate, ghostFactor);
            break;
        case AUTOTUNE:
            ApplyAutoTuneEffect(left, right, count, samplerate, false);
            break;
        case AUTOTUNE_HARD:
            ApplyAutoTuneEffect(left, right, count, samplerate, true);
            break;
        case STUDIO:
            ApplyStudioEffect(left, right, count, samplerate, false);
            break;
        case CONCERT_HALL:
            ApplyConcertHallEffect(left, right, count, samplerate);
            break;
        case DEMON:
            ApplyDemonEffect(left, right, count, samplerate);
            break;
        case HELIUM:
            ApplyHeliumEffect(left, right, count, samplerate);
            break;
        case FEMALE:
            ApplyFemaleEffect(left, right, count, samplerate);
            break;
        case MALE:
            ApplyMaleEffect(left, right, count, samplerate);
            break;
        case WALKIE_TALKIE:
            ApplyWalkieTalkieEffect(left, right, count, samplerate);
            break;
        case WHISPER:
            ApplyWhisperEffect(left, right, count, samplerate);
            break;
        case BROADCAST_DJ:
            ApplyStudioEffect(left, right, count, samplerate, true);
            break;
        case OFF:
        default:
            break;
        }

        short* dst = interleaved + static_cast<std::ptrdiff_t>(processed) * stride;
        for (int i = 0; i < count; ++i)
        {
            dst[0] = m_scratch_l[i];
            if (stereo)
                dst[1] = m_scratch_r[i];
            dst += stride;
        }

        processed += count;
    }
}

void ExtroVoiceChanger::Reset()
{
    std::lock_guard<std::mutex> lock(m_reset_mutex);

    std::fill(m_pitch_buf_l.begin(), m_pitch_buf_l.end(),
              static_cast<short>(0));
    std::fill(m_pitch_buf_r.begin(), m_pitch_buf_r.end(),
              static_cast<short>(0));
    m_pitch_write_pos = 0;
    m_pitch_phase = 0.0f;

    std::fill(m_at_buf.begin(), m_at_buf.end(), 0.0f);
    std::fill(m_at_win.begin(), m_at_win.end(), 0.0f);
    m_at_write_pos = 0;
    m_at_filled = 0;
    m_at_ratio = 1.0f;
    m_at_target_ratio = 1.0f;

    std::fill(m_echo_buf_l.begin(), m_echo_buf_l.end(), 0.0f);
    std::fill(m_echo_buf_r.begin(), m_echo_buf_r.end(), 0.0f);
    m_echo_index = 0;
    m_echo_filter_l = 0.0f;
    m_echo_filter_r = 0.0f;

    std::fill(m_comb_buf_l.begin(), m_comb_buf_l.end(), 0.0f);
    std::fill(m_comb_buf_r.begin(), m_comb_buf_r.end(), 0.0f);
    std::fill(m_comb_index_l.begin(), m_comb_index_l.end(), 0);
    std::fill(m_comb_index_r.begin(), m_comb_index_r.end(), 0);
    std::fill(m_comb_filter_l.begin(), m_comb_filter_l.end(), 0.0f);
    std::fill(m_comb_filter_r.begin(), m_comb_filter_r.end(), 0.0f);

    std::fill(m_allpass_buf_l.begin(), m_allpass_buf_l.end(), 0.0f);
    std::fill(m_allpass_buf_r.begin(), m_allpass_buf_r.end(), 0.0f);
    std::fill(m_allpass_index_l.begin(), m_allpass_index_l.end(), 0);
    std::fill(m_allpass_index_r.begin(), m_allpass_index_r.end(), 0);

    std::fill(m_chorus_buf_l.begin(), m_chorus_buf_l.end(), 0.0f);
    std::fill(m_chorus_buf_r.begin(), m_chorus_buf_r.end(), 0.0f);
    m_chorus_index = 0;

    m_robot_phase = 0.0;
    m_lfo_phase = 0.0;
    m_monster_phase = 0.0;

    m_filter_x1_l = 0.0;
    m_filter_x2_l = 0.0;
    m_filter_y1_l = 0.0;
    m_filter_y2_l = 0.0;
    m_filter_x1_r = 0.0;
    m_filter_x2_r = 0.0;
    m_filter_y1_r = 0.0;
    m_filter_y2_r = 0.0;

    m_eq_lp_l = 0.0f;
    m_eq_lp_r = 0.0f;
    m_eq_hp_l = 0.0f;
    m_eq_hp_r = 0.0f;
    m_gate_env = 0.0f;
    m_comp_env = 0.0f;
    m_whisper_env = 0.0f;
    m_whisper_noise_lp = 0.0f;
}

} // namespace teamtalk
