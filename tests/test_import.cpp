#include "test_utils.h"
#include "misuc/engine/flp/FlpImport.h"
#include "misuc/engine/flp/FlpReader.h"
#include "misuc/engine/project/Session.h"

#include <zlib.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace misuc::flp;
using namespace misuc::project;

namespace {

// ─── Tiny WAV writer (real sample file for the sampler materialization) ─────

static void writeToneWav(const std::string& path, uint16_t sr = 48000,
                          uint16_t sampleCount = 128) {
    std::vector<uint8_t> b;
    auto le16 = [&b](uint16_t u) {
        b.push_back(uint8_t(u));
        b.push_back(uint8_t(u >> 8));
    };
    auto le32 = [&b](uint32_t u) {
        b.push_back(uint8_t(u));
        b.push_back(uint8_t(u >> 8));
        b.push_back(uint8_t(u >> 16));
        b.push_back(uint8_t(u >> 24));
    };
    const uint32_t dataBytes = uint32_t(sampleCount) * 2;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    le32(36 + dataBytes);
    b.insert(b.end(), {'W', 'A', 'V', 'E'});
    b.insert(b.end(), {'f', 'm', 't', ' '});
    le32(16);
    le16(1);   // PCM
    le16(1);   // mono
    le32(sr);
    le32(uint32_t(sr) * 2);
    le16(2);   // block align
    le16(16);  // bits
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    le32(dataBytes);
    for (uint16_t i = 0; i < sampleCount; ++i) {
        const double t = double(i) / double(sr);
        const int16_t s = int16_t(7000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * t));
        le16(uint16_t(s));
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(b.data(), 1, b.size(), f);
        std::fclose(f);
    }
}

// zlib (RFC-1950) deflate — used to make the "zipped .flp" round-trip fixture.
static std::vector<uint8_t> zlibDeflate(const std::vector<uint8_t>& in) {
    if (in.empty())
        return {};
    z_stream zs{};
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK)
        return {};
    std::vector<uint8_t> out(in.size() + in.size() / 64 + 1024);
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = uInt(in.size());
    zs.next_out = out.data();
    zs.avail_out = uInt(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    out.resize(size_t(zs.total_out));
    deflateEnd(&zs);
    return (rc == Z_STREAM_END) ? out : std::vector<uint8_t>{};
}

// ─── Tiny FLP file builder (LE helpers + FLdt event emission) ───────────────

static void pushU16(std::vector<uint8_t>& v, uint16_t u) {
    v.push_back(uint8_t(u));
    v.push_back(uint8_t(u >> 8));
}
static void pushU32(std::vector<uint8_t>& v, uint32_t u) {
    v.push_back(uint8_t(u));
    v.push_back(uint8_t(u >> 8));
    v.push_back(uint8_t(u >> 16));
    v.push_back(uint8_t(u >> 24));
}
static void pushVarint(std::vector<uint8_t>& v, uint64_t n) {
    do {
        uint8_t byte = uint8_t(n & 0x7Fu);
        n >>= 7;
        if (n) byte |= 0x80u;
        v.push_back(byte);
    } while (n);
}
// Encode an ASCII string as a FL 16-bit text event: chars are padded
// with null bytes to two bytes each, terminated by two extra zero bytes.
static void pushAsciiText(std::vector<uint8_t>& v, uint8_t id,
                           const char* s) {
    v.push_back(id);
    std::vector<uint8_t> payload;
    for (; *s; ++s) {
        payload.push_back(uint8_t(*s));
        payload.push_back(0x00);
    }
    payload.push_back(0x00);
    payload.push_back(0x00);
    pushVarint(v, uint64_t(payload.size()));
    v.insert(v.end(), payload.begin(), payload.end());
}
static void pushPayloadEvent(std::vector<uint8_t>& v, uint8_t id,
                              const std::vector<uint8_t>& payload) {
    v.push_back(id);
    pushVarint(v, uint64_t(payload.size()));
    v.insert(v.end(), payload.begin(), payload.end());
}
static void pushFixedEvent(std::vector<uint8_t>& v, uint8_t id,
                            std::initializer_list<uint8_t> payload) {
    v.push_back(id);
    v.insert(v.end(), payload.begin(), payload.end());
}
// Append a 24-byte note struct.
static void pushNote24(std::vector<uint8_t>& v, uint32_t tick,
                        uint16_t flags, uint16_t channel, uint32_t len,
                        uint16_t key, uint16_t group, uint8_t pan,
                        uint8_t vel) {
    pushU32(v, tick);
    pushU16(v, flags);
    pushU16(v, channel);
    pushU32(v, len);
    pushU16(v, key);
    pushU16(v, group);
    v.insert(v.end(), {0, 0, 0, 0, pan, vel, 0, 0}); // fine, _, rel, midi, pan, vel, mx, my
}

static std::vector<uint8_t> buildTinyFp() {
    std::vector<uint8_t> ev;

    // Project version
    pushAsciiText(ev, 199, "24.0");                    // FLVersion

    // One channel (iid 0, type 1=FLKeys, insert 0)
    pushFixedEvent(ev, 64, {0x00, 0x00});              // NewChannel iid=0
    pushFixedEvent(ev, 21, {0x01});                     // type = 1 (FLKeys)
    pushFixedEvent(ev, 22, {0x00});               // insert = 0
    pushAsciiText(ev, 203, "Synth");                   // channel name

    // One pattern (iid 0)
    pushFixedEvent(ev, 65, {0x00, 0x00});              // NewPattern iid=0
    pushAsciiText(ev, 193, "P1");                      // pattern name

    // Three 24-byte notes (72 bytes)
    std::vector<uint8_t> notePayload;
    pushNote24(notePayload, 24, 0x0008u, 0, 48, 66, 0, 100, 120); // slide=true
    pushNote24(notePayload, 72, 0, 0, 24, 48, 0, 192, 5);
    pushNote24(notePayload, 0, 0, 0, 96, 72, 0, 64, 96);
    pushPayloadEvent(ev, 224, notePayload);            // Notes

    // Build FLhd/FLdt container.
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd'};
    pushU32(f, 6);                                     // header size
    pushU16(f, 0);                                     // format
    pushU16(f, 1);                                     // channels (header)
    pushU16(f, 96);                                    // ppq
    f.insert(f.end(), {'F', 'L', 'd', 't'});
    pushU32(f, uint32_t(ev.size()));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
}

// ─── Manual FlpResult builder (multi-channel / sampler / arrangement test) ──

static FlpNote mkNote(uint32_t tick, uint32_t len, uint8_t key, uint8_t vel,
                       uint8_t pan, uint16_t channel, bool slide = false) {
    FlpNote n;
    n.tick = tick;
    n.lengthTicks = len;
    n.key = key;
    n.velocity = vel;
    n.pan = pan;
    n.channel = channel;
    n.slide = slide;
    return n;
}

static FlpPlaylistItem mkItem(uint32_t pos, uint32_t len, uint16_t pat,
                               uint16_t track) {
    FlpPlaylistItem it;
    it.position = pos;
    it.length = len;
    it.pattern = pat;
    it.track = track;
    return it;
}

static FlpResult makeMultiChannelResult() {
    FlpResult r;
    r.ok = true;
    r.ppq = 96;
    r.tempo = 140.0;
    r.title = "Test";

    FlpChannel c0;
    c0.iid = 0;
    c0.type = 2;
    c0.insert = 0;
    c0.name = "SynthA";
    c0.enabled = true;
    r.channels.push_back(c0);

    FlpChannel c1;
    c1.iid = 1;
    c1.type = 1;
    c1.insert = 0;
    c1.name = "SynthB";
    c1.enabled = true;
    r.channels.push_back(c1);

    FlpMixerInsert ins;
    ins.index = 0;
    ins.name = "Bus0";
    ins.output = 0;
    ins.color = 0;
    ins.sawRouting = true;
    r.inserts.push_back(ins);

    FlpPattern p0;
    p0.iid = 0;
    p0.name = "Big";
    p0.length = 192;
    p0.noteWidth = 24;
    p0.notes = {
        mkNote(0, 48, 60, 80, 64, 0),   // ch0
        mkNote(48, 48, 62, 70, 64, 1),  // ch1
        mkNote(96, 48, 64, 90, 64, 0),  // ch0
        mkNote(144, 48, 66, 0, 64, 1),  // ch1  vel=0 → clamped 1
    };
    p0.noteCount = 4;
    p0.notesBytes = 4 * 24;
    r.patterns.push_back(p0);

    FlpPattern p1;
    p1.iid = 1;
    p1.name = "Solo";
    p1.length = 96;
    p1.noteWidth = 24;
    p1.notes = {mkNote(0, 96, 36, 100, 64, 1)};
    p1.noteCount = 1;
    p1.notesBytes = 24;
    r.patterns.push_back(p1);

    FlpPattern p2;
    p2.iid = 2;
    p2.name = "Legacy";
    p2.length = 96;
    p2.noteWidth = 8;
    p2.notes = {mkNote(0, 48, 48, 100, 64, 0xFFFF)};
    p2.noteCount = 1;
    p2.notesBytes = 8;
    r.patterns.push_back(p2);

    FlpPattern samplerPat;
    samplerPat.iid = 3;
    samplerPat.name = "Hats";
    samplerPat.length = 96;
    samplerPat.noteWidth = 24;
    samplerPat.notes = {
        mkNote(0, 24, 64, 100, 64, 2),
        mkNote(24, 24, 64, 80, 64, 2),
    };
    samplerPat.noteCount = 2;
    samplerPat.notesBytes = 2 * 24;
    r.patterns.push_back(samplerPat);

    FlpChannel samplerCh;
    samplerCh.iid = 2;
    samplerCh.type = 0;  // sampler
    samplerCh.insert = 0;
    samplerCh.name = "KickSampler";
    samplerCh.enabled = true;
    samplerCh.samplePath = "kick.wav";
    r.channels.push_back(samplerCh);

    FlpArrangement a;
    a.iid = 0;
    a.name = "Main";
    a.items = {
        mkItem(0, 192, 0, 0),     // pat0 ch0/ch1 split
        mkItem(192, 192, 0, 0),   // pat0 again (duplicate)
        mkItem(0, 96, 2, 0),      // legacy pattern
        mkItem(384, 96, 3, 0),    // sampler pattern → should be skipped
    };
    r.arrangements.push_back(a);

    // Second arrangement (fewer items)
    FlpArrangement a2;
    a2.iid = 1;
    a2.name = "Verse";
    a2.items = {mkItem(0, 192, 0, 0)};
    r.arrangements.push_back(a2);

    return r;
}

// ─── Test helpers ───────────────────────────────────────────────────────────

static bool contains(const std::vector<std::string>& v, const char* sub) {
    for (const auto& s : v)
        if (s.find(sub) != std::string::npos)
            return true;
    return false;
}

static bool approx(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

// ─── Fixture (from test_flp.cpp — identical 333-byte synthetic FLP) ────────

static std::vector<uint8_t> fixture() {
    static const unsigned char raw[] = {
        0x46, 0x4c, 0x68, 0x64, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00,
        0x60, 0x00, 0x46, 0x4c, 0x64, 0x74, 0x37, 0x01, 0x00, 0x00, 0xc7, 0x0c,
        0x32, 0x34, 0x2e, 0x32, 0x2e, 0x32, 0x2e, 0x34, 0x35, 0x39, 0x37, 0x00,
        0x9f, 0xf5, 0x11, 0x00, 0x00, 0x9c, 0x78, 0xe0, 0x01, 0x00, 0xc2, 0x10,
        0x4d, 0x00, 0x79, 0x00, 0x20, 0x00, 0x53, 0x00, 0x6f, 0x00, 0x6e, 0x00,
        0x67, 0x00, 0x00, 0x00, 0xce, 0x0a, 0x46, 0x00, 0x75, 0x00, 0x6e, 0x00,
        0x6b, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x15, 0x00, 0x91, 0x00, 0x00,
        0x00, 0x00, 0x16, 0x00, 0xcb, 0x0a, 0x4b, 0x00, 0x69, 0x00, 0x63, 0x00,
        0x6b, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00, 0x15, 0x00, 0x91, 0x00, 0x00,
        0x00, 0x00, 0x16, 0x03, 0xc4, 0x0c, 0x4b, 0x00, 0x2e, 0x00, 0x77, 0x00,
        0x61, 0x00, 0x76, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0xc1, 0x0a, 0x50,
        0x00, 0x61, 0x00, 0x74, 0x00, 0x31, 0x00, 0x00, 0x00, 0xe0, 0x18, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x3c,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41,
        0x01, 0x00, 0xc1, 0x0a, 0x42, 0x00, 0x61, 0x00, 0x73, 0x00, 0x73, 0x00,
        0x00, 0x00, 0xe0, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x60, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x60, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x63, 0x00, 0x00, 0xf1, 0x0a, 0x4d, 0x00, 0x61,
        0x00, 0x69, 0x00, 0x6e, 0x00, 0x00, 0x00, 0xe9, 0x3c, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x50, 0x00, 0x50, 0x60, 0x00, 0x00, 0x00, 0xf3, 0x01, 0x00,
        0x00, 0x78, 0x00, 0x40, 0x00, 0x40, 0x64, 0x80, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x93, 0x00, 0x00,
        0x00, 0x00, 0xcc, 0x08, 0x4f, 0x00, 0x75, 0x00, 0x74, 0x00, 0x00, 0x00,
        0x95, 0x44, 0x33, 0x22, 0x11, 0xeb, 0x00, 0xec, 0x00,
    };
    return std::vector<uint8_t>(raw, raw + sizeof(raw));
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE(importByteLevelNoteDecode)
// Parse the hand-built tiny FLP and assert every FlpNote field lands at the
// correct offset (authoritative 24-byte layout).
{
    const auto buf = buildTinyFp();
    FlpResult r = FlpReader::parse(buf);
    TEST_ASSERT(r.ok);
    TEST_ASSERT(r.ppq == 96);
    TEST_ASSERT(r.channels.size() == 1);
    TEST_ASSERT(r.patterns.size() == 1);
    if (r.patterns.empty())
        return;
    const auto& p = r.patterns[0];
    TEST_ASSERT(p.noteCount == 3);
    TEST_ASSERT(p.notes.size() == 3);
    TEST_ASSERT(p.noteWidth == 24);
    if (p.notes.size() != 3)
        return;

    const auto& n0 = p.notes[0];
    TEST_ASSERT(n0.tick == 24);
    TEST_ASSERT(n0.lengthTicks == 48);
    TEST_ASSERT(n0.key == 66);
    TEST_ASSERT(n0.velocity == 120);
    TEST_ASSERT(n0.pan == 100);
    TEST_ASSERT(n0.channel == 0);
    TEST_ASSERT(n0.slide == true);

    const auto& n1 = p.notes[1];
    TEST_ASSERT(n1.tick == 72);
    TEST_ASSERT(n1.lengthTicks == 24);
    TEST_ASSERT(n1.key == 48);
    TEST_ASSERT(n1.velocity == 5);
    TEST_ASSERT(n1.pan == 192);
    TEST_ASSERT(n1.channel == 0);
    TEST_ASSERT(n1.slide == false);

    const auto& n2 = p.notes[2];
    TEST_ASSERT(n2.tick == 0);
    TEST_ASSERT(n2.lengthTicks == 96);
    TEST_ASSERT(n2.key == 72);
    TEST_ASSERT(n2.velocity == 96);
    TEST_ASSERT(n2.pan == 64);
    TEST_ASSERT(n2.channel == 0);

    std::printf("  byte-level note decode ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE(importFixtureEndToEnd)
// Parse the 333-byte fixture and run the importer: both channels are
// synth-type=0 and type=? in the fixture — the sole playlist item
// references pattern 0 whose note lives on channel 0 which IS type 0
// (sampler "Kick"), so the importer must skip the sampler hit and leave
// the session as mixer-topology-only.
{
    FlpResult r = FlpReader::parse(fixture());
    TEST_ASSERT(r.ok);
    auto res = importSession(r);
    const Session& s = res.session;
    TEST_ASSERT(s.name() == "My Song");
    TEST_ASSERT(approx(s.tempo(), 123.0));
    // Two channel tracks + zero fallback tracks.
    TEST_ASSERT(s.tracks().size() == 2);
    // One mixer insert ("Out") becomes a bus.
    TEST_ASSERT(s.buses().size() == 1);
    TEST_ASSERT(s.buses()[0].name == "Out");
    // No regions: the only note is on the sampler channel and is skipped.
    size_t totalRegions = 0, totalNotes = 0;
    for (const auto& t : s.tracks()) {
        totalRegions += t.regions.size();
        for (const auto& rg : t.regions)
            totalNotes += rg.notes.size();
    }
    TEST_ASSERT(totalRegions == 0);
    TEST_ASSERT(totalNotes == 0);
    // Warnings mention the skipped sampler hit.
    TEST_ASSERT(contains(res.warnings, "sampler"));
    std::printf("  fixture end-to-end import ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE(importMultiChannelMapping)
// Hand-built FlpResult with:
//  3 channel tracks (2 synth + 1 sampler "KickSampler"),
//  4 patterns (Big/Solo/Legacy/Hats),
//  1 arrangement containing 4 items (two Big, one Legacy, one Hats),
//  multi-note patterns split by rack channel,
//  one unassigned (0xFFFF) pattern, two arrangements (the busiest wins).
{
    auto res = importSession(makeMultiChannelResult());
    const Session& s = res.session;

    // Header / meta
    TEST_ASSERT(s.name() == "Test");
    TEST_ASSERT(approx(s.tempo(), 140.0));

    // Buses
    TEST_ASSERT(s.buses().size() == 1);
    TEST_ASSERT(s.buses()[0].name == "Bus0");

    // 3 channel tracks + 1 fallback track (legacy pattern 2) = 4
    TEST_ASSERT(s.tracks().size() == 4);
    TEST_ASSERT(s.tracks()[0].name == "SynthA");
    TEST_ASSERT(s.tracks()[1].name == "SynthB");
    TEST_ASSERT(s.tracks()[2].name == "KickSampler");
    TEST_ASSERT(s.tracks()[3].name == "Legacy");
    TEST_ASSERT(s.tracks()[2].muted == false);

    // SynthA bus routing
    TEST_ASSERT(s.tracks()[0].busId == s.buses()[0].id);
    TEST_ASSERT(s.tracks()[1].busId == s.buses()[0].id);

    // SynthA: 2 regions (item0 + item1, each yields one region for ch0 group)
    const auto& ta = s.tracks()[0];
    TEST_ASSERT(ta.regions.size() == 2);
    if (ta.regions.size() == 2) {
        // First region: position 0, duration 2 beats (192/96)
        TEST_ASSERT(approx(ta.regions[0].startBeats, 0.0));
        TEST_ASSERT(approx(ta.regions[0].durationBeats, 2.0));
        TEST_ASSERT(ta.regions[0].name == "Big");
        // Two notes: n0 (tick0, vel80) and n2 (tick96, vel90)
        TEST_ASSERT(ta.regions[0].notes.size() == 2);
        if (ta.regions[0].notes.size() == 2) {
            const auto& na0 = ta.regions[0].notes[0];
            TEST_ASSERT(na0.pitch == 60);
            TEST_ASSERT(na0.velocity == 80);
            TEST_ASSERT(approx(na0.startBeats, 0.0));
            TEST_ASSERT(approx(na0.durationBeats, 48.0 / 96.0));
            const auto& na1 = ta.regions[0].notes[1];
            TEST_ASSERT(na1.pitch == 64);
            TEST_ASSERT(na1.velocity == 90);
            TEST_ASSERT(approx(na1.startBeats, 96.0 / 96.0));
        }
        // Second region: identical content, placed at beat 2
        TEST_ASSERT(approx(ta.regions[1].startBeats, 2.0));
        TEST_ASSERT(ta.regions[1].notes.size() == 2);
    }

    // SynthB: 2 regions, first note vel=70, second vel=1 (clamped from 0)
    const auto& tb = s.tracks()[1];
    TEST_ASSERT(tb.regions.size() == 2);
    if (tb.regions.size() == 2) {
        TEST_ASSERT(approx(tb.regions[0].startBeats, 0.0));
        TEST_ASSERT(tb.regions[0].notes.size() == 2);
        if (tb.regions[0].notes.size() == 2) {
            TEST_ASSERT(tb.regions[0].notes[0].pitch == 62);
            TEST_ASSERT(tb.regions[0].notes[0].velocity == 70);
            TEST_ASSERT(approx(tb.regions[0].notes[0].startBeats, 48.0 / 96.0));
            TEST_ASSERT(tb.regions[0].notes[1].pitch == 66);
            TEST_ASSERT(tb.regions[0].notes[1].velocity == 1);  // raw=0 → clamp
        }
    }

    // Legacy track: 1 region, 1 note (pitch 48)
    const auto& tl = s.tracks()[3];
    TEST_ASSERT(tl.regions.size() == 1);
    TEST_ASSERT(tl.name == "Legacy");
    if (!tl.regions.empty()) {
        TEST_ASSERT(tl.regions[0].notes.size() == 1);
        TEST_ASSERT(tl.regions[0].notes[0].pitch == 48);
    }

    // contentEndBeat: last region is item1 on beat 2, dur 2 → end 4
    TEST_ASSERT(approx(s.contentEndBeats(), 4.0));

    // Skipped sampler pattern item (pat3 Hats, ch2 sampler)
    TEST_ASSERT(contains(res.warnings, "sampler"));
    // Unassigned legacy notes (pat2, channel 0xFFFF)
    TEST_ASSERT(contains(res.warnings, "rack channel"));
    // Second arrangement skipped
    TEST_ASSERT(contains(res.warnings, "skipped"));
    // Total regions: 2 (SynthA) + 2 (SynthB) + 1 (Legacy) = 5
    size_t regions = 0;
    for (const auto& t : s.tracks()) regions += t.regions.size();
    TEST_ASSERT(regions == 5);

    // Channel rack rows (one per FL channel, names preserved).
    const auto& rack = s.channelRack().channels;
    TEST_ASSERT(rack.size() == 3);
    if (rack.size() == 3) {
        TEST_ASSERT(rack[0].name == "SynthA");
        TEST_ASSERT(rack[0].type == ChannelType::Instrument);
        TEST_ASSERT(rack[1].name == "SynthB");
        TEST_ASSERT(rack[1].type == ChannelType::Instrument);
        TEST_ASSERT(rack[2].name == "KickSampler");
        TEST_ASSERT(rack[2].type == ChannelType::Sampler);
        // No baseDir was supplied: the relative path stays unresolved (and
        // portable), exactly like a clip path at save/load time.
        TEST_ASSERT(rack[2].soundFile == "kick.wav");
        TEST_ASSERT(rack[0].muted == false);
    }
    // Import report counters (compact summary).
    TEST_ASSERT(res.channelsImported == 3);
    TEST_ASSERT(res.samplerChannels == 1);
    TEST_ASSERT(res.samplesFound == 0 && res.samplesMissing == 0);
    TEST_ASSERT(res.missingSamples.empty());
    TEST_ASSERT(res.samplerHitsSkipped == 2);
    TEST_ASSERT(res.playlistRegions == 5);
    TEST_ASSERT(res.notesImported == 9);
    TEST_ASSERT(res.patternsImported == 2);  // Big + Legacy (Hats never places)
    TEST_ASSERT(res.unsupportedPlugins == 0);
    TEST_ASSERT(res.unsupportedPluginNames.empty());

    std::printf("  multi-channel / arrangement / sampler-skip mapping ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE(importSamplerMaterialization)
// Channel-rack materialization with a real sample resolution context:
//  - sampler channel with an on-disk sample -> Sampler row, soundFile resolved
//    absolute, samplesFound++.
//  - sampler channel with a missing sample -> Sampler row keeps the reference,
//    samplesMissing++ and the missing path is reported.
//  - a FL generator (InternalName set) -> Instrument placeholder row, counted
//    as an unsupported plugin with its name preserved.
{
    const std::string base = misuc::test::makeTempDir("misuc_flpimp");
    TEST_ASSERT(!base.empty());
    if (base.empty())
        return;
    writeToneWav(base + "/kick.wav");

    FlpResult r;
    r.ok = true;
    r.ppq = 96;
    r.tempo = 128.0;
    r.title = "Samplers";

    FlpChannel kick;
    kick.iid = 0;
    kick.type = 0;                  // sampler
    kick.name = "Kick";
    kick.samplePath = "kick.wav";   // relative to the .flp directory
    r.channels.push_back(kick);

    FlpChannel clap;
    clap.iid = 1;
    clap.type = 0;
    clap.name = "Clap";
    clap.samplePath = "clap.wav";   // does not exist on disk
    r.channels.push_back(clap);

    FlpChannel lead;
    lead.iid = 2;
    lead.type = 1;                  // FL Keys generator
    lead.name = "Lead";
    lead.internalName = "FL Keys";
    r.channels.push_back(lead);

    auto res = importSession(r, base);
    const Session& s = res.session;
    TEST_ASSERT(s.name() == "Samplers");
    TEST_ASSERT(approx(s.tempo(), 128.0));

    // Tracks mirror the channels; rack rows carry correct types.
    TEST_ASSERT(s.tracks().size() == 3);
    const auto& rack = s.channelRack().channels;
    TEST_ASSERT(rack.size() == 3);
    if (rack.size() == 3) {
        TEST_ASSERT(rack[0].name == "Kick");
        TEST_ASSERT(rack[0].type == ChannelType::Sampler);
        TEST_ASSERT(rack[0].soundFile == base + "/kick.wav");
        TEST_ASSERT(rack[1].name == "Clap");
        TEST_ASSERT(rack[1].type == ChannelType::Sampler);
        // Missing sample: the reference survives for re-pointing.
        TEST_ASSERT(rack[1].soundFile == base + "/clap.wav");
        TEST_ASSERT(rack[2].name == "Lead");
        TEST_ASSERT(rack[2].type == ChannelType::Instrument);
        TEST_ASSERT(rack[2].soundFile.empty());
    }

    // Report counters reflect disk truth.
    TEST_ASSERT(res.channelsImported == 3);
    TEST_ASSERT(res.samplerChannels == 2);
    TEST_ASSERT(res.samplesFound == 1);
    TEST_ASSERT(res.samplesMissing == 1);
    TEST_ASSERT(res.missingSamples.size() == 1);
    if (res.missingSamples.size() == 1)
        TEST_ASSERT(res.missingSamples[0] == base + "/clap.wav");
    TEST_ASSERT(res.unsupportedPlugins == 1);
    TEST_ASSERT(res.unsupportedPluginNames.size() == 1);
    if (res.unsupportedPluginNames.size() == 1)
        TEST_ASSERT(res.unsupportedPluginNames[0] == "FL Keys");
    TEST_ASSERT(contains(res.warnings, "not found"));
    TEST_ASSERT(contains(res.warnings, "FL Keys") == false);  // only in names

    std::remove((base + "/kick.wav").c_str());
    std::remove(base.c_str());
    std::printf("  sampler materialization + sample resolution ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE(importZippedFlpRoundTrip)
// The "zipped .flp" container (zlib-compressed FLdt payload) must decode to
// the same structural model as the uncompressed form.
{
    const auto plain = buildTinyFp();
    const FlpResult ref = FlpReader::parse(plain);
    TEST_ASSERT(ref.ok);

    // Re-encode: header(22) + zlib(FLdt events).
    std::vector<uint8_t> zipped(plain.begin(), plain.begin() + 22);
    const auto body = zlibDeflate(std::vector<uint8_t>(plain.begin() + 22, plain.end()));
    TEST_ASSERT(!body.empty());
    zipped.insert(zipped.end(), body.begin(), body.end());

    FlpResult r = FlpReader::parse(zipped);
    TEST_ASSERT(r.ok);
    TEST_ASSERT(r.ppq == 96);
    TEST_ASSERT(r.channels.size() == 1);
    TEST_ASSERT(r.patterns.size() == 1);
    if (r.patterns.empty())
        return;
    TEST_ASSERT(r.patterns[0].noteCount == 3);
    TEST_ASSERT(r.patterns[0].notes.size() == 3);
    // Same structural model as the uncompressed parse (title/tempo too).
    TEST_ASSERT(r.channels[0].name == ref.channels[0].name);
    TEST_ASSERT(r.title == ref.title);
    TEST_ASSERT(float(r.tempo) == float(ref.tempo) || (r.tempo == 0.0 && ref.tempo == 0.0));
    if (r.patterns[0].notes.size() == 3 && ref.patterns[0].notes.size() == 3) {
        for (size_t i = 0; i < 3; ++i) {
            TEST_ASSERT(r.patterns[0].notes[i].tick == ref.patterns[0].notes[i].tick);
            TEST_ASSERT(r.patterns[0].notes[i].velocity == ref.patterns[0].notes[i].velocity);
        }
    }

    // And it imports identically: one track, no playlist, tempo untouched.
    auto res = importSession(r);
    TEST_ASSERT(res.channelsImported == 1);
    TEST_ASSERT(res.session.tracks().size() == 1);
    TEST_ASSERT(res.playlistRegions == 0);
    TEST_ASSERT(res.notesImported == 0);
    std::printf("  zipped-FLP round trip ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE(importItemLengthFallback)
// When item.length == 0, fall back to pattern Length; when both are zero,
// fall back to the last note end.
{
    FlpResult r;
    r.ok = true;
    r.ppq = 96;
    r.tempo = 120.0;

    FlpChannel ch;
    ch.iid = 0;
    ch.type = 2; // non-sampler
    ch.insert = -1;
    ch.name = "A";
    ch.enabled = true;
    r.channels.push_back(ch);

    FlpPattern p;
    p.iid = 0;
    p.name = "P";
    p.length = 288; // 3 beats in ticks
    p.noteWidth = 24;
    p.notes = {mkNote(0, 48, 60, 100, 64, 0)};
    p.noteCount = 1;
    p.notesBytes = 24;
    r.patterns.push_back(p);

    FlpArrangement a;
    a.iid = 0;
    a.items = {
        mkItem(96, 0, 0, 0),  // length=0 → fallback to pat length 288
    };
    r.arrangements.push_back(a);

    auto res = importSession(r);
    const Session& s = res.session;
    TEST_ASSERT(s.tracks().size() == 1);
    TEST_ASSERT(s.tracks()[0].regions.size() == 1);
    if (!s.tracks()[0].regions.empty()) {
        // region dur = 288/96 = 3.0
        TEST_ASSERT(approx(s.tracks()[0].regions[0].durationBeats, 3.0));
        TEST_ASSERT(approx(s.tracks()[0].regions[0].startBeats, 1.0)); // 96/96
    }
    TEST_ASSERT(approx(s.contentEndBeats(), 4.0)); // 1 + 3
    std::printf("  item-length fallback ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE(importArrangementPick)
// Two arrangements of different sizes; the importer must pick the one with
// more items and warn about the other.
{
    FlpResult r;
    r.ok = true;
    r.ppq = 96;
    r.tempo = 120.0;
    FlpChannel ch;
    ch.iid = 0;
    ch.type = 1;
    ch.name = "C";
    ch.insert = -1;
    r.channels.push_back(ch);

    FlpPattern p;
    p.iid = 0;
    p.name = "P";
    p.noteWidth = 24;
    p.notes = {mkNote(0, 48, 60, 100, 64, 0)};
    p.noteCount = 1;
    p.notesBytes = 24;
    r.patterns.push_back(p);

    FlpArrangement a1, a2;
    a1.iid = 0;
    a1.name = "Small";
    a1.items = {mkItem(0, 48, 0, 0)};
    a2.iid = 1;
    a2.name = "Big";
    a2.items = {mkItem(0, 48, 0, 0), mkItem(48, 48, 0, 0), mkItem(96, 48, 0, 0)};
    r.arrangements = {a1, a2};

    auto res = importSession(r);
    const Session& s = res.session;
    // Big arrangement wins (3 items): 3 regions on the single track
    TEST_ASSERT(s.tracks().size() == 1);
    TEST_ASSERT(s.tracks()[0].regions.size() == 3);
    TEST_ASSERT(contains(res.warnings, "skipped"));
    TEST_ASSERT(contains(res.warnings, "Small"));
    std::printf("  arrangement pick ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE(importDeterminism)
// Running the importer twice on the same input must produce byte-identical
// JSON when both sessions are flushed through fromJson/toJson.
{
    auto r1 = importSession(makeMultiChannelResult());
    auto r2 = importSession(makeMultiChannelResult());
    std::string j1 = r1.session.toJson();
    std::string j2 = r2.session.toJson();
    TEST_ASSERT(j1 == j2);
    // Re-import the JSON to exercise the v3 round-trip.
    Session r = Session::fromJson(j1);
    TEST_ASSERT(r.name() == "Test");
    TEST_ASSERT(approx(r.tempo(), 140.0));
    TEST_ASSERT(r.tracks().size() == 4);
    size_t totalNotes = 0;
    for (const auto& t : r.tracks())
        for (const auto& rg : t.regions)
            totalNotes += rg.notes.size();
    TEST_ASSERT(totalNotes == 9); // 2+2 + 2+2 + 0 + 1 = 9
    std::printf("  determinism + JSON round-trip ok\n");
}