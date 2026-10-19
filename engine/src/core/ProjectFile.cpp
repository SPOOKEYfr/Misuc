#include "misuc/engine/core/ProjectFile.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <system_error>
#include <vector>

#include <zlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace misuc::core {

FileReadResult readFile(const std::string& path) {
  FileReadResult r;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    r.error = "cannot open: " + path;
    // Distinguish "does not exist" (a recoverable condition) from other
    // failures so callers can treat a missing autosave as "nothing to do".
    struct stat st;
#ifdef _WIN32
    r.missing = ::_stat64(path.c_str(), &st) != 0;
#else
    r.missing = ::stat(path.c_str(), &st) != 0;
#endif
    return r;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  in.seekg(0, std::ios::beg);
  if (size > 0) {
    r.content.resize(static_cast<size_t>(size));
    in.read(&r.content[0], size);
  }
  if (!in) {
    r.ok = false;
    r.error = "short read: " + path;
    return r;
  }
  r.ok = true;
  return r;
}

#if defined(_WIN32)

bool atomicWrite(const std::string& path, const std::string& text,
                 std::string* error) {
  const std::string tmp = path + ".tmp";
  const HANDLE h = ::CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = "cannot create temp file: " + tmp;
    return false;
  }
  size_t written = 0;
  bool ok = true;
  while (written < text.size()) {
    DWORD n = 0;
    const DWORD chunk = (DWORD)std::min<size_t>(text.size() - written,
                                                1024u * 1024u);
    if (!::WriteFile(h, text.data() + written, chunk, &n, nullptr) || n == 0) {
      ok = false;
      break;
    }
    written += n;
  }
  if (ok && !::FlushFileBuffers(h))
    ok = false;
  ::CloseHandle(h);
  if (!ok) {
    ::DeleteFileA(tmp.c_str());
    if (error) *error = "write failed: " + tmp;
    return false;
  }
  if (!::MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileA(tmp.c_str());
    if (error) *error = "rename failed: " + path;
    return false;
  }
  return true;
}

#else

bool atomicWrite(const std::string& path, const std::string& text,
                 std::string* error) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    if (error) *error = "cannot create temp file: " + tmp + ": " + ::strerror(errno);
    return false;
  }
  size_t off = 0;
  bool ok = true;
  while (off < text.size()) {
    const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ok = false;
      break;
    }
    off += static_cast<size_t>(n);
  }
  if (ok && ::fsync(fd) != 0)
    ok = false;
  ::close(fd);
  if (!ok) {
    ::unlink(tmp.c_str());
    if (error) *error = "write/fsync failed: " + tmp + ": " + ::strerror(errno);
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    if (error) *error = "rename failed: " + path + ": " + ::strerror(errno);
    return false;
  }
  // Best-effort directory flush so the rename itself survives a power cut.
  const std::string::size_type slash = path.find_last_of('/');
  const std::string dir =
      slash == std::string::npos ? "." : path.substr(0, slash);
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd >= 0) {
    (void)::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

#endif

std::string utcNowIso() {
  using clock = std::chrono::system_clock;
  const std::time_t now = clock::to_time_t(clock::now());
  std::tm tmv;
#if defined(_WIN32)
  gmtime_s(&tmv, &now);
#else
  gmtime_r(&now, &tmv);
#endif
  char buf[32];
  const int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                              tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                              tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return std::string(buf, n);
}

// ────────────────────────────────────────────────────────────────────────
// Native .misuc container (ZIP archive, deflate via zlib).
//
// Layout for each entry:
//   [local file header 30B][name][extra][file data]
// followed by the central directory and the end-of-central-directory record,
// so the file is a standards-compliant ZIP that any archive tool can read.
// Every entry carries its CRC-32 (+ compressed/uncompressed sizes); the
// reader re-verifies them, which gives corruption detection for free.
// ────────────────────────────────────────────────────────────────────────

namespace {

constexpr uint32_t kLocalHeaderSig = 0x04034b50u;
constexpr uint32_t kCentralSig = 0x02014b50u;
constexpr uint32_t kEndOfCentralSig = 0x06054b50u;
constexpr uint32_t kMethodStore = 0;
constexpr uint32_t kMethodDeflate = 8;
constexpr uint16_t kVersionNeeded = 20;

void put16(std::string& out, uint16_t v) {
  out.push_back(char(v & 0xff));
  out.push_back(char((v >> 8) & 0xff));
}
void put32(std::string& out, uint32_t v) {
  out.push_back(char(v & 0xff));
  out.push_back(char((v >> 8) & 0xff));
  out.push_back(char((v >> 16) & 0xff));
  out.push_back(char((v >> 24) & 0xff));
}

uint16_t get16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

// Deflates `src` with zlib; returns true on success. `dst` is only modified
// on success.
bool zlibDeflate(const std::string& src, std::string& dst, std::string* err) {
  uLongf bound = ::compressBound(static_cast<uLong>(src.size()));
  if (src.empty()) {
    dst.clear();
    return true;
  }
  dst.resize(bound);
  uLongf outLen = bound;
  const int rc = ::compress2(reinterpret_cast<Bytef*>(&dst[0]), &outLen,
                             reinterpret_cast<const Bytef*>(src.data()),
                             static_cast<uLong>(src.size()), Z_BEST_COMPRESSION);
  if (rc != Z_OK) {
    if (err) *err = "deflate failed (zlib rc=" + std::to_string(rc) + ")";
    return false;
  }
  dst.resize(outLen);
  return true;
}

bool zlibInflate(const std::string& src, size_t expected, std::string& dst,
                 std::string* err) {
  if (src.empty()) {
    dst.clear();
    return true;
  }
  dst.resize(expected == 0 && !src.empty() ? src.size() * 2 + 64 : expected);
  // Guard huge expansions: cap at 4 GiB (usize is 32-bit in the format).
  if (dst.size() > 0xffffffffu) {
    if (err) *err = "inflate size over 4 GiB";
    return false;
  }
  uLongf outLen = static_cast<uLongf>(dst.size());
  const int rc = ::uncompress(reinterpret_cast<Bytef*>(&dst[0]), &outLen,
                              reinterpret_cast<const Bytef*>(src.data()),
                              static_cast<uLong>(src.size()));
  if (rc != Z_OK || (expected > 0 && outLen != expected)) {
    // The 4 GiB guard means we probe sequentially upward in rare oversized
    // cases; for the expected sizes here this path is effectively unused.
    if (err) *err = "inflate failed (zlib rc=" + std::to_string(rc) + ")";
    return false;
  }
  dst.resize(outLen);
  return true;
}

} // namespace

bool writeContainer(const std::string& path,
                    const std::vector<ContainerEntry>& entries,
                    std::string* error) {
  // Avoid giant allocations: serialize into a single buffer first.
  std::string out;
  out.reserve(4096);
  std::vector<std::string> compressed(entries.size());
  std::vector<uint32_t> csize(entries.size()), usize(entries.size()), crcs(entries.size());
  std::vector<uint16_t> method(entries.size());
  std::vector<uint32_t> lho(entries.size());

  const uint8_t* srcBytes = nullptr;

  // Pass 1: compress + CRC every payload and lay out local headers/data.
  for (size_t i = 0; i < entries.size(); ++i) {
    const std::string& name = entries[i].name;
    const std::string& data = entries[i].data;
    if (name.empty() || name.size() > 0xffffu) {
      if (error) *error = "container entry name invalid";
      return false;
    }
    uint32_t crc = ::crc32(0, Z_NULL, 0);
    if (!data.empty())
      crc = ::crc32(crc, reinterpret_cast<const Bytef*>(data.data()),
                    static_cast<uInt>(data.size()));
    usize[i] = static_cast<uint32_t>(data.size());
    crcs[i] = crc;

    // Prefer deflate; fall back to store only when compression would grow it.
    if (data.size() > 32) {
      std::string def;
      if (!zlibDeflate(data, def, error))
        return false;
      if (def.size() < data.size()) {
        compressed[i] = std::move(def);
        csize[i] = static_cast<uint32_t>(compressed[i].size());
        method[i] = kMethodDeflate;
      } else {
        compressed[i] = data;
        csize[i] = static_cast<uint32_t>(data.size());
        method[i] = kMethodStore;
      }
    } else {
      compressed[i] = data;
      csize[i] = static_cast<uint32_t>(data.size());
      method[i] = kMethodStore;
    }

    lho[i] = static_cast<uint32_t>(out.size());
    put32(out, kLocalHeaderSig);
    put16(out, kVersionNeeded);
    put16(out, 0); // flags: no data descriptor
    put16(out, method[i]);
    put16(out, 0); // mod time (kept 0; timestamps live in misuc.meta)
    put16(out, 0); // mod date
    put32(out, crc);
    put32(out, csize[i]);
    put32(out, usize[i]);
    put16(out, static_cast<uint16_t>(name.size()));
    put16(out, 0); // extra length
    out.append(name);
    out.append(compressed[i]);
  }

  const uint32_t cdOffset = static_cast<uint32_t>(out.size());
  std::vector<std::string> central;
  central.reserve(entries.size());
  size_t cdSize = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    std::string c;
    put32(c, kCentralSig);
    put16(c, kVersionNeeded); // version made by
    put16(c, kVersionNeeded);
    put16(c, 0); // flags
    put16(c, method[i]);
    put16(c, 0); // mod time
    put16(c, 0); // mod date
    put32(c, crcs[i]);
    put32(c, csize[i]);
    put32(c, usize[i]);
    put16(c, static_cast<uint16_t>(entries[i].name.size()));
    put16(c, 0); // extra
    put16(c, 0); // comment
    put16(c, 0); // disk number
    put16(c, 0); // internal attrs
    put32(c, 0); // external attrs
    put32(c, lho[i]);
    c.append(entries[i].name);
    cdSize += c.size();
    central.push_back(std::move(c));
  }
  for (const auto& c : central)
    out.append(c);

  // End-of-central-directory record.
  put32(out, kEndOfCentralSig);
  put16(out, 0);                            // disk number
  put16(out, 0);                            // disk where cd starts
  put16(out, static_cast<uint16_t>(entries.size()));
  put16(out, static_cast<uint16_t>(entries.size()));
  put32(out, static_cast<uint32_t>(cdSize));
  put32(out, cdOffset);
  put16(out, 0); // comment length

  (void)srcBytes;
  return atomicWrite(path, out, error);
}

ContainerReadResult readContainer(const std::string& path) {
  ContainerReadResult res;
  const FileReadResult raw = readFile(path);
  if (!raw.ok) {
    res.missing = raw.missing;
    res.error = raw.error;
    return res;
  }

  const uint8_t* b = reinterpret_cast<const uint8_t*>(raw.content.data());
  const size_t n = raw.content.size();

  // Legacy plain-JSON document (older builds wrote `format":"misuc.song"` as
  // raw text). Load it verbatim; the next save migrates it to a container.
  if (n < 4 || b[0] != 'P' || b[1] != 'K') {
    res.ok = true;
    res.legacyJson = true;
    res.legacyText = raw.content;
    return res;
  }
  const uint32_t sig = get32(b);
  if (sig != kLocalHeaderSig && sig != kEndOfCentralSig) {
    res.error = "unrecognized .misuc container signature";
    return res;
  }

  // Locate the end-of-central-directory record scanning from the tail. The
  // record is 22 bytes and begins with the signature PK\x05\x06 at `s`, so a
  // per-byte match spans [s, s+4) i.e. positions i-4..i-1 for i = s+4.
  size_t eocd = n;
  for (size_t i = n; i >= 4; --i) {
    if (uint8_t(raw.content[i - 4]) == 'P' && uint8_t(raw.content[i - 3]) == 'K' &&
        uint8_t(raw.content[i - 2]) == 0x05 && uint8_t(raw.content[i - 1]) == 0x06) {
      eocd = i - 4;
      break;
    }
  }
  if (eocd == n || eocd + 22 > n) {
    res.error = "truncated .misuc container (missing end record)";
    return res;
  }
  const uint8_t* e = b + eocd;
  uint32_t eventries = get16(e + 10);
  uint32_t cdSize = get32(e + 12);
  uint32_t cdOffset = get32(e + 16);
  if (eventries == 0 || cdSize == 0 || cdOffset == 0 ||
      cdOffset >= n || cdSize > n - cdOffset) {
    res.error = "corrupt .misuc container (bad central directory)";
    return res;
  }
  // Bounds: central records are at least 46 bytes + 1-char names each.
  if (cdSize < eventries * 47u) {
    res.error = "corrupt .misuc container (central directory too small)";
    return res;
  }

  const uint8_t* cd = b + cdOffset;
  const uint8_t* cdEnd = cd + cdSize;
  uint32_t parsed = 0;
  for (const uint8_t* p = cd; p + 46 <= cdEnd; ) {
    if (get32(p) != kCentralSig)
      break;
    const uint32_t method = get16(p + 10);
    const uint32_t crc = get32(p + 16);
    const uint32_t csize = get32(p + 20);
    const uint32_t usize = get32(p + 24);
    const uint16_t nameLen = get16(p + 28);
    const uint16_t extraLen = get16(p + 30);
    const uint32_t lho = get32(p + 42);
    p += 46;
    if (p + nameLen > cdEnd)
      break;
    ContainerEntry entry;
    entry.name.assign(reinterpret_cast<const char*>(p), nameLen);
    p += nameLen + extraLen;

    // Sanity-check the local header before trusting offsets.
    if (lho > n || lho + 30 > n) {
      res.error = "corrupt .misuc container (bad local offset for " + entry.name + ")";
      return res;
    }
    const uint8_t* lh = b + lho;
    if (get32(lh) != kLocalHeaderSig) {
      res.error = "corrupt .misuc container (missing local header for " + entry.name + ")";
      return res;
    }
    const uint16_t lNameLen = get16(lh + 26);
    const uint16_t lExtraLen = get16(lh + 28);
    const size_t dataOff = lho + 30u + lNameLen + lExtraLen;
    if (dataOff > n || csize > n - dataOff) {
      res.error = "corrupt .misuc container (data out of bounds for " + entry.name + ")";
      return res;
    }
    std::string rawData(raw.content.begin() + static_cast<ptrdiff_t>(dataOff),
                        raw.content.begin() + static_cast<ptrdiff_t>(dataOff + csize));
    if (method == kMethodDeflate) {
      if (!zlibInflate(rawData, usize, entry.data, &res.error))
        return res;
    } else if (method == kMethodStore || (method == 0 && usize == csize)) {
      entry.data = std::move(rawData);
    } else {
      res.error = "unsupported compression method in .misuc container";
      return res;
    }

    // Corruption detection: CRC-32 must match the uncompressed payload.
    uint32_t crcCheck = ::crc32(0, Z_NULL, 0);
    if (!entry.data.empty())
      crcCheck = ::crc32(crcCheck,
                         reinterpret_cast<const Bytef*>(entry.data.data()),
                         static_cast<uInt>(entry.data.size()));
    if (crcCheck != crc) {
      res.error = "corrupt .misuc container (CRC mismatch in " + entry.name + ")";
      return res;
    }
    res.entries.push_back(std::move(entry));
    ++parsed;
    if (parsed >= eventries)
      break;
  }
  if (parsed != eventries) {
    res.error = "corrupt .misuc container (central directory truncated)";
    return res;
  }
  res.ok = true;
  return res;
}

} // namespace misuc::core