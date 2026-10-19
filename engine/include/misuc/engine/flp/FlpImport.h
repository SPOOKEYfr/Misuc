#pragma once

#include <misuc/engine/flp/FlpReader.h>
#include <misuc/engine/project/Session.h>

#include <string>
#include <vector>

namespace misuc::flp {

// One structural FLP import: the converted Session plus a list of
// human-readable warnings about content that could not be represented.
// The import never fails on unrepresentable data — it drops the specific
// piece, records a warning, and keeps everything that maps cleanly.
struct FlpImportResult {
    misuc::project::Session session;
    std::vector<std::string> warnings;

    // Compact import report (the UI's summary dialog consumes these counts;
    // the free-form `warnings` list above carries the details).
    size_t channelsImported = 0;    // rack channels materialized (channel rack)
    size_t samplerChannels = 0;     // ... of which sampler channels (sample ref)
    size_t patternsImported = 0;    // distinct FL patterns placed on the timeline
    size_t notesImported = 0;       // notes materialized into MIDI regions
    size_t playlistRegions = 0;     // MIDI regions placed from playlist items
    size_t audioClipsSkipped = 0;   // playlist audio-clip items (unrepresentable)
    size_t samplerHitsSkipped = 0;  // sampler-channel note hits (not rendered)
    size_t samplesFound = 0;        // sample files present on disk
    size_t samplesMissing = 0;      // sample files referenced but absent
    // Paths of the absent samples (resolved forms), for the missing-samples list.
    std::vector<std::string> missingSamples;
    size_t unsupportedPlugins = 0;  // FL generator plugins this port cannot host
    // Preserved names of those plugins, so the user can see what was a "place".
    std::vector<std::string> unsupportedPluginNames;
};

// Converts a parsed FLP structural model into a Session. Mapping rules:
//   * Project title -> session name; tempo -> session tempo (4/4 default —
//     FLP time signatures are not decoded and are a documented limitation).
//   * Mixer inserts (encounter order) become buses by name ("Insert N" when
//     unnamed); a channel's RoutedTo insert becomes its track's busId.
//   * Every channel becomes a track AND a channel-rack row (so the rack
//     shows the imported kit). Sampler channels (FL type 0) become Sampler
//     rows carrying the channel's sample as `soundFile`; rows for pitched
//     generators become Instrument rows — their notes play through Misuc's
//     generic synth voice, and rows backed by a specific FL plugin (a non-
//     empty InternalName) are flagged as unsupported placed holding the
//     plugin's preserved name. This is "preserve the intent, not the byte
//     streams": Misuc never hosts FL plugin binaries.
//   * Sampler-channel notes are NOT converted to MIDI regions: FL triggers
//     the channel's sample per note, and pretending those hits are pitched
//     synth notes would be a lie. The sampler row (with its sample path)
//     is preserved and auditionable instead; a warning is emitted per
//     skipped hit.
//   * The arrangement with the most playlist items becomes the linear
//     timeline. One MIDI region per (item, channel) pair: notes are grouped by
//     their per-note rack channel and placed on that channel's track at the
//     item position (length from the item, pattern Length, or the last note).
//     A pattern referenced by several items yields one independent region per
//     occurrence. Notes of patterns never placed on the playlist are not
//     materialised (FL would not play them either).
//   * Notes with no rack channel (legacy 8-byte structs) go to a per-pattern
//     track named after the pattern.
// `baseDir` is the .flp file's directory used to resolve relative channel
// sample paths to absolutes and to check they exist (samplesFound /
// samplesMissing are only counted when a resolution context is available).
// Sample paths left relative (or unresolved) survive Session::resolveAssets
// at load, exactly like clip assets.
//
// Per-note pan / fine pitch / group / modulation / slide are decoded into
// FlpNote but not currently representable in NoteEvent — they are dropped and
// enumerated in the component report (the Session note clip model has no
// per-note pan yet). FL plugin preset bytes and sample data are never touched.
FlpImportResult importSession(const FlpResult& flp,
                              const std::string& baseDir = {});

}  // namespace misuc::flp