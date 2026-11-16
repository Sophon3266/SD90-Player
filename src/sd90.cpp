#include "sd90.h"
#include <cstdarg>

// ---------------------------------------------------------------- utilities
static void logmsg(bool verbose, const char* fmt, ...) {
    if (!verbose) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dirExists(const std::string& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool readFile(const std::string& p, std::vector<uint8_t>& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(got);
    return got == (size_t)n;
}
static uint16_t be16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
static std::string trimName(const uint8_t* p, int n) {
    std::string s((const char*)p, n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t a = 0; while (a < s.size() && s[a] == ' ') ++a;
    return s.substr(a);
}

static uint16_t le16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
static uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

static void decodeRdacWave(const std::vector<uint8_t>& rom, Wave& w, uint32_t st, uint32_t lp, uint32_t en) {
    if (st >= rom.size() || en < st || en >= rom.size()) return;
    auto sampleDelta = [&](uint32_t u) -> int32_t {
        uint32_t bk = u & ~0xFFFFFu;
        uint32_t ib = u & 0xFFFFFu;
        uint8_t sb = rom[bk + (ib >> 5)];
        uint8_t sh = (ib & 0x10) ? ((sb >> 4) & 0x0F) : (sb & 0x0F);
        return int32_t((int8_t)rom[u]) << sh;
    };
    uint32_t count = en - st + 1;
    w.pcm.resize(count);
    int32_t acc = 0;
    for (uint32_t i = 0; i < count; ++i) {
        acc += sampleDelta(st + i);
        w.pcm[i] = (float)acc * (1.0f / 32768.0f);
    }
    if (w.loopMode == 2 && lp >= st && lp < en) {
        // FUN_18026e190 ping-pong unroll: backward pass adds +delta(en..lp), inverting phase cleanly into a seamless fwd loop.
        w.pcm.reserve(count + (en - lp + 1));
        for (uint32_t u = en; ; --u) {
            acc += sampleDelta(u);
            w.pcm.push_back((float)acc * (1.0f / 32768.0f));
            if (u == lp) break;
        }
        w.data = (const uint8_t*)w.pcm.data();
        w.len = (uint32_t)w.pcm.size();
        w.loopStart = lp - st;
        w.loopEnd = w.len;
        w.loopMode = 0;
        return;
    }
    w.data = (const uint8_t*)w.pcm.data();
    w.len = count;
    if (lp >= st && lp < en) {
        w.loopStart = lp - st;
        w.loopEnd = count;
    } else {
        w.loopStart = 0;
        w.loopEnd = 0;
        w.loopMode = 1;
    }
}

// ---------------------------------------------------------------- PE reader
namespace {
struct PeSection {
    char name[9];
    uint32_t va, vsz, raw, rsz;
};
struct PeInfo {
    const uint8_t* file = nullptr;
    size_t size = 0;
    uint64_t imageBase = 0;
    uint32_t rsrcVa = 0, rsrcSize = 0, rsrcRaw = 0;
    std::vector<PeSection> sections;
    bool ok = false;

    size_t vaToOff(uint64_t va) const {
        if (va < imageBase) return (size_t)-1;
        uint64_t rva = va - imageBase;
        for (const auto& s : sections) {
            uint32_t span = std::max(s.vsz, s.rsz);
            if (rva >= s.va && rva < uint64_t(s.va) + span) {
                size_t off = size_t(s.raw) + size_t(rva - s.va);
                return (off < size) ? off : (size_t)-1;
            }
        }
        return (size_t)-1;
    }
};
static PeInfo parsePe(const std::vector<uint8_t>& b) {
    PeInfo pe; pe.file = b.data(); pe.size = b.size();
    if (b.size() < 0x40) return pe;
    uint32_t peOff = *(const uint32_t*)(b.data() + 0x3C);
    if (peOff + 24 > b.size()) return pe;
    if (memcmp(b.data() + peOff, "PE\0\0", 4) != 0) return pe;
    uint16_t nsec = *(const uint16_t*)(b.data() + peOff + 6);
    uint16_t opt = *(const uint16_t*)(b.data() + peOff + 20);
    if (peOff + 24 + 32 <= b.size() && *(const uint16_t*)(b.data() + peOff + 24) == 0x20b) {
        pe.imageBase = *(const uint64_t*)(b.data() + peOff + 24 + 24);
    }
    size_t sec = peOff + 24 + opt;
    for (uint16_t i = 0; i < nsec; ++i) {
        size_t o = sec + i * 40;
        if (o + 40 > b.size()) break;
        PeSection ps{};
        memcpy(ps.name, b.data() + o, 8);
        ps.vsz = *(const uint32_t*)(b.data() + o + 8);
        ps.va  = *(const uint32_t*)(b.data() + o + 12);
        ps.rsz = *(const uint32_t*)(b.data() + o + 16);
        ps.raw = *(const uint32_t*)(b.data() + o + 20);
        pe.sections.push_back(ps);
        if (memcmp(b.data() + o, ".rsrc", 5) == 0) {
            pe.rsrcVa = ps.va;
            pe.rsrcSize = ps.rsz;
            pe.rsrcRaw = ps.raw;
            pe.ok = true;
        }
    }
    return pe;
}
struct ResEntry { std::string name; uint32_t off = 0; bool dir = false; };
static std::vector<ResEntry> resEntries(const PeInfo& pe, uint32_t dirOff) {
    std::vector<ResEntry> out;
    const uint8_t* r = pe.file + pe.rsrcRaw;
    size_t rn = pe.rsrcSize;
    if (dirOff + 16 > rn) return out;
    uint16_t nn = *(const uint16_t*)(r + dirOff + 12);
    uint16_t ni = *(const uint16_t*)(r + dirOff + 14);
    for (uint16_t i = 0; i < nn + ni; ++i) {
        size_t eo = dirOff + 16 + i * 8;
        if (eo + 8 > rn) break;
        uint32_t nm = *(const uint32_t*)(r + eo);
        uint32_t off = *(const uint32_t*)(r + eo + 4);
        ResEntry e;
        if (nm & 0x80000000u) {
            uint32_t so = nm & 0x7FFFFFFFu;
            if (so + 2 <= rn) {
                uint16_t len = *(const uint16_t*)(r + so);
                if (so + 2 + len * 2 <= rn) {
                    for (uint16_t k = 0; k < len; ++k) {
                        uint16_t c = *(const uint16_t*)(r + so + 2 + k * 2);
                        e.name.push_back((char)(c & 0xFF));
                    }
                }
            }
        } else {
            char buf[32]; sprintf(buf, "%u", nm); e.name = buf;
        }
        e.off = off & 0x7FFFFFFFu;
        e.dir = (off & 0x80000000u) != 0;
        out.push_back(e);
    }
    return out;
}
static bool resMatch(const std::string& have, const char* want) {
    return _stricmp(have.c_str(), want) == 0;
}
static bool findResData(const PeInfo& pe, uint32_t dirOff, const char* const* path, int depth,
                        uint32_t& dataRva, uint32_t& dataSize) {
    auto entries = resEntries(pe, dirOff);
    for (const auto& e : entries) {
        if (!resMatch(e.name, path[depth])) continue;
        if (depth == 0) {
            if (!e.dir) continue;
            return findResData(pe, e.off, path, 1, dataRva, dataSize);
        }
        if (e.dir) {
            // language level: accept the first leaf
            auto child = resEntries(pe, e.off);
            for (const auto& c : child) {
                if (c.dir) continue;
                const uint8_t* r = pe.file + pe.rsrcRaw;
                if (c.off + 16 > pe.rsrcSize) continue;
                dataRva = *(const uint32_t*)(r + c.off);
                dataSize = *(const uint32_t*)(r + c.off + 4);
                return true;
            }
        }
    }
    return false;
}
} // namespace

bool extractPeResource(const char* path, const char* nameIn, std::vector<uint8_t>& out) {
    std::vector<uint8_t> file;
    if (!readFile(path, file)) return false;
    PeInfo pe = parsePe(file);
    if (!pe.ok) return false;
    const char* pathParts[2] = { "WROM", nameIn };
    uint32_t rva = 0, sz = 0;
    if (!findResData(pe, 0, pathParts, 0, rva, sz)) return false;
    // Resource data offsets are RVAs in these Roland files, but section-relative
    // in standard PE files. Try both and pick the one with a plausible ROM header.
    const uint8_t* candidates[2] = { nullptr, nullptr };
    size_t candidatesSize[2] = { 0, 0 };
    if (rva < pe.rsrcSize && size_t(rva) + sz <= pe.rsrcSize) {
        candidates[0] = pe.file + pe.rsrcRaw + rva; candidatesSize[0] = sz;
    }
    if (rva >= pe.rsrcVa && size_t(rva - pe.rsrcVa) + sz <= pe.rsrcSize) {
        candidates[1] = pe.file + pe.rsrcRaw + (rva - pe.rsrcVa); candidatesSize[1] = sz;
    }
    for (int i = 0; i < 2; ++i) {
        if (!candidates[i] || candidatesSize[i] < 0x100) continue;
        const uint8_t* d = candidates[i];
        bool printable = true;
        for (int k = 0; k < 12; ++k) {
            uint8_t c = d[0x20 + k];
            if (c < 32 || c > 126) { printable = false; break; }
        }
        if (printable) {
            out.assign(d, d + candidatesSize[i]);
            return true;
        }
    }
    return false;
}

bool findWaveTable(const std::vector<uint8_t>& rom, size_t& headerOff, uint32_t& count) {
    if (rom.size() < 0x10000) return false;
    const uint8_t pat[8] = { 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x18 };
    size_t start = rom.size() > 0x40000 ? rom.size() - 0x40000 : 0;
    for (size_t p = rom.size() - 8; p >= start; --p) {
        if (memcmp(rom.data() + p, pat, 8) == 0) {
            if (p < 4) break;
            uint32_t c = be16(rom.data() + p - 4);
            if (c >= 32 && c < 20000) {
                size_t entry = p + 8;
                if (entry + size_t(c) * 24 == rom.size()) {
                    headerOff = p - 4; count = c; return true;
                }
            }
        }
        if (p == 0) break;
    }
    return false;
}

// ---------------------------------------------------------------- ROM
bool Rom::loadPeResource(const char* pePath, const char* resourceName) {
    if (!extractPeResource(pePath, resourceName, bytes)) return false;
    kind = resourceName;
    return parseWaveTable();
}

bool Rom::loadInternalAFromDll(const char* pePath) {
    waves.clear(); multisamples.clear(); groupOf.clear(); groups.clear();
    std::vector<uint8_t> dll;
    if (!readFile(pePath, dll)) return false;
    PeInfo pe = parsePe(dll);
    if (!pe.ok || pe.imageBase == 0) return false;

    // Extract WROM/INTERNALA
    if (!extractPeResource(pePath, "INTERNALA", bytes)) return false;
    if (bytes.size() < 0x100000) return false;

    // Read InternalA static tables from .rdata (SD-8 XS_TISR.dll)
    size_t offCntMs = pe.vaToOff(pe.imageBase + 0xbe9b74);
    size_t offPtrMs = pe.vaToOff(pe.imageBase + 0xbf0910);
    size_t offCntWv = pe.vaToOff(pe.imageBase + 0xc17f18);
    size_t offWvTbl = pe.vaToOff(pe.imageBase + 0xc05b70);
    if (offCntMs == (size_t)-1 || offPtrMs == (size_t)-1 ||
        offCntWv == (size_t)-1 || offWvTbl == (size_t)-1) return false;

    uint16_t cntMs = le16(dll.data() + offCntMs);
    uint16_t cntWv = le16(dll.data() + offCntWv);
    if (cntMs < 100 || cntMs > 4096 || cntWv < 100 || cntWv > 10000) return false;
    if (offPtrMs + size_t(cntMs) * 16 > dll.size()) return false;
    if (offWvTbl + size_t(cntWv) * 24 > dll.size()) return false;

    // Parse 3111 24-byte LE wave descriptors and RDAC-decode
    waves.resize(cntWv);
    for (uint32_t i = 0; i < cntWv; ++i) {
        const uint8_t* e = dll.data() + offWvTbl + size_t(i) * 24;
        Wave& w = waves[i];
        w.keyHigh = e[0];
        uint8_t lm = e[1];
        // InternalA e[1]: 0x40 = one-shot (lp==en), 0x80 = ping-pong loop, 0x20 = reverse, 0x00 = fwd loop
        w.loopMode = (lm & 0x40) ? 1 : (lm & 0x80) ? 2 : (lm & 0x20) ? 3 : 0;
        w.velLayer = lm & 0x1F;
        w.rootKey = (float)e[2];
        w.keyLow = e[3];
        int fine1 = int(le16(e + 6)) - 0x400;
        int fine2 = int(le16(e + 8)) - 0x400;
        w.fineCents = float(fine1 + fine2) * 0.1f;
        uint16_t rateCode = le16(e + 10);
        w.rate = rateCode ? uint32_t(rateCode) * 100u : 32000u;
        if (w.rate < 8000 || w.rate > 192000) w.rate = 32000;
        uint32_t st = le32(e + 12), lp = le32(e + 16), en = le32(e + 20);
        decodeRdacWave(bytes, w, st, lp, en);
    }

    // Parse 1083 multisamples
    multisamples.resize(cntMs);
    for (uint32_t i = 0; i < cntMs; ++i) {
        uint64_t ptr = *(const uint64_t*)(dll.data() + offPtrMs + size_t(i) * 16);
        uint64_t msz = *(const uint64_t*)(dll.data() + offPtrMs + size_t(i) * 16 + 8);
        size_t moff = pe.vaToOff(ptr);
        if (moff == (size_t)-1 || moff + msz > dll.size() || msz < 16) continue;
        const uint8_t* m = dll.data() + moff;
        Multisample& ms = multisamples[i];
        ms.name = trimName(m, 12);
        for (size_t z = 16; z + 4 <= msz; z += 4) {
            uint16_t kh = le16(m + z);
            uint16_t wid = le16(m + z + 2);
            ms.zones.push_back({ (uint8_t)std::min<uint16_t>(kh, 127), wid });
            if (kh >= 0x7F) break;
        }
    }

    kind = "INTERNALA";
    buildGroups();
    return !waves.empty() && !multisamples.empty();
}

bool Rom::loadRaw(const char* path) {
    if (!readFile(path, bytes)) return false;
    kind = "raw";
    return parseWaveTable();
}

bool Rom::parseMultisampleTable() {
    multisamples.clear();
    if (bytes.size() < 0x40000) return false;
    // 0x20003 table header: u16 BE count, 0, u32 BE 12, u32 BE 8
    const uint8_t pat[8] = { 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x08 };
    size_t hdr = (size_t)-1;
    uint32_t count = 0;
    size_t start = bytes.size() > 0x40000 ? bytes.size() - 0x40000 : 0;
    for (size_t p = bytes.size() - 12; p >= start; --p) {
        if (memcmp(bytes.data() + p + 4, pat, 8) == 0) {
            uint32_t c = be16(bytes.data() + p);
            if (c >= 32 && c < 5000 && p + 12 + size_t(c) * 8 <= bytes.size()) {
                hdr = p; count = c; break;
            }
        }
        if (p == 0) break;
    }
    if (hdr == (size_t)-1) return false;
    multisamples.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* ent = bytes.data() + hdr + 12 + size_t(i) * 8;
        uint32_t off = be32(ent);
        uint32_t len = be32(ent + 4);
        if (hdr + off + len > bytes.size() || len < 16) continue;
        const uint8_t* m = bytes.data() + hdr + off;
        Multisample& ms = multisamples[i];
        ms.name = trimName(m, 12);
        for (size_t z = 16; z + 4 <= len; z += 4) {
            uint8_t kh = m[z];
            uint16_t wid = be16(m + z + 2);
            ms.zones.push_back({ std::min<uint8_t>(kh, 127), wid });
            if (kh >= 0x7F) break;
        }
    }
    return true;
}

bool Rom::parseWaveTable() {
    waves.clear(); multisamples.clear(); groupOf.clear(); groups.clear();
    size_t hdr = 0; uint32_t count = 0;
    if (!findWaveTable(bytes, hdr, count)) return false;
    const uint8_t* base = bytes.data() + hdr + 12;
    waves.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = base + size_t(i) * 24;
        Wave& w = waves[i];
        w.keyHigh = e[0];
        uint8_t b1 = e[1];
        // SRX e[1]: 0x02 = one-shot (lp==en), 0x01 = ping-pong loop, 0x04 = reverse, 0x00 = fwd loop
        w.loopMode = (b1 & 0x02) ? 1 : (b1 & 0x01) ? 2 : (b1 & 0x04) ? 3 : 0;
        w.velLayer = (b1 >> 3) & 0x1F;
        w.rootKey = (float)e[2];
        w.keyLow = e[3];
        int fine1 = int(be16(e + 6)) - 0x400;
        int fine2 = int(be16(e + 8)) - 0x400;
        w.fineCents = float(fine1 + fine2) * 0.1f;
        uint16_t rate = be16(e + 10);
        w.rate = rate ? uint32_t(rate) * 100u : 32000u;
        if (w.rate < 8000 || w.rate > 192000) w.rate = 32000;
        uint32_t st = be32(e + 12), lp = be32(e + 16), en = be32(e + 20);
        decodeRdacWave(bytes, w, st, lp, en);
    }
    parseMultisampleTable();
    buildGroups();
    return !waves.empty();
}

void Rom::buildGroups() {
    groupOf.assign(waves.size(), -1);
    groups.clear();
    int g = -1;
    float prevRoot = -1.0f;
    for (size_t i = 0; i < waves.size(); ++i) {
        if (waves[i].data == nullptr) { groupOf[i] = g; continue; }
        if (g < 0 || waves[i].rootKey < prevRoot - 0.75f) {
            g++; groups.push_back({});
        }
        groupOf[i] = g;
        waves[i].group = g;
        groups.back().push_back((int)i);
        prevRoot = waves[i].rootKey;
    }
}

// ---------------------------------------------------------------- Bank
// Koa bank record layout (verified against the XV-5080 MIDI implementation
// address map and the DT1 sysex embedded in the SD-90 test MIDI):
//   0x0054 Patch Common        (11 00 00 00, 88 bytes; XV map 0x4F + 9 ext)
//   0x00AC Patch Common MFX    (11 00 02 00, 52)
//   0x00E0 Patch Common Chorus (11 00 04 00, 83)
//   0x0133 Patch Common Reverb (11 00 06 00, 169)
//   0x01DC Patch TMT           (11 00 10 00, 41)
//   0x0205 Tone 1..4           (11 00 20/22/24/26, 154 each)
// A tone is the first 0x89 parameter bytes followed by Step-LFO type+16.
static uint16_t nibble16(const uint8_t* p) {
    return uint16_t((uint16_t(p[0] & 0x0F) << 12) | (uint16_t(p[1] & 0x0F) << 8) |
                    (uint16_t(p[2] & 0x0F) << 4) | uint16_t(p[3] & 0x0F));
}
static uint16_t dec16(const uint8_t* p) {
    // Roland 2-byte rate/delay values: high byte = integer, low = fraction.
    return uint16_t((p[0] << 8) | p[1]);
}
static KoaTone parseKoaTone(const uint8_t* b, int len = 0x9A) {
    KoaTone t;
    if (len < 0x89) return t;
    t.valid = true;
    t.level = b[0x00];
    t.coarse = b[0x01];
    t.fine = b[0x02];
    t.randPitch = b[0x03];
    t.pan = b[0x04];
    t.panKeyfollow = b[0x05];
    t.randPan = b[0x06];
    t.altPan = b[0x07];
    t.envMode = b[0x08];
    t.delayMode = b[0x09];
    t.delayTime = dec16(b + 0x0A);
    t.drySend = b[0x0C];
    t.chorusMfx = b[0x0D];
    t.reverbMfx = b[0x0E];
    t.chorusNon = b[0x0F];
    t.reverbNon = b[0x10];
    t.outputAssign = b[0x11];
    t.rxBender = b[0x12];
    t.rxExpression = b[0x13];
    t.rxHold = b[0x14];
    t.rxPanMode = b[0x15];
    t.redamper = b[0x16];
    for (int c = 0; c < 4; ++c)
        for (int s = 0; s < 4; ++s) t.ctrlSwitch[c][s] = b[0x17 + c * 4 + s];
    t.wavGroupType = b[0x27];
    t.wavGroupId = nibble16(b + 0x28);
    t.wavIdL = nibble16(b + 0x2C);
    t.wavIdR = nibble16(b + 0x30);
    t.waveGain = b[0x34];
    t.fxmSwitch = b[0x35];
    t.fxmColor = b[0x36];
    t.fxmDepth = b[0x37];
    t.tempoSync = b[0x38];
    t.pitchKeyfollow = b[0x39];
    t.pitchEnvDepth = b[0x3A];
    t.pitchEnvVelSens = b[0x3B];
    t.pitchEnvT1VelSens = b[0x3C];
    t.pitchEnvT4VelSens = b[0x3D];
    t.pitchEnvTimeKF = b[0x3E];
    for (int i = 0; i < 4; ++i) t.pitchEnvT[i] = b[0x3F + i];
    for (int i = 0; i < 5; ++i) t.pitchEnvL[i] = b[0x43 + i];
    t.tvfType = b[0x48];
    t.tvfCutoff = b[0x49];
    t.tvfCutoffKF = b[0x4A];
    t.tvfCutoffVelCurve = b[0x4B];
    t.tvfCutoffVelSens = b[0x4C];
    t.tvfResonance = b[0x4D];
    t.tvfResoVelSens = b[0x4E];
    t.tvfEnvDepth = b[0x4F];
    t.tvfEnvVelCurve = b[0x50];
    t.tvfEnvVelSens = b[0x51];
    t.tvfEnvT1VelSens = b[0x52];
    t.tvfEnvT4VelSens = b[0x53];
    t.tvfEnvTimeKF = b[0x54];
    for (int i = 0; i < 4; ++i) t.tvfEnvT[i] = b[0x55 + i];
    for (int i = 0; i < 5; ++i) t.tvfEnvL[i] = b[0x59 + i];
    t.biasLevel = b[0x5E];
    t.biasPos = b[0x5F];
    t.biasDir = b[0x60];
    t.tvaVelCurve = b[0x61];
    t.tvaVelSens = b[0x62];
    t.tvaEnvT1VelSens = b[0x63];
    t.tvaEnvT4VelSens = b[0x64];
    t.tvaEnvTimeKF = b[0x65];
    for (int i = 0; i < 4; ++i) t.tvaEnvT[i] = b[0x66 + i];
    for (int i = 0; i < 3; ++i) t.tvaEnvL[i] = b[0x6A + i];
    t.lfoWave[0] = b[0x6D];
    t.lfoRate[0] = dec16(b + 0x6E);
    t.lfoOffset[0] = b[0x70];
    t.lfoRateDetune[0] = b[0x71];
    t.lfoDelay[0] = b[0x72];
    t.lfoDelayKF[0] = b[0x73];
    t.lfoFadeMode[0] = b[0x74];
    t.lfoFadeTime[0] = b[0x75];
    t.lfoKeyTrigger[0] = b[0x76];
    for (int i = 0; i < 4; ++i) t.lfoDepth[0][i] = int8_t(int(b[0x77 + i]) - 64);
    t.lfoWave[1] = b[0x7B];
    t.lfoRate[1] = dec16(b + 0x7C);
    t.lfoOffset[1] = b[0x7E];
    t.lfoRateDetune[1] = b[0x7F];
    t.lfoDelay[1] = b[0x80];
    t.lfoDelayKF[1] = b[0x81];
    t.lfoFadeMode[1] = b[0x82];
    t.lfoFadeTime[1] = b[0x83];
    t.lfoKeyTrigger[1] = b[0x84];
    for (int i = 0; i < 4; ++i) t.lfoDepth[1][i] = int8_t(int(b[0x85 + i]) - 64);
    // Packed Step-LFO follows the 0x89 DT1 bytes.
    if (len >= 0x9A) {
        t.lfoStepType = b[0x89];
        for (int i = 0; i < 16; ++i) t.lfoStep[i] = b[0x8A + i];
    }
    return t;
}

static void parseRhythmKey(RhythmKey& rk, const uint8_t* p, int len, bool fromSysEx) {
    if (len < 0x95) return;
    rk.valid = true;
    rk.envMode = p[0x0C];
    rk.level = (p[0x0E] <= 127) ? p[0x0E] : 127;
    rk.coarse = (p[0x0F] <= 127) ? p[0x0F] : 60;
    rk.fine = (p[0x10] <= 127) ? p[0x10] : 64;
    rk.pan = (p[0x12] <= 127) ? p[0x12] : 64;
    rk.muteGroup = (p[0x1A] >= 1 && p[0x1A] < 127) ? p[0x1A] : 0;
    rk.rxNoteOff = p[0x1B];
    if (len >= 0xC1) {
        rk.tvfType = p[0xA2];
        rk.tvfCutoff = (p[0xA3] <= 127) ? p[0xA3] : 127;
        rk.tvfResonance = (p[0xA4] <= 127) ? p[0xA4] : 0;
        for (int i = 0; i < 4; ++i) rk.tvaEnvT[i] = p[0xBB + i];
        for (int i = 0; i < 3; ++i) rk.tvaEnvL[i] = p[0xBE + i];
    }
    for (int t = 0; t < 4; ++t) {
        const uint8_t* L = p + 0x21 + size_t(t) * 29;
        RhythmLayer& rl = rk.layers[t];
        bool sw = (L[0] != 0);
        uint8_t grpType = L[1];
        rl.wavGroupId = nibble16(L + 2);
        rl.wavIdL = nibble16(L + 6);
        rl.wavIdR = nibble16(L + 10);
        rl.waveGain = L[14];
        rl.coarse = L[19];
        rl.fine = L[20];
        rl.pan = (L[21] <= 127) ? L[21] : 64;
        rl.level = (L[24] <= 127) ? L[24] : 127;
        rl.velLow = L[25];
        rl.velHigh = L[26];
        rl.active = sw && (fromSysEx || grpType != 0) && rl.wavIdL > 0 && rl.wavIdL < 16384;
    }
}

bool Bank::load(const char* path) {
    std::vector<uint8_t> d;
    if (!readFile(path, d)) return false;
    if (d.size() < 0x18 + 128u * 23255u) return false;
    if (memcmp(d.data(), "KoaBankFile", 11) != 0) return false;
    patches.clear();
    const size_t stride = 23255;
    const size_t start = 0x18;
    bool isRhythmBank = (strstr(path, "A10.") != nullptr || strstr(path, "A10_") != nullptr);
    for (int i = 0; i < 128; ++i) {
        const uint8_t* rec = d.data() + start + size_t(i) * stride;
        const uint8_t* pc = rec + 0x54;   // patch common
        Patch pt;
        pt.name = trimName(pc, 12);
        if (pt.name == "------------") {
            pt.name = trimName(rec, 12); // A10 rhythm set
            pt.isRhythm = true;
        } else if (isRhythmBank) {
            pt.isRhythm = true;
        }
        pt.category = pc[0x0C];
        pt.level = pc[0x0E];
        pt.pan = pc[0x0F];
        if (pt.level > 127) pt.level = 100;
        if (pt.pan > 127) pt.pan = 64;
        for (int t = 0; t < 4; ++t) {
            pt.tones[t] = parseKoaTone(rec + 0x205 + size_t(t) * 0x9A);
            const uint8_t* tm = rec + 0x1DC + 5 + size_t(t) * 9;
            pt.tones[t].tmtOn = (tm[0] != 0);
            pt.tones[t].keyLow = tm[1];
            pt.tones[t].keyHigh = tm[2];
            pt.tones[t].velLow = tm[5];
            pt.tones[t].velHigh = tm[6];
        }
        if (pt.isRhythm) {
            for (int k = 0; k < 88; ++k) {
                size_t koff = 0x5B7 + size_t(k) * 195u;
                if (koff + 195u <= stride)
                    parseRhythmKey(pt.rhythmKeys[21 + k], rec + koff, 195, false);
            }
        }
        for (int t = 0; t < 4; ++t) {
            if (pt.tones[t].active()) { pt.waveId = pt.tones[t].wavIdL; break; }
        }
        pt.valid = !pt.name.empty() && pt.name != "============" &&
                   pt.name.find("KoaBankFile") == std::string::npos;
        patches.push_back(pt);
    }
    // Variant is only a small rotation for the SRX-Keyboards fallback; the
    // real wave identity is in tones[].wavIdL/R and wavGroupId.
    for (int i = 0; i < (int)patches.size(); ++i)
        patches[i].variant = (uint8_t)(i % 4);
    // The first bytes contain the bank/set name.
    name = trimName(d.data() + 0x10, 8);
    return true;
}

// ---------------------------------------------------------------- MIDI file
static uint32_t readVlq(const uint8_t*& p, const uint8_t* end) {
    uint32_t v = 0; int n = 0;
    while (p < end && n < 4) { uint8_t c = *p++; v = (v << 7) | (c & 0x7F); n++; if (!(c & 0x80)) break; }
    return v;
}

bool MidiFile::load(const char* path) {
    std::vector<uint8_t> d;
    if (!readFile(path, d)) return false;
    if (d.size() < 14 || memcmp(d.data(), "MThd", 4) != 0) return false;
    uint32_t hlen = be32(d.data() + 4);
    uint16_t ntrk = (uint16_t)be16(d.data() + 10);
    uint16_t div = be16(d.data() + 12);
    if (div & 0x8000) return false; // SMPTE unsupported
    double ticksPerQuarter = div ? div : 480;
    size_t pos = 8 + hlen;
    struct Raw { uint64_t tick; uint8_t data[16]; int len; };
    std::vector<Raw> raw;
    std::vector<SysExEvent> sysexTmp;
    for (uint16_t t = 0; t < ntrk && pos + 8 <= d.size(); ++t) {
        if (memcmp(d.data() + pos, "MTrk", 4) != 0) break;
        uint32_t len = be32(d.data() + pos + 4);
        const uint8_t* p = d.data() + pos + 8;
        const uint8_t* end = p + len;
        if (end > d.data() + d.size()) break;
        uint64_t tick = 0;
        uint8_t running = 0;
        while (p < end) {
            tick += readVlq(p, end);
            if (p >= end) break;
            uint8_t st = *p;
            if (st == 0xFF) {
                ++p;
                if (p >= end) break;
                uint8_t type = *p++;
                uint32_t l = readVlq(p, end);
                if (p + l > end) break;
                if (type == 0x51 && l == 3) {
                    Raw r; r.tick = tick; r.len = 5;
                    r.data[0] = 0xFF; r.data[1] = 0x51; r.data[2] = 3;
                    r.data[3] = p[0]; r.data[4] = p[1]; r.data[5] = p[2];
                    raw.push_back(r);
                }
                p += l;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {
                ++p;
                uint32_t l = readVlq(p, end);
                if (p + l > end) break;
                SysExEvent se;
                se.time = 0.0; // filled after tempo conversion below
                se.data.push_back(0xF0);
                se.data.insert(se.data.end(), p, p + l);
                // store as a pseudo-event for later timing
                Raw r; r.tick = tick; r.len = -1 - (int)sysexTmp.size();
                sysexTmp.push_back(se);
                raw.push_back(r);
                p += l;
                continue;
            }
            if (st < 0x80) { if (!running) break; st = running; }
            else { running = st; ++p; }
            uint8_t hi = st & 0xF0;
            int nbytes = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            if (p + nbytes > end) break;
            Raw r; r.tick = tick; r.len = nbytes + 1;
            r.data[0] = st;
            r.data[1] = p[0];
            if (nbytes > 1) r.data[2] = p[1];
            p += nbytes;
            raw.push_back(r);
        }
        pos += 8 + len;
    }
    // Build tempo map (tick -> seconds).
    std::vector<std::pair<uint64_t, double>> tempos;
    tempos.push_back({0, 500000.0});
    for (auto& r : raw)
        if (r.len == 5 && r.data[0] == 0xFF && r.data[1] == 0x51)
            tempos.push_back({r.tick, double((r.data[3] << 16) | (r.data[4] << 8) | r.data[5])});
    std::sort(tempos.begin(), tempos.end(), [](auto& a, auto& b) { return a.first < b.first; });
    auto tickToSec = [&](uint64_t tick) {
        double sec = 0; uint64_t last = 0; double us = 500000.0;
        for (auto& t : tempos) {
            if (t.first >= tick) break;
            sec += double(t.first - last) / ticksPerQuarter * us / 1e6;
            last = t.first; us = t.second;
        }
        sec += double(tick - last) / ticksPerQuarter * us / 1e6;
        return sec;
    };
    events.clear();
    sysex.clear();
    for (auto& r : raw) {
        if (r.len < 0) {
            int idx = -1 - r.len;
            if (idx >= 0 && idx < (int)sysexTmp.size()) {
                sysexTmp[idx].time = tickToSec(r.tick);
                sysex.push_back(sysexTmp[idx]);
            }
            continue;
        }
        if (r.len == 5 && r.data[0] == 0xFF) continue;
        MidiEvent e; e.time = tickToSec(r.tick); e.len = r.len;
        memcpy(e.data, r.data, r.len > 16 ? 16 : r.len);
        events.push_back(e);
    }
    std::stable_sort(events.begin(), events.end(), [](const MidiEvent& a, const MidiEvent& b) { return a.time < b.time; });
    double last = events.empty() ? 0.0 : events.back().time;
    for (auto& sx : sysex) last = std::max(last, sx.time);
    length = last + 2.0;
    return true;
}

// ---------------------------------------------------------------- Synth
static int drumWaveId(int note) {
    switch (note) {
    case 27: return 0;    // High Q
    case 28: return 1;    // Slap
    case 29: return 2;    // Scratch Push
    case 30: return 3;    // Scratch Pull
    case 31: return 4;    // Sticks
    case 32: return 5;    // Square Click
    case 33: return 6;    // Metronome Click
    case 34: return 7;    // Metronome Bell
    case 35: return 44;   // Acoustic Bass Drum -> Mondo Kick
    case 36: return 122;  // Bass Drum 1 -> Kick 1 mfL
    case 37: return 124;  // Side Stick
    case 38: return 108;  // Acoustic Snare
    case 39: return 110;  // Hand Clap
    case 40: return 100;  // Electric Snare
    case 41: return 142;  // Low Floor Tom
    case 42: return 164;  // Closed HH
    case 43: return 128;  // High Floor Tom
    case 44: return 172;  // Pedal HH
    case 45: return 142;  // Low Tom
    case 46: return 170;  // Open HH
    case 47: return 138;  // Low-Mid Tom
    case 48: return 130;  // Hi-Mid Tom
    case 49: return 152;  // Crash 1
    case 50: return 128;  // High Tom
    case 51: return 180;  // Ride 1
    case 52: return 154;  // China
    case 53: return 190;  // Ride Bell
    case 54: return 88;   // Tambourine
    case 55: return 154;  // Splash
    case 56: return 49;   // Cowbell
    case 57: return 156;  // Crash 2
    case 58: return 90;   // Vibraslap
    case 59: return 182;  // Ride 2
    case 60: return 198;  // Hi Bongo
    case 61: return 202;  // Low Bongo
    case 62: return 206;  // Mute Hi Conga
    case 63: return 206;  // Open Hi Conga
    case 64: return 212;  // Low Conga
    case 65: return 194;  // High Timbale
    case 66: return 196;  // Low Timbale
    case 67: return 83;   // High Agogo
    case 68: return 84;   // Low Agogo
    case 69: return 92;   // Cabasa
    case 70: return 2034; // Maracas
    case 71: return 85;   // Short Whistle
    case 72: return 86;   // Long Whistle
    case 73: return 53;   // Short Guiro
    case 74: return 55;   // Long Guiro
    case 75: return 51;   // Claves
    case 76: return 91;   // Hi Wood Block
    case 77: return 89;   // Low Wood Block
    case 78: return 221;  // Mute Cuica
    case 79: return 222;  // Open Cuica
    case 80: return 3;    // Mute Triangle
    case 81: return 10;   // Open Triangle
    case 82: return 93;   // Shaker
    case 83: return 10;   // Jingle Bell
    case 84: return 11;   // Belltree
    case 85: return 87;   // Castanets
    case 86: return 94;   // Mute Surdo
    case 87: return 95;   // Open Surdo
    default: return -1;
    }
}

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

bool Synth::findFile(const std::string& rel, std::string& out) const {
    std::vector<std::string> cands;
    auto add = [&](const std::string& p) { if (fileExists(p)) cands.push_back(p); };
    add(rel);
    add(baseDir_ + "/" + rel);
    add(baseDir_ + "/../" + rel);
    add(baseDir_ + "/../../" + rel);
    add("D:/Desktop/SD90-like/" + rel);
    add("C:/Desktop/SD90-like/" + rel);
    if (!cands.empty()) { out = cands[0]; return true; }
    return false;
}

bool Synth::readIniPath(const std::string& key, std::string& out) const {
    std::vector<std::string> iniCands = {
        baseDir_ + "/SD90Player.paths.ini",
        baseDir_ + "/../SD90Player.paths.ini",
        "SD90Player.paths.ini",
        baseDir_ + "/SD90Player.ini",
        baseDir_ + "/../SD90Player.ini",
        "SD90Player.ini"
    };
    for (auto& ip : iniCands) {
        FILE* f = fopen(ip.c_str(), "rb");
        if (!f) continue;
        char line[1024];
        std::string section;
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty() || s[0] == ';' || s[0] == '#') continue;
            if (s[0] == '[') { section = s; continue; }
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = s.substr(0, eq), v = s.substr(eq + 1);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            size_t a = 0; while (a < k.size() && (k[a] == ' ' || k[a] == '\t')) ++a;
            k = k.substr(a);
            a = 0; while (a < v.size() && (v[a] == ' ' || v[a] == '\t')) ++a;
            v = v.substr(a);
            if (k == key) { out = v; fclose(f); return !v.empty(); }
        }
        fclose(f);
    }
    return false;
}

bool Synth::init(const std::string& baseDir, bool verbose) {
    verbose_ = verbose;
    baseDir_ = baseDir;
    ready_ = false;
    lastError_.clear();

    std::string melPath, drumPath, banksDir;
    if (!readIniPath("melodic", melPath)) {
        if (!readIniPath("rom", melPath)) melPath.clear();
    }
    if (!readIniPath("drums", drumPath)) drumPath.clear();
    if (!readIniPath("banks", banksDir)) banksDir.clear();
    std::string sx;
    if (readIniPath("native_sysex", sx)) useNativeSysex_ = !(sx == "0" || sx == "false" || sx == "False");

    auto endsWith = [](const std::string& s, const char* suffix) {
        size_t n = strlen(suffix);
        return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suffix) == 0;
    };
    auto loadRom = [&](Rom& rom, const std::string& path, const char* res) -> bool {
        if (endsWith(path, ".bin")) return rom.loadRaw(path.c_str());
        return rom.loadPeResource(path.c_str(), res);
    };
    auto locateAsset = [&](const char* name) {
        std::vector<std::string> c = {
            baseDir_ + "/assets/" + name,
            baseDir_ + "/../assets/" + name,
            baseDir_ + "/../../assets/" + name,
            std::string("assets/") + name,
            std::string("D:/Desktop/SD90-like/assets/") + name
        };
        for (auto& x : c) if (fileExists(x)) return x;
        return std::string();
    };

    // ROM sources: the original local plugin binaries may be used as data
    // files (PE resource & .rdata reads only, never LoadLibrary/execution).
    {
        std::string source, romDll, romSrx, romInternalA;
        if (!readIniPath("source", source)) source = "sd8";
        if (!readIniPath("rom_dll", romDll)) romDll.clear();
        if (!readIniPath("rom_srx", romSrx)) romSrx.clear();
        if (!readIniPath("rom_internala", romInternalA)) romInternalA.clear();
        if (source != "sd8" && source != "auto")
            logmsg(verbose_, "SD90: source=%s not implemented; using SD-8/XV engine\n", source.c_str());
        if (romSrx.empty()) romSrx = romDll;
        if (romInternalA.empty()) romInternalA = romDll;
        logmsg(verbose_, "SD90: engine source = SD-8/XV (RDAC DPCM + multisample tables)\n");

        if (!romInternalA.empty() && fileExists(romInternalA) && !endsWith(romInternalA, ".bin")) {
            if (internalA_.loadInternalAFromDll(romInternalA.c_str())) {
                logmsg(verbose_, "SD90: InternalA ROM %s: %u waves, %zu multisamples\n",
                       romInternalA.c_str(), (unsigned)internalA_.waves.size(), internalA_.multisamples.size());
            }
        }

        std::string dr;
        if (!drumPath.empty() && fileExists(drumPath)) dr = drumPath;
        else if (!romSrx.empty() && fileExists(romSrx)) dr = romSrx;
        else if (!melPath.empty() && fileExists(melPath)) dr = melPath;
        else dr = locateAsset("drums.bin");
        if (!dr.empty() && fileExists(dr) && loadRom(srx_, dr, "SRX")) {
            logmsg(verbose_, "SD90: SRX (DMZ07) ROM %s: %u waves, %zu multisamples\n",
                   dr.c_str(), (unsigned)srx_.waves.size(), srx_.multisamples.size());
        }
        if (internalA_.waves.empty() && srx_.waves.empty()) {
            lastError_ = "no ROM; set rom_dll in paths.ini";
            logmsg(verbose_, "SD90: %s\n", lastError_.c_str());
            return false;
        }
    }

    if (banksDir.empty()) {
        std::vector<std::string> cands = {
            baseDir_ + "/assets/banks",
            baseDir_ + "/../assets/banks",
            baseDir_ + "/../../assets/banks",
            "assets/banks",
            "D:/Desktop/SD90-like/assets/banks"
        };
        for (auto& c : cands) if (dirExists(c)) { banksDir = c; break; }
    }
    int loaded = 0;
    for (int i = 0; i < 11; ++i) {
        char fn[32]; sprintf(fn, "A%02d.bin", i + 1);
        std::string path = banksDir.empty() ? std::string(fn) : banksDir + "/" + fn;
        if (banks_[i].load(path.c_str())) loaded++;
        else logmsg(verbose_, "SD90: bank %s not loaded (%s)\n", fn, path.c_str());
    }
    logmsg(verbose_, "SD90: %d/11 banks loaded from %s\n", loaded, banksDir.c_str());

    // Build category -> multisample-group offsets from the bank contents.
    for (int i = 0; i < 36; ++i) { catCount_[i] = 0; catOffset_[i] = 0; }
    for (int b = 0; b < 11; ++b)
        for (auto& pt : banks_[b].patches)
            if (pt.valid && pt.category < 36)
                catCount_[pt.category] = std::max(catCount_[pt.category], (int)pt.variant + 1);
    int off = 0;
    for (int c = 1; c < 36; ++c) {
        catOffset_[c] = off;
        off += std::max(1, catCount_[c]);
    }

    initEffects();
    reset();
    ready_ = true;
    return true;
}

void Synth::initEffects() {
    double scale = sampleRate_ / 44100.0;
    const size_t combLen[4] = { 1116, 1277, 1422, 1617 };
    const size_t apLen[2] = { 225, 556 };
    for (int i = 0; i < 4; ++i) {
        revCombL_[i].resize(std::max<size_t>(64, size_t(combLen[i] * scale)));
        revCombR_[i].resize(std::max<size_t>(64, size_t((combLen[i] + 23) * scale)));
    }
    for (int i = 0; i < 2; ++i) {
        revApL_[i].resize(std::max<size_t>(32, size_t(apLen[i] * scale)));
        revApR_[i].resize(std::max<size_t>(32, size_t((apLen[i] + 17) * scale)));
    }
    size_t choLen = std::max<size_t>(256, size_t(2048 * scale));
    choBufL_.assign(choLen, 0.0f);
    choBufR_.assign(choLen, 0.0f);
    choIdx_ = 0;
    choPhase_ = 0.0;
}

void Synth::clearEffects() {
    for (int i = 0; i < 4; ++i) {
        std::fill(revCombL_[i].buf.begin(), revCombL_[i].buf.end(), 0.0f); revCombL_[i].lp = 0.0f;
        std::fill(revCombR_[i].buf.begin(), revCombR_[i].buf.end(), 0.0f); revCombR_[i].lp = 0.0f;
    }
    for (int i = 0; i < 2; ++i) {
        std::fill(revApL_[i].buf.begin(), revApL_[i].buf.end(), 0.0f);
        std::fill(revApR_[i].buf.begin(), revApR_[i].buf.end(), 0.0f);
    }
    std::fill(choBufL_.begin(), choBufL_.end(), 0.0f);
    std::fill(choBufR_.begin(), choBufR_.end(), 0.0f);
}

void Synth::silence() {
    for (auto& v : voices_) v = Voice();
    clearEffects();
    queue_.clear();
}

void Synth::reset() {
    for (int i = 0; i < 16; ++i) {
        float savedUserTune = parts_[i].userTuneSemis;
        parts_[i] = Part();
        parts_[i].userTuneSemis = savedUserTune;
        parts_[i].drum = (i == 9);
        parts_[i].bankIndex = (i == 9) ? 9 : 2;
        parts_[i].patchIndex = 0;
        parts_[i].volume = 100;
        parts_[i].pan = 64;
        parts_[i].expression = 127;
        selectPatch(i);
    }
    silence();
    for (int i = 0; i < 16; ++i) { partWave_[i] = -1; partGain_[i] = 100; }
}

int Synth::patchCount() const {
    return banks_[0].patches.empty() ? 0 : 128;
}

std::string Synth::patchName(int ch) const {
    if (ch < 0 || ch > 15) return "";
    const Part& p = parts_[ch];
    if (p.hasSysExPatch) return p.sysExPatch.name;
    if (p.bankIndex < 0 || p.bankIndex > 10) return "";
    const Bank& b = banks_[p.bankIndex];
    if (p.patchIndex < 0 || p.patchIndex >= (int)b.patches.size()) return "";
    return b.patches[p.patchIndex].name;
}

// GM1 PC 0..127 -> { bankIndex (A03=2 .. A09=8, A11=10), baseSlot in Classical Set }
static const uint8_t kGmMap[128][2] = {
    {2,0},{2,3},{2,5},{2,7},{2,9},{2,13},{2,18},{2,22},
    {2,24},{2,25},{2,26},{2,27},{2,29},{2,31},{3,0},{3,3},
    {3,4},{3,8},{3,11},{3,12},{3,15},{3,17},{3,19},{3,20},
    {3,21},{3,25},{3,29},{4,0},{4,2},{4,6},{4,8},{4,11},
    {4,13},{4,14},{4,16},{4,17},{4,18},{4,19},{4,20},{4,25},
    {4,29},{4,31},{5,0},{5,1},{5,2},{5,3},{5,4},{5,6},
    {5,7},{5,10},{5,11},{5,13},{5,14},{5,16},{5,18},{5,20},
    {5,24},{5,26},{5,29},{5,30},{6,0},{6,2},{6,4},{6,8},
    {6,11},{6,12},{6,13},{6,14},{6,15},{6,16},{6,17},{6,18},
    {6,19},{6,20},{6,21},{6,22},{6,23},{6,24},{6,25},{6,26},
    {6,27},{6,30},{7,3},{7,4},{7,5},{7,7},{7,8},{7,9},
    {7,11},{7,12},{7,14},{7,15},{7,17},{7,18},{7,19},{7,20},
    {7,21},{7,22},{7,23},{7,25},{7,26},{7,27},{7,28},{7,31},
    {8,0},{8,2},{8,3},{8,4},{8,6},{8,7},{8,8},{8,9},
    {10,0},{10,1},{10,2},{10,3},{10,5},{10,7},{10,9},{10,12},
    {10,32},{10,35},{10,37},{10,43},{10,47},{10,53},{10,63},{10,69}
};

std::string Synth::gmProgramName(int pc) const {
    if (pc < 0 || pc > 127) return "";
    int bi = kGmMap[pc][0];
    int pi = kGmMap[pc][1];
    if (bi >= 0 && bi <= 10 && pi >= 0 && pi < (int)banks_[bi].patches.size()) {
        if (!banks_[bi].patches[pi].name.empty())
            return banks_[bi].patches[pi].name;
    }
    return "";
}

Synth::PartStatus Synth::partStatus(int ch) const {
    PartStatus st;
    if (ch < 0 || ch > 15) return st;
    const Part& p = parts_[ch];
    st.msb = p.bankMsb;
    st.lsb = p.bankLsb;
    st.program = p.program;
    st.drum = p.drum;
    st.hasSysEx = p.hasSysExPatch;
    st.volume = int(std::lround(p.volume));
    st.expression = int(std::lround(p.expression));
    st.pan = int(std::lround(p.pan));
    st.tuneSemis = p.userTuneSemis + float(p.coarseTune) + p.fineTuneCents * 0.01f;
    st.patch = patchName(ch);
    int cnt = 0;
    for (const auto& v : voices_) {
        if (v.active && v.part == ch) ++cnt;
    }
    st.activeVoices = cnt;
    return st;
}

void Synth::selectPatch(int ch) {
    Part& p = parts_[ch];
    int pc = std::max(0, std::min(127, p.program & 127));
    int msb = p.bankMsb;
    if (ch == 9 || p.drum || msb == 104 || msb == 105 || msb == 106 || msb == 107 || msb == 120 || msb == 127) {
        int base = 0;
        if (pc >= 56) base = 8;
        else if (pc >= 48) base = 7;
        else if (pc >= 40) base = 6;
        else if (pc >= 32) base = 5;
        else if (pc >= 25) base = 4;
        else if (pc >= 24) base = 3;
        else if (pc >= 16) base = 2;
        else if (pc >= 8) base = 1;
        int setOff = (msb == 105) ? 32 : (msb == 106) ? 64 : (msb == 107) ? 96 : 0;
        p.bankIndex = 9;
        p.patchIndex = base + setOff;
    } else if (msb == 80 || msb == 100) {
        p.bankIndex = 0;
        p.patchIndex = pc;
    } else if (msb == 81 || msb == 101) {
        p.bankIndex = 1;
        p.patchIndex = pc;
    } else {
        int bi = kGmMap[pc][0];
        int base = kGmMap[pc][1];
        int varMax = 0;
        if (pc < 127 && kGmMap[pc + 1][0] == bi && kGmMap[pc + 1][1] > base)
            varMax = int(kGmMap[pc + 1][1] - base) - 1;
        int lsbOff = (p.bankLsb > 0 && varMax > 0) ? std::min(p.bankLsb, varMax) : 0;
        int setOff = 0;
        if (bi != 10) {
            if (msb == 97) setOff = 32;
            else if (msb == 98) setOff = 64;
            else if (msb == 99) setOff = 96;
        }
        p.bankIndex = bi;
        p.patchIndex = std::min(127, setOff + base + lsbOff);
    }
    const Bank& b = banks_[p.bankIndex];
    if (!b.patches.empty()) {
        int pi = p.patchIndex;
        for (int k = 0; k < 128; ++k) {
            int q = (pi + k) % 128;
            if (q < (int)b.patches.size() && b.patches[q].valid) { p.patchIndex = q; break; }
        }
    }
}

const Wave* Synth::pickWave(const Rom& rom, int group, int note) const {
    if (group < 0 || group >= (int)rom.groups.size()) return nullptr;
    const auto& g = rom.groups[group];
    const Wave* best = nullptr;
    float bestd = 1e9f;
    for (int id : g) {
        if (id < 0 || id >= (int)rom.waves.size()) continue;
        const Wave& w = rom.waves[id];
        if (!w.data || !w.len) continue;
        float d = fabsf(w.rootKey - (float)note);
        if (d < bestd) { bestd = d; best = &w; }
    }
    return best;
}

// Roland TVA/TVF time 0..127 -> seconds (non-linear, capped).
static float toneTime(uint8_t t) {
    float s = 0.01f * powf(2.0f, (float)t / 16.0f);
    return std::min(8.0f, std::max(0.001f, s));
}

void Synth::applyTone(Voice& v, const KoaTone* t, int part, bool oneShot) {
    if (!t || oneShot) {
        v.envT[0] = 0.001f;
        v.envT[1] = oneShot ? 1.800f : 0.120f;
        v.envT[2] = 0.120f;
        v.envT[3] = oneShot ? 0.350f : 0.300f;
        v.envL[0] = 1.0f;
        v.envL[1] = oneShot ? 0.92f : 0.82f;
        v.envL[2] = oneShot ? 0.0f : 0.82f;
        v.tvfType = 0;
        v.tvfResDamp = 1.414f;
        v.tvfBaseCut = oneShot ? 127.0f : 110.0f;
        v.tvfDepth = 0.0f;
        v.tvfEnv = 0.0f;
        v.revSend = (parts_[part].reverbSend / 127.0f) * (oneShot ? 0.35f : 0.7f);
        v.choSend = (parts_[part].chorusSend / 127.0f) * (oneShot ? 0.1f : 0.5f);
        return;
    }
    for (int i = 0; i < 4; ++i) v.envT[i] = toneTime(t->tvaEnvT[i]);
    for (int i = 0; i < 3; ++i) v.envL[i] = clampf(t->tvaEnvL[i] / 127.0f, 0.0f, 1.0f);
    if (v.envL[2] < 0.001f) v.envL[2] = 0.0f;
    v.tvfType = t->tvfType;
    v.tvfResDamp = clampf(1.414f - (t->tvfResonance / 127.0f) * 1.10f, 0.28f, 1.414f);
    // TVF cutoff + velocity sensitivity + TVF envelope
    float velNorm = (v.vel - 64) / 64.0f;
    float velCut = velNorm * ((int(t->tvfCutoffVelSens) - 64) * 0.4f);
    float keyCut = ((v.note - 60) * ((int(t->tvfCutoffKF) - 64) * 0.05f));
    v.tvfBaseCut = clampf((float)t->tvfCutoff + velCut + keyCut, 0.0f, 127.0f);
    float envVel = 1.0f + velNorm * ((int(t->tvfEnvVelSens) - 64) / 64.0f);
    v.tvfDepth = (int(t->tvfEnvDepth) - 64) * 2.0f * clampf(envVel, 0.25f, 2.0f);
    for (int i = 0; i < 4; ++i) v.tvfEnvT[i] = toneTime(t->tvfEnvT[i]);
    for (int i = 0; i < 5; ++i) v.tvfEnvL[i] = clampf(t->tvfEnvL[i] / 127.0f, 0.0f, 1.0f);
    v.tvfEnv = v.tvfEnvL[0];
    v.tvfStage = 0;
    // LFO1 pitch vibrato & delay
    uint8_t rHi = t->lfoRate[0] >> 8;
    uint8_t rLo = t->lfoRate[0] & 0xFF;
    v.lfoRateHz = clampf(float(rHi) + float(rLo) / 100.0f, 0.1f, 30.0f);
    v.lfoDelaySec = (t->lfoDelay[0] > 0) ? (0.015f * t->lfoDelay[0]) : 0.0f;
    v.lfoPitchSemis = (t->lfoDepth[0][0]) / 12.0f;
    // Reverb/Chorus sends blended from part CC91/CC93 and tone send
    const Part& prt = parts_[part];
    v.revSend = (prt.reverbSend / 127.0f) * (0.3f + 0.7f * t->reverbNon / 127.0f);
    v.choSend = (prt.chorusSend / 127.0f) * (0.3f + 0.7f * t->chorusNon / 127.0f);
}

static void updateTptSvf(float cutHz, double sr, float k, float& a1, float& a2, float& a3) {
    float nyq = (float)sr * 0.45f;
    float c = clampf(cutHz, 40.0f, std::min(19000.0f, nyq));
    float g = tanf(3.14159265f * c / (float)sr);
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}

void Synth::startVoice(int part, int note, int vel, const Wave* w, const KoaTone* toneOverride, float panOverride, float gainScale) {
    if (!w || !w->ready()) return;
    Voice* v = nullptr;
    for (auto& x : voices_) if (!x.active) { v = &x; break; }
    if (!v) {
        uint64_t oldest = ~0ull;
        for (auto& x : voices_) if (x.age < oldest) { oldest = x.age; v = &x; }
    }
    if (!v) return;
    *v = Voice();
    v->active = true;
    v->part = part; v->note = note; v->wave = w; v->age = ++ageCounter_; v->vel = vel;
    v->pos = (w->loopMode == 3) ? double(w->len - 1) : 0.0;
    v->dir = (w->loopMode == 3) ? -1 : 1;
    const Part& p = parts_[part];
    const Patch* pt = nullptr;
    if (p.hasSysExPatch) pt = &p.sysExPatch;
    else if (p.bankIndex >= 0 && p.bankIndex <= 10 && p.patchIndex < (int)banks_[p.bankIndex].patches.size())
        pt = &banks_[p.bankIndex].patches[p.patchIndex];
    const KoaTone* tone = toneOverride ? toneOverride : ((pt && part != 9) ? pt->firstActiveTone() : nullptr);
    double keySemis = double(note) - w->rootKey;
    if (tone && part != 9) {
        keySemis = (60.0 + (double(note) - 60.0) * tone->pitchKeyfollowRatio()) - w->rootKey;
    }
    double ratio = (part == 9) ? pow(2.0, w->fineCents / 1200.0)
                               : pow(2.0, keySemis / 12.0 + w->fineCents / 1200.0);
    if (tone) ratio *= tone->pitchRatio();
    v->baseInc = ratio * double(w->rate) / sampleRate_;
    v->inc = v->baseInc * partTuneRatio(part);
    float level = pt ? powf(pt->level / 127.0f, 1.5f) : 1.0f;
    if (tone) {
        level *= powf(tone->level / 127.0f, 1.5f);
        if (tone->waveGain == 0) level *= 0.5f;
        else if (tone->waveGain == 2) level *= 1.414f;
        else if (tone->waveGain == 3) level *= 2.0f;
    }
    // Natural dynamic expression curve modulated by tvaVelSens
    float tvaSens = tone ? clampf((int(tone->tvaVelSens) - 64) / 63.0f, 0.0f, 1.0f) : 0.75f;
    float velLinear = clampf(vel / 127.0f, 0.02f, 1.0f);
    float velAmp = powf(velLinear, 1.05f + 0.50f * tvaSens);
    float partVol = powf(clampf(p.volume / 127.0f, 0.0f, 1.0f), 1.7f);
    float partExpr = powf(clampf(p.expression / 127.0f, 0.0f, 1.0f), 1.7f);
    float baseAmp = level * velAmp * gainScale * 0.55f;
    if (part == 9) baseAmp = velAmp * gainScale * 0.68f;
    float panOff = 0.0f;
    if (pt) panOff += (pt->pan - 64) / 127.0f;
    if (panOverride >= 0.0f) panOff += (panOverride - 64.0f) / 127.0f;
    else if (tone) panOff += (tone->pan - 64) / 127.0f;
    v->baseAmp = baseAmp;
    v->panOffset = panOff;
    float amp = baseAmp * partVol * partExpr;
    float pan = clampf(p.pan / 127.0f + panOff, 0.0f, 1.0f);
    float ang = pan * 1.5707963f;
    v->ampL = amp * cosf(ang);
    v->ampR = amp * sinf(ang);
    v->oneShot = (part == 9 || w->loopMode == 1);
    if (getenv("SD90_DEBUG"))
        fprintf(stderr, "voice part %d note %d wave root %.0f rate %u len %u loop %u..%u mode %u pcm[0..4]=%.3f,%.3f,%.3f,%.3f\n",
                part, note, w->rootKey, w->rate, w->len, w->loopStart, w->loopEnd, w->loopMode,
                w->len > 0 ? w->pcm[0] : 0.0f, w->len > 1 ? w->pcm[1] : 0.0f,
                w->len > 2 ? w->pcm[2] : 0.0f, w->len > 3 ? w->pcm[3] : 0.0f);
    applyTone(*v, tone, part, part == 9);
    if (getenv("SD90_DEBUG") && tone)
        fprintf(stderr, "tone tvaL %u,%u,%u tvaT %u,%u,%u,%u envL %.2f,%.2f,%.2f tvfType %u cut %.1f depth %.1f ampL %.4f ampR %.4f\n",
            tone->tvaEnvL[0], tone->tvaEnvL[1], tone->tvaEnvL[2],
            tone->tvaEnvT[0], tone->tvaEnvT[1], tone->tvaEnvT[2], tone->tvaEnvT[3],
            v->envL[0], v->envL[1], v->envL[2], v->tvfType, v->tvfBaseCut, v->tvfDepth, v->ampL, v->ampR);
    v->env = 0.0f; v->stage = 0;
    float cut = (part == 9) ? 16000.0f : (2600.0f + vel * 55.0f);
    if (tone && part != 9 && v->tvfType != 0) {
        cut = 60.0f * powf(2.0f, clampf(v->tvfBaseCut + v->tvfDepth * v->tvfEnv, 10.0f, 135.0f) / 12.0f);
        cut = clampf(cut, 80.0f, 18500.0f);
    } else if (v->tvfType == 0) {
        cut = 18500.0f;
    }
    if (v->tvfType != 3 && v->inc > 1.05) {
        cut = std::min(cut, (float)(0.44 * sampleRate_ / v->inc));
    }
    updateTptSvf(cut, sampleRate_, v->tvfResDamp, v->svfA1, v->svfA2, v->svfA3);
    v->svfLow = v->svfBand = 0.0f;
    v->filtA = expf(-6.2831853f * cut / (float)sampleRate_);
    v->filt2A = v->filtA;
    v->filtL = v->filtR = 0.0f;
    v->filt2L = v->filt2R = 0.0f;
}


void Synth::advanceEnv(Voice& v, float dt) {
    if (v.released) {
        v.env -= v.env * dt / std::max(v.envT[3], 1e-4f);
        if (v.env <= 0.0005f) v.active = false;
        return;
    }
    if (v.stage == 0) {
        float target = v.envL[0];
        if (target <= 0.0005f) { v.env = 0.0f; v.stage = 1; }
        else {
            v.env += target * dt / std::max(v.envT[0], 1e-4f);
            if (v.env >= target) { v.env = target; v.stage = 1; }
        }
    } else if (v.stage == 1) {
        float a = v.envL[0], b = v.envL[1];
        if (v.env <= b) { v.env = b; v.stage = 2; }
        else v.env -= (a - b) * dt / std::max(v.envT[1], 1e-4f);
    } else if (v.stage == 2) {
        float a = v.envL[1], b = v.envL[2];
        if (v.env <= b) { v.env = b; v.stage = 3; }
        else v.env -= (a - b) * dt / std::max(v.envT[2], 1e-4f);
    } else {
        v.env = v.envL[2];
        if (v.env <= 0.0005f) v.active = false;
    }
}

void Synth::startOsc(int part, int note, int vel, int type) {
    Voice* v = nullptr;
    for (auto& x : voices_) if (!x.active) { v = &x; break; }
    if (!v) {
        uint64_t oldest = ~0ull;
        for (auto& x : voices_) if (x.age < oldest) { oldest = x.age; v = &x; }
    }
    if (!v) return;
    *v = Voice();
    v->active = true;
    v->part = part; v->note = note; v->vel = vel; v->age = ++ageCounter_;
    v->osc = type;
    v->baseFreq = 440.0 * pow(2.0, (note - 69) / 12.0);
    v->freq = v->baseFreq * partTuneRatio(part);
    v->phase = 0.0; v->phase2 = 0.25;
    const Part& p = parts_[part];
    const Patch* pt = nullptr;
    if (p.bankIndex >= 0 && p.bankIndex <= 10 && p.patchIndex < (int)banks_[p.bankIndex].patches.size())
        pt = &banks_[p.bankIndex].patches[p.patchIndex];
    float level = pt ? (pt->level / 127.0f) : 1.0f;
    float baseAmp = level * (vel / 127.0f) * 0.65f;
    float panOff = pt ? ((pt->pan - 64) / 127.0f) : 0.0f;
    v->baseAmp = baseAmp;
    v->panOffset = panOff;
    float partVol = powf(clampf(p.volume / 127.0f, 0.0f, 1.0f), 1.7f);
    float partExpr = powf(clampf(p.expression / 127.0f, 0.0f, 1.0f), 1.7f);
    float amp = baseAmp * partVol * partExpr;
    float pan = clampf(p.pan / 127.0f + panOff, 0.0f, 1.0f);
    float ang = pan * 1.5707963f;
    v->ampL = amp * cosf(ang);
    v->ampR = amp * sinf(ang);
    applyTone(*v, pt ? pt->firstActiveTone() : nullptr, part, false);
    if (!pt || !pt->firstActiveTone()) {
        v->envT[0] = 0.25f; v->envT[1] = 0.45f; v->envT[2] = 0.45f; v->envT[3] = 0.65f;
        v->envL[0] = 1.0f; v->envL[1] = 0.78f; v->envL[2] = 0.78f;
    }
    v->env = 0.0f; v->stage = 0;
    v->filtA = expf(-6.2831853f * 3200.0f / (float)sampleRate_);
    v->filt2A = expf(-6.2831853f * 6000.0f / (float)sampleRate_);
}

void Synth::noteOn(int ch, int note, int vel) {
    if (ch < 0 || ch > 15) return;
    Part& p = parts_[ch];
    if (ch == 9) {
        const Patch* rpt = p.hasSysExPatch ? &p.sysExPatch :
                           (banks_[9].patches.size() > (size_t)p.patchIndex ? &banks_[9].patches[p.patchIndex] : nullptr);
        if (rpt && note >= 0 && note < 128 && rpt->rhythmKeys[note].valid) {
            const RhythmKey& rk = rpt->rhythmKeys[note];
            if (rk.muteGroup > 0 && rk.muteGroup < 127) {
                for (auto& vx : voices_) {
                    if (vx.active && vx.part == 9 && vx.muteGroup == rk.muteGroup) {
                        vx.released = true; vx.envT[3] = 0.015f;
                    }
                }
            }
            bool anyStarted = false;
            size_t startIdx = 0;
            double drumTune = pow(2.0, (int(rk.coarse) - 60) / 12.0 + (int(rk.fine) - 64) / 1200.0);
            for (int t = 0; t < 4; ++t) {
                const RhythmLayer& rl = rk.layers[t];
                if (!rl.active || vel < rl.velLow || vel > rl.velHigh) continue;
                // SD-8 Bank files (A01..A11) store DMZ07 (SRX) indices under both wavGroupId=1 and 70;
                // SD-90 Native SysEx uses wavGroupId=70 for DMZ07 (SRX) and wavGroupId=1 for InternalA.
                const bool useSrx = (!p.hasSysExPatch && rl.wavIdL <= srx_.multisamples.size()) ||
                                    (rl.wavGroupId == 70 || rl.wavGroupId == 13);
                const Rom& rom = useSrx ? (!srx_.multisamples.empty() ? srx_ : internalA_)
                                        : (!internalA_.multisamples.empty() ? internalA_ : srx_);
                const Wave* wL = rom.pickMultisample(rl.wavIdL, 60);
                const Wave* wR = (rl.wavIdR != 0 && rl.wavIdR != rl.wavIdL) ? rom.pickMultisample(rl.wavIdR, 60) : nullptr;
                float gain = powf(rk.level / 127.0f, 1.5f) * powf(rl.level / 127.0f, 1.5f);
                if (rl.waveGain == 0) gain *= 0.5f;
                else if (rl.waveGain == 2) gain *= 1.414f;
                else if (rl.waveGain == 3) gain *= 2.0f;
                float basePan = clampf(float(rk.pan) + float(rl.pan - 64), 0.0f, 127.0f);
                double layerTune = drumTune * pow(2.0, (int(rl.coarse) - 64) / 12.0 + (int(rl.fine) - 64) / 1200.0);
                auto tuneNewest = [&](int targetNote) {
                    uint64_t bestAge = 0; Voice* newest = nullptr;
                    for (auto& vx : voices_)
                        if (vx.active && vx.part == 9 && vx.note == targetNote && vx.age > bestAge) { bestAge = vx.age; newest = &vx; }
                    if (newest) {
                        newest->baseInc *= layerTune;
                        newest->inc = newest->baseInc * partTuneRatio(9);
                        newest->muteGroup = rk.muteGroup;
                        newest->envT[0] = toneTime(rk.tvaEnvT[0]);
                        newest->envT[1] = toneTime(rk.tvaEnvT[1]);
                        newest->envT[2] = toneTime(rk.tvaEnvT[2]);
                        newest->envT[3] = toneTime(rk.tvaEnvT[3]);
                        newest->envL[0] = rk.tvaEnvL[0] / 127.0f;
                        newest->envL[1] = rk.tvaEnvL[1] / 127.0f;
                        newest->envL[2] = rk.tvaEnvL[2] / 127.0f;
                        if (rk.envMode == 0 && rk.tvaEnvL[2] == 0) {
                            newest->envT[2] = std::max(newest->envT[1] + newest->envT[2], 0.90f);
                        } else if (rk.tvaEnvL[2] >= 120) {
                            newest->envL[2] = 0.75f; // natural cymbal/hihat tail until w->len
                        }
                        if (rk.tvfType != 0 && rk.tvfCutoff < 127) {
                            float c = clampf(60.0f * powf(2.0f, rk.tvfCutoff / 12.0f), 120.0f, 18500.0f);
                            newest->tvfType = rk.tvfType;
                            newest->tvfResDamp = clampf(1.414f - (rk.tvfResonance / 127.0f) * 1.1f, 0.28f, 1.414f);
                            updateTptSvf(c, sampleRate_, newest->tvfResDamp, newest->svfA1, newest->svfA2, newest->svfA3);
                        }
                    }
                    (void)startIdx;
                };
                if (wL && wR) {
                    startVoice(ch, note, vel, wL, nullptr, std::max(0.0f, basePan - 32.0f), gain * 0.707f);
                    tuneNewest(note);
                    startVoice(ch, note, vel, wR, nullptr, std::min(127.0f, basePan + 32.0f), gain * 0.707f);
                    tuneNewest(note);
                    anyStarted = true;
                } else if (wL) {
                    startVoice(ch, note, vel, wL, nullptr, basePan, gain);
                    tuneNewest(note);
                    anyStarted = true;
                }
            }
            if (anyStarted) {
                p.heldNotes.push_back(note);
                return;
            }
        }
        const Wave* w = nullptr;
        int wid = drumWaveId(note);
        if (wid >= 0) {
            int grp = srx_.groupForWave((uint16_t)wid);
            w = pickWave(srx_, grp, note);
            if (!w) w = pickWave(srx_, grp, 60);
        }
        if (!w) w = pickWave(srx_, 0, note);
        if (w) {
            startVoice(ch, note, vel, w);
            p.heldNotes.push_back(note);
        }
        return;
    }

    if (partWave_[ch] >= 0) {
        const Wave* w = internalA_.pickMultisample(partWave_[ch], note);
        if (!w) w = srx_.pickMultisample(partWave_[ch], note);
        if (!w) w = pickWave(melodicRom(), 0, note);
        if (w) {
            startVoice(ch, note, vel, w);
            p.heldNotes.push_back(note);
        }
        return;
    }

    const Bank& bank0 = banks_[p.bankIndex];
    const Patch* pt0 = p.hasSysExPatch ? &p.sysExPatch :
                       ((p.patchIndex < (int)bank0.patches.size()) ? &bank0.patches[p.patchIndex] : nullptr);
    if (pt0) {
        std::vector<const KoaTone*> activeTones;
        for (int t = 0; t < 4; ++t) {
            const KoaTone& tn = pt0->tones[t];
            if (!tn.active() || !tn.tmtOn) continue;
            if (note < tn.keyLow || note > tn.keyHigh) continue;
            if (vel < tn.velLow || vel > tn.velHigh) continue;
            if (tn.wavIdL == 0 || tn.wavIdL >= 16384) continue;
            activeTones.push_back(&tn);
        }
        if (!activeTones.empty()) {
            float layerScale = 1.0f / powf((float)activeTones.size(), 0.75f);
            bool anyStarted = false;
            for (const KoaTone* tn : activeTones) {
                // SD-8 Bank files (A01..A11) store DMZ07 (SRX) indices under both wavGroupId=1 and 70;
                // SD-90 Native SysEx uses wavGroupId=70 for DMZ07 (SRX) and wavGroupId=1 for InternalA.
                const bool useSrx = (!p.hasSysExPatch && tn->wavIdL <= srx_.multisamples.size()) ||
                                    (tn->wavGroupId == 70 || tn->wavGroupId == 13);
                const Rom& rom = useSrx ? (!srx_.multisamples.empty() ? srx_ : internalA_)
                                        : (!internalA_.multisamples.empty() ? internalA_ : srx_);
                const Wave* wL = rom.pickMultisample(tn->wavIdL, note);
                const Wave* wR = (tn->wavIdR != 0 && tn->wavIdR != tn->wavIdL) ?
                                 rom.pickMultisample(tn->wavIdR, note) : nullptr;
                if (wL && wR) {
                    float panL = std::max(0.0f, float(tn->pan) - 32.0f);
                    float panR = std::min(127.0f, float(tn->pan) + 32.0f);
                    startVoice(ch, note, vel, wL, tn, panL, layerScale * 0.707f);
                    startVoice(ch, note, vel, wR, tn, panR, layerScale * 0.707f);
                    anyStarted = true;
                } else if (wL) {
                    startVoice(ch, note, vel, wL, tn, -1.0f, layerScale);
                    anyStarted = true;
                }
            }
            if (anyStarted) {
                p.heldNotes.push_back(note);
                return;
            }
        }
        if (!pt0->firstActiveTone() && pt0->category >= 24 && pt0->category <= 30) {
            startOsc(ch, note, vel, (pt0->category == 30) ? 2 : 1);
            p.heldNotes.push_back(note);
            return;
        }
    }

    const Rom& fb = melodicRom();
    const Wave* w = pickWave(fb, 0, note);
    if (w) {
        startVoice(ch, note, vel, w);
        p.heldNotes.push_back(note);
    }
}

void Synth::noteOff(int ch, int note) {
    if (ch < 0 || ch > 15) return;
    Part& p = parts_[ch];
    if (p.sustain) return;
    for (auto& v : voices_) {
        if (v.active && !v.released && v.part == ch && v.note == note) {
            v.released = true; v.stage = 3;
        }
    }
}

void Synth::allNotesOff(int ch) {
    if (ch < 0 || ch > 15) { for (int i = 0; i < 16; ++i) allNotesOff(i); return; }
    for (auto& v : voices_)
        if (v.active && !v.released && v.part == ch) { v.released = true; v.stage = 3; }
    parts_[ch].heldNotes.clear();
}

void Synth::controlChange(int ch, int cc, int val) {
    if (ch < 0 || ch > 15) return;
    Part& p = parts_[ch];
    switch (cc) {
    case 0: p.bankMsb = val; selectPatch(ch); break;
    case 1: p.modWheel = (float)val; break;
    case 32: p.bankLsb = val; selectPatch(ch); break;
    case 6:
        p.dataMsb = val;
        if (p.rpnMsb == 0 && p.rpnLsb == 0) p.bendRange = std::max(1, std::min(24, val));
        else if (p.rpnMsb == 0 && p.rpnLsb == 2) {
            p.coarseTune = val - 64;
            updatePartPitch(ch);
        } else if (p.rpnMsb == 0 && p.rpnLsb == 1) {
            int v14 = ((p.dataMsb & 0x7F) << 7) | (p.dataLsb & 0x7F);
            p.fineTuneCents = (v14 - 8192) * (100.0f / 8192.0f);
            updatePartPitch(ch);
        }
        break;
    case 38:
        p.dataLsb = val;
        if (p.rpnMsb == 0 && p.rpnLsb == 1) {
            int v14 = ((p.dataMsb & 0x7F) << 7) | (p.dataLsb & 0x7F);
            p.fineTuneCents = (v14 - 8192) * (100.0f / 8192.0f);
            updatePartPitch(ch);
        }
        break;
    case 7: p.volume = (float)val; updatePartVolume(ch); break;
    case 10: p.pan = (float)val; updatePartVolume(ch); break;
    case 11: p.expression = (float)val; updatePartVolume(ch); break;
    case 64:
        if (val < 64 && p.sustain) {
            p.sustain = false;
            for (auto& v : voices_)
                if (v.active && !v.released && v.part == ch) { v.released = true; v.stage = 3; }
        }
        p.sustain = val >= 64;
        break;
    case 91: p.reverbSend = (float)val; break;
    case 93: p.chorusSend = (float)val; break;
    case 100: p.rpnLsb = val; break;
    case 101: p.rpnMsb = val; break;
    case 120: for (auto& v : voices_) if (v.active && v.part == ch) v.active = false; break;
    case 121:
        p.pitchBend = 0; p.bendMul = 1.0f; p.expression = 127; p.sustain = false; p.bendRange = 2;
        p.modWheel = 0.0f; p.reverbSend = 40.0f; p.chorusSend = 0.0f;
        p.rpnMsb = 127; p.rpnLsb = 127;
        break;
    case 123: allNotesOff(ch); break;
    default: break;
    }
}

void Synth::updatePartVolume(int ch) {
    if (ch < 0 || ch > 15) return;
    const Part& p = parts_[ch];
    float partVol = powf(clampf(p.volume / 127.0f, 0.0f, 1.0f), 1.7f);
    float partExpr = powf(clampf(p.expression / 127.0f, 0.0f, 1.0f), 1.7f);
    float gainMul = partVol * partExpr;
    float basePan = p.pan / 127.0f;
    for (auto& v : voices_) {
        if (!v.active || v.part != ch) continue;
        float amp = v.baseAmp * gainMul;
        float pan = clampf(basePan + v.panOffset, 0.0f, 1.0f);
        float ang = pan * 1.5707963f;
        v.ampL = amp * cosf(ang);
        v.ampR = amp * sinf(ang);
    }
}

double Synth::partTuneRatio(int ch) const {
    if (ch < 0 || ch > 15) return 1.0;
    const Part& p = parts_[ch];
    double semis = masterTuneSemis_ + double(p.userTuneSemis);
    if (ch != 9) semis += double(p.coarseTune) + double(p.fineTuneCents) / 100.0;
    return pow(2.0, semis / 12.0);
}

void Synth::updatePartPitch(int ch) {
    if (ch < 0 || ch > 15) return;
    double mul = partTuneRatio(ch);
    for (auto& v : voices_) {
        if (!v.active || v.part != ch) continue;
        if (v.osc) v.freq = v.baseFreq * mul;
        else v.inc = v.baseInc * mul;
    }
}

void Synth::setPartTune(int ch, float semitones) {
    if (ch < 0 || ch > 15) return;
    parts_[ch].userTuneSemis = clampf(semitones, -24.0f, 24.0f);
    updatePartPitch(ch);
}

float Synth::partTune(int ch) const {
    if (ch < 0 || ch > 15) return 0.0f;
    return parts_[ch].userTuneSemis;
}

void Synth::setMasterTuneHz(double a4Hz) {
    if (a4Hz <= 200.0 || a4Hz >= 1000.0) return;
    masterTuneSemis_ = 12.0 * log2(a4Hz / 440.0);
    for (int ch = 0; ch < 16; ++ch) updatePartPitch(ch);
}

void Synth::pitchBend(int ch, int val) {
    if (ch < 0 || ch > 15) return;
    Part& p = parts_[ch];
    p.pitchBend = val - 8192;
    p.bendMul = (float)pow(2.0, (p.pitchBend / 8192.0) * p.bendRange / 12.0);
}

void Synth::applyGsReset() {
    masterTuneSemis_ = 0.0;
    for (int i = 0; i < 16; ++i) {
        float savedUserTune = parts_[i].userTuneSemis;
        parts_[i] = Part();
        parts_[i].userTuneSemis = savedUserTune;
        parts_[i].drum = (i == 9);
        parts_[i].bankIndex = (i == 9) ? 9 : 0;
        parts_[i].patchIndex = 0;
        parts_[i].volume = 100;
        parts_[i].pan = 64;
        parts_[i].expression = 127;
        parts_[i].bendRange = 2;
        selectPatch(i);
    }
    for (auto& v : voices_) v.active = false;
    for (int i = 0; i < 16; ++i) { partWave_[i] = -1; partGain_[i] = 100; }
    clearEffects();
    masterVolume_ = 0.8f;
}

void Synth::setPartWave(int part, int waveId, int gain) {
    if (part < 0 || part > 15) return;
    if (waveId > 0) {
        partWave_[part] = waveId;
        partGain_[part] = (gain >= 0 && gain <= 127) ? gain : 100;
    }
}

void Synth::sysEx(const uint8_t* data, int len) {
    if (len < 5) return;
    auto applyRolandMasterTune = [&](const uint8_t* n) {
        int val16 = ((n[0] & 0xF) << 12) | ((n[1] & 0xF) << 8) | ((n[2] & 0xF) << 4) | (n[3] & 0xF);
        double cents = (val16 - 0x0400) * 0.1;
        masterTuneSemis_ = cents / 100.0;
        for (int ch = 0; ch < 16; ++ch) updatePartPitch(ch);
    };
    // Roland SD-90 DT1: F0 41 <dev> 00 48 12 <aa> <bb> <cc> <dd> <payload...> <sum> F7
    if (useNativeSysex_ && len >= 12 && data[0] == 0xF0 && data[1] == 0x41 &&
        data[3] == 0x00 && data[4] == 0x48 && data[5] == 0x12) {
        uint8_t a0 = data[6], a1 = data[7], a2 = data[8], a3 = data[9];
        const uint8_t* p = data + 10;
        int plen = len - 12; // exclude checksum + F7 (or trailing bytes)
        if (plen <= 0) return;

        // 00 00 00 00: SD-90 Native Setup / Reset
        if (a0 == 0x00 && a1 == 0x00 && a2 == 0x00 && a3 == 0x00 && plen >= 1) {
            applyGsReset();
            return;
        }

        // 01 00 00 00: SD-90 System Common Master Tune (4 nibbles, 0.1 cents/step, 0x0400 = 0)
        if (a0 == 0x01 && a1 == 0x00 && a2 == 0x00 && a3 == 0x00 && plen >= 4) {
            applyRolandMasterTune(p);
            return;
        }

        // 10 00 2p 00: Port A Part p Setup (MSB/LSB/PC/Level/Pan/Coarse/Fine)
        if (a0 == 0x10 && a1 == 0x00 && a2 >= 0x20 && a2 <= 0x2F && a3 == 0x00 && plen >= 11) {
            int part = a2 & 0x0F;
            Part& prt = parts_[part];
            prt.bankMsb = p[4];
            prt.bankLsb = p[5];
            prt.program = p[6];
            // p[7] is the Part Level (same role as CC7); do not double-attenuate against CC7.
            prt.volume = (p[7] <= 127) ? (float)p[7] : 100.0f;
            prt.pan = (p[8] <= 127) ? (float)p[8] : 64.0f;
            prt.coarseTune = int(p[9]) - 64;
            prt.fineTuneCents = float(int(p[10]) - 64);
            selectPatch(part);
            updatePartVolume(part);
            updatePartPitch(part);
            return;
        }

        // 13 30/31 xx 00: Port A Part 9 Temporary Rhythm Kit
        if (a0 == 0x13 && (a1 == 0x30 || a1 == 0x31) && a3 == 0x00) {
            Part& prt = parts_[9];
            Patch& sp = prt.sysExPatch;
            sp.isRhythm = true;
            sp.valid = true;
            prt.hasSysExPatch = true;
            if (a1 == 0x30 && a2 == 0x00 && plen >= 12) {
                sp.name = trimName(p, 12);
            } else if ((a1 == 0x30 && a2 >= 0x10 && (a2 & 1) == 0) || (a1 == 0x31 && (a2 & 1) == 0)) {
                int note = (a1 == 0x30) ? (21 + (a2 - 0x10) / 2) : (77 + a2 / 2);
                if (note >= 0 && note < 128) parseRhythmKey(sp.rhythmKeys[note], p, plen, true);
            }
            return;
        }

        // 11..14 pp xx 00: Port A Temporary Patch Common / TMT / Tone 1..4
        if (a0 >= 0x11 && a0 <= 0x14 && (a1 == 0x00 || a1 == 0x20 || a1 == 0x40 || a1 == 0x60) && a3 == 0x00) {
            int part = int(a0 - 0x11) * 4 + (a1 >> 5);
            if (part >= 0 && part < 16) {
                Part& prt = parts_[part];
                Patch& sp = prt.sysExPatch;
                if (a2 == 0x00 && plen >= 16) {
                    sp.name = trimName(p, 12);
                    sp.category = p[0x0C];
                    sp.level = (p[0x0E] <= 127) ? p[0x0E] : 100;
                    sp.pan = (p[0x0F] <= 127) ? p[0x0F] : 64;
                    sp.valid = true;
                    prt.hasSysExPatch = true;
                } else if (a2 == 0x10 && plen >= 41) {
                    for (int t = 0; t < 4; ++t) {
                        const uint8_t* tm = p + 5 + size_t(t) * 9;
                        sp.tones[t].tmtOn = (tm[0] != 0);
                        sp.tones[t].keyLow = tm[1];
                        sp.tones[t].keyHigh = tm[2];
                        sp.tones[t].velLow = tm[5];
                        sp.tones[t].velHigh = tm[6];
                    }
                    prt.hasSysExPatch = true;
                } else if ((a2 == 0x20 || a2 == 0x22 || a2 == 0x24 || a2 == 0x26) && plen >= 0x89) {
                    int ti = (a2 - 0x20) >> 1;
                    KoaTone tn = parseKoaTone(p, plen);
                    // Preserve TMT fields if TMT block arrived earlier
                    tn.tmtOn = sp.tones[ti].tmtOn;
                    tn.keyLow = sp.tones[ti].keyLow;
                    tn.keyHigh = sp.tones[ti].keyHigh;
                    tn.velLow = sp.tones[ti].velLow;
                    tn.velHigh = sp.tones[ti].velHigh;
                    if (tn.wavIdL > 0 && tn.wavIdL < 16384) tn.wavGroupType = 1;
                    sp.tones[ti] = tn;
                    sp.valid = true;
                    prt.hasSysExPatch = true;
                }
            }
            return;
        }
        return;
    }
    // GM System On / GM Off / GM2 System On: F0 7E <dev> 09 01/02/03 F7
    if (len >= 6 && data[0] == 0xF0 && data[1] == 0x7E && data[3] == 0x09 &&
        (data[4] == 0x01 || data[4] == 0x02 || data[4] == 0x03)) {
        applyGsReset();
        return;
    }
    // XG System On: F0 43 <1n> 4C 00 00 7E 00 F7
    if (len >= 9 && data[0] == 0xF0 && data[1] == 0x43 && (data[2] & 0xF0) == 0x10 &&
        data[3] == 0x4C && data[4] == 0x00 && data[5] == 0x00 && data[6] == 0x7E && data[7] == 0x00) {
        applyGsReset();
        return;
    }
    // XG Master Tune: F0 43 <1n> 4C 00 00 00 n3 n2 n1 n0 F7
    if (len >= 12 && data[0] == 0xF0 && data[1] == 0x43 && (data[2] & 0xF0) == 0x10 &&
        data[3] == 0x4C && data[4] == 0x00 && data[5] == 0x00 && data[6] == 0x00) {
        applyRolandMasterTune(data + 7);
        return;
    }
    // GS Master Tune: F0 41 <dev> 42 12 40 00 00 n3 n2 n1 n0 <sum> F7
    if (len >= 14 && data[0] == 0xF0 && data[1] == 0x41 && data[3] == 0x42 &&
        data[4] == 0x12 && data[5] == 0x40 && data[6] == 0x00 && data[7] == 0x00) {
        applyRolandMasterTune(data + 8);
        return;
    }
    // GM2/Universal Master Fine Tuning: F0 7F 7F 04 03 lsb msb F7
    if (len >= 8 && data[0] == 0xF0 && data[1] == 0x7F && data[3] == 0x04 && data[4] == 0x03) {
        int v14 = ((data[6] & 0x7F) << 7) | (data[5] & 0x7F);
        masterTuneSemis_ = (v14 - 8192) / 8192.0;
        for (int ch = 0; ch < 16; ++ch) updatePartPitch(ch);
        return;
    }
    // GM2/Universal Master Coarse Tuning: F0 7F 7F 04 04 lsb msb F7
    if (len >= 8 && data[0] == 0xF0 && data[1] == 0x7F && data[3] == 0x04 && data[4] == 0x04) {
        int semi = int(data[6] & 0x7F) - 64;
        masterTuneSemis_ = double(semi);
        for (int ch = 0; ch < 16; ++ch) updatePartPitch(ch);
        return;
    }
    // GS Reset: F0 41 <dev> 42 12 40 00 7F 00 41 F7
    if (len >= 11 && data[0] == 0xF0 && data[1] == 0x41 && data[3] == 0x42 &&
        data[4] == 0x12 && data[5] == 0x40 && data[6] == 0x00 && data[7] == 0x7F && data[8] == 0x00) {
        applyGsReset();
        return;
    }
}

void Synth::queueMidi(int deltaFrames, const uint8_t* msg, int len) {
    if (len <= 0) return;
    if (msg[0] == 0xF0) {
        sysEx(msg, len);
        return;
    }
    MidiEvent e;
    e.frames = std::max(0, deltaFrames);
    e.len = std::min(len, 16);
    memcpy(e.data, msg, e.len);
    queue_.push_back(e);
}

void Synth::render(float* left, float* right, int frames) {
    if (!left || !right || frames <= 0) return;
    memset(left, 0, sizeof(float) * frames);
    memset(right, 0, sizeof(float) * frames);
    if (!ready_) return;
    std::stable_sort(queue_.begin(), queue_.end(),
                     [](const MidiEvent& a, const MidiEvent& b) { return a.frames < b.frames; });
    size_t qi = 0;
    for (int i = 0; i < frames; ++i) {
        while (qi < queue_.size() && queue_[qi].frames <= i) {
            MidiEvent& e = queue_[qi++];
            uint8_t st = e.data[0];
            int chn = st & 0x0F, hi = st & 0xF0;
            if (hi == 0x90) { if (e.data[2]) noteOn(chn, e.data[1], e.data[2]); else noteOff(chn, e.data[1]); }
            else if (hi == 0x80) noteOff(chn, e.data[1]);
            else if (hi == 0xB0) controlChange(chn, e.data[1], e.data[2]);
            else if (hi == 0xC0) { parts_[chn].program = e.data[1]; selectPatch(chn); }
            else if (hi == 0xE0) pitchBend(chn, e.data[1] | (e.data[2] << 7));
        }
        float l = 0, r = 0, revL = 0, revR = 0, choL = 0, choR = 0;
        for (auto& v : voices_) {
            if (!v.active) continue;
            float vl = 0, vr = 0;
            renderVoice(v, vl, vr);
            l += vl; r += vr;
            revL += vl * v.revSend; revR += vr * v.revSend;
            choL += vl * v.choSend; choR += vr * v.choSend;
        }
        // Stereo Chorus
        if (!choBufL_.empty()) {
            size_t n = choBufL_.size();
            choBufL_[choIdx_] = choL;
            choBufR_[choIdx_] = choR;
            double mod1 = 0.5 + 0.5 * sin(choPhase_);
            double mod2 = 0.5 + 0.5 * cos(choPhase_);
            double d1 = (0.012 + 0.004 * mod1) * sampleRate_;
            double d2 = (0.014 + 0.004 * mod2) * sampleRate_;
            auto tap = [&](const std::vector<float>& b, double d) {
                double p = double(choIdx_) - d;
                while (p < 0.0) p += double(n);
                size_t i0 = size_t(p) % n, i1 = (i0 + 1) % n;
                float fr = float(p - floor(p));
                return b[i0] * (1.0f - fr) + b[i1] * fr;
            };
            float cL = tap(choBufL_, d1), cR = tap(choBufR_, d2);
            l += (cL + 0.35f * cR) * 0.45f;
            r += (cR + 0.35f * cL) * 0.45f;
            choIdx_ = (choIdx_ + 1) % n;
            choPhase_ += 6.28318530718 * 0.85 / sampleRate_;
            if (choPhase_ >= 6.28318530718) choPhase_ -= 6.28318530718;
        }
        // Stereo Reverb
        {
            float inL = (revL + revR * 0.35f) * 0.18f;
            float inR = (revR + revL * 0.35f) * 0.18f;
            float rL = 0.0f, rR = 0.0f;
            for (int c = 0; c < 4; ++c) {
                rL += revCombL_[c].process(inL, 0.80f, 0.35f);
                rR += revCombR_[c].process(inR, 0.80f, 0.35f);
            }
            for (int a = 0; a < 2; ++a) {
                rL = revApL_[a].process(rL);
                rR = revApR_[a].process(rR);
            }
            l += rL * 0.28f + rR * 0.10f;
            r += rR * 0.28f + rL * 0.10f;
        }
        l *= masterVolume_; r *= masterVolume_;
        // Transparent below 0.85; smooth soft-knee limiting only on extreme peaks
        auto softLimit = [](float x) {
            float ax = fabsf(x);
            if (ax <= 0.85f) return x;
            float s = 0.85f + 0.15f * tanhf((ax - 0.85f) / 0.15f);
            return (x < 0.0f) ? -s : s;
        };
        left[i] = softLimit(l);
        right[i] = softLimit(r);
    }
    queue_.erase(queue_.begin(), queue_.begin() + qi);
    for (auto& e : queue_) e.frames = std::max(0, e.frames - frames);
}

void Synth::renderVoice(Voice& v, float& outL, float& outR) {
    const Wave* w = v.wave;
    if (!v.osc && (!w || !w->ready() || v.pos < 0.0 || v.pos >= (double)w->len)) { v.active = false; return; }
    const Part& p = parts_[v.part];
    float x = 0.0f;
    if (v.osc) {
        double ph = v.phase;
        if (v.osc == 1) {
            x = (float)(2.0 * (ph - floor(ph + 0.5))) + 0.35f * sinf(6.2831853f * (float)(ph * 1.004));
        } else if (v.osc == 2) {
            x = 0.6f * sinf(6.2831853f * (float)ph) + 0.4f * sinf(6.2831853f * (float)(v.phase2)) + 0.15f * sinf(12.5663706f * (float)ph);
        } else {
            x = ((float)rand() / RAND_MAX) * 1.2f - 0.6f;
        }
        v.phase += v.freq * p.bendMul / sampleRate_;
        if (v.phase >= 1.0) v.phase -= 1.0;
        v.phase2 += v.freq * 1.005 * p.bendMul / sampleRate_;
        if (v.phase2 >= 1.0) v.phase2 -= 1.0;
    } else {
        auto at = [&](long i, long ip) -> float {
            long ls = (long)w->loopStart, le = (long)w->loopEnd;
            if (w->looped() && !v.oneShot) {
                long span = le - ls;
                if (span < 2) span = 1;
                if (w->loopMode == 2) {
                    if (i >= le) i = le - 1 - ((i - le) % span);
                    else if (i < ls && ip >= ls) i = ls + ((ls - i) % span);
                } else {
                    if (i >= le) i = ls + ((i - ls) % span);
                    else if (i < ls && ip >= ls) i = ls + (((i - ls) % span) + span) % span;
                }
            }
            if (i < 0) i = 0;
            if (i >= (long)w->len) i = (long)w->len - 1;
            return w->pcm[i];
        };
        long ip = (long)v.pos;
        float fr = (float)(v.pos - (double)ip);
        float y0 = at(ip - 1, ip), y1 = at(ip, ip), y2 = at(ip + 1, ip), y3 = at(ip + 2, ip);
        float c0 = y1;
        float c1 = 0.5f * (y2 - y0);
        float c2v = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        x = ((c3 * fr + c2v) * fr + c1) * fr + c0;
    }
    // envelope
    float dt = 1.0f / (float)sampleRate_;
    advanceEnv(v, dt);
    if (!v.active) return;
    // advance TVF envelope and update cutoff every 16 samples
    if (v.tvfDepth != 0.0f && v.tvfType != 0) {
        if (v.released) {
            float target = v.tvfEnvL[4];
            v.tvfEnv += (target - v.tvfEnv) * std::min(1.0f, dt / std::max(v.tvfEnvT[3], 1e-4f));
        } else if (v.tvfStage < 3) {
            float target = v.tvfEnvL[v.tvfStage + 1];
            float dur = std::max(v.tvfEnvT[v.tvfStage], 1e-4f);
            float step = dt / dur;
            if (fabsf(target - v.tvfEnv) <= step) {
                v.tvfEnv = target;
                v.tvfStage++;
            } else {
                v.tvfEnv += (target > v.tvfEnv ? 1.0f : -1.0f) * step;
            }
        }
        if ((++v.tvfTick & 15) == 0) {
            float cut = 60.0f * powf(2.0f, clampf(v.tvfBaseCut + v.tvfDepth * v.tvfEnv, 10.0f, 135.0f) / 12.0f);
            cut = clampf(cut, 80.0f, 18500.0f);
            if (v.tvfType != 3 && v.inc > 1.05) {
                cut = std::min(cut, (float)(0.44 * sampleRate_ / v.inc));
            }
            v.filtA = expf(-6.2831853f * cut / (float)sampleRate_);
            v.filt2A = v.filtA;
            if (v.tvfType == 2 || v.tvfType == 3) {
                updateTptSvf(cut, sampleRate_, v.tvfResDamp, v.svfA1, v.svfA2, v.svfA3);
            }
        }
    }
    // Use TPT SVF for HPF(3)/BPF(2); warm cascaded one-pole LPF for Roland TVF LPF/OFF.
    float y = x;
    if (v.tvfType == 2 || v.tvfType == 3) {
        float v3 = x - v.svfBand;
        float v1 = v.svfA1 * v.svfLow + v.svfA2 * v3;
        float v2 = v.svfBand + v.svfA2 * v.svfLow + v.svfA3 * v3;
        v.svfLow = 2.0f * v1 - v.svfLow;
        v.svfBand = 2.0f * v2 - v.svfBand;
        y = (v.tvfType == 3) ? (x - v.tvfResDamp * v1 - v2) : v1;
    } else if (v.tvfType != 0) {
        v.filtL = v.filtL * v.filtA + x * (1.0f - v.filtA);
        v.filt2L = v.filt2L * v.filt2A + v.filtL * (1.0f - v.filt2A);
        y = v.filt2L;
    } else if (v.inc > 1.05) {
        v.filtL = v.filtL * v.filtA + x * (1.0f - v.filtA);
        y = v.filtL;
    }
    outL += y * v.env * v.ampL;
    outR += y * v.env * v.ampR;
    // advance
    if (v.osc) return;
    double lfoMul = 1.0;
    v.lfoAgeSec += dt;
    float vibSemis = v.lfoPitchSemis + (p.modWheel / 127.0f) * 0.35f;
    if (vibSemis != 0.0f && v.lfoAgeSec >= v.lfoDelaySec) {
        v.lfoPhase += v.lfoRateHz * dt;
        if (v.lfoPhase >= 1.0f) v.lfoPhase -= 1.0f;
        float s = sinf(6.2831853f * v.lfoPhase);
        lfoMul = pow(2.0, (s * vibSemis) / 12.0);
    }
    v.pos += v.dir * v.inc * p.bendMul * lfoMul;
    if (w->looped() && !v.oneShot) {
        if (w->loopMode == 2) {
            double hi = double(w->loopEnd > 1 ? w->loopEnd - 1 : w->loopEnd);
            double lo = double(w->loopStart);
            if (v.dir > 0 && v.pos >= hi) {
                v.pos = hi - (v.pos - hi);
                v.dir = -1;
            } else if (v.dir < 0 && v.pos <= lo) {
                v.pos = lo + (lo - v.pos);
                v.dir = 1;
            }
            if (v.pos < 0.0) v.pos = 0.0;
            if (v.pos >= (double)w->len) v.pos = double(w->len - 1);
        } else if (v.pos >= (double)w->loopEnd) {
            double span = double(w->loopEnd - w->loopStart);
            if (span > 1) v.pos = w->loopStart + fmod(v.pos - w->loopStart, span);
            else v.pos = w->loopStart;
        }
    } else if (v.pos < 0.0 || v.pos >= (double)w->len) {
        v.active = false;
    }
}
