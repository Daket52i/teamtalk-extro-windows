/*
 * TeamTalk Extro voice changer.
 *
 * C++ port of the Android module
 * org.teamtalkextro.backend.VoiceChanger (TeamTalk Extro).
 *
 * SetMode() / SetEchoReverb() / Reset() are called from the UI thread,
 * Process() from the audio thread. Configuration is stored atomically and
 * Process() never allocates. Reset() is serialised against Process() with a
 * mutex that Process() only try-locks, so the audio thread never blocks for
 * longer than a single lock attempt.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <vector>

// <termios.h> defines ECHO as a macro; the voice-mode enumerator must win.
#if defined(ECHO)
#undef ECHO
#endif

namespace teamtalk
{

// Same ids and names as the Java VoiceChanger.Mode enum.
enum ExtroVoiceMode : int
{
    OFF = 0,
    ECHO,
    DEEP,
    CHIPMUNK,
    ROBOT,
    RADIO,
    MEGAPHONE,
    MONSTER,
    ALIEN,
    UNDERWATER,
    CHORUS,
    TELEPHONE,
    GHOST
};

// Snapshot of the voice changer configuration, as stored in the client node.
struct VoiceChangerSettings
{
    int mode = OFF;
    bool echo = true;
    int echo_level = 35;
    bool reverb = true;
    int reverb_level = 50;
    int room_size = 60;
};

class ExtroVoiceChanger
{

public:
    ExtroVoiceChanger();
    ~ExtroVoiceChanger() = default;

    ExtroVoiceChanger(const ExtroVoiceChanger&) = delete;
    ExtroVoiceChanger& operator=(const ExtroVoiceChanger&) = delete;

    void SetMode(int mode);
    int GetMode() const;

    void SetEchoReverb(bool echo, int echoLevel, bool reverb, int reverbLevel,
                       int roomSize);

    static void SetGlobalSettings(const VoiceChangerSettings& settings);
    static VoiceChangerSettings GetGlobalSettings();

    // Soft-knee saturation helper (Java semantics).
    static short ClampToShort(float f);

    // In-place S16 processing of interleaved mono or stereo samples.
    // For channels > 2 only the first two channels are processed.
    void Process(short* interleaved, int samplesPerChannel, int samplerate,
                 int channels);

    void Reset();

private:
    static constexpr int kChorusMaxSamples = 2048;
    static constexpr int kMaxAllpassSize = 1024;
    static constexpr int kMaxCombSize = 3200;
    static constexpr int kMaxEchoSamples = 48000;
    static constexpr int kNumAllpass = 4;
    static constexpr int kNumCombs = 8;
    static constexpr int kPitchBufferSize = 8192;
    static constexpr int kStereoSpread = 23;

    // Process() works through the caller's buffer in chunks of at most this
    // many samples per channel, so the audio thread never allocates.
    static constexpr int kMaxProcessSamples = 4096;

    // Math helpers (Java semantics).
    static double SoftSaturate(double d);
    static int JavaRound(float v);
    static float ReadInterpolated(const short* buf, float pos);

    // Per-mode DSP. left/right point at de-interleaved, contiguous blocks;
    // right is null for mono.
    void ApplyPitchShift(short* left, short* right, int numSamples,
                         float factor, int samplerate);
    void ApplyAlienEffect(short* left, short* right, int numSamples,
                          int samplerate);
    void ApplyChorusEffect(short* left, short* right, int numSamples,
                           int samplerate);
    void ApplyEchoAndReverbEffect(short* left, short* right, int numSamples,
                                  int samplerate);
    void ApplyGhostEffect(short* left, short* right, int numSamples,
                          int samplerate, float pitchFactor);
    void ApplyMegaphoneEffect(short* left, short* right, int numSamples,
                              int samplerate);
    void ApplyMonsterEffect(short* left, short* right, int numSamples,
                            int samplerate);
    void ApplyRadioEffect(short* left, short* right, int numSamples,
                          int samplerate);
    void ApplyRobotEffect(short* left, short* right, int numSamples,
                          int samplerate);
    void ApplyTelephoneEffect(short* left, short* right, int numSamples,
                              int samplerate);
    void ApplyUnderwaterEffect(short* left, short* right, int numSamples,
                               int samplerate);

    // Configuration written from the UI thread, read by the audio thread.
    std::atomic<int> m_mode;
    std::atomic<int> m_echo_enabled;
    std::atomic<int> m_echo_level;
    std::atomic<int> m_reverb_enabled;
    std::atomic<int> m_reverb_level;
    std::atomic<int> m_reverb_room_size;

    // Serialises Reset() against Process().
    std::mutex m_reset_mutex;

    // Pitch shifter.
    std::vector<short> m_pitch_buf_l;
    std::vector<short> m_pitch_buf_r;
    int m_pitch_write_pos;
    float m_pitch_phase;

    // Echo delay line.
    std::vector<float> m_echo_buf_l;
    std::vector<float> m_echo_buf_r;
    int m_echo_index;
    float m_echo_filter_l;
    float m_echo_filter_r;

    // Reverb comb filters, kNumCombs * kMaxCombSize floats per channel.
    std::vector<float> m_comb_buf_l;
    std::vector<float> m_comb_buf_r;
    std::vector<int> m_comb_index_l;
    std::vector<int> m_comb_index_r;
    std::vector<float> m_comb_filter_l;
    std::vector<float> m_comb_filter_r;

    // Reverb all-pass filters, kNumAllpass * kMaxAllpassSize floats/channel.
    std::vector<float> m_allpass_buf_l;
    std::vector<float> m_allpass_buf_r;
    std::vector<int> m_allpass_index_l;
    std::vector<int> m_allpass_index_r;

    // Chorus delay line.
    std::vector<float> m_chorus_buf_l;
    std::vector<float> m_chorus_buf_r;
    int m_chorus_index;

    // LFO phases.
    double m_lfo_phase;
    double m_monster_phase;
    double m_robot_phase;

    // One-pole / comb filter state.
    double m_filter_x1_l;
    double m_filter_x1_r;
    double m_filter_x2_l;
    double m_filter_x2_r;
    double m_filter_y1_l;
    double m_filter_y1_r;
    double m_filter_y2_l;
    double m_filter_y2_r;

    // De-interleave scratch, allocated once in the constructor.
    std::vector<short> m_scratch_l;
    std::vector<short> m_scratch_r;
};

} // namespace teamtalk
