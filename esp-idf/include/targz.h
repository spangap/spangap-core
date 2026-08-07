/**
 * targz — streaming .tar.gz, in both directions, with nothing resident.
 *
 * Written for safe-mode backup and restore, where the archive exists only as
 * bytes passing through: there is no room on the device to hold one, so neither
 * side may buffer more than a window. The writer takes entries and body bytes
 * and pushes compressed output at a sink as it is produced; the reader takes
 * compressed input in arbitrary chunks and calls back per entry and per data
 * run as it inflates. Neither knows what the other end is — a socket, a file, a
 * test buffer.
 *
 * Deliberately narrow dependencies (miniz + mem.h + the ROM CRC32): no fs, no
 * storage, no ITS. Whoever drives it owns the directory walk and the file I/O.
 *
 * FORMAT. Plain ustar inside gzip. Two entry types only, regular file ('0') and
 * directory ('5'); the reader REJECTS everything else — links, devices, FIFOs —
 * along with absolute paths, `..` components, and names it cannot hold. An
 * archive is attacker-supplied input written straight onto a filesystem, so the
 * checks are in the reader, not in the caller.
 *
 * INTEGRITY. The gzip footer's CRC32/ISIZE is checked by the reader when the
 * stream ends, which is a CONTENT check and lands after the last byte has been
 * written out — inherent to streaming without staging. It is not the truncation
 * detector: that is the transport's job (HTTP chunked framing on the way out, a
 * short read on the way in), and the two catch different failures.
 */
#ifndef SPANGAP_TARGZ_H
#define SPANGAP_TARGZ_H

#include <cstddef>
#include <cstdint>

/* ---- Writer: entries in, gzip bytes out ---- */

/** Sink for compressed output. Called with each produced run, in order.
 *  Return false to abort the archive (e.g. the client went away); the writer
 *  then fails every later call and targzWriterClose() returns false. */
typedef bool (*targz_sink_fn)(void* ctx, const uint8_t* data, size_t len);

struct targz_writer_t;

/** Start an archive. Allocates the deflate state (~160 KB) plus a small output
 *  buffer; returns nullptr if either allocation fails. */
targz_writer_t* targzWriterOpen(targz_sink_fn sink, void* ctx);

/** Add a directory entry. `name` is stored verbatim and must be relative. */
bool targzWriterAddDir(targz_writer_t* w, const char* name, uint32_t mtime);

/** Begin a regular file of exactly `size` bytes. Follow with targzWriterData()
 *  calls totalling `size`; the trailing 512-block padding is written for you.
 *  A short body is zero-filled to the declared size when the next entry starts
 *  or the archive closes, so the tar never desynchronises — the file is wrong,
 *  but only that file.
 *
 *  A name ustar cannot represent (over 100 bytes, with no '/' leaving a split
 *  into a 155-byte prefix and a 100-byte tail) is SKIPPED with a warning rather
 *  than truncated — an extractor would believe a truncated path. */
bool targzWriterAddFile(targz_writer_t* w, const char* name, uint32_t size,
                        uint32_t mtime);

/** Body bytes for the entry opened by targzWriterAddFile(). */
bool targzWriterData(targz_writer_t* w, const void* data, size_t len);

/** Finish: pad the open entry, write the two zero blocks, flush deflate, emit
 *  the gzip footer. Returns false if anything along the way failed. Frees the
 *  writer either way — do not touch it afterwards. */
bool targzWriterClose(targz_writer_t* w);

/* ---- Reader: gzip bytes in, entries out ---- */

typedef enum {
    TARGZ_OK = 0,
    TARGZ_ERR_MAGIC,      /* not a gzip stream */
    TARGZ_ERR_DEFLATE,    /* corrupt compressed data */
    TARGZ_ERR_TAR,        /* corrupt or unsupported tar */
    TARGZ_ERR_UNSAFE,     /* rejected entry: absolute, '..', or a non-file type */
    TARGZ_ERR_CRC,        /* gzip footer disagrees with what we inflated */
    TARGZ_ERR_TRUNCATED,  /* stream ended mid-entry or before the footer */
    TARGZ_ERR_SINK,       /* a callback returned false */
    TARGZ_ERR_MEM,
} targz_err_t;

/** One-line description of a targz_err_t, for a log line or an HTTP body. */
const char* targzErrStr(targz_err_t e);

/** Extraction callbacks. Any of them returning false stops the extraction with
 *  TARGZ_ERR_SINK. Paths are relative and already validated. */
typedef struct {
    bool (*onDir)(void* ctx, const char* name);
    bool (*onFile)(void* ctx, const char* name, uint32_t size);
    bool (*onData)(void* ctx, const void* data, size_t len);
    bool (*onFileEnd)(void* ctx);
} targz_reader_cb_t;

struct targz_reader_t;

/** Start an extraction. Allocates ~11 KB of inflate state plus the 32 KB
 *  wrapping window the LZ77 back-references need — the output is not resident,
 *  which is the whole point, so the window has to be. */
targz_reader_t* targzReaderOpen(const targz_reader_cb_t* cb, void* ctx);

/** Feed the next chunk of the compressed stream. Callbacks fire from inside.
 *  Returns TARGZ_OK while it wants more; any other value is terminal and the
 *  caller should stop feeding and call targzReaderFinish() for the tally. */
targz_err_t targzReaderFeed(targz_reader_t* r, const void* data, size_t len);

/** End of input: verify the gzip footer and that the tar ended on an entry
 *  boundary. Reports how much was extracted (either pointer may be null).
 *  Frees the reader — do not touch it afterwards. */
targz_err_t targzReaderFinish(targz_reader_t* r, uint32_t* entries,
                              uint64_t* bytes);

#endif /* SPANGAP_TARGZ_H */
