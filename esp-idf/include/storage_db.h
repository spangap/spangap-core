/**
 * storage_db — packed fixed-schema record stores, outside the cJSON config tree.
 *
 * The config store (storage.cpp) is a cJSON DOM: right for the small,
 * heterogeneous config/state tree, wrong as the database for large homogeneous
 * collections (LXMF message history, announce catalogues, nomad page bodies).
 * Those move here: packed records in a contiguous PSRAM arena with a per-store
 * key->offset index, not a graph of tiny per-node cJSON allocations.
 *
 * This module is the STORE PRIMITIVE only — value-agnostic, addressing-agnostic,
 * and it never talks to the browser or the notify bus. storage.cpp owns the
 * routing (which dot-path keys map to which store), the change-event synthesis,
 * and the registry (storage.db) that carries schemas to the browser. Keeping the
 * two apart is what lets the same records back a durable+paged collection
 * (messages) and an ephemeral+capped one (announces) with two knobs.
 *
 * Representation (RAM block == on-disk block, the disk copy just gzip-wrapped):
 *
 *   [file header 16 B] [record] [record] ...
 *
 *   file header: 'SGDB' magic, u16 format_ver, u16 schema_id, u16 schema_ver,
 *                u16 hdr_size, u32 record_count
 *   record:      [u32 rec_len][u8 flags][fixed fields...] [key][text fields...]
 *                fixed fields are overwritten IN PLACE (the mutable ones);
 *                the key and text fields are length-prefixed and immutable once
 *                written (a changed text field rebuilds the record).
 *
 * The load-bearing rule (see plans/storage-structured-db.md): fields that change
 * are fixed-width so they mutate in place with no reindex/realloc; variable text
 * is immutable. A tombstone bit marks a deleted record; the next flush drops
 * tombstoned records (the flush IS the compaction) and rebuilds the index.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <unordered_map>

/* ---- Schema description ---- */

enum sdb_kind : uint8_t {
  SDB_U8     = 1,  /* fixed 1 B, mutable in place (small counters / flags) */
  SDB_U32    = 2,  /* fixed 4 B, mutable in place (timestamps) */
  SDB_FIXSTR = 3,  /* fixed N B NUL-padded string, mutable in place (enum-like
                    * strings: stage/method/dir, plus the coarse last_error) */
  SDB_TEXT   = 4,  /* variable length, length-prefixed, IMMUTABLE once written
                    * (title/content/thread) */
};

/* One field in a record schema. `name` must contain no '.' (the routing layer
 * splits a key tail on '.' into <record>.<field>). */
struct sdb_field {
  std::string name;
  sdb_kind    kind;
  uint16_t    off;    /* SDB_U8/U32/FIXSTR: byte offset within the fixed header.
                       * SDB_TEXT: the field's order index among text fields. */
  uint16_t    width;  /* SDB_FIXSTR only: total slot bytes (incl. the NUL room). */
};

/* Built-in header layout, present in every schema regardless of fields:
 *   [0] u32 rec_len   — total record bytes (header + key + text); lets the
 *                       loader walk records to rebuild the index and skip tombstones
 *   [4] u8  flags     — bit0 = deleted (tombstone; RAM-only, never persisted)
 * User fixed fields are declared at offsets >= SDB_HDR_BUILTIN. */
static constexpr uint16_t SDB_REC_LEN_OFF  = 0;
static constexpr uint16_t SDB_FLAGS_OFF    = 4;
static constexpr uint8_t  SDB_FLAG_DELETED = 0x01;
static constexpr uint16_t SDB_HDR_BUILTIN  = 8;   /* rec_len(4)+flags(1)+pad(3) */

struct sdb_schema {
  uint16_t schema_id  = 0;
  uint16_t schema_ver = 0;
  uint16_t hdr_size   = SDB_HDR_BUILTIN;  /* fixed header total; >= SDB_HDR_BUILTIN */
  std::vector<sdb_field> fields;

  /* Builder helpers — append a field and grow hdr_size for fixed kinds. */
  sdb_schema& u8(const char* name);
  sdb_schema& u32(const char* name);
  sdb_schema& fixstr(const char* name, uint16_t width);
  sdb_schema& text(const char* name);

  const sdb_field* find(const char* name) const;
  uint16_t textCount() const;
};

/* ---- Store instance ---- */

struct sdb_store {
  const sdb_schema* schema = nullptr;
  std::string path;              /* on-disk .db.gz path; empty => RAM-only (ephemeral) */
  uint32_t    cap_records = 0;   /* 0 = unbounded; else DROP oldest past this (ephemeral cap) */

  uint8_t* block = nullptr;      /* gp_alloc arena: [file hdr][records...] */
  size_t   used  = 0;            /* bytes in use (>= SDB file-header size once inited) */
  size_t   cap   = 0;            /* allocated capacity of `block` */
  bool     loaded = false;       /* file decompressed into RAM (or RAM-only inited) */
  bool     dirty  = false;       /* needs flush */

  std::unordered_map<std::string, uint32_t> index;  /* record key -> byte offset in block */
};

/* ---- Instance lifecycle ---- */

/* Prepare an empty resident block (file header written, no records). Idempotent:
 * a no-op if already loaded. RAM-only stores call this instead of a load. */
void sdbInitEmpty(sdb_store* s);

/* Load `s->path` into RAM (gunzip + validate header + rebuild index). On a
 * missing file, inits empty. On a corrupt/incompatible file, inits empty and
 * returns false. Safe to call repeatedly (no-op if already loaded). */
bool sdbLoad(sdb_store* s);

/* Free the resident block and index (write-back is the caller's job — check
 * s->dirty and sdbFlush first). After this the store is not loaded. */
void sdbEvict(sdb_store* s);

/* Rewrite the whole gz file from the resident block, dropping tombstoned records
 * (compaction) and rebuilding the index. No-op for RAM-only stores. Clears
 * s->dirty on success. Returns false on I/O failure (stays dirty). */
bool sdbFlush(sdb_store* s);

/* ---- Record access (value as string, to mirror storageSet/GetStr) ---- */

/* True if a live (non-tombstoned) record with this key exists. */
bool sdbHasRecord(const sdb_store* s, const char* key);

/* Read one field of one record into `out`. Returns false if the record or field
 * is absent (out is left untouched). Numeric fields render as decimal. */
bool sdbGetField(sdb_store* s, const char* key, const char* field, std::string& out);

/* Set one field of one record. Creates the record (with defaulted fields) if the
 * key is new. Fixed fields mutate in place; a text field whose value changes
 * length rebuilds the record (rare — text is write-once for messages). Marks the
 * store dirty. Enforces the ephemeral cap (drops oldest) when cap_records > 0. */
void sdbSetField(sdb_store* s, const char* key, const char* field, const char* val);

/* Tombstone a record (RAM-only; dropped at the next flush). Marks dirty. */
void sdbDeleteRecord(sdb_store* s, const char* key);

/* Number of live (non-tombstoned) records. */
size_t sdbRecordCount(const sdb_store* s);

/* Iterate every field of every live record, oldest-first (arena order), calling
 * cb(recordKey, fieldName, valueStr, ctx) for each non-empty field. Mirrors
 * storageForEach over a subtree. */
void sdbForEach(sdb_store* s,
                void (*cb)(const char* key, const char* field, const char* val, void* ctx),
                void* ctx);

/* Serialize the resident block to a freshly gp_alloc'd gz buffer (the exact
 * bytes of the .db.gz file), for shipping a resident conversation to the
 * browser. Caller frees with free(). NULL on failure. */
uint8_t* sdbSerializeGz(sdb_store* s, size_t* outLen);

/* Compact (drop tombstones) and return a freshly gp_alloc'd copy of the raw
 * uncompressed block. Cheap enough to run under the config lock; the caller then
 * deflates + writes it lock-free (the writeExternalFile discipline — never hold
 * the actor's lock across a deflate/flash write). Caller frees with free(). */
uint8_t* sdbSnapshotRaw(sdb_store* s, size_t* outLen);

/* The binary gz codec, exposed so the flush path can deflate a snapshot taken
 * under lock without re-entering the store. Caller frees with free(). */
uint8_t* sdbGzDeflate(const void* in, size_t inLen, size_t* outLen);
