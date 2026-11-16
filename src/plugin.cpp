// Minimal VST2.4 instrument wrapper for SD90Player.
#include "sd90.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>

#pragma pack(push, 8)
struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
};
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
typedef intptr_t (*AEffectDispatcherProc)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
typedef void (*AEffectProcessProc)(AEffect*, float**, float**, int32_t);
typedef void (*AEffectProcessDoubleProc)(AEffect*, double**, double**, int32_t);
typedef void (*AEffectSetParameterProc)(AEffect*, int32_t, float);
typedef float (*AEffectGetParameterProc)(AEffect*, int32_t);

struct AEffect {
    int32_t magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueID;
    int32_t version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char future[56];
};

struct VstEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    char data[16];
};
struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent* events[2];
};
struct VstMidiEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t noteLength;
    int32_t noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};
struct VstMidiSysexEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t dumpBytes;
    intptr_t resvd1;
    char* sysexDump;
    intptr_t resvd2;
};
#pragma pack(pop)

enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3,
    effSetProgramName = 4, effGetProgramName = 5,
    effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effGetVu = 9, effSetSampleRate = 10, effSetBlockSize = 11,
    effMainsChanged = 12, effEditGetRect = 13, effEditOpen = 14, effEditClose = 15,
    effEditIdle = 19, effGetChunk = 23, effSetChunk = 24,
    effProcessEvents = 25, effCanBeAutomated = 26,
    effGetNumProgramCategories = 28, effGetProgramNameIndexed = 29,
    effGetPlugCategory = 35, effGetCurrentPosition = 36,
    effSetSpeakerArrangement = 38, effGetSpeakerArrangement = 39,
    effGetMidiProgramName = 40, effGetCurrentMidiProgram = 41,
    effGetMidiProgramCategory = 42, effHasMidiProgramsChanged = 43,
    effGetMidiKeyName = 44, effGetEffectName = 45,
    effGetVendorString = 47, effGetProductString = 48, effGetVendorVersion = 49,
    effVendorSpecific = 50, effCanDo = 51, effGetTailSize = 52,
    effGetVstVersion = 58
};
static const int32_t kEffectMagic = 0x56737450; // 'VstP'
static const int32_t kPlugCategSynth = 2;
static const int32_t kVstMidiType = 1;
static const int32_t kVstSysexType = 6;

struct PluginInstance {
    AEffect effect{};
    Synth synth;
    int32_t blockSize = 1024;
    double sampleRate = 44100.0;
    bool inited = false;
    std::string dllDir;
    float masterParam = 0.4f; // 0..1 -> 0..2
    std::mutex lock;

    // 16-channel VST Editor UI state
    ERect editRect{ 0, 0, 412, 670 };
    HWND editHwnd = nullptr;
    Synth::PartStatus uiParts[16]{};
    double uiSampleRate = 44100.0;
    double uiMasterTuneHz = 440.0;
    float uiMasterParam = 0.4f;
    bool uiInited = false;
};

static std::string dllDirectory(HMODULE* outModule = nullptr) {
    char buf[MAX_PATH] = {0};
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&dllDirectory, &hm)) {
        if (outModule) *outModule = hm;
        GetModuleFileNameA(hm, buf, MAX_PATH);
        std::string s(buf);
        size_t p = s.find_last_of("\\/");
        if (p != std::string::npos) s = s.substr(0, p);
        return s;
    }
    return ".";
}

static PluginInstance* getInstance(AEffect* effect) {
    return effect ? static_cast<PluginInstance*>(effect->object) : nullptr;
}

static void ensureInit(PluginInstance* inst) {
    if (!inst || inst->inited) return;
    if (inst->dllDir.empty()) inst->dllDir = dllDirectory();
    inst->synth.setSampleRate(inst->sampleRate);
    inst->synth.init(inst->dllDir, false);
    inst->synth.setMasterVolume(inst->masterParam * 2.0f);
    inst->inited = true;
}

static int midiShortLength(uint8_t status) {
    uint8_t hi = status & 0xF0;
    if (hi == 0xC0 || hi == 0xD0) return 2;
    if (status == 0xF1 || status == 0xF3) return 2;
    if (status >= 0xF4) return 1;
    return 3;
}

static void refreshUiSnapshot(PluginInstance* inst) {
    if (!inst) return;
    std::unique_lock<std::mutex> lk(inst->lock, std::try_to_lock);
    if (!lk.owns_lock()) return;
    inst->uiInited = inst->inited;
    inst->uiSampleRate = inst->sampleRate;
    inst->uiMasterParam = inst->masterParam;
    if (inst->inited) {
        inst->uiMasterTuneHz = inst->synth.masterTuneHz();
        for (int ch = 0; ch < 16; ++ch) {
            inst->uiParts[ch] = inst->synth.partStatus(ch);
        }
    }
}

static void applyMasterTuneStep(PluginInstance* inst, double deltaHz, bool resetToDefault = false) {
    if (!inst) return;
    std::lock_guard<std::mutex> guard(inst->lock);
    ensureInit(inst);
    double next = 440.0;
    if (!resetToDefault) {
        double cur = inst->synth.masterTuneHz();
        next = std::round((cur + deltaHz) * 10.0) / 10.0;
        if (next < 400.0) next = 400.0;
        if (next > 480.0) next = 480.0;
    }
    inst->synth.setMasterTuneHz(next);
}

static void handleEditorClick(HWND hwnd, PluginInstance* inst, int x, int y, UINT msg, WPARAM wp) {
    if (!inst) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    if (W <= 0) return;

    bool changed = false;
    bool fineA4 = (msg == WM_RBUTTONDOWN) || ((wp & MK_SHIFT) != 0);
    double stepA4 = fineA4 ? 0.1 : 1.0;

    // Header A4 controls: [-] [A4: %.1f Hz] [+]
    if (y >= 6 && y <= 28 && x >= W - 170 && x <= W - 150) {
        applyMasterTuneStep(inst, -stepA4, msg == WM_MBUTTONDOWN);
        changed = true;
    } else if (y >= 6 && y <= 28 && x >= W - 32 && x <= W - 12) {
        applyMasterTuneStep(inst, +stepA4, msg == WM_MBUTTONDOWN);
        changed = true;
    } else if (y >= 6 && y <= 28 && x >= W - 148 && x <= W - 34) {
        std::lock_guard<std::mutex> guard(inst->lock);
        ensureInit(inst);
        if (msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
            inst->synth.setMasterTuneHz(440.0);
        } else {
            double cur = inst->synth.masterTuneHz();
            double next = 440.0;
            if (std::fabs(cur - 440.0) < 0.25) next = 432.0;
            else if (std::fabs(cur - 432.0) < 0.25) next = 452.0;
            else next = 440.0;
            inst->synth.setMasterTuneHz(next);
        }
        changed = true;
    } else if (y >= 58 && y < 58 + 16 * 22) {
        int ch = (y - 58) / 22;
        if (ch >= 0 && ch < 16) {
            bool fine = (msg == WM_RBUTTONDOWN) || ((wp & MK_SHIFT) != 0);
            float step = fine ? 0.10f : 1.0f;
            if (x >= W - 166 && x <= W - 148) {
                std::lock_guard<std::mutex> guard(inst->lock);
                ensureInit(inst);
                float cur = inst->synth.partTune(ch);
                inst->synth.setPartTune(ch, (msg == WM_MBUTTONDOWN) ? 0.0f : (cur - step));
                changed = true;
            } else if (x >= W - 90 && x <= W - 72) {
                std::lock_guard<std::mutex> guard(inst->lock);
                ensureInit(inst);
                float cur = inst->synth.partTune(ch);
                inst->synth.setPartTune(ch, (msg == WM_MBUTTONDOWN) ? 0.0f : (cur + step));
                changed = true;
            } else if (x >= W - 146 && x <= W - 92) {
                std::lock_guard<std::mutex> guard(inst->lock);
                ensureInit(inst);
                inst->synth.setPartTune(ch, 0.0f);
                changed = true;
            }
        }
    }

    if (changed) {
        refreshUiSnapshot(inst);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

static LRESULT CALLBACK editorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTA* cs = reinterpret_cast<CREATESTRUCTA*>(lp);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    PluginInstance* inst = reinterpret_cast<PluginInstance*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        handleEditorClick(hwnd, inst, int(short(LOWORD(lp))), int(short(HIWORD(lp))), msg, wp);
        return 0;
    case WM_MOUSEWHEEL: {
        if (inst) {
            POINT pt{ int(short(LOWORD(lp))), int(short(HIWORD(lp))) };
            ScreenToClient(hwnd, &pt);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int W = rc.right - rc.left;
            if (pt.y >= 0 && pt.y <= 34 && pt.x >= W - 170 && pt.x <= W - 12) {
                int delta = int(short(HIWORD(wp)));
                if (delta != 0) {
                    bool fine = (LOWORD(wp) & MK_SHIFT) != 0;
                    double step = (delta > 0 ? 1.0 : -1.0) * (fine ? 0.1 : 1.0);
                    applyMasterTuneStep(inst, step);
                    refreshUiSnapshot(inst);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int W = rc.right - rc.left;
        int H = rc.bottom - rc.top;
        if (W <= 0 || H <= 0) { EndPaint(hwnd, &ps); return 0; }

        if (inst) refreshUiSnapshot(inst);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
        HGDIOBJ oldBmp = SelectObject(memDC, bmp);

        HBRUSH bgBrush = CreateSolidBrush(RGB(22, 25, 31));
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        HFONT hdrFont = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        HFONT subFont = CreateFontA(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        HFONT rowFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

        SetBkMode(memDC, TRANSPARENT);

        // Header bar
        RECT hdrRect{ 0, 0, W, 34 };
        HBRUSH hdrBrush = CreateSolidBrush(RGB(34, 40, 50));
        FillRect(memDC, &hdrRect, hdrBrush);
        DeleteObject(hdrBrush);

        int totalVoices = 0;
        if (inst) {
            for (int ch = 0; ch < 16; ++ch) totalVoices += inst->uiParts[ch].activeVoices;
        }
        char title[160];
        std::snprintf(title, sizeof(title),
            "SD-90 Player (SD-8/XV)  |  SR: %.0f  |  Voices: %2d  |  Vol: %.0f%%",
            inst ? inst->uiSampleRate : 44100.0,
            totalVoices,
            inst ? inst->uiMasterParam * 200.0f : 80.0f);
        HGDIOBJ oldFont = SelectObject(memDC, hdrFont);
        SetTextColor(memDC, RGB(230, 238, 248));
        RECT titleRc{ 12, 8, W - 176, 30 };
        DrawTextA(memDC, title, -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // A4 master tune controls: [-] [A4: %.1f Hz] [+]
        double a4 = inst ? inst->uiMasterTuneHz : 440.0;
        bool a4Custom = std::fabs(a4 - 440.0) >= 0.05;
        HBRUSH hdrBtnBrush = CreateSolidBrush(RGB(46, 54, 68));
        RECT rA4Minus{ W - 170, 6, W - 150, 28 };
        RECT a4Rc{ W - 148, 6, W - 34, 28 };
        RECT rA4Plus{ W - 32, 6, W - 12, 28 };
        FillRect(memDC, &rA4Minus, hdrBtnBrush);
        FillRect(memDC, &rA4Plus, hdrBtnBrush);
        DeleteObject(hdrBtnBrush);
        HBRUSH a4Brush = CreateSolidBrush(a4Custom ? RGB(64, 92, 130) : RGB(48, 56, 70));
        FillRect(memDC, &a4Rc, a4Brush);
        DeleteObject(a4Brush);
        char a4Str[32];
        std::snprintf(a4Str, sizeof(a4Str), "A4: %.1f Hz", a4);
        SelectObject(memDC, subFont);
        SetTextColor(memDC, RGB(215, 225, 240));
        DrawTextA(memDC, "-", -1, &rA4Minus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextA(memDC, "+", -1, &rA4Plus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(memDC, a4Custom ? RGB(255, 225, 135) : RGB(210, 222, 238));
        DrawTextA(memDC, a4Str, -1, &a4Rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Column header
        RECT colRect{ 0, 34, W, 56 };
        HBRUSH colBrush = CreateSolidBrush(RGB(28, 32, 40));
        FillRect(memDC, &colRect, colBrush);
        DeleteObject(colBrush);
        SelectObject(memDC, subFont);
        SetTextColor(memDC, RGB(145, 165, 190));
        RECT colTxtRc{ 12, 36, W - 170, 54 };
        DrawTextA(memDC, "CH    BANK(M/L)  PC   TIMBRE / PATCH NAME          VOL  EXP  PAN", -1, &colTxtRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT colTuneRc{ W - 166, 36, W - 72, 54 };
        DrawTextA(memDC, "TUNE(st)", -1, &colTuneRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT colActRc{ W - 62, 36, W - 12, 54 };
        DrawTextA(memDC, "ACT", -1, &colActRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDC, rowFont);
        HBRUSH evenBrush = CreateSolidBrush(RGB(24, 28, 35));
        HBRUSH oddBrush = CreateSolidBrush(RGB(28, 33, 41));
        HBRUSH actRowBrush = CreateSolidBrush(RGB(34, 46, 56));
        HBRUSH btnBrush = CreateSolidBrush(RGB(46, 54, 68));
        HBRUSH meterBgBrush = CreateSolidBrush(RGB(42, 48, 58));
        HBRUSH meterFillBrush = CreateSolidBrush(RGB(72, 208, 136));
        HBRUSH drumMeterBrush = CreateSolidBrush(RGB(240, 175, 68));

        for (int ch = 0; ch < 16; ++ch) {
            int y0 = 58 + ch * 22;
            RECT rRow{ 6, y0, W - 6, y0 + 21 };
            const Synth::PartStatus* st = inst ? &inst->uiParts[ch] : nullptr;
            bool active = st && st->activeVoices > 0;
            FillRect(memDC, &rRow, active ? actRowBrush : ((ch & 1) ? oddBrush : evenBrush));

            char panStr[8] = "  C";
            if (st) {
                int dp = st->pan - 64;
                if (dp < 0) std::snprintf(panStr, sizeof(panStr), "L%2d", -dp);
                else if (dp > 0) std::snprintf(panStr, sizeof(panStr), "R%2d", dp);
            }
            const char* pName = (st && !st->patch.empty()) ? st->patch.c_str() : (inst && inst->uiInited ? "(none)" : "(loading...)");
            char line[160];
            if (st) {
                std::snprintf(line, sizeof(line),
                    "%02d%s %03d/%03d   %03d  %-26.26s  %3d  %3d  %s",
                    ch + 1,
                    st->drum ? "*" : " ",
                    st->msb, st->lsb, st->program + 1,
                    pName,
                    st->volume, st->expression, panStr);
            } else {
                std::snprintf(line, sizeof(line), "%02d  000/000   001  %-26.26s  100  127    C", ch + 1, pName);
            }

            SetTextColor(memDC, active ? RGB(255, 255, 255) : (st && st->drum ? RGB(236, 195, 115) : RGB(195, 206, 220)));
            RECT rTxt{ 12, y0 + 2, W - 170, y0 + 19 };
            DrawTextA(memDC, line, -1, &rTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Per-channel tune controls: [-] value [+]
            RECT rMinus{ W - 166, y0 + 3, W - 148, y0 + 18 };
            RECT rTuneVal{ W - 146, y0 + 2, W - 92, y0 + 19 };
            RECT rPlus{ W - 90, y0 + 3, W - 72, y0 + 18 };
            FillRect(memDC, &rMinus, btnBrush);
            FillRect(memDC, &rPlus, btnBrush);
            SetTextColor(memDC, RGB(215, 225, 240));
            DrawTextA(memDC, "-", -1, &rMinus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextA(memDC, "+", -1, &rPlus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            float tSt = st ? st->tuneSemis : 0.0f;
            bool tuned = std::fabs(tSt) >= 0.005f;
            char tuneStr[16];
            std::snprintf(tuneStr, sizeof(tuneStr), "%+6.2f", tSt);
            SetTextColor(memDC, tuned ? RGB(130, 225, 255) : RGB(135, 146, 162));
            DrawTextA(memDC, tuneStr, -1, &rTuneVal, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Activity bar
            RECT rMbg{ W - 62, y0 + 6, W - 12, y0 + 15 };
            FillRect(memDC, &rMbg, meterBgBrush);
            if (st && st->activeVoices > 0) {
                int bw = std::min(50, 8 + st->activeVoices * 7);
                RECT rMfill{ W - 62, y0 + 6, W - 62 + bw, y0 + 15 };
                FillRect(memDC, &rMfill, st->drum ? drumMeterBrush : meterFillBrush);
            }
        }

        DeleteObject(evenBrush);
        DeleteObject(oddBrush);
        DeleteObject(actRowBrush);
        DeleteObject(btnBrush);
        DeleteObject(meterBgBrush);
        DeleteObject(meterFillBrush);
        DeleteObject(drumMeterBrush);
        SelectObject(memDC, oldFont);
        DeleteObject(hdrFont);
        DeleteObject(subFont);
        DeleteObject(rowFont);

        BitBlt(hdc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (inst && inst->editHwnd == hwnd) inst->editHwnd = nullptr;
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

static const char* kEditorClassName = "SD90PlayerEditorWnd";
static std::once_flag gEditorClassOnce;

static void registerEditorClass(HMODULE hm) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = editorWndProc;
    wc.hInstance = hm;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = kEditorClassName;
    RegisterClassExA(&wc);
}

static intptr_t dispatcher(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    PluginInstance* inst = getInstance(effect);
    if (!inst) return 0;

    switch (opcode) {
    case effOpen: {
        std::lock_guard<std::mutex> guard(inst->lock);
        if (inst->dllDir.empty()) inst->dllDir = dllDirectory();
        ensureInit(inst);
        return 0;
    }
    case effClose:
        if (inst->editHwnd) {
            DestroyWindow(inst->editHwnd);
            inst->editHwnd = nullptr;
        }
        delete inst;
        return 0;
    case effSetProgram:
        if (index >= 0 && index < 128) {
            std::lock_guard<std::mutex> guard(inst->lock);
            ensureInit(inst);
            uint8_t msg[2] = { (uint8_t)0xC0, (uint8_t)index };
            inst->synth.queueMidi(0, msg, 2);
        }
        return 0;
    case effGetProgram: {
        std::lock_guard<std::mutex> guard(inst->lock);
        ensureInit(inst);
        return inst->synth.partStatus(0).program;
    }
    case effSetProgramName:
        return 0;
    case effGetProgramName: {
        if (ptr) {
            std::lock_guard<std::mutex> guard(inst->lock);
            ensureInit(inst);
            std::string n = inst->synth.patchName(0);
            if (n.empty()) n = "SD90 Player";
            std::snprintf((char*)ptr, 64, "%s", n.c_str());
        }
        return 0;
    }
    case effGetParamLabel:
        if (ptr) { ((char*)ptr)[0] = 0; }
        return 0;
    case effGetParamDisplay:
        if (ptr) {
            if (index == 0) std::snprintf((char*)ptr, 32, "%.0f%%", inst->masterParam * 200.0f);
            else std::snprintf((char*)ptr, 32, "0");
        }
        return 0;
    case effGetParamName:
        if (ptr) { std::snprintf((char*)ptr, 64, "%s", index == 0 ? "Master" : "Reverb"); }
        return 0;
    case effSetSampleRate: {
        std::lock_guard<std::mutex> guard(inst->lock);
        inst->sampleRate = (opt > 1000.0f) ? (double)opt : 44100.0;
        inst->synth.setSampleRate(inst->sampleRate);
        return 0;
    }
    case effSetBlockSize: {
        std::lock_guard<std::mutex> guard(inst->lock);
        inst->blockSize = value > 0 ? (int32_t)value : 1024;
        return 0;
    }
    case effMainsChanged: {
        std::lock_guard<std::mutex> guard(inst->lock);
        ensureInit(inst);
        if (inst->inited) inst->synth.silence();
        return 0;
    }
    case effEditGetRect: {
        if (!ptr) return 0;
        *reinterpret_cast<ERect**>(ptr) = &inst->editRect;
        return 1;
    }
    case effEditOpen: {
        HWND parent = reinterpret_cast<HWND>(ptr);
        if (!parent) return 0;
        HMODULE hm = nullptr;
        dllDirectory(&hm);
        std::call_once(gEditorClassOnce, registerEditorClass, hm);
        if (inst->editHwnd) {
            DestroyWindow(inst->editHwnd);
            inst->editHwnd = nullptr;
        }
        refreshUiSnapshot(inst);
        int w = inst->editRect.right - inst->editRect.left;
        int h = inst->editRect.bottom - inst->editRect.top;
        inst->editHwnd = CreateWindowExA(
            0, kEditorClassName, "SD90 Player",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, w, h,
            parent, nullptr, hm, inst);
        if (!inst->editHwnd) return 0;
        SetTimer(inst->editHwnd, 1, 80, nullptr);
        return 1;
    }
    case effEditClose: {
        if (inst->editHwnd) {
            DestroyWindow(inst->editHwnd);
            inst->editHwnd = nullptr;
        }
        return 0;
    }
    case effEditIdle: {
        if (inst->editHwnd) InvalidateRect(inst->editHwnd, nullptr, FALSE);
        return 0;
    }
    case effGetChunk:
        return 0;
    case effSetChunk:
        return 0;
    case effProcessEvents: {
        if (!ptr) return 0;
        std::lock_guard<std::mutex> guard(inst->lock);
        ensureInit(inst);
        VstEvents* evs = (VstEvents*)ptr;
        for (int32_t i = 0; i < evs->numEvents; ++i) {
            VstEvent* e = evs->events[i];
            if (!e) continue;
            if (e->type == kVstMidiType) {
                VstMidiEvent* me = (VstMidiEvent*)e;
                const uint8_t* data = (const uint8_t*)me->midiData;
                inst->synth.queueMidi(me->deltaFrames, data, midiShortLength(data[0]));
            } else if (e->type == kVstSysexType) {
                VstMidiSysexEvent* se = (VstMidiSysexEvent*)e;
                if (se->sysexDump && se->dumpBytes > 0)
                    inst->synth.queueMidi(se->deltaFrames, (const uint8_t*)se->sysexDump, se->dumpBytes);
            }
        }
        return 1;
    }
    case effCanBeAutomated:
        return 1;
    case effGetNumProgramCategories:
        return 1;
    case effGetProgramNameIndexed: {
        if (index < 0 || index >= 128) return 0;
        if (ptr) {
            std::lock_guard<std::mutex> guard(inst->lock);
            ensureInit(inst);
            std::string n = inst->synth.gmProgramName(index);
            if (n.empty()) { char b[32]; std::snprintf(b, sizeof(b), "Program %d", index + 1); n = b; }
            std::snprintf((char*)ptr, 64, "%s", n.c_str());
        }
        return 1;
    }
    case effGetPlugCategory:
        return kPlugCategSynth;
    case effGetCurrentPosition:
        return 0;
    case effSetSpeakerArrangement:
    case effGetSpeakerArrangement:
        return 0;
    case effGetMidiProgramName:
    case effGetCurrentMidiProgram:
    case effGetMidiProgramCategory:
    case effHasMidiProgramsChanged:
    case effGetMidiKeyName:
        return 0;
    case effGetEffectName:
        if (ptr) { std::snprintf((char*)ptr, 64, "SD90 Player"); }
        return 1;
    case effGetVendorString:
        if (ptr) { std::snprintf((char*)ptr, 64, "SD90-like"); }
        return 1;
    case effGetProductString:
        if (ptr) { std::snprintf((char*)ptr, 64, "SD90 Player"); }
        return 1;
    case effGetVendorVersion:
        return 1000;
    case effVendorSpecific:
        return 0;
    case effCanDo:
        if (ptr) {
            const char* s = (const char*)ptr;
            if (!strcmp(s, "receiveVstEvents") || !strcmp(s, "receiveVstMidiEvent") ||
                !strcmp(s, "receiveVstSysexEvent") || !strcmp(s, "sendVstEvents"))
                return 1;
        }
        return 0;
    case effGetTailSize:
        return 44100;
    case effGetVstVersion:
        return 2400;
    default:
        return 0;
    }
}

static void processReplacing(AEffect* effect, float** inputs, float** outputs, int32_t sampleFrames) {
    (void)inputs;
    PluginInstance* inst = getInstance(effect);
    if (!inst || sampleFrames <= 0 || !outputs || !outputs[0]) return;
    std::lock_guard<std::mutex> guard(inst->lock);
    ensureInit(inst);
    float* out[2] = { outputs[0], outputs[1] ? outputs[1] : outputs[0] };
    inst->synth.render(out[0], out[1], sampleFrames);
}

static void processAccum(AEffect* effect, float** inputs, float** outputs, int32_t sampleFrames) {
    (void)inputs;
    PluginInstance* inst = getInstance(effect);
    if (!inst || sampleFrames <= 0 || !outputs || !outputs[0]) return;
    std::lock_guard<std::mutex> guard(inst->lock);
    ensureInit(inst);
    const int chunk = 1024;
    float l[chunk], r[chunk];
    float* out[2] = { outputs[0], outputs[1] ? outputs[1] : outputs[0] };
    int done = 0;
    while (done < sampleFrames) {
        int n = std::min(chunk, sampleFrames - done);
        inst->synth.render(l, r, n);
        for (int i = 0; i < n; ++i) {
            out[0][done + i] += l[i];
            out[1][done + i] += r[i];
        }
        done += n;
    }
}

static void processDouble(AEffect* effect, double** inputs, double** outputs, int32_t sampleFrames) {
    (void)inputs;
    PluginInstance* inst = getInstance(effect);
    if (!inst || sampleFrames <= 0 || !outputs || !outputs[0]) return;
    std::lock_guard<std::mutex> guard(inst->lock);
    ensureInit(inst);
    const int chunk = 1024;
    float l[chunk], r[chunk];
    int done = 0;
    while (done < sampleFrames) {
        int n = std::min(chunk, sampleFrames - done);
        inst->synth.render(l, r, n);
        for (int i = 0; i < n; ++i) {
            outputs[0][done + i] = l[i];
            if (outputs[1]) outputs[1][done + i] = r[i];
        }
        done += n;
    }
}

static void setParameter(AEffect* effect, int32_t index, float parameter) {
    PluginInstance* inst = getInstance(effect);
    if (!inst) return;
    if (index == 0) {
        std::lock_guard<std::mutex> guard(inst->lock);
        inst->masterParam = std::max(0.0f, std::min(1.0f, parameter));
        inst->synth.setMasterVolume(inst->masterParam * 2.0f);
    }
}
static float getParameter(AEffect* effect, int32_t index) {
    PluginInstance* inst = getInstance(effect);
    if (!inst) return 0.0f;
    return index == 0 ? inst->masterParam : 0.0f;
}

extern "C" __declspec(dllexport) AEffect* VSTPluginMain(audioMasterCallback) {
    PluginInstance* inst = new PluginInstance();
    inst->dllDir = dllDirectory();
    AEffect& e = inst->effect;
    std::memset(&e, 0, sizeof(e));
    e.magic = kEffectMagic;
    e.dispatcher = dispatcher;
    e.process = processAccum;
    e.setParameter = setParameter;
    e.getParameter = getParameter;
    e.numPrograms = 128;
    e.numParams = 2;
    e.numInputs = 0;
    e.numOutputs = 2;
    e.flags = 0x01 /*HasEditor*/ | 0x10 /*CanReplacing*/ | 0x100 /*IsSynth*/ | 0x200 /*NoSoundInStop*/;
    e.initialDelay = 0;
    e.object = inst;
    e.user = nullptr;
    e.uniqueID = 0x53443930; // 'SD90'
    e.version = 1000;
    e.processReplacing = processReplacing;
    e.processDoubleReplacing = processDouble;
    return &e;
}
