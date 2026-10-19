#include "misuc/engine/flp/FlpReader.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <zlib.h>

namespace misuc::flp {

namespace {

// Inflates a whole zlib (RFC-1950) stream into a plain byte vector. Returns an
// empty vector on any decompression failure. FL Studio's "zipped .flp" stores
// the entire FLdt event stream as one zlib payload; historically some files
// also zipped the trailing FIF blocks (a 6-byte "FIF " wrapper around a zlib
// stream). We restore the uncompressed bytes so the shared event-stream walker
// sees exactly what an uncompressed project would have produced.
std::vector<uint8_t> inflateZlib(const uint8_t* src, size_t len) {
    if (!src || len < 2)
        return {};
    z_stream zs{};
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = uInt(len);
    if (inflateInit(&zs) != Z_OK)
        return {};
    std::vector<uint8_t> out;
    uint8_t buf[65536];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = buf;
        zs.avail_out = uInt(sizeof(buf));
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END)
            break;
        const size_t produced = sizeof(buf) - size_t(zs.avail_out);
        out.insert(out.end(), buf, buf + produced);
        if (rc == Z_STREAM_END)
            break;
    }
    inflateEnd(&zs);
    return (rc == Z_STREAM_END) ? out : std::vector<uint8_t>{};
}

// For a zipped .flp, rebuild the full in-memory buffer as header (first 22
// bytes) + inflated FLdt payload, so the shared event walker sees the same
// byte layout an uncompressed project has. Returns an empty vector when the
// payload does not inflate.
std::vector<uint8_t> rebuildContainer(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 22)
        return {};
    std::vector<uint8_t> body = inflateZlib(bytes.data() + 22, bytes.size() - 22);
    if (body.empty())
        return {};
    std::vector<uint8_t> out(bytes.begin(), bytes.begin() + 22);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Fixed-value payload sizes by event-id band (publicly documented scheme).
int valueSizeForId(uint8_t id) {
    if (id < 64) return 1;
    if (id < 128) return 2;
    if (id < 192) return 4;
    return -1;  // variable length follows
}

// Little-endian reads.
uint32_t leU16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
uint32_t leU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

// FLDT event stream reader. Encodes one "type byte + fixed/variable value".
struct StreamReader {
    const std::vector<uint8_t>& b;
    uint64_t off = 0;
    uint64_t limit = 0;  // exclusive end of FLdt payload
    bool truncated = false;
    uint64_t bytesPerNote24 = 24;  // modern note struct width (FL>=~20)
};

bool decodeVarint(const std::vector<uint8_t>& b, uint64_t off, uint64_t limit,
                  uint64_t& value, uint64_t& consumed) {
    value = 0;
    consumed = 0;
    uint32_t shift = 0;
    while (off + consumed < limit && shift < 64) {
        uint8_t byte = b[off + consumed];
        ++consumed;
        value |= uint64_t(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) return true;
        shift += 7;
    }
    return false;  // unterminated or out of range
}

bool decodeNext(StreamReader& s, FlpEvent& ev) {
    const auto& b = s.b;
    if (s.off >= s.limit) return false;

    ev = FlpEvent();
    ev.offset = s.off;
    ev.id = b[s.off];
    uint64_t afterId = s.off + 1;
    if (afterId > s.limit) {
        s.truncated = true;
        return true;  // consumed the last (dangling) byte
    }

    int fixed = valueSizeForId(ev.id);
    if (fixed >= 0) {
        if (afterId + uint64_t(fixed) > s.limit) {
            s.truncated = true;
            s.off = s.limit;
            return true;
        }
        ev.value.assign(b.begin() + long(afterId), b.begin() + long(afterId + fixed));
        ev.size = 1 + uint64_t(fixed);
        s.off = afterId + uint64_t(fixed);
    } else {
        uint64_t len = 0;
        uint64_t consumed = 0;
        if (!decodeVarint(b, afterId, s.limit, len, consumed)) {
            s.truncated = true;
            s.off = s.limit;
            return true;
        }
        uint64_t valueOff = afterId + consumed;
        if (valueOff > s.limit || len > s.limit - valueOff) {
            s.truncated = true;
            s.off = s.limit;
            return true;
        }
        ev.value.assign(b.begin() + long(valueOff), b.begin() + long(valueOff + len));
        ev.size = 1 + consumed + len;
        s.off = valueOff + len;
    }
    return true;
}

bool looksLikeUtf16(const std::vector<uint8_t>& raw) {
    if (raw.size() < 2 || (raw.size() % 2) != 0) return false;
    for (size_t i = 1; i < raw.size(); i += 2) {
        if (raw[i] != 0x00) return false;
    }
    return true;
}

std::string latinOrUtf8(const std::vector<uint8_t>& raw) {
    std::string out;
    out.reserve(raw.size());
    for (uint8_t c : raw) {
        if (c == 0) break;  // ASCII/NUL-terminated legacy strings
        out.push_back(char(c));
    }
    return out;
}

std::string utf16ToUtf8(const std::vector<uint8_t>& raw) {
    std::string out;
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        uint16_t code = uint16_t(raw[i]) | (uint16_t(raw[i + 1]) << 8);
        if (code == 0) break;
        if (code < 0x80) {
            out.push_back(char(code));
        } else if (code < 0x800) {
            out.push_back(char(0xC0 | (code >> 6)));
            out.push_back(char(0x80 | (code & 0x3F)));
        } else {
            out.push_back(char(0xE0 | (code >> 12)));
            out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(char(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

}  // namespace

std::string FlpReader::decodeString(const std::vector<uint8_t>& raw) {
    if (looksLikeUtf16(raw)) return utf16ToUtf8(raw);
    return latinOrUtf8(raw);
}

uint32_t FlpReader::readU16(const std::vector<uint8_t>& b, uint64_t off) {
    if (off + 2 > b.size()) return 0;
    return leU16(b.data() + off);
}
uint32_t FlpReader::readU32(const std::vector<uint8_t>& b, uint64_t off) {
    if (off + 4 > b.size()) return 0;
    return leU32(b.data() + off);
}

FlpResult FlpReader::parse(const std::vector<uint8_t>& bytes) {
    FlpResult r;
    auto fail = [&r](const std::string& msg) {
        r.ok = false;
        r.error = msg;
        return r;
    };

    if (bytes.size() < 22) return fail("file too small for FLP header");
    if (bytes[0] == 0xF4 || bytes[0] == 0xF5)
        return fail("container wrapper variant (F4/F5 magic) not decoded");
    // zlib-compressed FLdt payload: first payload byte is a zlib header.
    bool zipped = bytes.size() > 23 && bytes[22] == 0x78 &&
                  (bytes[23] == 0x9C || bytes[23] == 0x01 || bytes[23] == 0xDA);

    r.magic.assign(reinterpret_cast<const char*>(bytes.data()), 4);
    if (r.magic != "FLhd") return fail("bad magic (expected 'FLhd')");

    r.headerSize = readU32(bytes, 4);
    r.format = uint16_t(readU16(bytes, 8));
    r.headerChannels = uint16_t(readU16(bytes, 10));
    r.ppq = uint16_t(readU16(bytes, 12));

    if (bytes[14] != 'F' || bytes[15] != 'L' || bytes[16] != 'd' || bytes[17] != 't')
        return fail("missing data chunk magic (expected 'FLdt')");
    r.dataSize = readU32(bytes, 18);
    // Base view = the raw buffer. For a "zipped .flp" the FLdt payload is one
    // zlib stream: inflate it and run the shared event walker over the
    // restored bytes (header kept, so offsets into the container stay as the
    // file declared them). Failure to inflate is a hard import error.
    std::vector<uint8_t> inflated;
    const std::vector<uint8_t>& view =
        zipped ? (inflated = rebuildContainer(bytes)) : bytes;
    if (zipped && inflated.empty())
        return fail("FLdt payload is zlib-compressed but failed to inflate");
    if (view.size() < 22)
        return fail("decompressed project is too small for an FLP header");

    const uint64_t streamBegin = 22;
    const uint64_t avail = view.size() - streamBegin;
    const uint64_t streamEnd = streamBegin + std::min<uint64_t>(r.dataSize, avail);

    StreamReader s{view, streamBegin, streamEnd};
    FlpEvent ev;
    r.idCounts.assign(256, 0);

    // Structural accumulation state.
    int curChannel = -1;
    int curPattern = -1;
    int curInsert = -1;
    int curArrangement = -1;
    bool inInsertSection = false;

    auto& channels = r.channels;
    auto& patterns = r.patterns;
    auto& inserts = r.inserts;
    auto& arrangements = r.arrangements;

    while (decodeNext(s, ev)) {
        r.events.push_back(ev);
        ++r.idCounts[ev.id];

        const uint8_t id = ev.id;
        uint32_t u16 = id < 128 ? readU16(view, ev.offset + 1) : 0;
        uint32_t u32 = readU32(view, ev.offset + 1);

        switch (id) {
            case kEvFLVersion:
                r.flVersion = decodeString(ev.value);
                break;
            case kEvFLBuild:
                r.build = u32;
                break;
            case kEvTempo:
                r.tempo = double(u32) / 1000.0;
                break;
            case kEvTempoCoarse:
                if (r.tempo == 0) r.tempo = double(u16) / 1000.0;
                break;
            case kEvTempoFine:
                if (r.tempo == 0) r.tempo = double(u16) / 1000.0;
                break;
            case kEvProjectTitle:
                r.title = decodeString(ev.value);
                break;
            case kEvGenre:
                r.genre = decodeString(ev.value);
                break;
            case kEvArtists:
                r.artists = decodeString(ev.value);
                break;

            // Channels.
            case kEvChannelNew:
                channels.push_back(FlpChannel{});
                channels.back().iid = int(u16);
                curChannel = int(channels.size()) - 1;
                break;
            case kEvChannelType:
                if (!inInsertSection && curChannel >= 0 && ev.value.size() >= 1)
                    channels[size_t(curChannel)].type = int(ev.value[0]);
                break;
            case kEvChannelRoutedTo:
                if (!inInsertSection && curChannel >= 0 && ev.value.size() >= 1)
                    channels[size_t(curChannel)].insert =
                        int(int8_t(ev.value[0]));
                break;
            case kEvChannelIsEnabled:
                if (!inInsertSection && curChannel >= 0 && ev.value.size() >= 1)
                    channels[size_t(curChannel)].enabled = ev.value[0] != 0;
                break;
            case kEvChanNameLegacy:
                if (!inInsertSection && curChannel >= 0)
                    channels[size_t(curChannel)].name = decodeString(ev.value);
                break;
            case kEvPluginName:
                if (!inInsertSection && curChannel >= 0 && channels[size_t(curChannel)].name.empty())
                    channels[size_t(curChannel)].name = decodeString(ev.value);
                else if (inInsertSection && curInsert >= 0)
                    inserts[size_t(curInsert)].name = decodeString(ev.value);
                break;
            case kEvInternalName:
                if (!inInsertSection && curChannel >= 0)
                    channels[size_t(curChannel)].internalName = decodeString(ev.value);
                break;
            case kEvSamplePath:
                if (!inInsertSection && curChannel >= 0)
                    channels[size_t(curChannel)].samplePath = decodeString(ev.value);
                break;
            case kEvColor:
                if (!inInsertSection && curChannel >= 0)
                    channels[size_t(curChannel)].color = u32;
                if (inInsertSection && curInsert >= 0)
                    inserts[size_t(curInsert)].color = u32;
                break;
            case 149:  // InsertID.Color (DWORD+21)
                if (inInsertSection && curInsert >= 0)
                    inserts[size_t(curInsert)].color = u32;
                break;
            case kEvGroupNum:
                if (!inInsertSection && curChannel >= 0) channels[size_t(curChannel)].group = int(int32_t(u32));
                break;

            // Patterns.
            case kEvPatternNew: {
                int existing = -1;
                for (size_t i = 0; i < patterns.size(); ++i)
                    if (patterns[i].iid == int(u16)) { existing = int(i); break; }
                if (existing >= 0) {
                    curPattern = existing;
                } else {
                    patterns.push_back(FlpPattern{});
                    patterns.back().iid = int(u16);
                    curPattern = int(patterns.size()) - 1;
                }
                break;
            }
            case kEvPatternName:
                if (curPattern >= 0)
                    patterns[size_t(curPattern)].name = decodeString(ev.value);
                break;
            case kEvPatternColor:
                if (curPattern >= 0) patterns[size_t(curPattern)].color = u32;
                break;
            case kEvPatternLength:
                if (curPattern >= 0) patterns[size_t(curPattern)].length = u32;
                break;
            case kEvPatternLooped:
                if (curPattern >= 0 && ev.value.size() >= 1)
                    patterns[size_t(curPattern)].looped = ev.value[0] != 0;
                break;
            case 224: {  // PatternID.Notes (DATA base). payload = note structs.
                if (curPattern >= 0) {
                    auto& pat = patterns[size_t(curPattern)];
                    pat.notesBytes = ev.value.size();
                    pat.notes.clear();
                    const int width = (ev.value.size() % 24 == 0) ? 24 : 8;
                    pat.noteWidth = width;
                    if (width == 24) {
                        for (size_t i = 0; i + 24 <= ev.value.size(); i += 24) {
                            const uint8_t* p = ev.value.data() + i;
                            FlpNote n;
                            n.tick = leU32(p + 0);
                            const uint16_t flags = leU16(p + 4);
                            n.channel = leU16(p + 6);
                            n.lengthTicks = leU32(p + 8);
                            n.key = uint8_t(leU16(p + 12));
                            n.group = leU16(p + 14);
                            n.finePitch = p[16];
                            n.release = p[18];
                            n.midiChannel = p[19];
                            n.pan = p[20];
                            n.velocity = p[21];
                            n.modX = p[22];
                            n.modY = p[23];
                            n.slide = (flags & 0x0008u) != 0;
                            pat.notes.push_back(n);
                        }
                    } else {
                        for (size_t i = 0; i + 8 <= ev.value.size(); i += 8) {
                            const uint8_t* p = ev.value.data() + i;
                            FlpNote n;
                            n.tick = leU32(p + 0);
                            n.key = p[4];
                            n.velocity = p[5];
                            n.pan = p[6];
                            n.lengthTicks = p[7];
                            n.channel = 0xFFFF;  // legacy structs carry no channel
                            pat.notes.push_back(n);
                        }
                    }
                    pat.noteCount = pat.notes.size();
                }
                break;
            }

            // Mixer inserts.
            case kEvInsertOutput:
                inInsertSection = true;
                inserts.push_back(FlpMixerInsert{});
                inserts.back().index = int(inserts.size()) - 1;
                inserts.back().output = int(int32_t(u32));
                curInsert = int(inserts.size()) - 1;
                break;
            case kEvInsertName:
                if (inInsertSection && curInsert >= 0)
                    inserts[size_t(curInsert)].name = decodeString(ev.value);
                break;
            case 235:  // InsertID.Routing (DATA+27)
                if (inInsertSection && curInsert >= 0)
                    inserts[size_t(curInsert)].sawRouting = true;
                break;
            case kEvInsertFlags:
                if (inInsertSection) curInsert = -1;
                break;

            // Arrangement / playlist / tracks.
            case kEvArrangementName:
                if (curArrangement >= 0)
                    arrangements[size_t(curArrangement)].name = decodeString(ev.value);
                break;
            case 99:  // ArrangementID.New (WORD+35)
                arrangements.push_back(FlpArrangement{});
                arrangements.back().iid = int(u16);
                curArrangement = int(arrangements.size()) - 1;
                break;
            case kEvTrack:
                if (curArrangement >= 0)
                    arrangements[size_t(curArrangement)].trackIds.push_back(int(int32_t(u32)));
                break;
            case kEvTrackName:
                if (curArrangement >= 0 && ev.value.size() >= 1 &&
                    ev.value[0] != 0) {
                    auto& a = arrangements[size_t(curArrangement)];
                    int trackId = a.trackIds.empty() ? -1 : int(a.trackIds.back());
                    a.trackNames.push_back({trackId, decodeString(ev.value)});
                }
                break;
            case kEvPlaylist: {
                if (curArrangement < 0) {
                    arrangements.push_back(FlpArrangement{});
                    curArrangement = 0;
                }
                auto& a = arrangements[size_t(curArrangement)];
                const std ::vector<size_t> widths = {60, 32};
                size_t w = 0;
                for (size_t c : widths) {
                    if (ev.value.size() % c == 0) {
                        w = c;
                        break;
                    }
                }
                if (w) {
                    size_t n = ev.value.size() / w;
                    for (size_t i = 0; i < n; ++i) {
                        const uint8_t* p = ev.value.data() + i * w;
                        FlpPlaylistItem it;
                        it.position = leU32(p);
                        // Playlist items carry pattern references in a
                        // base+index band (PyFLP struct): u16 pattern_base at
                        // +4 (always 0x5000) and u16 item_index at +6. A
                        // pattern's number is item_index - pattern_base; values
                        // below the base reference a channel (audio clip), not
                        // a pattern.
                        const uint32_t base = leU16(p + 4);
                        const uint32_t index = leU16(p + 6);
                        if (index >= base) {
                            it.pattern = uint16_t(index - base);
                        } else {
                            it.isClip = true;
                            it.pattern = uint16_t(index);  // channel IID
                        }
                        it.length = leU32(p + 8);
                        it.track = uint16_t(leU16(p + 12));
                        it.group = uint16_t(leU16(p + 14));
                        if (w >= 32) {
                            it.startOffset = *reinterpret_cast<const float*>(p + 24);
                            it.endOffset = *reinterpret_cast<const float*>(p + 28);
                        }
                        a.items.push_back(it);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    r.truncated = s.truncated;
    r.streamEnd = s.off;
    r.unknownEventCount = 0;
    for (size_t i = 0; i < r.idCounts.size(); ++i) {
        // IDs with no named structural meaning are still counted; we report the
        // histogram in full via idCounts() — nothing is treated as "unknown".
    }

    r.ok = true;
    return r;
}

FlpResult FlpReader::parseFile(const std::string& path) {
    FlpResult r;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        r.ok = false;
        r.error = "cannot open '" + path + "'";
        return r;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    return parse(bytes);
}

}  // namespace misuc::flp