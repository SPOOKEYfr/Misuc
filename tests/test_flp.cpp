// Clean-room structural FLP reader tests.
//
// The 333-byte fixture below is a synthesised FL Studio 24 project built with
// the publicly-documented container + event rules (FLhd/FLdt chunks, banded
// value sizes, varint-prefixed text/data events, UTF-16 names). It is NOT
// copied from any Image-Line or third-party binary. Real-file cross-validation
// against the independent PyFLP tool happens during QA on the user's projects.

#include "misuc/engine/flp/FlpReader.h"

#include <cstdio>
#include <string>
#include <vector>

using misuc::flp::FlpReader;
using misuc::flp::FlpResult;

namespace {
int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

std::vector<uint8_t> fixture() {
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

void testContainer(const FlpResult& r) {
    CHECK(r.ok, "fixture should parse");
    CHECK(r.error.empty(), "no error text on success");
    CHECK(r.magic == "FLhd", "magic");
    CHECK(r.headerSize == 6, "header size");
    CHECK(r.format == 0, "format");
    CHECK(r.headerChannels == 5, "header channel count");
    CHECK(r.ppq == 96, "ppq");
    CHECK(r.dataSize == 311, "FLdt size");
    CHECK(!r.truncated, "full stream consumed");
    CHECK(r.streamEnd == 333, "stream end == file size");
}

void testMetadata(const FlpResult& r) {
    CHECK(r.flVersion == "24.2.2.4597", "FL version string");
    CHECK(r.build == 4597, "build");
    CHECK(r.tempo == 123.0, "tempo");
    CHECK(r.title == "My Song", "title");
    CHECK(r.genre == "Funk", "genre");
}

void testChannels(const FlpResult& r) {
    CHECK(r.channels.size() == 2, "two channels");
    if (r.channels.size() != 2) return;
    CHECK(r.channels[0].iid == 0, "ch0 iid");
    CHECK(r.channels[0].type == 0, "ch0 type sampler");
    CHECK(r.channels[0].insert == 0, "ch0 insert");
    CHECK(r.channels[0].name == "Kick", "ch0 name");
    CHECK(r.channels[1].iid == 1, "ch1 iid");
    CHECK(r.channels[1].insert == 3, "ch1 insert");
    CHECK(r.channels[1].samplePath == "K.wav", "ch1 sample path");
}

void testPatterns(const FlpResult& r) {
    CHECK(r.patterns.size() == 2, "two patterns");
    if (r.patterns.size() != 2) return;
    CHECK(r.patterns[0].iid == 0, "pat0 iid");
    CHECK(r.patterns[0].name == "Pat1", "pat0 name");
    CHECK(r.patterns[0].noteCount == 1, "pat0 one note");
    CHECK(r.patterns[0].notesBytes == 24, "pat0 24-byte payload");
    CHECK(r.patterns[1].iid == 1, "pat1 iid");
    CHECK(r.patterns[1].name == "Bass", "pat1 name");
    CHECK(r.patterns[1].noteCount == 2, "pat1 two notes");
    CHECK(r.patterns[1].notesBytes == 48, "pat1 48-byte payload");
}

void testPlaylist(const FlpResult& r) {
    CHECK(r.arrangements.size() == 1, "one arrangement");
    if (r.arrangements.empty()) return;
    const auto& a = r.arrangements[0];
    CHECK(a.iid == 0, "arr iid");
    CHECK(a.name == "Main", "arr name");
    CHECK(a.items.size() == 1, "one playlist item");
    if (a.items.empty()) return;
    const auto& it = a.items[0];
    CHECK(it.position == 0, "item position");
    CHECK(it.pattern == 0, "item pattern index");
    CHECK(it.length == 96, "item length");
    CHECK(it.track == 499, "item reversed track index");
}

void testMixer(const FlpResult& r) {
    CHECK(r.inserts.size() == 1, "one insert");
    if (r.inserts.empty()) return;
    CHECK(r.inserts[0].index == 0, "insert0 index");
    CHECK(r.inserts[0].name == "Out", "insert0 name");
    CHECK(r.inserts[0].output == 0, "insert0 output");
    CHECK(r.inserts[0].color == 0x11223344u, "insert0 color");
    CHECK(r.inserts[0].sawRouting, "insert0 routing flag");
}

void testHistogram(const FlpResult& r) {
    CHECK(r.events.size() == 29, "29 events");
    CHECK(r.idCount(64) == 2, "2 x channel-new");
    CHECK(r.idCount(65) == 2, "2 x pattern-new");
    CHECK(r.idCount(99) == 1, "1 x arrangement-new");
    CHECK(r.idCount(147) == 1, "1 x insert-output");
    CHECK(r.idCount(156) == 1, "1 x tempo");
    CHECK(r.idCount(199) == 1, "1 x FLVersion");
    CHECK(r.idCount(224) == 2, "2 x pattern-notes");
    CHECK(r.idCount(233) == 1, "1 x playlist");
    CHECK(r.idCount(235) == 1, "1 x insert-routing");
    CHECK(r.idCount(236) == 1, "1 x insert-flags");
    CHECK(r.idCount(145) == 2, "2 x group-num");
    CHECK(r.idCount(203) == 1, "1 x channel name");
    CHECK(r.idCount(14) == 0, "0 x unused id 14");
}

void testErrors() {
    FlpResult empty = FlpReader::parse({});
    CHECK(!empty.ok, "empty input rejected");

    FlpResult tiny = FlpReader::parse({0x46, 0x4c});
    CHECK(!tiny.ok, "tiny input rejected");

    FlpResult bad = FlpReader::parse(std::vector<uint8_t>(64, 0x41));
    CHECK(!bad.ok, "bad magic rejected");
    CHECK(!bad.error.empty(), "error message present");

    std::vector<uint8_t> cut = fixture();
    cut.pop_back();
    FlpResult truncated = FlpReader::parse(cut);
    CHECK(truncated.ok, "truncated stream still yields container result");
    CHECK(truncated.truncated, "truncated flag set");

    std::vector<uint8_t> hdr = {0x46, 0x4c, 0x68, 0x64, 0x06, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x05, 0x00, 0x60, 0x00,
                                'F',  'L',  'd',  't'};
    std::vector<uint8_t> zlib = hdr;
    zlib.push_back(0x04); zlib.push_back(0x00); zlib.push_back(0x00); zlib.push_back(0x00);
    zlib.push_back(0x78); zlib.push_back(0x9C);
    FlpResult z = FlpReader::parse(zlib);
    CHECK(!z.ok, "zlib-compressed payload politely refused");
    CHECK(z.error.find("zlib") != std::string::npos, "zlib mention in error");
}

void testPlaylistBandDecode() {
    // Playlist items encode pattern refs as base+index: u16 pattern_base at
    // +4 (0x5000) and u16 item_index at +6; pattern number = index - base.
    // Item 1 = pattern 4 (0x5004), item 2 = audio clip referencing channel 9
    // (below the base). Build a minimal FLdt with one playlist event:
    // 120 bytes = two 60-byte items; 60 is a declared item width.
    std::vector<uint8_t> b = {0x46, 0x4c, 0x68, 0x64, 0x06, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x05, 0x00, 0x60, 0x00,
                              'F',  'L',  'd',  't',  0x7a, 0x00, 0x00, 0x00,
                              0xe9, 0x78};  // id 233, len 120 (122 event bytes total)
    for (int item = 0; item < 2; ++item) {
        for (int i = 0; i < 60; ++i) b.push_back(0x00);
        const int o = int(b.size()) - 60;
        b[o + 0] = 0x40;                // position = 0x40
        b[o + 4] = 0x00; b[o + 5] = 0x50;  // pattern_base = 0x5000
        b[o + 8] = 0x80; b[o + 9] = 0x01;  // length = 0x180
        b[o + 12] = 0xf3; b[o + 13] = 0x01; // track = 499
        if (item == 0) {
            b[o + 6] = 0x04; b[o + 7] = 0x50;  // item_index = 0x5004
        } else {
            b[o + 6] = 0x09; b[o + 7] = 0x00;  // item_index = 9 (clip/channel)
        }
    }
    FlpResult r = FlpReader::parse(b);
    CHECK(r.ok, "band-decode stream parses");
    CHECK(r.arrangements.size() == 1, "one arrangement");
    if (r.arrangements.empty()) return;
    const auto& a = r.arrangements[0];
    CHECK(a.items.size() == 2, "two playlist items");
    if (a.items.size() != 2) return;
    CHECK(a.items[0].pattern == 4, "0x5004 decodes to pattern 4");
    CHECK(!a.items[0].isClip, "above-base item is a pattern");
    CHECK(a.items[0].position == 0x40, "item0 position");
    CHECK(a.items[0].length == 0x180, "item0 length");
    CHECK(a.items[0].track == 499, "item0 reversed track");
    CHECK(a.items[1].isClip, "below-base item is an audio clip");
    CHECK(a.items[1].pattern == 9, "clip item keeps channel iid");
    std::printf("  playlist base+index band decode ok\n");
}

void testVarintEdge() {
    // A 2-byte varint length for a variable event: id 224 (notes) with
    // length 0x123 (291) declared via 0x83 0x02 (LE128), payload truncated.
    std::vector<uint8_t> b = {0x46, 0x4c, 0x68, 0x64, 0x06, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x05, 0x00, 0x60, 0x00,
                              'F',  'L',  'd',  't',  0x05, 0x00, 0x00, 0x00,
                              0xe0, 0x83, 0x02, 0x00};
    FlpResult r = FlpReader::parse(b);
    CHECK(r.ok, "varint stream parses");
    CHECK(r.truncated, "truncated varint payload flagged");
    CHECK(r.idCount(224) == 1, "partial event still counted");
}

}  // namespace

int main() {
    FlpResult r = FlpReader::parse(fixture());
    testContainer(r);
    testMetadata(r);
    testChannels(r);
    testPatterns(r);
    testPlaylist(r);
    testMixer(r);
    testHistogram(r);
    testErrors();
    testVarintEdge();
    testPlaylistBandDecode();

    if (failures == 0) {
        std::printf("RESULT PASS (flp structural reader)\n");
        return 0;
    }
    std::fprintf(stderr, "RESULT FAIL (%d failures)\n", failures);
    return 1;
}