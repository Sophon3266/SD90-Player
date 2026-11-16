#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include "sd90.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::atomic<bool> gStopRequested{false};

static BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            gStopRequested.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

static std::string exeDirectory() {
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf, n);
    size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? "." : s.substr(0, pos);
}

static int16_t floatToPcm16(float x) {
    if (!(x == x)) return 0;
    if (x > 1.0f) x = 1.0f;
    else if (x < -1.0f) x = -1.0f;
    long v = std::lrintf(x * 32767.0f);
    if (v > 32767L) v = 32767L;
    if (v < -32768L) v = -32768L;
    return static_cast<int16_t>(v);
}

static void printUsage() {
    std::fprintf(stderr,
        "Usage: sd90play.exe <input.mid> [--rate 44100] [--vol 0.8] [--tail 2.5] [--a4 440] [--tune <ch|all>:<semis>] [--quiet]\n");
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

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    const char* midiPath = nullptr;
    double sampleRate = 44100.0;
    float masterVol = 0.8f;
    double tailSec = 2.5;
    double a4Hz = 440.0;
    struct TuneCmd { int ch; float semis; };
    std::vector<TuneCmd> tunes;
    bool verbose = true;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            sampleRate = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--vol") == 0 && i + 1 < argc) {
            masterVol = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            tailSec = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--a4") == 0 && i + 1 < argc) {
            a4Hz = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--tune") == 0 && i + 1 < argc) {
            TuneCmd tc{};
            if (!parseTuneArg(argv[++i], tc.ch, tc.semis)) {
                std::fprintf(stderr, "Invalid --tune spec (expected <1..16|all>:<semis>): %s\n", argv[i]);
                return 1;
            }
            tunes.push_back(tc);
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            verbose = false;
        } else if (argv[i][0] == '-') {
            printUsage();
            return 1;
        } else {
            midiPath = argv[i];
        }
    }

    if (!midiPath) {
        printUsage();
        return 1;
    }
    if (sampleRate < 8000.0 || sampleRate > 192000.0) sampleRate = 44100.0;
    if (tailSec < 0.0) tailSec = 0.0;

    MidiFile midi;
    if (!midi.load(midiPath)) {
        std::fprintf(stderr, "Failed to load MIDI: %s\n", midiPath);
        return 1;
    }

    Synth synth;
    synth.setSampleRate(sampleRate);
    std::string dir = exeDirectory();
    if (!synth.init(dir, verbose)) {
        std::fprintf(stderr, "Synth init failed: %s\n", synth.lastError().c_str());
        return 1;
    }
    synth.setMasterVolume(masterVol);
    synth.setMasterTuneHz(a4Hz);
    for (const auto& tc : tunes) {
        if (tc.ch < 0) {
            for (int c = 0; c < 16; ++c) synth.setPartTune(c, tc.semis);
        } else {
            synth.setPartTune(tc.ch, tc.semis);
        }
    }

    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    constexpr int kNumBufs = 8;
    constexpr int kBlockFrames = 1024;

    HANDLE doneEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!doneEvent) {
        std::fprintf(stderr, "CreateEventA failed\n");
        return 1;
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = static_cast<DWORD>(std::llround(sampleRate));
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    HWAVEOUT hWaveOut = nullptr;
    MMRESULT mr = waveOutOpen(
        &hWaveOut,
        WAVE_MAPPER,
        &wfx,
        reinterpret_cast<DWORD_PTR>(doneEvent),
        0,
        CALLBACK_EVENT
    );
    if (mr != MMSYSERR_NOERROR || !hWaveOut) {
        std::fprintf(stderr, "waveOutOpen failed: mmresult=%u\n", static_cast<unsigned>(mr));
        CloseHandle(doneEvent);
        return 1;
    }

    struct AudioBuf {
        WAVEHDR hdr{};
        std::vector<int16_t> pcm;
        bool prepared = false;
    };
    std::vector<AudioBuf> bufs(kNumBufs);
    for (int i = 0; i < kNumBufs; ++i) {
        bufs[i].pcm.resize(kBlockFrames * 2, 0);
        bufs[i].hdr.lpData = reinterpret_cast<LPSTR>(bufs[i].pcm.data());
        bufs[i].hdr.dwBufferLength = static_cast<DWORD>(bufs[i].pcm.size() * sizeof(int16_t));
        bufs[i].hdr.dwFlags = WHDR_DONE;
    }

    std::vector<float> left(kBlockFrames, 0.0f);
    std::vector<float> right(kBlockFrames, 0.0f);

    const uint64_t totalFrames = static_cast<uint64_t>(std::ceil((midi.length + tailSec) * sampleRate));
    size_t evIdx = 0;
    size_t sxIdx = 0;
    uint64_t curFrame = 0;
    int nextBuf = 0;

    if (verbose) {
        std::printf("Playing: %s (%.2f s + %.1f s tail, %u Hz, events=%zu, sysex=%zu)\n",
            midiPath, midi.length, tailSec, static_cast<unsigned>(wfx.nSamplesPerSec),
            midi.events.size(), midi.sysex.size());
        std::fflush(stdout);
    }

    while (curFrame < totalFrames && !gStopRequested.load()) {
        AudioBuf& b = bufs[nextBuf];
        while ((b.hdr.dwFlags & WHDR_DONE) == 0 && !gStopRequested.load()) {
            WaitForSingleObject(doneEvent, 50);
        }
        if (gStopRequested.load()) break;

        if (b.prepared) {
            waveOutUnprepareHeader(hWaveOut, &b.hdr, sizeof(WAVEHDR));
            b.prepared = false;
        }

        const int frames = static_cast<int>(std::min<uint64_t>(kBlockFrames, totalFrames - curFrame));

        while (sxIdx < midi.sysex.size()) {
            uint64_t sf = static_cast<uint64_t>(std::llround(midi.sysex[sxIdx].time * sampleRate));
            if (sf >= curFrame + static_cast<uint64_t>(frames)) break;
            if (!midi.sysex[sxIdx].data.empty()) {
                synth.queueMidi(0, midi.sysex[sxIdx].data.data(), static_cast<int>(midi.sysex[sxIdx].data.size()));
            }
            ++sxIdx;
        }

        while (evIdx < midi.events.size()) {
            uint64_t ef = static_cast<uint64_t>(std::llround(midi.events[evIdx].time * sampleRate));
            if (ef >= curFrame + static_cast<uint64_t>(frames)) break;
            int delta = ef > curFrame ? static_cast<int>(ef - curFrame) : 0;
            synth.queueMidi(delta, midi.events[evIdx].data, midi.events[evIdx].len);
            ++evIdx;
        }

        synth.render(left.data(), right.data(), frames);

        for (int i = 0; i < frames; ++i) {
            b.pcm[i * 2 + 0] = floatToPcm16(left[i]);
            b.pcm[i * 2 + 1] = floatToPcm16(right[i]);
        }

        std::memset(&b.hdr, 0, sizeof(WAVEHDR));
        b.hdr.lpData = reinterpret_cast<LPSTR>(b.pcm.data());
        b.hdr.dwBufferLength = static_cast<DWORD>(frames * 2 * sizeof(int16_t));
        mr = waveOutPrepareHeader(hWaveOut, &b.hdr, sizeof(WAVEHDR));
        if (mr != MMSYSERR_NOERROR) {
            std::fprintf(stderr, "waveOutPrepareHeader failed: %u\n", static_cast<unsigned>(mr));
            break;
        }
        b.prepared = true;

        mr = waveOutWrite(hWaveOut, &b.hdr, sizeof(WAVEHDR));
        if (mr != MMSYSERR_NOERROR) {
            std::fprintf(stderr, "waveOutWrite failed: %u\n", static_cast<unsigned>(mr));
            break;
        }

        curFrame += static_cast<uint64_t>(frames);
        nextBuf = (nextBuf + 1) % kNumBufs;
    }

    if (gStopRequested.load()) {
        waveOutReset(hWaveOut);
    } else {
        for (int pass = 0; pass < 100; ++pass) {
            bool anyPending = false;
            for (const auto& b : bufs) {
                if (b.prepared && (b.hdr.dwFlags & WHDR_DONE) == 0) {
                    anyPending = true;
                    break;
                }
            }
            if (!anyPending) break;
            WaitForSingleObject(doneEvent, 50);
        }
    }

    for (auto& b : bufs) {
        if (b.prepared) {
            waveOutUnprepareHeader(hWaveOut, &b.hdr, sizeof(WAVEHDR));
            b.prepared = false;
        }
    }

    waveOutClose(hWaveOut);
    CloseHandle(doneEvent);

    if (verbose) {
        std::printf(gStopRequested.load() ? "Stopped.\n" : "Done.\n");
    }
    return 0;
}
