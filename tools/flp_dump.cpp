// misuc_flp — clean-room structural reader for FL Studio project files (.flp).
// Extracts container + structural model only (channels, patterns, playlist,
// mixer topology, tempo, version, event histogram). Never reproduces FL-plugin
// preset bytes, audio/sample payloads, or any plugin-internal parameters.
// Optionally imports the structural model into a .misuc project document.
//
// Usage:
//   misuc_flp <song.flp>            structural summary
//   misuc_flp --events <song.flp>   raw event stream (id: offset size)
//   misuc_flp --verify <song.flp>   exit 0 if container walks clean to EOF
//   misuc_flp --import <song.flp> --out <project.misuc>

#include "misuc/engine/core/ProjectFile.h"
#include "misuc/engine/flp/FlpImport.h"
#include "misuc/engine/flp/FlpReader.h"

#include <cstdio>
#include <string>
#include <vector>

using misuc::flp::FlpArrangement;
using misuc::flp::FlpChannel;
using misuc::flp::FlpReader;
using misuc::flp::FlpResult;
using misuc::flp::importSession;

static const char* channelTypeName(int t) {
    switch (t) {
        case 0: return "sampler";
        case 1: return "FLKeys";
        case 2: return "3xOSC";
        case 3: return "TS404";
        case 4: return "BeepMap";
        case 5: return "Layer";
        case 6: return "MidiOut";
        case 7: return "SimSynth";
        case 8: return "DirectWave";
        case 9: return "SoundFontPlayer";
        case 10: return "Patcher-ins";
        default: return nullptr;
    }
}

static void printEvents(const FlpResult& r) {
    for (const auto& e : r.events) {
        std::printf("%3u @%10llu sz=%5llu\n", unsigned(e.id),
                    (unsigned long long)e.offset, (unsigned long long)e.size);
    }
    std::printf("events=%zu  streamEnd=%llu  truncated=%s\n", r.events.size(),
                (unsigned long long)r.streamEnd, r.truncated ? "yes" : "no");
}

static void printSummary(const FlpResult& r) {
    std::printf("container: magic=%s format=%u channels(hdr)=%u ppq=%u "
                "fldt=%u bytes\n",
                r.magic.c_str(), unsigned(r.format), unsigned(r.headerChannels),
                unsigned(r.ppq), unsigned(r.dataSize));
    std::printf("project:   fl_version=%s build=%u tempo=%.3f\n",
                r.flVersion.c_str(), unsigned(r.build), r.tempo);
    if (!r.title.empty()) std::printf("  title=%s\n", r.title.c_str());
    if (!r.genre.empty()) std::printf("  genre=%s\n", r.genre.c_str());
    if (!r.artists.empty()) std::printf("  artists=%s\n", r.artists.c_str());

    std::printf("events:    total=%zu streamEnd=%llu truncated=%s\n",
                r.events.size(), (unsigned long long)r.streamEnd,
                r.truncated ? "yes" : "no");

    std::printf("channels:  %zu\n", r.channels.size());
    for (const auto& c : r.channels) {
        const char* tn = channelTypeName(c.type);
        std::printf("  [%d] %-20s type=%-12s(%d) insert=%d group=%d%s%s%s\n",
                    c.iid, c.name.empty() ? "(unnamed)" : c.name.c_str(),
                    tn ? tn : "?", c.type, c.insert, c.group,
                    c.internalName.empty() ? ""
                                           : (std::string(" int=") + c.internalName).c_str(),
                    c.samplePath.empty() ? ""
                                         : (std::string(" src=") + c.samplePath).c_str(),
                    c.enabled ? "" : " OFF");
    }

    std::printf("patterns:  %zu\n", r.patterns.size());
    for (const auto& p : r.patterns) {
        std::printf("  [%d] %-20s notes=%llu len=%u%s%s\n", p.iid,
                    p.name.empty() ? "(unnamed)" : p.name.c_str(),
                    (unsigned long long)p.noteCount, p.length,
                    p.looped ? " LOOP" : "", p.notesBytes ? "" : " (no notes ev)");
    }

    std::printf("mixer:     %zu inserts\n", r.inserts.size());
    for (const auto& m : r.inserts) {
        std::printf("  [%d] %-20s out=%d %s\n", m.index,
                    m.name.empty() ? "(unnamed)" : m.name.c_str(), m.output,
                    m.sawRouting ? "" : "");
    }

    std::printf("playlist:  %zu arrangements\n", r.arrangements.size());
    size_t totalItems = 0;
    for (const auto& a : r.arrangements) {
        totalItems += a.items.size();
        std::printf("  [%d] %-20s items=%zu tracks=%zu\n",
                    a.iid, a.name.empty() ? "(unnamed)" : a.name.c_str(),
                    a.items.size(), a.trackIds.size());
    }
    std::printf("  total playlist items=%zu\n", totalItems);

    std::printf("histogram: ");
    unsigned shown = 0;
    for (size_t i = 0; i < r.idCounts.size(); ++i) {
        if (r.idCounts[i] == 0) continue;
        std::printf("%zu:%u ", i, unsigned(r.idCounts[i]));
        if (++shown >= 24) break;
    }
    std::printf("\n");
}

int main(int argc, char** argv) {
    bool events = false;
    bool verify = false;
    bool import = false;
    std::string outPath;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--events") events = true;
        else if (a == "--verify") verify = true;
        else if (a == "--import") import = true;
        else if (a == "--out" && i + 1 < argc)
            outPath = argv[++i];
        else if (a.empty() || a[0] == '-') {
            std::fprintf(stderr, "usage: misuc_flp [--events|--verify|--import --out <out.misuc>] <song.flp>\n");
            return 2;
        } else {
            path = a;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: misuc_flp [--events|--verify|--import --out <out.misuc>] <song.flp>\n");
        return 2;
    }
    if (import && outPath.empty()) {
        std::fprintf(stderr, "error: --import requires --out <project.misuc>\n");
        return 2;
    }

    FlpResult r = FlpReader::parseFile(path);
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }

    if (events) {
        printEvents(r);
        return r.truncated ? 1 : 0;
    }
    if (verify) return r.truncated ? 1 : 0;

    if (import) {
        // Highlight decoded note fields that differ from counts.
        for (const auto& p : r.patterns) {
            if (p.noteCount != uint64_t(p.notes.size()))
                std::fprintf(stderr, "warning: pattern %d noteCount %llu != notes.size() %zu\n",
                             p.iid, (unsigned long long)p.noteCount, p.notes.size());
        }
        auto res = importSession(r);
        std::printf("imported: %s\n", res.session.name().c_str());
        std::printf("  tempo=%.1f bpm  tracks=%zu  buses=%zu\n",
                    res.session.tempo(),
                    res.session.tracks().size(),
                    res.session.buses().size());
        size_t totalNotes = 0, totalRegions = 0;
        for (const auto& t : res.session.tracks()) {
            totalRegions += t.regions.size();
            for (const auto& rg : t.regions)
                totalNotes += rg.notes.size();
        }
        std::printf("  regions=%zu  notes=%zu\n", totalRegions, totalNotes);
        if (r.truncated)
            std::printf("  (FL stream truncated; output reflects decoded data)\n");
        std::printf("warnings:\n");
        if (res.warnings.empty()) {
            std::printf("  (none)\n");
        } else {
            for (const auto& w : res.warnings)
                std::printf("  - %s\n", w.c_str());
        }
        std::string err;
        if (!misuc::core::atomicWrite(outPath, res.session.toJson(), &err)) {
            std::fprintf(stderr, "error writing %s: %s\n", outPath.c_str(), err.c_str());
            return 1;
        }
        std::printf("wrote %s\n", outPath.c_str());
        return r.truncated ? 1 : 0;
    }

    printSummary(r);
    return r.truncated ? 1 : 0;
}