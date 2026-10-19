#include "misuc/engine/flp/FlpImport.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace misuc::flp {

using misuc::project::AudioClip;
using misuc::project::ChannelType;
using misuc::project::MidiRegion;
using misuc::project::NoteEvent;
using misuc::project::Session;
using misuc::project::StepChannel;
using misuc::project::Track;

namespace {

double toBeats(uint32_t ticks, uint32_t ppq) {
    return ppq ? double(ticks) / double(ppq) : 0.0;
}

uint8_t clampVelocity(uint8_t raw) {
    if (raw == 0) return 1;       // FL writes no silent notes; keep audible
    if (raw > 127) return 127;
    return raw;
}

// The channel's FL rack type as reported by ChannelID.Type. 0 = sampler
// (sample-triggered); anything else drives a synth/instrument.
int channelTypeFor(const FlpResult& flp, int channelIid) {
    for (const auto& ch : flp.channels)
        if (ch.iid == channelIid) return ch.type;
    return -2;  // channel not in the rack
}

// Resolves a channel sample path into the absolute candidate used for the
// rack row and on-disk checks. Absolute paths pass through; relative paths
// are joined against `baseDir` (the .flp's folder) when one is provided.
std::string resolveSamplePath(const std::string& path,
                              const std::string& baseDir) {
    if (path.empty() || path.front() == '/') return path;
    if (baseDir.empty()) return path;
    std::string joined = baseDir;
    if (!joined.empty() && joined.back() != '/') joined.push_back('/');
    joined += path;
    return joined;
}

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// Whether we can meaningfully check the sample on disk: we have an absolute
// candidate (either the path itself was absolute, or baseDir resolved it).
bool canCheckSample(const std::string& path, const std::string& baseDir) {
    if (path.empty()) return false;
    if (path.front() == '/') return true;
    return !baseDir.empty();
}

}  // namespace

FlpImportResult importSession(const FlpResult& flp, const std::string& baseDir) {
    FlpImportResult out;
    auto& s = out.session;
    auto& warnings = out.warnings;

    if (!flp.title.empty())
        s.setName(flp.title);
    if (flp.tempo > 0.0)
        s.setTempo(flp.tempo);
    const uint32_t ppq = flp.ppq ? flp.ppq : 96;

    // Mixer inserts (encounter order) -> buses. Channel RoutedTo references
    // match these ordinals in real files (the mixer section is written in
    // insert-number order), so a missing reference only warns.
    std::map<int, std::string> insertToBus;
    for (const auto& ins : flp.inserts) {
        const std::string busName = !ins.name.empty()
                                        ? ins.name
                                        : ("Insert " + std::to_string(ins.index));
        const std::string busId = s.addBus(busName);
        insertToBus[ins.index] = busId;
    }

    // Channels -> tracks (one per rack slot, names + routing preserved) and a
    // matching channel-rack row so the imported kit is live in the rack.
    std::map<int, size_t> channelToTrack;  // channel IID -> track index
    auto& rack = s.channelRack().channels;
    for (const auto& ch : flp.channels) {
        const std::string tname = !ch.name.empty()
                                      ? ch.name
                                      : ("Channel " + std::to_string(ch.iid));
        const std::string trackId = s.addTrack(tname);
        Track* t = s.findTrack(trackId);
        if (!t)
            continue;
        t->muted = !ch.enabled;
        if (ch.insert >= 0) {
            auto busIt = insertToBus.find(ch.insert);
            if (busIt != insertToBus.end())
                t->busId = busIt->second;
            else
                warnings.push_back("channel '" + tname + "' routes to an "
                                   "unresolved mixer insert (" +
                                   std::to_string(ch.insert) + ")");
        }
        channelToTrack[ch.iid] = s.tracks().size() - 1;

        // Channel-rack row (first-class part of the imported session).
        StepChannel sc;
        sc.name = tname;
        sc.id = s.newChannelId();
        sc.muted = !ch.enabled;
        if (ch.type == 0) {
            // Sampler: the row triggers a sample; carry the resolved file.
            sc.type = ChannelType::Sampler;
            if (!ch.samplePath.empty()) {
                sc.soundFile = resolveSamplePath(ch.samplePath, baseDir);
                if (canCheckSample(ch.samplePath, baseDir)) {
                    if (fileExists(sc.soundFile)) {
                        ++out.samplesFound;
                    } else {
                        ++out.samplesMissing;
                        out.missingSamples.push_back(sc.soundFile);
                        warnings.push_back("sample not found: '" +
                                           sc.soundFile + "' (channel '" +
                                           tname + "'); the sampler row keeps "
                                           "the reference so you can still "
                                           "re-point it");
                    }
                }
                ++out.samplerChannels;
            } else {
                warnings.push_back("sampler channel '" + tname +
                                   "' has no sample file; it stays silent until "
                                   "a sample is assigned");
            }
        } else {
            // Pitched generator. Notes import as MIDI (played by the generic
            // synth voice); a channel backed by a specific FL plugin keeps its
            // name as an honest placeholder — the plugin itself is not hosted.
            sc.type = ChannelType::Instrument;
            if (!ch.internalName.empty()) {
                ++out.unsupportedPlugins;
                out.unsupportedPluginNames.push_back(ch.internalName);
            }
        }
        sc.volume = 0.8f;
        sc.pan = 0.0f;
        rack.push_back(std::move(sc));
    }
    out.channelsImported = rack.size();

    // The busiest arrangement becomes the timeline.
    const FlpArrangement* arr = nullptr;
    for (const auto& a : flp.arrangements)
        if (!arr || a.items.size() > arr->items.size())
            arr = &a;
    if (flp.arrangements.size() > 1 && arr) {
        std::string skipped;
        for (const auto& a : flp.arrangements) {
            if (a.iid == arr->iid) continue;
            if (!skipped.empty()) skipped += ", ";
            skipped += "'" + a.name + "'";
        }
        warnings.push_back("imported arrangement '" + arr->name + "' with " +
                           std::to_string(arr->items.size()) + " items; " +
                           std::to_string(flp.arrangements.size() - 1) +
                           " other arrangement(s) skipped: " + skipped);
    }

    // Pattern iid resolution + notes grouped per pattern by rack channel.
    std::map<int, size_t> patternByIid;
    for (size_t pi = 0; pi < flp.patterns.size(); ++pi)
        patternByIid[flp.patterns[pi].iid] = pi;

    using GroupMap = std::map<int, std::vector<const FlpNote*>>;  // channel -> notes
    std::vector<GroupMap> groups(flp.patterns.size());
    size_t unassignedNotes = 0;
    for (size_t pi = 0; pi < flp.patterns.size(); ++pi) {
        for (const auto& n : flp.patterns[pi].notes) {
            const int key = (n.channel == 0xFFFF) ? -1 : int(n.channel);
            groups[pi][key].push_back(&n);
            if (key < 0)
                ++unassignedNotes;
        }
    }

    // Per-pattern fallback "no owning channel" track (legacy structs only).
    std::map<size_t, size_t> fallbackTrackByPattern;  // pattern index -> track index

    std::set<size_t> placedPatterns;

    if (arr) {
        for (const auto& it : arr->items) {
            if (it.isClip) {
                ++out.audioClipsSkipped;
                continue;  // audio-clip item: no pattern, cannot be placed yet
            }
            auto pit = patternByIid.find(int(it.pattern));
            if (pit == patternByIid.end()) {
                warnings.push_back("playlist item references an unknown "
                                   "pattern (" + std::to_string(it.pattern) +
                                   "); skipped");
                continue;
            }
            const size_t pi = pit->second;
            const auto& pat = flp.patterns[pi];
            if (groups[pi].empty())
                continue;  // empty pattern item: nothing to place

            const double regionStart = toBeats(it.position, ppq);
            double regionDur = it.length ? toBeats(it.length, ppq) : 0.0;
            if (regionDur <= 0.0 && pat.length)
                regionDur = toBeats(pat.length, ppq);
            const std::string regionName = !pat.name.empty()
                                               ? pat.name
                                               : ("Pattern " + std::to_string(pat.iid));

            for (const auto& entry : groups[pi]) {
                const int key = entry.first;
                const auto& notes = entry.second;

                // Sampler channels trigger samples per note; we cannot render
                // that yet (Misuc regions are pitched). Keep the channel's
                // track + sampler rack row, skip the hits, and say so.
                if (key >= 0 && channelTypeFor(flp, key) == 0) {
                    out.samplerHitsSkipped += notes.size();
                    continue;
                }

                size_t trackIdx = 0;
                if (key >= 0 && channelToTrack.count(key)) {
                    trackIdx = channelToTrack[key];
                } else {
                    // No owning rack channel (or not present in the rack):
                    // route the whole pattern to its own named track.
                    auto fb = fallbackTrackByPattern.find(pi);
                    if (fb == fallbackTrackByPattern.end()) {
                        const std::string fname = !pat.name.empty()
                                                      ? pat.name
                                                      : ("Pattern " + std::to_string(pat.iid));
                        s.addTrack(fname);
                        fb = fallbackTrackByPattern
                                 .emplace(pi, s.tracks().size() - 1)
                                 .first;
                        warnings.push_back("pattern '" + fname +
                                           "' has notes without a rack channel; "
                                           "placed on its own track");
                    }
                    trackIdx = fb->second;
                }

                const std::string& trackId = s.tracks()[trackIdx].id;
                if (regionDur <= 0.0) {
                    // No length anywhere: bound the region to its last note.
                    uint32_t end = 0;
                    for (const FlpNote* n : notes)
                        end = std::max(end, n->tick + n->lengthTicks);
                    regionDur = toBeats(end, ppq);
                    if (regionDur <= 0.0)
                        regionDur = 0.25;
                }
                std::string regionId =
                    s.addRegion(trackId, regionName, regionStart, regionDur);
                MidiRegion* reg = s.findRegion(trackId, regionId);
                if (!reg)
                    continue;
                reg->notes.reserve(notes.size());
                for (const FlpNote* n : notes) {
                    NoteEvent ne;
                    ne.pitch = n->key;
                    ne.velocity = clampVelocity(n->velocity);
                    ne.startBeats = toBeats(n->tick, ppq);
                    ne.durationBeats = toBeats(n->lengthTicks, ppq);
                    if (ne.durationBeats <= 0.0)
                        ne.durationBeats = 1.0 / double(ppq ? ppq : 96);  // 1 tick minimum
                    reg->notes.push_back(ne);
                    ++out.notesImported;
                }
                ++out.playlistRegions;
                placedPatterns.insert(pi);
            }
        }
    }
    out.patternsImported = placedPatterns.size();

    if (out.samplerHitsSkipped > 0)
        warnings.push_back("skipped " + std::to_string(out.samplerHitsSkipped) +
                           " sampler-channel note(s): Misuc does not render "
                           "sample-triggered hits yet; their sampler rows are "
                           "preserved in the channel rack and can be auditioned");
    if (out.audioClipsSkipped > 0)
        warnings.push_back(std::to_string(out.audioClipsSkipped) +
                           " playlist item(s) reference an audio clip; "
                           "Misuc does not arrange audio clips yet");
    if (unassignedNotes > 0)
        warnings.push_back(std::to_string(unassignedNotes) +
                           " note(s) carried no rack channel (legacy structs)");
    if (flp.patterns.empty() || groups.empty())
        warnings.push_back("no musical patterns; the imported project is mixer "
                           "topology only");

    return out;
}

}  // namespace misuc::flp