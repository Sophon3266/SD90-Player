// Standalone MIDI renderer and VST2 host smoke tester.
// Build twice: sd90render.exe (direct engine) and vsthost_test.exe (loads the DLL).
#include "sd90.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

#pragma pack(push, 8)
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
typedef intptr_t (*AEffectDispatcherProc)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
typedef void (*AEffectProcessProc)(AEffect*, float**, float**, int32_t);
struct AEffect {
    int32_t magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;
    void* setParameter;
    void* getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void* object; void* user;
    int32_t uniqueID, version;
    AEffectProcessProc processReplacing;
    void* processDoubleReplacing;
    char future[56];
};
struct VstEvent {
    int32_t type, byteSize, deltaFrames, flags;
    char data[16];
};
struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent* events[2];
};
struct VstMidiEvent {
    int32_t type, byteSize, deltaFrames, flags;
    int32_t noteLength, noteOffset;
    char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
};
struct VstMidiSysexEvent {
    int32_t type, byteSize, deltaFrames, flags;
    int32_t dumpBytes;
    intptr_t resvd1;
    char* sysexDump;
    intptr_t resvd2;
};
#pragma pack(pop)

static std::string exeDir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : s.substr(0, p);
}

static bool writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r, int sr) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    auto u32 = [](uint32_t v) { return v; };
    auto u16 = [](uint16_t v) { return v; };
    uint32_t dataBytes = (uint32_t)(l.size() * 4);
    uint32_t riffSize = 36 + dataBytes;
    uint16_t ch = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); uint32_t fmtSize = 16; fwrite(&fmtSize, 4, 1, f);
    uint16_t pcm = 1; fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f); uint32_t byteRate = sr * 4; fwrite(&byteRate, 4, 1, f);
    uint16_t align = 4; fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    for (size_t i = 0; i < l.size(); ++i) {
        float a = std::max(-1.0f, std::min(1.0f, l[i]));
        float b = std::max(-1.0f, std::min(1.0f, r[i]));
        int16_t x = (int16_t)std::lround(a * 32767.0f);
        int16_t y = (int16_t)std::lround(b * 32767.0f);
        fwrite(&x, 2, 1, f); fwrite(&y, 2, 1, f);
    }
    fclose(f);
    return true;
}

static void printUsage(const char* exe) {
    fprintf(stderr,
            "usage:\n"
            "  %s <midi> <out.wav> [rom] [banks] [--a4 <hz>] [--tune <ch|all>:<semis>]\n"
            "  %s --vst <dll> <midi> <out.wav>\n"
            "  %s --dump-banks\n", exe, exe, exe);
}

static bool parseTuneArg(const char* spec, int& outCh, float& outSemis) {
    if (!spec) return false;
    const char* colon = std::strchr(spec, ':');
    if (!colon) return false;
    std::string target(spec, colon - spec);
    if (target == "all" || target == "ALL" || target == "*") {
        outCh = -1;
    } else {
        char* endp = nullptr;
        long ch = std::strtol(target.c_str(), &endp, 10);
        if (!endp || *endp != '\0' || ch < 1 || ch > 16) return false;
        outCh = int(ch - 1);
    }
    char* endp = nullptr;
    double val = std::strtod(colon + 1, &endp);
    if (!endp || *endp != '\0') return false;
    outSemis = float(val);
    return true;
}

static int runEngine(int argc, char** argv) {
    if (argc < 3) { printUsage(argv[0]); return 2; }
    const char* midiPath = nullptr;
    const char* outPath = nullptr;
    double a4Hz = 440.0;
    struct TuneCmd { int ch; float semis; };
    std::vector<TuneCmd> tunes;
    int pos = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--a4") == 0 && i + 1 < argc) {
            a4Hz = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--tune") == 0 && i + 1 < argc) {
            TuneCmd tc{};
            if (!parseTuneArg(argv[++i], tc.ch, tc.semis)) {
                fprintf(stderr, "invalid --tune spec (expected <1..16|all>:<semis>): %s\n", argv[i]);
                return 2;
            }
            tunes.push_back(tc);
        } else {
            if (pos == 0) midiPath = argv[i];
            else if (pos == 1) outPath = argv[i];
            ++pos;
        }
    }
    if (!midiPath || !outPath) { printUsage(argv[0]); return 2; }
    MidiFile midi;
    if (!midi.load(midiPath)) {
        fprintf(stderr, "failed to load MIDI: %s\n", midiPath);
        return 3;
    }
    Synth synth;
    std::string base = exeDir();
    if (!synth.init(base, true)) {
        fprintf(stderr, "engine init failed: %s\n", synth.lastError().c_str());
        return 4;
    }
    synth.setSampleRate(44100.0);
    synth.setMasterVolume(0.8f);
    synth.reset();
    synth.setMasterTuneHz(a4Hz);
    for (const auto& tc : tunes) {
        if (tc.ch < 0) {
            for (int c = 0; c < 16; ++c) synth.setPartTune(c, tc.semis);
        } else {
            synth.setPartTune(tc.ch, tc.semis);
        }
    }

    const int sr = 44100;
    const int block = 512;
    std::vector<float> outL, outR;
    outL.reserve((size_t)(midi.length * sr) + block);
    outR.reserve((size_t)(midi.length * sr) + block);
    size_t ei = 0, si = 0;
    double t = 0.0;
    const double end = midi.length + 2.0;
    float l[block], r[block];
    while (t < end) {
        int n = block;
        double blockEnd = t + double(n) / sr;
        while (si < midi.sysex.size() && midi.sysex[si].time < blockEnd) {
            const SysExEvent& sx = midi.sysex[si];
            synth.queueMidi(0, sx.data.data(), (int)sx.data.size());
            ++si;
        }
        while (ei < midi.events.size() && midi.events[ei].time < blockEnd) {
            const MidiEvent& e = midi.events[ei];
            int d = (int)std::lround((e.time - t) * sr);
            if (d < 0) d = 0;
            if (d >= n) d = n - 1;
            synth.queueMidi(d, e.data, e.len);
            ++ei;
        }
        synth.render(l, r, n);
        outL.insert(outL.end(), l, l + n);
        outR.insert(outR.end(), r, r + n);
        t = blockEnd;
    }
    if (!writeWav(outPath, outL, outR, sr)) {
        fprintf(stderr, "failed to write %s\n", outPath);
        return 5;
    }
    double rms = 0; for (size_t i = 0; i < outL.size(); ++i) rms += double(outL[i]) * outL[i] + double(outR[i]) * outR[i];
    rms = outL.empty() ? 0 : sqrt(rms / (2.0 * outL.size()));
    fprintf(stderr, "rendered %zu frames, RMS %.5f -> %s\n", outL.size(), rms, outPath);
    return rms > 1e-5 ? 0 : 6;
}

struct HostTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
static HostTimeInfo gHostTime = {0, 44100.0, 0, 0, 120.0, 0, 0, 0, 4, 4, 0, 0, 0, 0};
static std::string gHostDir;
static intptr_t __stdcall hostAudioMaster(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    (void)index; (void)value; (void)ptr; (void)opt;
    switch (opcode) {
    case 1: return 2400;                 // audioMasterVersion
    case 7: return (intptr_t)&gHostTime; // audioMasterGetTime
    case 17: return 44100;               // audioMasterGetSampleRate
    case 18: return 512;                 // audioMasterGetBlockSize
    case 19: case 20: return 0;          // latencies
    case 23: return 1;                   // willReplaceOrAccumulate
    case 24: return 2;                   // currentProcessLevel = realtime
    case 25: return 1;                   // automationState = read
    case 36: return (intptr_t)gHostDir.c_str(); // audioMasterGetDirectory
    default: return 0;
    }
}

static int runDumpBanks() {
    Synth synth;
    std::string base = exeDir();
    if (!synth.init(base, false)) {
        fprintf(stderr, "engine init failed: %s\n", synth.lastError().c_str());
        return 4;
    }
    for (int bi = 0; bi < 11; ++bi) {
        const Bank& b = synth.bank(bi);
        if (b.patches.empty()) continue;
        printf("BANK %02d %s (%s)\n", bi + 1, b.name.c_str(), b.patches.empty() ? "?" : "KoaBankFile");
        for (int pi = 0; pi < (int)b.patches.size(); ++pi) {
            const Patch& p = b.patches[pi];
            if (!p.valid) continue;
            printf("  %3d %-12s cat=%2u lvl=%3u pan=%2u wave=%u\n",
                   pi, p.name.c_str(), p.category, p.level, p.pan, p.waveId);
            for (int ti = 0; ti < 4; ++ti) {
                const KoaTone& t = p.tones[ti];
                if (!t.active()) continue;
                printf("       t%d grp=%u/%u wave=%u/%u lvl=%u coarse=%+d fine=%+d pan=%u "
                       "TVF=%u cut=%u res=%u env=%u,%u,%u,%u/%u,%u,%u "
                       "LFO=%u@%u.%02u\n",
                       ti + 1, t.wavGroupType, t.wavGroupId, t.wavIdL, t.wavIdR,
                       t.level, int(t.coarse) - 64, int(t.fine) - 64, t.pan,
                       t.tvfType, t.tvfCutoff, t.tvfResonance,
                       t.tvfEnvT[0], t.tvfEnvT[1], t.tvfEnvT[2], t.tvfEnvT[3],
                       t.tvfEnvL[0], t.tvfEnvL[1], t.tvfEnvL[2],
                       t.lfoWave[0], t.lfoRate[0] >> 8, t.lfoRate[0] & 0xFF);
            }
        }
    }
    return 0;
}

static int runVstHost(int argc, char** argv) {
    if (argc < 5) { printUsage(argv[0]); return 2; }
    const char* dllPath = argv[2];
    const char* midiPath = argv[3];
    const char* outPath = argv[4];
    std::string dllDir = dllPath;
    size_t slash = dllDir.find_last_of("\\/");
    if (slash != std::string::npos) dllDir = dllDir.substr(0, slash);
    SetCurrentDirectoryA(dllDir.c_str());
    SetDllDirectoryA(dllDir.c_str());
    gHostDir = dllDir;
    LoadLibraryA("RLCLEMU.dll");
    HMODULE h = LoadLibraryA(dllPath);
    if (!h) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 3; }
    typedef AEffect* (*VstMain)(audioMasterCallback);
    VstMain mainFn = (VstMain)GetProcAddress(h, "VSTPluginMain");
    if (!mainFn) mainFn = (VstMain)GetProcAddress(h, "main");
    if (!mainFn) { fprintf(stderr, "VSTPluginMain not exported\n"); return 4; }
    OleInitialize(nullptr);
    AEffect* eff = mainFn(hostAudioMaster);
    if (!eff || eff->magic != 0x56737450) { fprintf(stderr, "bad AEffect\n"); return 5; }
    eff->dispatcher(eff, 0, 0, 0, nullptr, 0.0f);          // effOpen
    eff->dispatcher(eff, 10, 0, 0, nullptr, 44100.0f);     // effSetSampleRate
    eff->dispatcher(eff, 11, 0, 512, nullptr, 0.0f);       // effSetBlockSize
    eff->dispatcher(eff, 12, 0, 1, nullptr, 0.0f);         // effMainsChanged on

    MidiFile midi;
    if (!midi.load(midiPath)) { fprintf(stderr, "failed to load MIDI: %s\n", midiPath); return 6; }

    const int sr = 44100, block = 512;
    std::vector<float> outL, outR, l(block), r(block);
    size_t ei = 0, si = 0;
    double t = 0.0, end = midi.length + 2.0;
    float* outputs[2] = { l.data(), r.data() };
    while (t < end) {
        double blockEnd = t + double(block) / sr;
        alignas(16) char evBuf[sizeof(VstEvents) + sizeof(VstEvent*) * 1024];
        VstEvents* evs = (VstEvents*)evBuf;
        evs->numEvents = 0; evs->reserved = 0;
        static VstMidiEvent mev[1024];
        static VstMidiSysexEvent sev[1024];
        int nEv = 0, nSy = 0;
        while (si < midi.sysex.size() && midi.sysex[si].time < blockEnd && nSy < 1024 && nEv < 1024) {
            const SysExEvent& sx = midi.sysex[si];
            VstMidiSysexEvent& s = sev[nSy];
            memset(&s, 0, sizeof(s));
            s.type = 6;
            s.byteSize = sizeof(VstMidiSysexEvent);
            s.deltaFrames = 0;
            s.dumpBytes = (int32_t)sx.data.size();
            s.sysexDump = (char*)sx.data.data();
            evs->events[nEv] = (VstEvent*)&s;
            nEv++; nSy++;
            ++si;
        }
        while (ei < midi.events.size() && midi.events[ei].time < blockEnd && nEv < 1024) {
            const MidiEvent& e = midi.events[ei];
            if (e.len >= 1 && e.data[0] != 0xF0) {
                VstMidiEvent& m = mev[nEv];
                memset(&m, 0, sizeof(m));
                m.type = 1;
                m.byteSize = sizeof(VstMidiEvent);
                int d = (int)std::lround((e.time - t) * sr);
                if (d < 0) d = 0;
                if (d >= block) d = block - 1;
                m.deltaFrames = d;
                memcpy(m.midiData, e.data, std::min(e.len, 3));
                evs->events[nEv] = (VstEvent*)&m;
                nEv++;
            }
            ++ei;
        }
        evs->numEvents = nEv;
        if (nEv) eff->dispatcher(eff, 25, 0, 0, evs, 0.0f); // effProcessEvents
        eff->processReplacing(eff, nullptr, outputs, block);
        outL.insert(outL.end(), l.begin(), l.end());
        outR.insert(outR.end(), r.begin(), r.end());
        t = blockEnd;
    }
    writeWav(outPath, outL, outR, sr);
    double rms = 0; for (size_t i = 0; i < outL.size(); ++i) rms += double(outL[i]) * outL[i] + double(outR[i]) * outR[i];
    rms = outL.empty() ? 0 : sqrt(rms / (2.0 * outL.size()));
    fprintf(stderr, "VST host rendered %zu frames, RMS %.5f -> %s\n", outL.size(), rms, outPath);
    eff->dispatcher(eff, 12, 0, 0, nullptr, 0.0f);
    eff->dispatcher(eff, 1, 0, 0, nullptr, 0.0f);
    FreeLibrary(h);
    return rms > 1e-5 ? 0 : 7;
}

static LONG WINAPI vehLogger(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        HMODULE hm = nullptr;
        char path[MAX_PATH] = {0};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &hm)) {
            GetModuleFileNameA(hm, path, MAX_PATH);
        }
        fprintf(stderr, "AV at %p module=%s\n", addr, path);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandledLogger(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("C:/temp/sd8_crash.txt", "a");
    if (f) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        HMODULE hm = nullptr; char path[MAX_PATH] = {0};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &hm))
            GetModuleFileNameA(hm, path, MAX_PATH);
        fprintf(f, "code=%08lx addr=%p module=%s\n", ep->ExceptionRecord->ExceptionCode, addr, path);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    AddVectoredExceptionHandler(1, vehLogger);
    SetUnhandledExceptionFilter(unhandledLogger);
    if (argc >= 2 && !strcmp(argv[1], "--vst")) return runVstHost(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--dump-banks")) return runDumpBanks();
    return runEngine(argc, argv);
}
