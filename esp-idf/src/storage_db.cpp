/**
 * storage_db — packed fixed-schema record stores. See storage_db.h for the
 * design; this file is the value-agnostic store engine (no routing, no notify,
 * no browser). storage.cpp wires it into the config-key namespace.
 */
#include "storage_db.h"
#include "fs.h"
#include "log.h"
#include "mem.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"        /* ROM tdefl/tinfl — same gz codec as storage.cpp */
#include "esp_rom_crc.h"  /* esp_rom_crc32_le == zlib crc32 for the gzip footer */

/* ---- File header ---- */

static const uint8_t SDB_MAGIC[4] = { 'S', 'G', 'D', 'B' };
static constexpr uint16_t SDB_FORMAT_VER = 1;
static constexpr size_t   SDB_FILE_HDR   = 16;  /* magic(4)+fmt(2)+id(2)+ver(2)+hdr(2)+count(4) */

/* ---- little-endian scalar access into the block ---- */

static inline uint16_t rdU16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rdU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void     wrU16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static inline void     wrU32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

/* ---- binary gzip (own copies: storage.cpp's are text/NUL-oriented; these
 *      carry explicit lengths so a record block with embedded NULs round-trips) */

#define SDB_GZ_HDR   10
#define SDB_GZ_FOOT  8
#define SDB_GZ_FLAGS (32 | TDEFL_GREEDY_PARSING_FLAG)

static uint8_t* gzDeflateBin(const void* in, size_t inLen, size_t* outLen) {
  size_t cap = SDB_GZ_HDR + SDB_GZ_FOOT + inLen + inLen / 8 + 192;
  uint8_t* out = (uint8_t*)gp_alloc(cap);
  if (!out) return nullptr;
  tdefl_compressor* comp = (tdefl_compressor*)gp_alloc(sizeof(tdefl_compressor));
  if (!comp) { free(out); return nullptr; }
  static const uint8_t hdr[SDB_GZ_HDR] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff };
  memcpy(out, hdr, SDB_GZ_HDR);
  size_t inSz = inLen, outSz = cap - SDB_GZ_HDR - SDB_GZ_FOOT;
  tdefl_init(comp, nullptr, nullptr, SDB_GZ_FLAGS);
  tdefl_status st = tdefl_compress(comp, in, &inSz, out + SDB_GZ_HDR, &outSz, TDEFL_FINISH);
  free(comp);
  if (st != TDEFL_STATUS_DONE || inSz != inLen) { free(out); return nullptr; }
  uint32_t crc = esp_rom_crc32_le(0, (const uint8_t*)in, inLen);
  uint32_t isz = (uint32_t)inLen;
  memcpy(out + SDB_GZ_HDR + outSz,     &crc, 4);
  memcpy(out + SDB_GZ_HDR + outSz + 4, &isz, 4);
  *outLen = SDB_GZ_HDR + outSz + SDB_GZ_FOOT;
  return out;
}

static uint8_t* gzInflateBin(const uint8_t* gz, size_t n, size_t* outLen) {
  if (n < SDB_GZ_HDR + SDB_GZ_FOOT || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8)
    return nullptr;
  uint8_t flg = gz[3];
  size_t off = SDB_GZ_HDR;
  if (flg & 0x04) { if (off + 2 > n) return nullptr; off += 2 + rdU16(gz + off); }
  if (flg & 0x08) { while (off < n && gz[off]) off++; off++; }
  if (flg & 0x10) { while (off < n && gz[off]) off++; off++; }
  if (flg & 0x02) off += 2;
  if (off + SDB_GZ_FOOT > n) return nullptr;
  uint32_t crc = rdU32(gz + n - 8), isz = rdU32(gz + n - 4);
  if (isz > 8 * 1024 * 1024) return nullptr;
  uint8_t* out = (uint8_t*)gp_alloc(isz ? isz : 1);
  if (!out) return nullptr;
  tinfl_decompressor* inf = (tinfl_decompressor*)gp_alloc(sizeof(tinfl_decompressor));
  if (!inf) { free(out); return nullptr; }
  size_t inSz = n - off - SDB_GZ_FOOT, outSz = isz;
  tinfl_init(inf);
  tinfl_status st = tinfl_decompress(inf, gz + off, &inSz, out, out, &outSz,
                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  free(inf);
  if (st != TINFL_STATUS_DONE || outSz != isz ||
      esp_rom_crc32_le(0, out, outSz) != crc) { free(out); return nullptr; }
  *outLen = isz;
  return out;
}

/* ---- hex transcode (SDB_DATA renders/accepts its raw bytes as hex, so the
 *      string-oriented routing layer and browser mirror carry it losslessly) ---- */

static inline int hexNib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static void hexEncodeBytes(const uint8_t* p, size_t n, std::string& out) {
  static const char* H = "0123456789abcdef";
  out.clear(); out.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { out.push_back(H[p[i] >> 4]); out.push_back(H[p[i] & 0xf]); }
}
/* Decode up to maxOut bytes from a hex string into out; returns bytes written.
 * Stops at a non-hex char or an odd trailing nibble. */
static size_t hexDecodeInto(const char* s, uint8_t* out, size_t maxOut) {
  size_t n = 0;
  while (s[0] && s[1] && n < maxOut) {
    int hi = hexNib(s[0]), lo = hexNib(s[1]);
    if (hi < 0 || lo < 0) break;
    out[n++] = (uint8_t)((hi << 4) | lo);
    s += 2;
  }
  return n;
}
static bool isAllZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i]) return false;
  return true;
}

/* ---- schema builders ---- */

sdb_schema& sdb_schema::u8(const char* name) {
  fields.push_back({ name, SDB_U8, hdr_size, 0 }); hdr_size += 1; return *this;
}
sdb_schema& sdb_schema::u32(const char* name) {
  /* keep u32 4-byte aligned within the header */
  if (hdr_size & 3) hdr_size = (hdr_size + 3) & ~3u;
  fields.push_back({ name, SDB_U32, hdr_size, 0 }); hdr_size += 4; return *this;
}
sdb_schema& sdb_schema::fixstr(const char* name, uint16_t width) {
  fields.push_back({ name, SDB_FIXSTR, hdr_size, width }); hdr_size += width; return *this;
}
sdb_schema& sdb_schema::data(const char* name, uint16_t nbytes) {
  fields.push_back({ name, SDB_DATA, hdr_size, nbytes }); hdr_size += nbytes; return *this;
}
sdb_schema& sdb_schema::text(const char* name) {
  fields.push_back({ name, SDB_TEXT, textCount(), 0 }); return *this;
}
const sdb_field* sdb_schema::find(const char* name) const {
  for (auto& f : fields) if (f.name == name) return &f;
  return nullptr;
}
uint16_t sdb_schema::textCount() const {
  uint16_t n = 0;
  for (auto& f : fields) if (f.kind == SDB_TEXT) n++;
  return n;
}

/* ---- block / record helpers ---- */

static void writeFileHeader(sdb_store* s, uint32_t recCount) {
  uint8_t* b = s->block;
  memcpy(b, SDB_MAGIC, 4);
  wrU16(b + 4,  SDB_FORMAT_VER);
  wrU16(b + 6,  s->schema->schema_id);
  wrU16(b + 8,  s->schema->schema_ver);
  wrU16(b + 10, s->schema->hdr_size);
  wrU32(b + 12, recCount);
}

static bool ensureCap(sdb_store* s, size_t need) {
  if (need <= s->cap) { s->grow_failed = false; return true; }
  size_t ncap = s->cap ? s->cap : 256;
  while (ncap < need) ncap *= 2;
  uint8_t* nb = (uint8_t*)gp_realloc(s->block, ncap);
  if (!nb) {
    /* Doubling asks for a large contiguous run a fragmented heap may lack even
     * with ample total free; retry at just what's needed (page-rounded) before
     * declaring failure. Sacrifices amortized-O(1) growth only under pressure. */
    ncap = (need + 4095) & ~size_t(4095);
    nb = (uint8_t*)gp_realloc(s->block, ncap);
  }
  if (!nb) {
    if (!s->grow_failed) {   /* one warning per failed episode, with the fragmentation signal */
      s->grow_failed = true;
      warn("storage_db[%s]: grow %u->%u B failed (heap free %u, largest block %u)\n",
           s->path.empty() ? "(ram)" : s->path.c_str(),
           (unsigned)s->cap, (unsigned)need,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
    return false;
  }
  s->block = nb;
  s->cap = ncap;
  s->grow_failed = false;
  return true;
}

/* Walk the resident block and (re)build the key->offset index over live records.
 * Returns false on a structurally corrupt record (caller resets to empty). */
static bool rebuildIndex(sdb_store* s) {
  s->index.clear();
  uint16_t hs = s->schema->hdr_size;
  size_t off = SDB_FILE_HDR;
  while (off < s->used) {
    if (off + hs > s->used) return false;
    uint32_t rlen = rdU32(s->block + off + SDB_REC_LEN_OFF);
    if (rlen < hs || off + rlen > s->used) return false;
    uint8_t flags = s->block[off + SDB_FLAGS_OFF];
    if (!(flags & SDB_FLAG_DELETED)) {
      const uint8_t* tp = s->block + off + hs;
      uint16_t klen = rdU16(tp);
      if (hs + 2u + klen > rlen) return false;
      s->index[std::string((const char*)tp + 2, klen)] = (uint32_t)off;
    }
    off += rlen;
  }
  return true;
}

/* Locate a live record's byte offset; returns SIZE_MAX if absent. */
static size_t findRec(const sdb_store* s, const char* key) {
  auto it = s->index.find(key);
  return it == s->index.end() ? SIZE_MAX : it->second;
}

/* Read the key and every text field of the record at `off` into out params.
 * `texts` is sized to the schema's text-field count. */
static void readRecordText(const sdb_store* s, size_t off,
                           std::string& key, std::vector<std::string>& texts) {
  uint16_t hs = s->schema->hdr_size;
  const uint8_t* p = s->block + off + hs;
  uint16_t klen = rdU16(p); p += 2;
  key.assign((const char*)p, klen); p += klen;
  uint16_t nt = s->schema->textCount();
  texts.assign(nt, std::string());
  for (uint16_t i = 0; i < nt; i++) {
    uint16_t l = rdU16(p); p += 2;
    texts[i].assign((const char*)p, l); p += l;
  }
}

/* Build a record image: [fixed hdr copy][key][text fields], rec_len fixed up. */
static std::string buildRecord(const sdb_store* s, const uint8_t* hdrCopy,
                               const std::string& key,
                               const std::vector<std::string>& texts) {
  uint16_t hs = s->schema->hdr_size;
  std::string r;
  r.append((const char*)hdrCopy, hs);
  uint8_t lb[2];
  wrU16(lb, (uint16_t)key.size()); r.append((const char*)lb, 2); r.append(key);
  for (auto& t : texts) { wrU16(lb, (uint16_t)t.size()); r.append((const char*)lb, 2); r.append(t); }
  wrU32((uint8_t*)&r[0] + SDB_REC_LEN_OFF, (uint32_t)r.size());
  r[SDB_FLAGS_OFF] = (char)(hdrCopy[SDB_FLAGS_OFF]);
  return r;
}

/* Scratch buffer for in-place compaction — batches live records so we flush a
 * few large copies instead of thousands of per-record moves. Static (store
 * mutations are serialized under the storage lock, single-threaded), and
 * PSRAM-resident, so the OOM reclaim path needs no heap and no scarce DRAM. */
static constexpr size_t SDB_COMPACT_SCRATCH = 8192;
static uint8_t PSRAM_BSS sdbScratch[SDB_COMPACT_SCRATCH];

/* Repoint the live index entry for the record whose image starts at `recPtr` to
 * `newOff`. Every live key already owns a map node (tombstoned keys are erased at
 * tombstone time), so this only overwrites an offset — no node allocation. */
static inline void sdbReindexRec(sdb_store* s, const uint8_t* recPtr, size_t newOff) {
  const uint8_t* tp = recPtr + s->schema->hdr_size;
  uint16_t klen = rdU16(tp);
  s->index[std::string((const char*)tp + 2, klen)] = (uint32_t)newOff;
}

/* Compact the resident block in place, dropping tombstones. Live records stream
 * through sdbScratch and flush back to the arena write cursor in big batches; the
 * write cursor always trails the read cursor (we only remove bytes) so a flush
 * never clobbers unread records. Allocates nothing large. Rebuilds no index —
 * offsets are patched as records move. */
static void sdbCompactInPlace(sdb_store* s) {
  uint16_t hs = s->schema->hdr_size;
  size_t w = SDB_FILE_HDR;    /* destination (write) cursor */
  size_t off = SDB_FILE_HDR;  /* source (read) cursor */
  size_t sc = 0;              /* live bytes currently buffered in sdbScratch */
  uint32_t kept = 0;
  while (off < s->used) {
    uint32_t rlen = rdU32(s->block + off + SDB_REC_LEN_OFF);
    if (rlen < hs || off + rlen > s->used) break;
    if (!(s->block[off + SDB_FLAGS_OFF] & SDB_FLAG_DELETED)) {
      if (rlen > SDB_COMPACT_SCRATCH) {                 /* too big to batch: move direct */
        if (sc) { memcpy(s->block + w, sdbScratch, sc); w += sc; sc = 0; }
        sdbReindexRec(s, s->block + off, w);
        memmove(s->block + w, s->block + off, rlen);
        w += rlen; kept++;
      } else {
        if (sc + rlen > SDB_COMPACT_SCRATCH) { memcpy(s->block + w, sdbScratch, sc); w += sc; sc = 0; }
        sdbReindexRec(s, s->block + off, w + sc);       /* final resting offset once flushed */
        memcpy(sdbScratch + sc, s->block + off, rlen);
        sc += rlen; kept++;
      }
    }
    off += rlen;
  }
  if (sc) { memcpy(s->block + w, sdbScratch, sc); w += sc; }
  s->used = w;
  writeFileHeader(s, kept);
}

/* OOM reclaim for an append that neither fit nor could grow the arena. One scan
 * sizes the live set, then: if tombstones are scarce (live still ~fills the
 * block) relocate the live records into a fresh, 20%-larger contiguous block — a
 * new allocation can land where an in-place grow could not; otherwise compact in
 * place (cheap, no large allocation). Returns true if `recSize` more bytes now
 * fit within s->cap. */
static bool reclaimForAppend(sdb_store* s, size_t recSize) {
  uint16_t hs = s->schema->hdr_size;
  const char* label = s->path.empty() ? "(ram)" : s->path.c_str();
  size_t oldUsed = s->used;
  size_t liveBytes = SDB_FILE_HDR;
  size_t off = SDB_FILE_HDR;
  while (off < s->used) {
    uint32_t rlen = rdU32(s->block + off + SDB_REC_LEN_OFF);
    if (rlen < hs || off + rlen > s->used) break;
    if (!(s->block[off + SDB_FLAGS_OFF] & SDB_FLAG_DELETED)) liveBytes += rlen;
    off += rlen;
  }
  size_t target = liveBytes + recSize;

  if (liveBytes * 10 >= s->cap * 9) {   /* < ~10% reclaimable: relocate rather than shuffle */
    size_t newcap = liveBytes + liveBytes / 5;      /* +20% headroom */
    if (newcap < target) newcap = target;
    newcap = (newcap + 4095) & ~size_t(4095);
    uint8_t* nb = (uint8_t*)gp_alloc(newcap);
    if (nb) {
      memcpy(nb, s->block, SDB_FILE_HDR);
      size_t w = SDB_FILE_HDR, o = SDB_FILE_HDR;
      uint32_t kept = 0;
      while (o < s->used) {
        uint32_t rlen = rdU32(s->block + o + SDB_REC_LEN_OFF);
        if (rlen < hs || o + rlen > s->used) break;
        if (!(s->block[o + SDB_FLAGS_OFF] & SDB_FLAG_DELETED)) {
          memcpy(nb + w, s->block + o, rlen);
          sdbReindexRec(s, nb + w, w);
          w += rlen; kept++;
        }
        o += rlen;
      }
      gp_free(s->block);
      s->block = nb; s->cap = newcap; s->used = w;
      writeFileHeader(s, kept);
      s->grow_failed = false;
      dbg("storage_db[%s]: relocate compact %u->%u B, cap ->%u\n",
          label, (unsigned)oldUsed, (unsigned)s->used, (unsigned)s->cap);
      return s->used + recSize <= s->cap;
    }
    /* the larger block didn't fit either — reclaim what we can in place */
  }

  sdbCompactInPlace(s);
  dbg("storage_db[%s]: inplace compact %u->%u B, cap %u\n",
      label, (unsigned)oldUsed, (unsigned)s->used, (unsigned)s->cap);
  if (s->used + recSize <= s->cap) { s->grow_failed = false; return true; }
  return false;
}

/* Append a fully-built record image, indexing it by `key`. */
static bool appendRecord(sdb_store* s, const std::string& key, const std::string& rec) {
  if (!ensureCap(s, s->used + rec.size())) {
    /* Grow failed on a fragmented heap: reclaim (drop tombstones, or relocate the
     * live set into a fresh block) and retry without a big in-place grow. */
    if (!reclaimForAppend(s, rec.size())) return false;
  }
  memcpy(s->block + s->used, rec.data(), rec.size());
  s->index[key] = (uint32_t)s->used;
  s->used += rec.size();
  return true;
}

/* Rewrite the block keeping only live records (arena order), optionally dropping
 * the oldest `dropOldest` of them (ephemeral cap). Rebuilds the index. */
static void compact(sdb_store* s, size_t dropOldest) {
  uint16_t hs = s->schema->hdr_size;
  /* Collect live record offsets in arena order. */
  std::vector<uint32_t> live;
  size_t off = SDB_FILE_HDR;
  while (off < s->used) {
    uint32_t rlen = rdU32(s->block + off + SDB_REC_LEN_OFF);
    if (rlen < hs || off + rlen > s->used) break;
    if (!(s->block[off + SDB_FLAGS_OFF] & SDB_FLAG_DELETED)) live.push_back((uint32_t)off);
    off += rlen;
  }
  size_t start = dropOldest < live.size() ? dropOldest : live.size();
  /* Build the new block in a temp buffer, then swap in. */
  std::string nb;
  nb.resize(SDB_FILE_HDR, '\0');
  uint32_t kept = 0;
  for (size_t i = start; i < live.size(); i++) {
    uint32_t o = live[i];
    uint32_t rlen = rdU32(s->block + o + SDB_REC_LEN_OFF);
    nb.append((const char*)s->block + o, rlen);
    kept++;
  }
  if (!ensureCap(s, nb.size())) return;
  size_t oldUsed = s->used;
  memcpy(s->block, nb.data(), nb.size());
  s->used = nb.size();
  writeFileHeader(s, kept);
  rebuildIndex(s);
  /* drop-oldest fires on every insert past a full ephemeral cap — verbose;
   * the flush/serialize compaction (dropOldest==0) is occasional — debug. */
  const char* label = s->path.empty() ? "(ram)" : s->path.c_str();
  if (dropOldest)
    verb("storage_db[%s]: cap compact %u->%u B, dropped %u, kept %u\n",
         label, (unsigned)oldUsed, (unsigned)s->used, (unsigned)dropOldest, kept);
  else
    dbg("storage_db[%s]: flush compact %u->%u B, kept %u\n",
        label, (unsigned)oldUsed, (unsigned)s->used, kept);
}

/* ---- lifecycle ---- */

void sdbInitEmpty(sdb_store* s) {
  if (s->loaded) return;
  s->index.clear();
  if (!ensureCap(s, SDB_FILE_HDR)) return;
  s->used = SDB_FILE_HDR;
  writeFileHeader(s, 0);
  s->loaded = true;
  s->dirty = false;
}

bool sdbPeekHeader(const char* path, uint16_t* schema_id,
                   uint16_t* schema_ver, uint16_t* hdr_size) {
  if (!path) return false;
  struct stat st;
  if (fs_stat(path, &st) != 0 || st.st_size <= 0) return false;
  int f = fs_open(path, "rb");
  if (f < 0) return false;
  uint8_t* raw = (uint8_t*)gp_alloc(st.st_size);
  if (!raw) { fs_close(f); return false; }
  size_t got = fs_read(raw, 1, st.st_size, f);
  fs_close(f);
  size_t inflated = 0;
  uint8_t* blk = gzInflateBin(raw, got, &inflated);
  free(raw);
  if (!blk) return false;
  bool ok = inflated >= SDB_FILE_HDR && memcmp(blk, SDB_MAGIC, 4) == 0;
  if (ok) {
    if (schema_id)  *schema_id  = rdU16(blk + 6);
    if (schema_ver) *schema_ver = rdU16(blk + 8);
    if (hdr_size)   *hdr_size   = rdU16(blk + 10);
  }
  free(blk);
  return ok;
}

bool sdbLoad(sdb_store* s) {
  if (s->loaded) return true;
  if (s->path.empty()) { sdbInitEmpty(s); return true; }

  std::string gzPath = s->path;   /* path already names the .db.gz file */
  struct stat st;
  if (fs_stat(gzPath.c_str(), &st) != 0 || st.st_size <= 0) { sdbInitEmpty(s); return true; }
  int f = fs_open(gzPath.c_str(), "rb");
  if (f < 0) { sdbInitEmpty(s); return true; }
  uint8_t* raw = (uint8_t*)gp_alloc(st.st_size);
  if (!raw) { fs_close(f); sdbInitEmpty(s); return false; }
  size_t got = fs_read(raw, 1, st.st_size, f);
  fs_close(f);

  size_t inflated = 0;
  uint8_t* blk = gzInflateBin(raw, got, &inflated);
  free(raw);
  if (!blk) { warn("storage_db: corrupt %s, starting empty\n", gzPath.c_str()); sdbInitEmpty(s); return false; }

  /* Validate the file header against this schema. Version mismatch is tolerated
   * best-effort only when hdr_size matches (future: per-version decoders). */
  bool ok = inflated >= SDB_FILE_HDR && memcmp(blk, SDB_MAGIC, 4) == 0 &&
            rdU16(blk + 6)  == s->schema->schema_id &&
            rdU16(blk + 10) == s->schema->hdr_size;
  if (!ok) { free(blk); warn("storage_db: header mismatch %s, starting empty\n", gzPath.c_str()); sdbInitEmpty(s); return false; }

  free(s->block);
  s->block = blk;
  s->cap = inflated;
  s->used = inflated;
  s->loaded = true;
  s->dirty = false;
  if (!rebuildIndex(s)) {
    warn("storage_db: bad records in %s, starting empty\n", gzPath.c_str());
    free(s->block); s->block = nullptr; s->cap = 0; s->used = 0; s->loaded = false;
    sdbInitEmpty(s);
    return false;
  }
  return true;
}

void sdbEvict(sdb_store* s) {
  free(s->block);
  s->block = nullptr;
  s->cap = 0;
  s->used = 0;
  s->loaded = false;
  s->dirty = false;
  s->index.clear();
}

bool sdbFlush(sdb_store* s) {
  if (!s->loaded || s->path.empty()) { s->dirty = false; return true; }
  compact(s, 0);   /* drop tombstones; the flush IS the compaction */

  size_t gzLen = 0;
  uint8_t* gz = gzDeflateBin(s->block, s->used, &gzLen);
  if (!gz) { warn("storage_db: deflate %s failed\n", s->path.c_str()); return false; }

  std::string tmp = s->path + ".new";
  int f = fs_open(tmp.c_str(), "w");
  if (f < 0) { free(gz); return false; }
  bool ok = true;
  for (size_t o = 0; o < gzLen; ) {
    size_t n = gzLen - o; if (n > 8192) n = 8192;   /* chunk: keep PSRAM cache windows short */
    if (fs_write((const char*)gz + o, 1, n, f) != n) { ok = false; break; }
    o += n; if (o < gzLen) vTaskDelay(1);
  }
  fs_close(f);
  free(gz);
  if (!ok) { fs_remove(tmp.c_str()); return false; }
  fs_rename(tmp.c_str(), s->path.c_str());
  s->dirty = false;
  return true;
}

/* ---- record access ---- */

bool sdbHasRecord(const sdb_store* s, const char* key) {
  return s->loaded && s->index.count(key) != 0;
}

size_t sdbRecordCount(const sdb_store* s) {
  return s->index.size();
}

bool sdbGetField(sdb_store* s, const char* key, const char* field, std::string& out) {
  if (!s->loaded) return false;
  size_t off = findRec(s, key);
  if (off == SIZE_MAX) return false;
  const sdb_field* fd = s->schema->find(field);
  if (!fd) return false;
  const uint8_t* h = s->block + off;
  char buf[16];
  switch (fd->kind) {
    case SDB_U8:  snprintf(buf, sizeof(buf), "%u", (unsigned)h[fd->off]); out = buf; return true;
    case SDB_U32: snprintf(buf, sizeof(buf), "%u", (unsigned)rdU32(h + fd->off)); out = buf; return true;
    case SDB_FIXSTR: {
      const char* p = (const char*)h + fd->off;
      size_t l = strnlen(p, fd->width);
      out.assign(p, l);
      return true;
    }
    case SDB_DATA: {
      const uint8_t* p = h + fd->off;
      if (isAllZero(p, fd->width)) { out.clear(); return true; }   /* unset */
      hexEncodeBytes(p, fd->width, out);
      return true;
    }
    case SDB_TEXT: {
      std::string k; std::vector<std::string> texts;
      readRecordText(s, off, k, texts);
      if (fd->off < texts.size()) { out = texts[fd->off]; return true; }
      return false;
    }
  }
  return false;
}

bool sdbGetFieldBin(sdb_store* s, const char* key, const char* field, void* out, size_t* len) {
  if (!len) return false;
  size_t cap = *len; *len = 0;
  if (!s->loaded) return false;
  size_t off = findRec(s, key);
  if (off == SIZE_MAX) return false;
  const sdb_field* fd = s->schema->find(field);
  if (!fd || fd->kind != SDB_DATA) return false;
  const uint8_t* p = s->block + off + fd->off;
  if (isAllZero(p, fd->width)) return false;          /* unset sentinel */
  if (fd->width > cap) { *len = fd->width; return false; }  /* buffer too small */
  memcpy(out, p, fd->width);
  *len = fd->width;
  return true;
}

void sdbSetField(sdb_store* s, const char* key, const char* field, const char* val) {
  if (!s->loaded) return;
  const sdb_field* fd = s->schema->find(field);
  if (!fd) { warn("storage_db: unknown field '%s'\n", field); return; }

  size_t off = findRec(s, key);
  if (off == SIZE_MAX) {
    /* Create an empty record: zeroed fixed header, empty key+text. */
    uint16_t hs = s->schema->hdr_size;
    std::vector<uint8_t> hdr(hs, 0);
    std::vector<std::string> texts(s->schema->textCount());
    std::string rec = buildRecord(s, hdr.data(), key, texts);
    if (!appendRecord(s, key, rec)) return;
    off = findRec(s, key);
    if (off == SIZE_MAX) return;
    /* Enforce an ephemeral cap by dropping the oldest live records. */
    if (s->cap_records && s->index.size() > s->cap_records) {
      compact(s, s->index.size() - s->cap_records);
      off = findRec(s, key);
      if (off == SIZE_MAX) { s->dirty = true; return; }
    }
  }

  uint8_t* h = s->block + off;
  switch (fd->kind) {
    case SDB_U8:  h[fd->off] = (uint8_t)atoi(val); break;
    case SDB_U32: wrU32(h + fd->off, (uint32_t)strtoul(val, nullptr, 10)); break;
    case SDB_FIXSTR: {
      size_t l = strlen(val);
      if (l >= fd->width) l = fd->width - 1;
      memset(h + fd->off, 0, fd->width);
      memcpy(h + fd->off, val, l);
      break;
    }
    case SDB_DATA: {
      /* val is 2N hex; anything else (incl. "") clears the field to the unset
       * sentinel. Mutated in place — it lives in the fixed header. */
      memset(h + fd->off, 0, fd->width);
      if (strlen(val) == (size_t)fd->width * 2) hexDecodeInto(val, h + fd->off, fd->width);
      break;
    }
    case SDB_TEXT: {
      std::string k; std::vector<std::string> texts;
      readRecordText(s, off, k, texts);
      if (fd->off < texts.size() && texts[fd->off] == val) break;   /* no change */
      if (fd->off < texts.size()) texts[fd->off] = val;
      /* Rebuild: copy the fixed header (in-place fields preserved), reserialize.
       * If the size is unchanged we overwrite in place; else tombstone + append. */
      std::vector<uint8_t> hdr(s->schema->hdr_size);
      memcpy(hdr.data(), h, s->schema->hdr_size);
      std::string rec = buildRecord(s, hdr.data(), k, texts);
      if (rec.size() == rdU32(h + SDB_REC_LEN_OFF)) {
        memcpy(h, rec.data(), rec.size());
      } else {
        s->block[off + SDB_FLAGS_OFF] |= SDB_FLAG_DELETED;   /* tombstone old */
        s->index.erase(k);
        if (!appendRecord(s, k, rec)) { s->dirty = true; return; }
      }
      break;
    }
  }
  s->dirty = true;
}

void sdbDeleteRecord(sdb_store* s, const char* key) {
  if (!s->loaded) return;
  size_t off = findRec(s, key);
  if (off == SIZE_MAX) return;
  s->block[off + SDB_FLAGS_OFF] |= SDB_FLAG_DELETED;
  s->index.erase(key);
  s->dirty = true;
}

void sdbForEach(sdb_store* s,
                void (*cb)(const char* key, const char* field, const char* val, void* ctx),
                void* ctx) {
  if (!s->loaded) return;
  uint16_t hs = s->schema->hdr_size;
  size_t off = SDB_FILE_HDR;
  char buf[16];
  while (off < s->used) {
    uint32_t rlen = rdU32(s->block + off + SDB_REC_LEN_OFF);
    if (rlen < hs || off + rlen > s->used) break;
    if (!(s->block[off + SDB_FLAGS_OFF] & SDB_FLAG_DELETED)) {
      std::string key; std::vector<std::string> texts;
      readRecordText(s, off, key, texts);
      const uint8_t* h = s->block + off;
      for (auto& fd : s->schema->fields) {
        switch (fd.kind) {
          case SDB_U8:  snprintf(buf, sizeof(buf), "%u", (unsigned)h[fd.off]); cb(key.c_str(), fd.name.c_str(), buf, ctx); break;
          case SDB_U32: snprintf(buf, sizeof(buf), "%u", (unsigned)rdU32(h + fd.off)); cb(key.c_str(), fd.name.c_str(), buf, ctx); break;
          case SDB_FIXSTR: {
            const char* p = (const char*)h + fd.off;
            size_t l = strnlen(p, fd.width);
            if (l) cb(key.c_str(), fd.name.c_str(), std::string(p, l).c_str(), ctx);
            break;
          }
          case SDB_DATA: {
            const uint8_t* p = h + fd.off;
            if (!isAllZero(p, fd.width)) {
              std::string hx; hexEncodeBytes(p, fd.width, hx);
              cb(key.c_str(), fd.name.c_str(), hx.c_str(), ctx);
            }
            break;
          }
          case SDB_TEXT:
            if (fd.off < texts.size() && !texts[fd.off].empty())
              cb(key.c_str(), fd.name.c_str(), texts[fd.off].c_str(), ctx);
            break;
        }
      }
    }
    off += rlen;
  }
}

uint8_t* sdbSerializeGz(sdb_store* s, size_t* outLen) {
  if (!s->loaded) return nullptr;
  compact(s, 0);
  return gzDeflateBin(s->block, s->used, outLen);
}

uint8_t* sdbSnapshotRaw(sdb_store* s, size_t* outLen) {
  if (!s->loaded) return nullptr;
  compact(s, 0);
  uint8_t* copy = (uint8_t*)gp_alloc(s->used ? s->used : 1);
  if (!copy) return nullptr;
  memcpy(copy, s->block, s->used);
  *outLen = s->used;
  return copy;
}

uint8_t* sdbGzDeflate(const void* in, size_t inLen, size_t* outLen) {
  return gzDeflateBin(in, inLen, outLen);
}
