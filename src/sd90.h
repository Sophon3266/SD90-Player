// SD-90 Player — shared engine API
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <windows.h>

struct Wave {
    const uint8_t* data = nullptr;
    std::vector<float> pcm; // RDAC-decoded PCM normalized to ~[-1,1] without int16 clamping
    uint32_t len = 0;
    uint32_t loopStart = 0;   // relative to pcm
    uint32_t loopEnd = 0;     // relative to pcm
    float rootKey = 60.0f;
    float fineCents = 0.0f;
    uint32_t rate = 32000;
    uint8_t keyLow = 0, keyHigh = 127;  // descriptor +0x03 / +0x00
    uint8_t velLayer = 0;               // descriptor +0x01
    uint8_t loopMode = 0;               // 0=fwd loop, 1=one-shot, 2=ping-pong, 3=reverse
    int group = -1;
    bool looped() const { return loopMode != 1 && loopEnd > loopStart + 2 && loopEnd <= len; }
    bool ready() const { return !pcm.empty() && len > 0; }
};

struct Multisample {
    std::string name;
    struct Zone {
        uint8_t keyHigh = 127;
        uint16_t waveIdx = 0;
    };
    std::vector<Zone> zones;
    const Wave* pickZone(const std::vector<Wave>& waves, int note) const {
        if (zones.empty()) return nullptr;
        uint16_t idx = zones.back().waveIdx;
        for (const auto& z : zones) {
            if (note <= z.keyHigh) { idx = z.waveIdx; break; }
        }
        if (idx < waves.size() && waves[idx].ready()) return &waves[idx];
        return nullptr;
    }
};

struct Rom {
    std::vector<uint8_t> bytes;
    std::vector<Wave> waves;
    std::vector<Multisample> multisamples;
    std::vector<int> groupOf;
    std::vector<std::vector<int>> groups;
    std::string kind;

    bool loadPeResource(const char* pePath, const char* resourceName);
    bool loadInternalAFromDll(const char* pePath);
    bool loadRaw(const char* path);
    bool parseWaveTable();
    bool parseMultisampleTable();
    void decodeAllWaves();
    void buildGroups();
    const Wave* pickMultisample(int msId1Based, int note) const {
        if (msId1Based < 1 || msId1Based > (int)multisamples.size()) return nullptr;
        return multisamples[msId1Based - 1].pickZone(waves, note);
    }
    int groupForWave(uint16_t id) const {
        if (id < groupOf.size()) return groupOf[id];
        return -1;
    }
};

// One SD-90/XV-5080 tone block as stored in the Koa bank record.
// Field offsets follow the XV-5080 MIDI Implementation tone map
// (11 00 20 00, total 0x109); the bank stores offsets 0x00..0x88 verbatim
// followed by the packed Step-LFO block (type + 16 steps).
struct KoaTone {
    bool valid = false;
    uint8_t level = 127;
    uint8_t coarse = 64;       // 64 = 0 semitones
    uint8_t fine = 64;         // 64 = 0 cents
    uint8_t randPitch = 0;
    uint8_t pan = 64;
    uint8_t panKeyfollow = 64;
    uint8_t randPan = 0;
    uint8_t altPan = 64;
    uint8_t envMode = 0;
    uint8_t delayMode = 0;
    uint16_t delayTime = 0;
    uint8_t drySend = 127, chorusMfx = 0, reverbMfx = 0;
    uint8_t chorusNon = 0, reverbNon = 0, outputAssign = 0;
    uint8_t rxBender = 0, rxExpression = 0, rxHold = 0, rxPanMode = 0, redamper = 0;
    uint8_t ctrlSwitch[4][4] = {};
    uint8_t wavGroupType = 0;   // 0 = OFF, 1 = INT (SD-90)
    uint16_t wavGroupId = 1;
    uint16_t wavIdL = 0, wavIdR = 0;   // 1-based InternalA wave numbers
    uint8_t waveGain = 1;       // 0=-6dB, 1=0dB, 2=+6dB, 3=+12dB
    uint8_t fxmSwitch = 0, fxmColor = 0, fxmDepth = 0, tempoSync = 0;
    uint8_t pitchKeyfollow = 74; // 64 = 0%, 74 = +100% (10%/step)
    uint8_t pitchEnvDepth = 64, pitchEnvVelSens = 64;
    uint8_t pitchEnvT1VelSens = 64, pitchEnvT4VelSens = 64, pitchEnvTimeKF = 64;
    uint8_t pitchEnvT[4] = {0, 0, 0, 0};
    uint8_t pitchEnvL[5] = {64, 64, 64, 64, 64};
    uint8_t tvfType = 0, tvfCutoff = 127, tvfCutoffKF = 64;
    uint8_t tvfCutoffVelCurve = 0, tvfCutoffVelSens = 64, tvfResonance = 0, tvfResoVelSens = 64;
    uint8_t tvfEnvDepth = 64, tvfEnvVelCurve = 0, tvfEnvVelSens = 64;
    uint8_t tvfEnvT1VelSens = 64, tvfEnvT4VelSens = 64, tvfEnvTimeKF = 64;
    uint8_t tvfEnvT[4] = {0, 0, 0, 0};
    uint8_t tvfEnvL[5] = {0, 0, 0, 0, 0};
    uint8_t biasLevel = 64, biasPos = 64, biasDir = 3;
    uint8_t tvaVelCurve = 0, tvaVelSens = 64;
    uint8_t tvaEnvT1VelSens = 64, tvaEnvT4VelSens = 64, tvaEnvTimeKF = 64;
    uint8_t tvaEnvT[4] = {0, 0, 0, 0};
    uint8_t tvaEnvL[3] = {127, 127, 0};
    uint8_t lfoWave[2] = {0, 0};
    uint16_t lfoRate[2] = {0, 0};       // high byte.int byte (e.g. 5.12)
    uint8_t lfoOffset[2] = {0, 0};
    uint8_t lfoRateDetune[2] = {0, 0};
    uint8_t lfoDelay[2] = {0, 0};
    uint8_t lfoDelayKF[2] = {64, 64};
    uint8_t lfoFadeMode[2] = {0, 0};
    uint8_t lfoFadeTime[2] = {0, 0};
    uint8_t lfoKeyTrigger[2] = {0, 0};
    int8_t lfoDepth[2][4] = {};         // pitch, TVF, TVA, pan (-63..+63)
    uint8_t lfoStepType = 0;
    uint8_t lfoStep[16] = {};
    bool tmtOn = true;
    uint8_t keyLow = 0, keyHigh = 127;
    uint8_t velLow = 1, velHigh = 127;

    bool active() const { return wavGroupType != 0 && wavIdL != 0; }
    double pitchKeyfollowRatio() const {
        return (int(pitchKeyfollow) - 64) * 0.1;
    }
    double pitchRatio() const {
        return pow(2.0, (int(coarse) - 64) / 12.0 + (int(fine) - 64) / 1200.0);
    }
};

struct RhythmLayer {
    bool active = false;
    uint16_t wavGroupId = 70;
    uint16_t wavIdL = 0, wavIdR = 0;
    uint8_t waveGain = 1;
    uint8_t coarse = 64, fine = 64, pan = 64, level = 127;
    uint8_t velLow = 1, velHigh = 127;
};

struct RhythmKey {
    bool valid = false;
    uint8_t envMode = 0;
    uint8_t level = 127;
    uint8_t coarse = 60;       // 60 = C4 (0 semitones)
    uint8_t fine = 64;         // 64 = 0 cents
    uint8_t pan = 64;
    uint8_t muteGroup = 0;
    uint8_t rxNoteOff = 1;
    uint8_t tvfType = 0;
    uint8_t tvfCutoff = 127;
    uint8_t tvfResonance = 0;
    uint8_t tvaEnvT[4] = {0, 40, 40, 40};
    uint8_t tvaEnvL[3] = {127, 127, 0};
    RhythmLayer layers[4];
};

struct Patch {
    std::string name;
    uint16_t category = 0;   // 0..35 wave family
    uint8_t variant = 0;     // index within family (for the SRX fallback)
    uint16_t waveId = 0;     // real tone 1 wave number (InternalA index)
    uint8_t level = 100;
    uint8_t pan = 64;
    KoaTone tones[4];
    RhythmKey rhythmKeys[128];
    bool isRhythm = false;
    bool valid = false;
    const KoaTone* firstActiveTone() const {
        for (const auto& t : tones) if (t.active()) return &t;
        return nullptr;
    }
};

struct Bank {
    std::string name;
    std::vector<Patch> patches;
    bool load(const char* path);
};

struct MidiEvent {
    double time = 0.0;
    int frames = 0;
    uint8_t data[16] = {0};
    int len = 0;
};

struct SysExEvent {
    double time = 0.0;
    std::vector<uint8_t> data;
};

struct MidiFile {
    std::vector<MidiEvent> events;
    std::vector<SysExEvent> sysex;
    double length = 0.0;
    bool load(const char* path);
};

class Synth {
public:
    bool init(const std::string& baseDir, bool verbose = true);
    void reset();
    void silence();
    void setSampleRate(double sr) { if (sr > 0 && sr != sampleRate_) { sampleRate_ = sr; if (ready_) initEffects(); } }
    double sampleRate() const { return sampleRate_; }
    void setMasterVolume(float v) { masterVolume_ = std::max(0.0f, std::min(2.0f, v)); }
    float masterVolume() const { return masterVolume_; }
    void setPartTune(int ch, float semitones);
    float partTune(int ch) const;
    void setMasterTuneHz(double a4Hz);
    double masterTuneHz() const { return 440.0 * std::pow(2.0, masterTuneSemis_ / 12.0); }
    void queueMidi(int deltaFrames, const uint8_t* msg, int len);
    void render(float* left, float* right, int frames);
    std::string patchName(int ch) const;
    std::string gmProgramName(int pc) const;
    struct PartStatus {
        int msb = 0;
        int lsb = 0;
        int program = 0;
        bool drum = false;
        bool hasSysEx = false;
        int volume = 100;
        int expression = 127;
        int pan = 64;
        float tuneSemis = 0.0f;
        int activeVoices = 0;
        std::string patch;
    };
    PartStatus partStatus(int ch) const;
    int patchCount() const;
    const std::string& lastError() const { return lastError_; }

    // diagnostics
    const Rom& melodicRom() const { return !internalA_.waves.empty() ? internalA_ : srx_; }
    const Rom& drumRom() const { return srx_; }
    const Rom& internalARom() const { return internalA_; }
    const Rom& srxRom() const { return srx_; }
    const Bank& bank(int i) const { return banks_[i]; }
    bool ready() const { return ready_; }

private:
    struct Voice {
        bool active = false;
        int part = 0;
        int note = 0;
        const Wave* wave = nullptr;
        int osc = 0; // 0=sample, 1=saw pad, 2=sine pad, 3=noise
        double phase = 0.0, phase2 = 0.0, freq = 0.0, baseFreq = 0.0;
        double pos = 0.0;
        double inc = 1.0, baseInc = 1.0;
        int dir = 1; // +1 forward, -1 backward (for ping-pong)
        float ampL = 0.0f, ampR = 0.0f;
        float baseAmp = 0.0f;
        float panOffset = 0.0f;
        float env = 0.0f;
        float envT[4] = {0.005f, 0.15f, 0.15f, 0.25f}; // T1..T4 seconds
        float envL[3] = {1.0f, 0.85f, 0.85f};          // L1..L3
        int stage = 0; // 0 attack, 1 decay1, 2 decay2, 3 sustain
        bool released = false;
        bool oneShot = false;
        float filtL = 0.0f, filtR = 0.0f;
        float filt2L = 0.0f, filt2R = 0.0f;
        float filtA = 0.7f, filt2A = 0.5f;
        uint8_t tvfType = 1;
        float tvfResDamp = 1.414f;
        float svfA1 = 0.25f, svfA2 = 0.25f, svfA3 = 0.25f;
        float svfLow = 0.0f, svfBand = 0.0f;
        float tvfBaseCut = 100.0f;
        float tvfDepth = 0.0f;
        float tvfEnv = 0.0f;
        float tvfEnvT[4] = {0.01f, 0.2f, 0.3f, 0.3f};
        float tvfEnvL[5] = {0.0f, 1.0f, 0.8f, 0.5f, 0.0f};
        int tvfStage = 0;
        int tvfTick = 0;
        float revSend = 0.25f;
        float choSend = 0.0f;
        float lfoPhase = 0.0f;
        float lfoRateHz = 5.0f;
        float lfoDelaySec = 0.0f;
        float lfoAgeSec = 0.0f;
        float lfoPitchSemis = 0.0f;
        int muteGroup = 0;
        uint64_t age = 0;
        int vel = 100;
    };
    struct Part {
        int bankMsb = 0, bankLsb = 0, program = 0;
        int bankIndex = 0;
        int patchIndex = 0;
        bool drum = false;
        float volume = 100.0f, expression = 127.0f;
        float pan = 64.0f;
        float partLevel = 100.0f;
        float reverbSend = 40.0f;
        float chorusSend = 0.0f;
        float modWheel = 0.0f;
        int rpnMsb = 127, rpnLsb = 127, dataMsb = 0, dataLsb = 0;
        int coarseTune = 0;
        float fineTuneCents = 0.0f;
        float userTuneSemis = 0.0f;
        int pitchBend = 0;
        int bendRange = 2;
        float bendMul = 1.0f;
        bool sustain = false;
        bool hasSysExPatch = false;
        Patch sysExPatch;
        std::vector<int> heldNotes;
    };

    // Stereo Reverb & Chorus DSP state (allocated in init/reset)
    struct Comb {
        std::vector<float> buf;
        size_t idx = 0;
        float lp = 0.0f;
        void resize(size_t n) { buf.assign(n, 0.0f); idx = 0; lp = 0.0f; }
        float process(float in, float fb, float damp) {
            float out = buf[idx];
            lp = out * (1.0f - damp) + lp * damp;
            buf[idx] = in + lp * fb;
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        size_t idx = 0;
        void resize(size_t n) { buf.assign(n, 0.0f); idx = 0; }
        float process(float in) {
            float b = buf[idx];
            float out = -in + b;
            buf[idx] = in + b * 0.5f;
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };
    Comb revCombL_[4], revCombR_[4];
    Allpass revApL_[2], revApR_[2];
    std::vector<float> choBufL_, choBufR_;
    size_t choIdx_ = 0;
    double choPhase_ = 0.0;
    void initEffects();
    void clearEffects();

    bool ready_ = false;
    bool verbose_ = true;
    bool useNativeSysex_ = true;
    double sampleRate_ = 44100.0;
    float masterVolume_ = 0.8f;
    double masterTuneSemis_ = 0.0;
    Rom internalA_, srx_;
    Bank banks_[11];
    Part parts_[16];
    Voice voices_[160];
    uint64_t ageCounter_ = 0;
    std::vector<MidiEvent> queue_;
    int partWave_[16];
    int partGain_[16];
    std::string lastError_;
    std::string baseDir_;
    int catOffset_[36];
    int catCount_[36];

    void selectPatch(int ch);
    void noteOn(int ch, int note, int vel);
    void noteOff(int ch, int note);
    void allNotesOff(int ch);
    void controlChange(int ch, int cc, int val);
    void pitchBend(int ch, int val);
    void sysEx(const uint8_t* data, int len);
    void applyGsReset();
    void setPartWave(int part, int waveId, int gain);
    void updatePartVolume(int ch);
    void updatePartPitch(int ch);
    double partTuneRatio(int ch) const;
    const Wave* pickWave(const Rom& rom, int group, int note) const;
    void startVoice(int part, int note, int vel, const Wave* w, const KoaTone* tone = nullptr, float panOverride = -1.0f, float gainScale = 1.0f);
    void applyTone(Voice& v, const KoaTone* t, int part, bool oneShot);
    void advanceEnv(Voice& v, float dt);
    void startOsc(int part, int note, int vel, int type);
    void renderVoice(Voice& v, float& outL, float& outR);
    bool findFile(const std::string& rel, std::string& out) const;
    bool readIniPath(const std::string& key, std::string& out) const;
};

// PE / ROM helpers exposed for tools
bool extractPeResource(const char* path, const char* name, std::vector<uint8_t>& out);
bool findWaveTable(const std::vector<uint8_t>& rom, size_t& headerOff, uint32_t& count);
