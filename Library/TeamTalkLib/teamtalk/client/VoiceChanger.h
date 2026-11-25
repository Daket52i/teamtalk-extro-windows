/*
 * TeamTalk Extro voice changer (24 modes: classic + Auto-Tune + Studio Rack).
 *
 * SetMode() / SetEchoReverb() / SetAutoTuneScale() / Reset() are called from
 * the UI thread, Process() from the audio thread. Configuration is stored
 * atomically and Process() never allocates. Reset() is serialised against
 * Process() with a mutex that Process() only try-locks, so the audio thread
 * never blocks for longer than a single lock attempt.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

// <termios.h> defines ECHO as a macro; the voice-mode enumerator must win.
#if defined(ECHO)
#undef ECHO
#endif

namespace teamtalk
{

// Same ids and names as the Android VoiceChanger.Mode enum in TeamTalk Extro.
enum ExtroVoiceMode : int
{
    OFF = 0,
    ECHO = 1,
    DEEP = 2,
    CHIPMUNK = 3,
    ROBOT = 4,
    RADIO = 5,
    MEGAPHONE = 6,
    MONSTER = 7,
    ALIEN = 8,
    UNDERWATER = 9,
    CHORUS = 10,
    TELEPHONE = 11,
    GHOST = 12,
    AUTOTUNE = 13,
    AUTOTUNE_HARD = 14,
    STUDIO = 15,
    CONCERT_HALL = 16,
    DEMON = 17,
    HELIUM = 18,
    FEMALE = 19,
    MALE = 20,
    WALKIE_TALKIE = 21,
    WHISPER = 22,
    BROADCAST_DJ = 23,

    VOICE_MODE_MAX = BROADCAST_DJ
};

enum ExtroAutoTuneScale : int
{
    AUTOTUNE_SCALE_CHROMATIC = 0,
    AUTOTUNE_SCALE_MAJOR = 1,
    AUTOTUNE_SCALE_MINOR = 2
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
    int autotune_scale = AUTOTUNE_SCALE_CHROMATIC;
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

    void SetAutoTuneScale(int scale);
    int GetAutoTuneScale() const;

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
    static constexpr int kAutoTuneBufSize = 2048;

    // Process() works through the caller's buffer in chunks of at most this
    // many samples per channel, so the audio thread never allocates.
    static constexpr int kMaxProcessSamples = 4096;

    // Math helpers (Java semantics).
    static double SoftSaturate(double d);
    static int JavaRound(float v);
    static float ReadInterpolated(const short* buf, float pos);
    float QuantizeMidiToScale(float midi, int scale) const;
    float EstimatePitchHz(int samplerate);

    // Per-mode DSP. left/right point at de-interleaved, contiguous blocks;
    // right is null for mono.
    void ApplyPitchShift(short* left, short* right, int numSamples,
                         float factor, int samplerate);
    void ApplyAlienEffect(short* left, short* right, int numSamples,
                          int samplerate);
    void ApplyChorusEffect(short* left, short* right, int numSamples,
                           int samplerate);
    void ApplyChorusMix(short* left, short* right, int numSamples,
                        int samplerate, float wetMix);
    void ApplyEchoAndReverbEffect(short* left, short* right, int numSamples,
                                  int samplerate);
    void ApplyEchoAndReverbCore(short* left, short* right, int numSamples,
                                int samplerate, bool echoOn, int echoLevel,
                                bool reverbOn, int reverbLevel, int roomSize);
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

    // v5.31.0 modes (13..23)
    void ApplyAutoTuneEffect(short* left, short* right, int numSamples,
                             int samplerate, bool hardSnap);
    void ApplyStudioEffect(short* left, short* right, int numSamples,
                           int samplerate, bool broadcastDj);
    void ApplyConcertHallEffect(short* left, short* right, int numSamples,
                                int samplerate);
    void ApplyDemonEffect(short* left, short* right, int numSamples,
                          int samplerate);
    void ApplyHeliumEffect(short* left, short* right, int numSamples,
                           int samplerate);
    void ApplyFemaleEffect(short* left, short* right, int numSamples,
                           int samplerate);
    void ApplyMaleEffect(short* left, short* right, int numSamples,
                         int samplerate);
    void ApplyWalkieTalkieEffect(short* left, short* right, int numSamples,
                                 int samplerate);
    void ApplyWhisperEffect(short* left, short* right, int numSamples,
                            int samplerate);
    void ApplyShelfTilt(short* left, short* right, int numSamples,
                        int samplerate, float lowBoost, float highBoost);

    // Configuration written from the UI thread, read by the audio thread.
    std::atomic<int> m_mode;
    std::atomic<int> m_echo_enabled;
    std::atomic<int> m_echo_level;
    std::atomic<int> m_reverb_enabled;
    std::atomic<int> m_reverb_level;
    std::atomic<int> m_reverb_room_size;
    std::atomic<int> m_autotune_scale;

    // Serialises Reset() against Process().
    std::mutex m_reset_mutex;

    // Pitch shifter.
    std::vector<short> m_pitch_buf_l;
    std::vector<short> m_pitch_buf_r;
    int m_pitch_write_pos;
    float m_pitch_phase;

    // Auto-Tune F0 detector & pitch glide state.
    std::vector<float> m_at_buf;
    std::vector<float> m_at_win;
    int m_at_write_pos;
    int m_at_filled;
    float m_at_ratio;
    float m_at_target_ratio;

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

    // One-pole / shelf / gate / compressor / whisper filter state.
    double m_filter_x1_l;
    double m_filter_x1_r;
    double m_filter_x2_l;
    double m_filter_x2_r;
    double m_filter_y1_l;
    double m_filter_y1_r;
    double m_filter_y2_l;
    double m_filter_y2_r;

    float m_eq_lp_l;
    float m_eq_lp_r;
    float m_eq_hp_l;
    float m_eq_hp_r;
    float m_gate_env;
    float m_comp_env;
    float m_whisper_env;
    float m_whisper_noise_lp;
    uint32_t m_prng_state;

    // De-interleave scratch, allocated once in the constructor.
    std::vector<short> m_scratch_l;
    std::vector<short> m_scratch_r;
};

} // namespace teamtalk
