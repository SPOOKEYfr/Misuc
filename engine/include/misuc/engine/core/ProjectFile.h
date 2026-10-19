#pragma once

#include <string>
#include <vector>

namespace misuc::core {

// Result of reading a .misuc (or any UTF-8 text) document from disk.
struct FileReadResult {
  bool ok = false;      // file was opened and fully read
  bool missing = false; // file does not exist (ok == false, no content)
  std::string content;  // raw document text when ok
  std::string error;    // human-readable reason when !ok && !missing
};

// Reads the whole file into `content`. Never throws for filesystem errors.
FileReadResult readFile(const std::string& path);

// Atomic save primitive: writes `text` to a sibling `path + ".tmp"`, flushes
// it to disk, then renames over `path` (atomic on POSIX and NTFS). On any
// failure the partial temp file is removed and the previous `path` is left
// untouched, so a crash mid-write can never corrupt an existing document.
// Returns false (with `error` populated when non-null) on failure.
bool atomicWrite(const std::string& path, const std::string& text,
                 std::string* error = nullptr);

// ────────────────────────────────────────────────────────────────────────
// Native .misuc project container.
//
// `.misuc` is NOT plain JSON text: it is a real ZIP archive (deflate via
// zlib, already a dependency) holding at least
//   `misuc.meta`   — container JSON metadata (format + version)
//   `misuc.song`   — the session JSON (structured internal state)
// and optionally `assets/<rel>` entries for bundled project assets. The
// container keeps the project atomically writable, versioned, migration-safe
// and corruption-checked (every entry carries a CRC-32 that is verified on
// read). Legacy plain-JSON `.misuc` documents from older builds are detected
// on read and load transparently; the next save rewrites them as containers.
// ────────────────────────────────────────────────────────────────────────

// Legacy documents carry the old text marker; container documents are ZIP.
constexpr int kContainerFormatVersion = 1;
constexpr const char* kContainerMetaEntry = "misuc.meta";
constexpr const char* kContainerSongEntry = "misuc.song";

struct ContainerEntry {
  std::string name; // "misuc.song", "misuc.meta", "assets/<rel>", ...
  std::string data; // raw entry bytes (deflate handled internally)
};

struct ContainerReadResult {
  bool ok = false;          // opened + verified (ZIP magic, sizes, CRC-32s)
  bool missing = false;     // no such file (ok == false)
  bool legacyJson = false;  // a pre-container plain-JSON document, loaded
                            // verbatim into `legacyText` (migrated on save)
  std::string legacyText;   // full session JSON when legacyJson
  std::vector<ContainerEntry> entries; // verified container entries
  std::string error;        // human-readable reason when !ok && !missing
};

// Reads a .misuc document: native ZIP container OR legacy plain JSON. On a
// container, every entry's CRC-32 is verified and sizes are bounds-checked, so
// a truncated/corrupted archive fails cleanly instead of producing garbage.
ContainerReadResult readContainer(const std::string& path);

// Atomic-writes `entries` as a native .misuc container. The caller supplies
// `misuc.song` (+ optional `assets/...`); the writer embeds `misuc.meta`
// with the container format version. Renames over `path` only after a full
// verified temp write (fsync + directory fsync), so a crash never leaves a
// truncated document. Returns false with `error` populated on failure.
bool writeContainer(const std::string& path,
                    const std::vector<ContainerEntry>& entries,
                    std::string* error = nullptr);

// UTC timestamp in RFC-3339 form (e.g. "2026-09-10T14:03:22Z"), used for the
// createdUtc / modifiedUtc project metadata.
std::string utcNowIso();

} // namespace misuc::core