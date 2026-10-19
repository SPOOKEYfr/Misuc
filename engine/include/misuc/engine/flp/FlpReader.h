#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace misuc::flp {

// Decoded structural model of an FL Studio project (.flp).
//
// CLEAN-ROOM STRUCTURAL READER: this parser extracts only layout/structure
// (channels, patterns, playlist, mixer topology, tempo, version). It never
// reproduces FL-plugin preset bytes or bundled audio/sample data. It is
// written from publicly documented file-format facts (image-line public notes,
// the community FLP_Format writeup and the PyFLP/Kaitai independent
// re-implementations) and cross-validated against those tools on real files.

// Ground-truth event IDs relevant to structure (see FlpReader docs).
// Byte events (0-63).
inline constexpr uint8_t kEvChannelIsEnabled = 0;
inline constexpr uint8_t kEvChannelType = 21;      // ChannelID.Type
inline constexpr uint8_t kEvChannelRoutedTo = 22;  // ChannelID.RoutedTo
inline constexpr uint8_t kEvChannelIsLocked = 32;
inline constexpr uint8_t kEvPatternLooped = 26;
// Word events (64-127).
inline constexpr uint8_t kEvChannelNew = 64;       // ChannelID.New
inline constexpr uint8_t kEvPatternNew = 65;       // PatternID.New
inline constexpr uint8_t kEvTempoCoarse = 66;      // ProjectID._TempoCoarse
inline constexpr uint8_t kEvPatternCurSel = 67;    // ProjectID._CurrentPat
inline constexpr uint8_t kEvPatDataLegacy = 68;    // legacy note data
inline constexpr uint8_t kEvTempoFine = 93;        // ProjectID._TempoFine  (64+29)
inline constexpr uint8_t kEvSlotIndex = 98;        // SlotID.Index (64+34)
// DWord events (128-191).
inline constexpr uint8_t kEvColor = 128;           // PluginID.Color / InsertID.Color
inline constexpr uint8_t kEvPlaylistLegacy = 129;  // legacy playlist item
inline constexpr uint8_t kEvInsertIcon = 155;      // InsertID.Icon (128+27)
inline constexpr uint8_t kEvTempo = 156;           // ProjectID.Tempo (128+28)
inline constexpr uint8_t kEvFLBuild = 159;         // ProjectID.FLBuild (128+31)
inline constexpr uint8_t kEvInsertOutput = 147;    // InsertID.Output (128+19)
inline constexpr uint8_t kEvGroupNum = 145;        // ChannelID.GroupNum (128+17)
inline constexpr uint8_t kEvPatternLength = 164;   // PatternID.Length (128+36)
inline constexpr uint8_t kEvPatternColor = 150;    // PatternID.Color (128+22)
// Text events (192-255).
inline constexpr uint8_t kEvChanNameLegacy = 192;  // ChannelID._Name
inline constexpr uint8_t kEvPatternName = 193;     // PatternID.Name
inline constexpr uint8_t kEvProjectTitle = 194;    // ProjectID.Title (192+2)
inline constexpr uint8_t kEvSamplePath = 196;      // ChannelID.SamplePath (192+4)
inline constexpr uint8_t kEvFLVersion = 199;       // ProjectID.FLVersion (192+7)
inline constexpr uint8_t kEvInternalName = 201;    // PluginID.InternalName (192+9)
inline constexpr uint8_t kEvPluginName = 203;      // PluginID.Name (192+11)
inline constexpr uint8_t kEvInsertName = 204;      // InsertID.Name (192+12)
inline constexpr uint8_t kEvGenre = 206;           // ProjectID.Genre (192+14)
inline constexpr uint8_t kEvArtists = 207;         // ProjectID.Artists (192+15)
inline constexpr uint8_t kEvDisplayGroupName = 231;
inline constexpr uint8_t kEvPlaylist = 233;        // ArrangementID.Playlist (208+25)
inline constexpr uint8_t kEvInsertFlags = 236;     // InsertID.Flags (208+28)
inline constexpr uint8_t kEvTrack = 238;           // TrackID.Data (208+30)
inline constexpr uint8_t kEvTrackName = 239;       // TrackID.Name (192+47)
inline constexpr uint8_t kEvArrangementName = 241; // ArrangementID.Name (192+49)

// One raw FLdt event (header recognised, fixed-size value, or varint-prefixed).
struct FlpEvent {
    uint8_t id = 0;
    uint64_t offset = 0;      // absolute file offset of the id byte
    uint64_t size = 0;        // total encoded length (id + len + value)
    std::vector<uint8_t> value;  // value bytes (NOT decoded/converted)
};

struct FlpChannel {
    int iid = -1;
    int type = -1;        // ChannelID.Type value; -1 if unset
    int insert = -1;      // ChannelID.RoutedTo (mixer insert index); -1 if unset
    int group = -1;       // ChannelID.GroupNum
    bool enabled = true;
    uint32_t color = 0;
    std::string name;         // PluginID.Name / ChannelID._Name
    std::string internalName; // PluginID.InternalName
    std::string samplePath;   // ChannelID.SamplePath
};

// One decoded note from a pattern's NotesEvent (PatternID.Notes, id 224).
// The modern struct is 24 bytes (FL >= ~20): u32 tick | u16 flags | u16
// rack_channel | u32 length | u16 key | u16 group | u8 fine_pitch | u8 _u1 |
// u8 release | u8 midi_channel | u8 pan | u8 velocity | u8 mod_x | u8 mod_y.
// Legacy 8-byte structs (older files) decode to the same fields with the
// missing members at their defaults. Byte offsets per PyFLP (independent
// oracle consulted for validation only) and community FLP_Format notes.
struct FlpNote {
    uint32_t tick = 0;        // start offset within the pattern (PPQ ticks)
    uint32_t lengthTicks = 0; // duration (PPQ ticks)
    uint8_t key = 0;          // MIDI note number
    uint8_t velocity = 0;     // raw FL velocity (1..127 typical)
    uint8_t pan = 64;         // raw FL pan (64 = centre, 0..127 typically)
    uint16_t channel = 0;     // containing channel IID (FL v20.1b1+);
                              // 0xFFFF = unassigned (legacy structs)
    uint16_t group = 0;
    uint8_t finePitch = 0;
    uint8_t release = 0;
    uint8_t midiChannel = 0;
    uint8_t modX = 0;
    uint8_t modY = 0;
    bool slide = false;       // NotesEvent flags bit 3
};

struct FlpPattern {
    int iid = -1;
    std::string name;         // PatternID.Name
    uint32_t color = 0;
    uint32_t length = 0;      // PatternID.Length (0 if absent)
    bool looped = false;
    uint64_t noteCount = 0;   // == notes.size() (PatternID.Notes decoded)
    uint64_t notesBytes = 0;  // raw NotesEvent payload in bytes
    int noteWidth = 0;        // 24 (modern) or 8 (legacy) per-note struct bytes
    std::vector<FlpNote> notes;  // decoded notes, stream order preserved
};

struct FlpMixerInsert {
    int index = -1;           // ordinal encountered in the mixer section
    std::string name;         // InsertID.Name
    uint32_t color = 0;
    int output = -1;          // InsertID.Output
    bool sawRouting = false;  // InsertID.Routing present
};

struct FlpPlaylistItem {
    uint32_t position = 0;
    uint32_t length = 0;
    uint16_t pattern = 0;     // pattern index (item_index field - pattern_base)
    uint16_t track = 0;       // reversed track index (0->top)
    uint16_t group = 0;
    bool isClip = false;      // item references a channel (audio clip), not a pattern
    float startOffset = 0;
    float endOffset = 0;
};

struct FlpArrangement {
    int iid = -1;
    std::string name;
    std::vector<FlpPlaylistItem> items;
    std::vector<int> trackIds;      // playlist tracks seen (TrackID.Data ids)
    std::vector<std::pair<int, std::string>> trackNames;
};

struct FlpResult {
    bool ok = false;
    std::string error;

    // Container.
    std::string magic;          // "FLhd"
    uint32_t headerSize = 0;
    uint16_t format = 0;
    uint16_t headerChannels = 0;
    uint16_t ppq = 0;
    uint32_t dataSize = 0;      // FLdt chunk size
    uint64_t streamEnd = 0;     // last fully-decoded event end offset

    // Project metadata (structural only).
    std::string flVersion;      // e.g. "24.2.2.4597"
    uint32_t build = 0;
    double tempo = 0;           // BPM (tempo/1000)
    std::string title;
    std::string genre;
    std::string artists;
    std::string comments;

    // Event stream.
    std::vector<FlpEvent> events;
    std::vector<uint32_t> idCounts;        // index = event id, value = count
    uint64_t unknownEventCount = 0;
    bool truncated = false;

    // Structural model.
    std::vector<FlpChannel> channels;
    std::vector<FlpPattern> patterns;
    std::vector<FlpMixerInsert> inserts;
    std::vector<FlpArrangement> arrangements;

    uint32_t idCount(uint8_t id) const {
        return id < idCounts.size() ? idCounts[id] : 0;
    }
};

class FlpReader {
public:
    // Parse a whole .flp byte buffer. Never throws.
    static FlpResult parse(const std::vector<uint8_t>& bytes);
    // Read + parse a file; empty path or IO error sets result.ok=false.
    static FlpResult parseFile(const std::string& path);

private:
    static std::string decodeString(const std::vector<uint8_t>& raw);
    static uint32_t readU16(const std::vector<uint8_t>& b, uint64_t off);
    static uint32_t readU32(const std::vector<uint8_t>& b, uint64_t off);
};

}  // namespace misuc::flp