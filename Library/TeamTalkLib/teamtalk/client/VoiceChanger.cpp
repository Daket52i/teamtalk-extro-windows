/*
 * TeamTalk Extro voice changer.
 *
 * C++ port of the Android module
 * org.teamtalkextro.backend.VoiceChanger (TeamTalk Extro). The DSP
 * coefficients, tuning tables and clamping are kept identical to the Java
 * reference; only the plumbing (fixed buffers, atomics, chunked in-place
 * processing) differs.
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
    , m_pitch_write_pos(0)
    , m_pitch_phase(0.0f)
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
{
    m_pitch_buf_l.assign(kPitchBufferSize, static_cast<short>(0));
    m_pitch_buf_r.assign(kPitchBufferSize, static_cast<short>(0));

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
    if (mode < static_cast<int>(OFF) || mode > static_cast<int>(GHOST))
        mode = OFF;

    m_mode.store(mode, std::memory_order_relaxed);
    Reset();
}

int ExtroVoiceChanger::GetMode() const
{
    return m_mode.load(std::memory_order_relaxed);
}

void ExtroVoiceChanger::SetEchoReverb(bool echo, int echoLevel, bool reverb,
                                      int reverbLevel, int roomSize)
{
    m_echo_enabled.store(echo ? 1 : 0, std::memory_order_relaxed);
    m_echo_level.store(std::max(0, std::min(100, echoLevel)),
                       std::memory_order_relaxed);
    m_reverb_enabled.store(reverb ? 1 : 0, std::memory_order_relaxed);
    m_reverb_level.store(std::max(0, std::min(100, reverbLevel)),
                         std::memory_order_relaxed);
    m_reverb_room_size.store(std::max(0, std::min(100, roomSize)),
                             std::memory_order_relaxed);
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
        std::min(2048, std::max(512, (samplerate * 42) / 1000));
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

void ExtroVoiceChanger::ApplyChorusEffect(short* left, short* right,
                                          int numSamples, int samplerate)
{
    const double delta = 5.340707511102648 / samplerate;
    const float sampleRate = static_cast<float>(samplerate);
    const int delay = JavaRound(0.02f * sampleRate);
    const int sweep = JavaRound(sampleRate * 0.006f);

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

        left[i] = ClampToShort((outL * 0.4f) + (inL * 0.6f));
        if (right != nullptr)
            right[i] = ClampToShort((outR * 0.4f) + (inR * 0.6f));

        m_chorus_index = (m_chorus_index + 1) % 2048;
        m_lfo_phase += delta;
        if (m_lfo_phase > kTwoPi)
            m_lfo_phase -= kTwoPi;
    }
}

void ExtroVoiceChanger::ApplyEchoAndReverbEffect(short* left, short* right,
                                                 int numSamples,
                                                 int samplerate)
{
    const int echoLevel = m_echo_level.load(std::memory_order_relaxed);
    const int reverbLevel = m_reverb_level.load(std::memory_order_relaxed);
    const int roomSize = m_reverb_room_size.load(std::memory_order_relaxed);

    const bool echoOn =
        m_echo_enabled.load(std::memory_order_relaxed) != 0 && echoLevel > 0;
    const bool reverbOn =
        m_reverb_enabled.load(std::memory_order_relaxed) != 0 &&
        reverbLevel > 0;

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
        combTuningsL[c] = std::min(
            3199, std::max(16, JavaRound(kCombTuningsL[c] * rateScale)));
        combTuningsR[c] = std::min(
            3199, std::max(16,
                           JavaRound((kCombTuningsL[c] + kStereoSpread) *
                                     rateScale)));
    }

    int allpassTuningsL[kNumAllpass];
    int allpassTuningsR[kNumAllpass];
    for (int a = 0; a < kNumAllpass; ++a)
    {
        allpassTuningsL[a] = std::min(
            1023, std::max(8, JavaRound(kAllpassTuningsL[a] * rateScale)));
        allpassTuningsR[a] = std::min(
            1023, std::max(8,
                           JavaRound((kAllpassTuningsL[a] + kStereoSpread) *
                                     rateScale)));
    }

    const int echoDelaySamples =
        std::min(47999, (samplerate * 190) / 1000);
    const float echoFeedback =
        ((static_cast<float>(echoLevel) / 100.0f) * 0.45f) + 0.1f;
    const float echoGain = static_cast<float>(std::pow(
        static_cast<double>(static_cast<float>(echoLevel) / 100.0f), 1.5));
    const float dryGain =
        std::max(0.2f, (1.0f - (reverbOn ? reverbMix * 0.35f : 0.0f)) -
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
            std::min(samplesPerChannel - processed, kMaxProcessSamples);
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
}

} // namespace teamtalk
