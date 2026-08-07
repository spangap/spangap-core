/**
 * targz — streaming ustar-inside-gzip, in both directions. See targz.h for the
 * contract; this file is the mechanism.
 *
 * WRITER. tdefl is driven with an explicit output buffer and `tdefl_compress`,
 * never its put_buf callback — see deflateRun(). The tar bytes we feed it are
 * the only thing we track (CRC32 and length, for the gzip footer). Entries are
 * emitted as they are added; nothing is held.
 *
 * READER. tinfl inflates into a 32 KB WRAPPING window. The window is not the
 * output: it is the LZ77 back-reference history, which has to be resident
 * because the real output (files on a filesystem) is not. Each freshly-inflated
 * run is handed straight to the tar parser, which is itself a byte-at-a-time
 * state machine over 512-byte blocks, so a run may straddle any boundary.
 *
 * The reader withholds the last 8 bytes of the stream from tinfl and finishes
 * with one flag-cleared pass — the two things that make a complete archive
 * actually read as complete. Both are explained at inflateSome() and at
 * targz_reader_t::hold, and both cost a great deal to rediscover.
 *
 * The reader treats the archive as hostile: it is a file an operator uploaded,
 * expanded directly onto a filesystem. Names are validated before any callback
 * sees them, and the only entry types that exist here are regular file and
 * directory.
 */
#include "targz.h"

#include "log.h"
#include "mem.h"

#include <cstring>
#include <cstdlib>

#include "miniz.h"   /* ROM tdefl/tinfl; mz_crc32 is esp_rom_crc32_le */

namespace {

constexpr size_t TAR_BLOCK = 512;
constexpr size_t GZ_HDR_LEN = 10;
constexpr size_t GZ_FOOT_LEN = 8;

/* Same deflate settings as the storage files: greedy parsing, 32 dictionary
 * probes. A state store is JSON, config blobs and small binaries — it still
 * compresses several-fold here, at roughly twice the speed of the 128-probe
 * default, and on a backup the CPU is what the transfer is waiting on. */
constexpr int TDEFL_FLAGS = 32 | TDEFL_GREEDY_PARSING_FLAG;

/* deflate, no name, OS unknown — the same fixed header storage.cpp writes. */
const uint8_t GZ_HEADER[GZ_HDR_LEN] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff };

/* ---- ustar header ---- */

struct ustar_t {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};
static_assert(sizeof(ustar_t) == TAR_BLOCK, "ustar header must be one block");

/** Write `v` as `width-1` zero-padded octal digits plus a NUL — the ustar
 *  numeric field convention. */
void octalField(char* dst, size_t width, uint64_t v) {
    for (size_t i = width - 1; i-- > 0; ) {
        dst[i] = (char)('0' + (v & 7));
        v >>= 3;
    }
    dst[width - 1] = '\0';
}

/** Read a ustar numeric field: octal digits, terminated by NUL or space. */
uint64_t octalValue(const char* src, size_t width) {
    uint64_t v = 0;
    for (size_t i = 0; i < width; i++) {
        char c = src[i];
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7') return 0;
        v = (v << 3) | (uint64_t)(c - '0');
    }
    return v;
}

/** The header checksum: every byte summed with the chksum field read as
 *  spaces. Stored as six octal digits, a NUL, then a space. */
uint32_t headerChecksum(const ustar_t& h) {
    const uint8_t* p = (const uint8_t*)&h;
    uint32_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++)
        sum += (i >= offsetof(ustar_t, chksum) &&
                i <  offsetof(ustar_t, chksum) + sizeof(h.chksum)) ? ' ' : p[i];
    return sum;
}

void fillHeader(ustar_t& h, char typeflag, uint32_t size, uint32_t mtime) {
    memset(&h, 0, sizeof(h));
    memcpy(h.mode, typeflag == '5' ? "0000755" : "0000644", 8);
    memcpy(h.uid, "0000000", 8);
    memcpy(h.gid, "0000000", 8);
    octalField(h.size, sizeof(h.size), size);
    octalField(h.mtime, sizeof(h.mtime), mtime);
    h.typeflag = typeflag;
    memcpy(h.magic, "ustar", 6);
    memcpy(h.version, "00", 2);
    memcpy(h.uname, "root", 5);
    memcpy(h.gname, "root", 5);
}

void sealHeader(ustar_t& h) {
    uint32_t sum = headerChecksum(h);
    octalField(h.chksum, 7, sum);   /* six digits + NUL … */
    h.chksum[7] = ' ';              /* … then the conventional trailing space */
}

/** Place `name` into the header's name/prefix pair. ustar can hold 100 bytes in
 *  `name`, or a `prefix` + '/' + `name` split on a component boundary, for 255
 *  in total. Returns false when neither fits — the caller skips the entry
 *  rather than writing a truncated path an extractor would happily believe. */
bool setEntryName(ustar_t& h, const char* name) {
    size_t len = strlen(name);
    if (len == 0) return false;
    if (len <= sizeof(h.name)) { memcpy(h.name, name, len); return true; }
    if (len > sizeof(h.prefix) + 1 + sizeof(h.name)) return false;
    /* Latest split that leaves a representable tail: keeps the prefix short and
     * the leaf whole, which is what a reader reconstructs from. */
    for (size_t i = len; i-- > 0; ) {
        if (name[i] != '/') continue;
        size_t tail = len - i - 1;
        if (tail == 0 || tail > sizeof(h.name) || i > sizeof(h.prefix)) continue;
        memcpy(h.prefix, name, i);
        memcpy(h.name, name + i + 1, tail);
        return true;
    }
    return false;
}

/** True if `name` is a path we are willing to write to a filesystem: relative,
 *  no `..` component, no empty component, and short enough to hold. */
bool safeEntryName(const char* name) {
    if (!name || !*name) return false;
    if (name[0] == '/') return false;
    size_t start = 0, len = strlen(name);
    if (len >= 256) return false;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && name[i] != '/') continue;
        size_t seg = i - start;
        if (seg == 0 && i != len) return false;                    /* "a//b" */
        if (seg == 2 && name[start] == '.' && name[start + 1] == '.') return false;
        start = i + 1;
    }
    return true;
}

}  // namespace

/* ======================================================================
 * Writer
 * ==================================================================== */

/* Deflate output staging. Sized so one HTTP chunk is a worthwhile write without
 * outrunning a 16 KB connection buffer by much — the sink applies back-pressure
 * either way, this just sets how often it is asked to. */
constexpr size_t DEFLATE_OUT = 8192;

struct targz_writer_t {
    targz_sink_fn     sink;
    void*             ctx;
    tdefl_compressor* comp;
    uint8_t*          out;        /* DEFLATE_OUT staging for tdefl_compress */
    uint32_t          crc;        /* over the tar bytes, for the gzip footer */
    uint64_t          rawLen;     /* likewise, for ISIZE */
    uint32_t          pending;    /* body bytes still owed for the open entry */
    uint32_t          pad;        /* padding owed once the body is complete */
    bool              ok;
};

namespace {

/** Drive deflate over `len` bytes with `flush`, handing every produced run to
 *  the sink as it appears.
 *
 *  tdefl is driven with an EXPLICIT output buffer (`tdefl_init(…, nullptr,
 *  nullptr, …)` + `tdefl_compress`), not with its put_buf callback and
 *  `tdefl_compress_buffer`. That is the only shape this platform's ROM miniz is
 *  known good for — it is what storage.cpp's gzip has always used — and the
 *  callback shape produced a compressor that accepted input and never emitted a
 *  byte. Loop because one call consumes as much input as its output buffer has
 *  room for, no more. */
bool deflateRun(targz_writer_t* w, const void* data, size_t len,
                tdefl_flush flush) {
    const uint8_t* p = (const uint8_t*)data;
    size_t left = len;
    for (;;) {
        size_t inBytes  = left;
        size_t outBytes = DEFLATE_OUT;
        tdefl_status st = tdefl_compress(w->comp, p, &inBytes,
                                         w->out, &outBytes, flush);
        if (st < TDEFL_STATUS_OKAY) { w->ok = false; return false; }
        if (outBytes && !w->sink(w->ctx, w->out, outBytes)) {
            w->ok = false;
            return false;
        }
        p    += inBytes;
        left -= inBytes;
        if (flush == TDEFL_FINISH) {
            if (st == TDEFL_STATUS_DONE) return true;
        } else if (!left) {
            return true;
        }
        if (!inBytes && !outBytes) {   /* no progress — refuse to spin */
            w->ok = false;
            return false;
        }
    }
}

/** Feed `len` tar bytes into deflate, keeping the footer's running CRC/length
 *  in step. This is the single place raw archive bytes enter the compressor. */
bool writeRaw(targz_writer_t* w, const void* data, size_t len) {
    if (!w->ok) return false;
    if (len == 0) return true;
    w->crc = (uint32_t)mz_crc32(w->crc, (const uint8_t*)data, len);
    w->rawLen += len;
    return deflateRun(w, data, len, TDEFL_NO_FLUSH);
}

bool writeZeros(targz_writer_t* w, size_t len) {
    static const uint8_t zeros[64] = {};
    while (len) {
        size_t n = len < sizeof(zeros) ? len : sizeof(zeros);
        if (!writeRaw(w, zeros, n)) return false;
        len -= n;
    }
    return true;
}

/** Close whatever entry is open: zero-fill a body that came up short (the file
 *  shrank under us mid-walk), then the 512-block padding. Without this a short
 *  read would slide every later header off its block boundary and cost the
 *  whole archive rather than the one file. */
bool finishEntry(targz_writer_t* w) {
    if (w->pending) {
        warn("targz: entry short by %u B, zero-filling\n", (unsigned)w->pending);
        if (!writeZeros(w, w->pending)) return false;
        w->pending = 0;
    }
    if (w->pad) {
        if (!writeZeros(w, w->pad)) return false;
        w->pad = 0;
    }
    return true;
}

}  // namespace

targz_writer_t* targzWriterOpen(targz_sink_fn sink, void* ctx) {
    if (!sink) return nullptr;
    auto* w = (targz_writer_t*)gp_alloc(sizeof(targz_writer_t));
    if (!w) return nullptr;
    memset(w, 0, sizeof(*w));
    w->sink = sink;
    w->ctx  = ctx;
    w->ok   = true;
    w->comp = (tdefl_compressor*)gp_alloc(sizeof(tdefl_compressor));
    w->out  = (uint8_t*)gp_alloc(DEFLATE_OUT);
    if (!w->comp || !w->out) { free(w->comp); free(w->out); free(w); return nullptr; }
    /* The gzip header rides the sink directly — it is framing, not deflate
     * input, so it must not reach the compressor or the CRC. */
    if (!sink(ctx, GZ_HEADER, GZ_HDR_LEN)) {
        free(w->comp); free(w->out); free(w);
        return nullptr;
    }
    tdefl_init(w->comp, nullptr, nullptr, TDEFL_FLAGS);
    return w;
}

bool targzWriterAddDir(targz_writer_t* w, const char* name, uint32_t mtime) {
    if (!w || !w->ok) return false;
    if (!finishEntry(w)) return false;
    ustar_t h;
    fillHeader(h, '5', 0, mtime);
    /* A trailing slash is the convention for a directory entry and is what
     * makes an extractor create it even when it holds no files. */
    char withSlash[256];
    int n = snprintf(withSlash, sizeof(withSlash), "%s/", name);
    if (n < 0 || (size_t)n >= sizeof(withSlash) || !setEntryName(h, withSlash)) {
        warn("targz: skipping directory, name too long: %s\n", name);
        return true;
    }
    sealHeader(h);
    return writeRaw(w, &h, sizeof(h));
}

bool targzWriterAddFile(targz_writer_t* w, const char* name, uint32_t size,
                        uint32_t mtime) {
    if (!w || !w->ok) return false;
    if (!finishEntry(w)) return false;
    ustar_t h;
    fillHeader(h, '0', size, mtime);
    if (!setEntryName(h, name)) {
        warn("targz: skipping file, name too long: %s\n", name);
        return true;   /* not fatal — but the caller must not send a body */
    }
    sealHeader(h);
    if (!writeRaw(w, &h, sizeof(h))) return false;
    w->pending = size;
    w->pad     = (uint32_t)((TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK);
    return true;
}

bool targzWriterData(targz_writer_t* w, const void* data, size_t len) {
    if (!w || !w->ok) return false;
    if (len > w->pending) len = w->pending;   /* declared size is the contract */
    if (!writeRaw(w, data, len)) return false;
    w->pending -= (uint32_t)len;
    if (w->pending == 0 && w->pad) {
        if (!writeZeros(w, w->pad)) return false;
        w->pad = 0;
    }
    return true;
}

bool targzWriterClose(targz_writer_t* w) {
    if (!w) return false;
    bool ok = w->ok;
    /* Two zero blocks end a tar. */
    if (ok) ok = finishEntry(w) && writeZeros(w, 2 * TAR_BLOCK);
    /* Drain deflate: FINISH keeps producing until it reports DONE. */
    if (ok) ok = deflateRun(w, nullptr, 0, TDEFL_FINISH);
    if (ok) {
        uint8_t foot[GZ_FOOT_LEN];
        uint32_t isize = (uint32_t)w->rawLen;   /* gzip's ISIZE is mod 2^32 */
        memcpy(foot,     &w->crc, 4);           /* LE target, direct copy */
        memcpy(foot + 4, &isize,  4);
        ok = w->sink(w->ctx, foot, sizeof(foot));
    }
    free(w->comp);
    free(w->out);
    free(w);
    return ok;
}

/* ======================================================================
 * Reader
 * ==================================================================== */

namespace {

constexpr size_t DICT_SIZE = 32768;   /* must be a power of two: we mask */

enum gz_stage_t {
    GZ_FIXED,      /* the 10 fixed header bytes */
    GZ_EXTRA_LEN, GZ_EXTRA, GZ_FNAME, GZ_FCOMMENT, GZ_FHCRC,
    GZ_DEFLATE,
    GZ_FOOTER,     /* the deflate stream has ended; the footer is in `hold` */
};

enum tar_stage_t { TAR_HDR, TAR_DATA, TAR_PAD, TAR_END };

}  // namespace

struct targz_reader_t {
    targz_reader_cb_t   cb;
    void*               ctx;
    tinfl_decompressor* inf;
    uint8_t*            dict;
    size_t              dictOfs;

    gz_stage_t gz;
    uint8_t    gzBuf[GZ_HDR_LEN];   /* fixed header, and the FEXTRA length */
    size_t     gzHave;
    uint8_t    gzFlags;
    uint32_t   extraLeft;

    /* A rolling window of the last GZ_FOOT_LEN bytes seen, never handed to the
     * decompressor. gzip's footer IS the last eight bytes of the stream, so it
     * is identified by position; whatever is still held when the input ends is
     * the footer. Asking tinfl to hand it back does not work — by the time it
     * stops it has already pulled those bytes into its bit buffer, and reports
     * them as consumed. */
    uint8_t  hold[GZ_FOOT_LEN];
    size_t   holdLen;

    uint32_t crc;        /* over what we inflated, checked against the footer */
    uint64_t rawLen;

    tar_stage_t tar;
    uint8_t     hdr[TAR_BLOCK];
    size_t      hdrHave;
    uint64_t    pending;
    uint32_t    pad;
    int         zeroBlocks;

    uint32_t entries;
    uint64_t bytes;

    targz_err_t err;
    bool        inFile;
};

const char* targzErrStr(targz_err_t e) {
    switch (e) {
        case TARGZ_OK:            return "ok";
        case TARGZ_ERR_MAGIC:     return "not a gzip archive";
        case TARGZ_ERR_DEFLATE:   return "corrupt compressed data";
        case TARGZ_ERR_TAR:       return "corrupt archive";
        case TARGZ_ERR_UNSAFE:    return "archive contains an unsafe entry";
        case TARGZ_ERR_CRC:       return "archive checksum mismatch";
        case TARGZ_ERR_TRUNCATED: return "archive truncated";
        case TARGZ_ERR_SINK:      return "could not write extracted data";
        case TARGZ_ERR_MEM:       return "out of memory";
    }
    return "unknown error";
}

namespace {

/** One complete tar header block. Returns the running error state. */
targz_err_t tarHeader(targz_reader_t* r) {
    const ustar_t& h = *(const ustar_t*)r->hdr;

    bool allZero = true;
    for (size_t i = 0; i < TAR_BLOCK; i++)
        if (r->hdr[i]) { allZero = false; break; }
    if (allZero) {
        /* Two of these end the archive; a single one is tolerated mid-stream
         * only in the sense that we wait for its partner. */
        if (++r->zeroBlocks >= 2) r->tar = TAR_END;
        return TARGZ_OK;
    }
    r->zeroBlocks = 0;

    if (octalValue(h.chksum, sizeof(h.chksum)) != headerChecksum(h))
        return TARGZ_ERR_TAR;

    /* Reconstruct prefix + '/' + name. Both fields may run the full width with
     * no NUL, so bound every read by the field size — and size the buffer for
     * the largest legal ustar path (155 + '/' + 100) plus its terminator, so a
     * maximal name is rejected by safeEntryName's rule rather than by ours. */
    char name[264];
    size_t plen = strnlen(h.prefix, sizeof(h.prefix));
    size_t nlen = strnlen(h.name, sizeof(h.name));
    if (plen) {
        if (plen + 1 + nlen >= sizeof(name)) return TARGZ_ERR_UNSAFE;
        memcpy(name, h.prefix, plen);
        name[plen] = '/';
        memcpy(name + plen + 1, h.name, nlen);
        name[plen + 1 + nlen] = '\0';
    } else {
        if (nlen >= sizeof(name)) return TARGZ_ERR_UNSAFE;
        memcpy(name, h.name, nlen);
        name[nlen] = '\0';
    }

    uint64_t size = octalValue(h.size, sizeof(h.size));
    char type = h.typeflag;
    bool isDir = (type == '5');
    /* A trailing slash names a directory whatever the typeflag says. */
    size_t len = strlen(name);
    while (len && name[len - 1] == '/') { name[--len] = '\0'; isDir = true; }

    if (!isDir && type != '0' && type != '\0') {
        /* Links, devices, FIFOs, GNU long-name extensions: we never write them
         * and will not create them from someone else's archive. */
        warn("targz: rejecting entry type '%c': %s\n", type ? type : '0', name);
        return TARGZ_ERR_UNSAFE;
    }
    if (!safeEntryName(name)) {
        warn("targz: rejecting unsafe path: %s\n", name);
        return TARGZ_ERR_UNSAFE;
    }

    if (isDir) {
        r->entries++;
        if (r->cb.onDir && !r->cb.onDir(r->ctx, name)) return TARGZ_ERR_SINK;
        /* Directory entries carry no body; if one claims a size, skip it. */
        r->pending = size;
    } else {
        r->entries++;
        if (r->cb.onFile && !r->cb.onFile(r->ctx, name, (uint32_t)size))
            return TARGZ_ERR_SINK;
        r->inFile  = true;
        r->pending = size;
        r->bytes  += size;
    }
    r->pad = (uint32_t)((TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK);
    if (r->pending) {
        r->tar = TAR_DATA;
    } else {
        if (r->inFile && r->cb.onFileEnd && !r->cb.onFileEnd(r->ctx))
            return TARGZ_ERR_SINK;
        r->inFile = false;
        r->tar = r->pad ? TAR_PAD : TAR_HDR;
    }
    return TARGZ_OK;
}

/** Consume freshly-inflated bytes as tar. Any amount, any alignment. */
targz_err_t tarConsume(targz_reader_t* r, const uint8_t* p, size_t len) {
    while (len && r->tar != TAR_END) {
        if (r->tar == TAR_HDR) {
            size_t n = TAR_BLOCK - r->hdrHave;
            if (n > len) n = len;
            memcpy(r->hdr + r->hdrHave, p, n);
            r->hdrHave += n; p += n; len -= n;
            if (r->hdrHave < TAR_BLOCK) return TARGZ_OK;
            r->hdrHave = 0;
            targz_err_t e = tarHeader(r);
            if (e != TARGZ_OK) return e;
        } else if (r->tar == TAR_DATA) {
            size_t n = (r->pending < len) ? (size_t)r->pending : len;
            if (r->inFile && r->cb.onData && !r->cb.onData(r->ctx, p, n))
                return TARGZ_ERR_SINK;
            r->pending -= n; p += n; len -= n;
            if (r->pending == 0) {
                if (r->inFile && r->cb.onFileEnd && !r->cb.onFileEnd(r->ctx))
                    return TARGZ_ERR_SINK;
                r->inFile = false;
                r->tar = r->pad ? TAR_PAD : TAR_HDR;
            }
        } else {   /* TAR_PAD */
            size_t n = (r->pad < len) ? r->pad : len;
            r->pad -= (uint32_t)n; p += n; len -= n;
            if (!r->pad) r->tar = TAR_HDR;
        }
    }
    return TARGZ_OK;
}

/** The gzip member header, one byte at a time — it can be split across feeds
 *  like anything else. Returns the number of bytes consumed; sets r->err. */
size_t gzHeader(targz_reader_t* r, const uint8_t* p, size_t len) {
    size_t used = 0;
    while (used < len && r->gz != GZ_DEFLATE) {
        uint8_t c = p[used];
        switch (r->gz) {
        case GZ_FIXED:
            r->gzBuf[r->gzHave++] = c; used++;
            if (r->gzHave < GZ_HDR_LEN) break;
            if (r->gzBuf[0] != 0x1f || r->gzBuf[1] != 0x8b || r->gzBuf[2] != 8) {
                r->err = TARGZ_ERR_MAGIC;
                return used;
            }
            r->gzFlags = r->gzBuf[3];
            r->gzHave = 0;
            r->gz = (r->gzFlags & 0x04) ? GZ_EXTRA_LEN
                  : (r->gzFlags & 0x08) ? GZ_FNAME
                  : (r->gzFlags & 0x10) ? GZ_FCOMMENT
                  : (r->gzFlags & 0x02) ? GZ_FHCRC : GZ_DEFLATE;
            break;
        case GZ_EXTRA_LEN:
            r->gzBuf[r->gzHave++] = c; used++;
            if (r->gzHave < 2) break;
            r->extraLeft = (uint32_t)r->gzBuf[0] | ((uint32_t)r->gzBuf[1] << 8);
            r->gzHave = 0;
            r->gz = r->extraLeft ? GZ_EXTRA
                  : (r->gzFlags & 0x08) ? GZ_FNAME
                  : (r->gzFlags & 0x10) ? GZ_FCOMMENT
                  : (r->gzFlags & 0x02) ? GZ_FHCRC : GZ_DEFLATE;
            break;
        case GZ_EXTRA: {
            size_t n = len - used;
            if (n > r->extraLeft) n = r->extraLeft;
            used += n; r->extraLeft -= (uint32_t)n;
            if (r->extraLeft) break;
            r->gz = (r->gzFlags & 0x08) ? GZ_FNAME
                  : (r->gzFlags & 0x10) ? GZ_FCOMMENT
                  : (r->gzFlags & 0x02) ? GZ_FHCRC : GZ_DEFLATE;
            break;
        }
        case GZ_FNAME:
            used++;
            if (c) break;
            r->gz = (r->gzFlags & 0x10) ? GZ_FCOMMENT
                  : (r->gzFlags & 0x02) ? GZ_FHCRC : GZ_DEFLATE;
            break;
        case GZ_FCOMMENT:
            used++;
            if (c) break;
            r->gz = (r->gzFlags & 0x02) ? GZ_FHCRC : GZ_DEFLATE;
            break;
        case GZ_FHCRC:
            used++;
            if (++r->gzHave < 2) break;
            r->gzHave = 0;
            r->gz = GZ_DEFLATE;
            break;
        default:
            return used;
        }
    }
    return used;
}

}  // namespace

targz_reader_t* targzReaderOpen(const targz_reader_cb_t* cb, void* ctx) {
    if (!cb) return nullptr;
    auto* r = (targz_reader_t*)gp_alloc(sizeof(targz_reader_t));
    if (!r) return nullptr;
    memset(r, 0, sizeof(*r));
    r->cb  = *cb;
    r->ctx = ctx;
    r->gz  = GZ_FIXED;
    r->tar = TAR_HDR;
    r->err = TARGZ_OK;
    r->inf  = (tinfl_decompressor*)gp_alloc(sizeof(tinfl_decompressor));
    r->dict = (uint8_t*)gp_alloc(DICT_SIZE);
    if (!r->inf || !r->dict) {
        free(r->inf); free(r->dict); free(r);
        return nullptr;
    }
    tinfl_init(r->inf);
    return r;
}

namespace {

/** Inflate as much of [p,len) as tinfl will take, feeding every produced run to
 *  the tar parser. Returns the bytes it would not consume.
 *
 *  `final` says "there is no more input after this", and it is load-bearing.
 *  With TINFL_FLAG_HAS_MORE_INPUT set, miniz's byte fetch returns
 *  NEEDS_MORE_INPUT whenever it runs dry rather than finishing — and at the end
 *  of a stream it needs a few more bits to close the final block. So a complete
 *  deflate stream fed entirely under that flag PARKS instead of reporting DONE,
 *  waiting for bytes that will never come. Whether it parks depends on where the
 *  last block ends within a byte, which is to say on the archive's content: it
 *  is why one backup restored and the next, same size, read as truncated. The
 *  caller feeds everything with the flag set, then calls once with it clear when
 *  the input has genuinely ended, which is the only way tinfl will close that
 *  block. Do NOT expect the leftover to be the gzip footer: by the time tinfl
 *  parks it has drawn those bytes into its bit buffer and counted them consumed.
 *  The footer is withheld by position instead — see targz_reader_t::hold. */
size_t inflateSome(targz_reader_t* r, const uint8_t* p, size_t len, bool final) {
    while (r->gz == GZ_DEFLATE) {
        size_t inBytes  = len;
        size_t outBytes = DICT_SIZE - r->dictOfs;
        tinfl_status st = tinfl_decompress(r->inf, p, &inBytes,
                                           r->dict, r->dict + r->dictOfs,
                                           &outBytes,
                                           final ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
        p += inBytes; len -= inBytes;
        if (outBytes) {
            r->crc = (uint32_t)mz_crc32(r->crc, r->dict + r->dictOfs, outBytes);
            r->rawLen += outBytes;
            targz_err_t e = tarConsume(r, r->dict + r->dictOfs, outBytes);
            if (e != TARGZ_OK) { r->err = e; return len; }
            r->dictOfs = (r->dictOfs + outBytes) & (DICT_SIZE - 1);
        }
        if (st == TINFL_STATUS_DONE) { r->gz = GZ_FOOTER; break; }
        if (st < TINFL_STATUS_DONE) { r->err = TARGZ_ERR_DEFLATE; return len; }
        if (inBytes == 0 && outBytes == 0) break;   /* wants input we don't have */
    }
    return len;
}

}  // namespace

targz_err_t targzReaderFeed(targz_reader_t* r, const void* data, size_t len) {
    if (!r) return TARGZ_ERR_MEM;
    if (r->err != TARGZ_OK) return r->err;

    const uint8_t* p = (const uint8_t*)data;
    size_t avail = len;

    if (r->gz != GZ_DEFLATE && r->gz != GZ_FOOTER) {
        size_t used = gzHeader(r, p, avail);
        p += used; avail -= used;
        if (r->err != TARGZ_OK) return r->err;
        if (r->gz != GZ_DEFLATE) return TARGZ_OK;   /* header still incomplete */
    }

    /* Hold back the trailing GZ_FOOT_LEN bytes of everything we have seen, and
     * give the decompressor the rest. gzip's footer is the last eight bytes of
     * the stream by definition, so this identifies it by POSITION — the one way
     * that does not depend on tinfl telling us where the deflate data stopped,
     * which it cannot: it draws bytes into its bit buffer before it knows it is
     * finished, and counts them consumed. */
    size_t total = r->holdLen + avail;
    if (total <= GZ_FOOT_LEN) {          /* not even a footer's worth yet */
        memcpy(r->hold + r->holdLen, p, avail);
        r->holdLen = total;
        return TARGZ_OK;
    }

    size_t feed     = total - GZ_FOOT_LEN;
    size_t fromHold = feed < r->holdLen ? feed : r->holdLen;
    size_t fromData = feed - fromHold;

    /* Work out the new hold BEFORE inflating: `p` is the caller's buffer and
     * the bytes we want are the last eight of hold ++ data. */
    uint8_t next[GZ_FOOT_LEN];
    size_t keptHold = r->holdLen - fromHold;
    memcpy(next, r->hold + fromHold, keptHold);
    memcpy(next + keptHold, p + fromData, avail - fromData);

    /* Carry first, then the new data: one stream, in order. Mid-stream tinfl
     * always takes what it is given, so a leftover while the deflate stream is
     * still open means it has stopped making progress. After DONE a leftover is
     * just alignment padding it read ahead and pushed back — not ours. */
    if (fromHold) {
        size_t left = inflateSome(r, r->hold, fromHold, /*final=*/false);
        if (r->err != TARGZ_OK) return r->err;
        if (left && r->gz == GZ_DEFLATE) { r->err = TARGZ_ERR_DEFLATE; return r->err; }
    }
    if (fromData) {
        size_t left = inflateSome(r, p, fromData, /*final=*/false);
        if (r->err != TARGZ_OK) return r->err;
        if (left && r->gz == GZ_DEFLATE) { r->err = TARGZ_ERR_DEFLATE; return r->err; }
    }

    memcpy(r->hold, next, GZ_FOOT_LEN);
    r->holdLen = GZ_FOOT_LEN;
    return TARGZ_OK;
}

targz_err_t targzReaderFinish(targz_reader_t* r, uint32_t* entries,
                              uint64_t* bytes) {
    if (!r) return TARGZ_ERR_MEM;

    /* No more input is coming, so this is the one call that clears
     * HAS_MORE_INPUT — the only way tinfl will close a final block whose last
     * bits it was still waiting on. It needs no bytes from us: the ones it was
     * short of are already in its bit buffer. */
    if (r->err == TARGZ_OK && r->gz == GZ_DEFLATE)
        inflateSome(r, nullptr, 0, /*final=*/true);

    targz_err_t e = r->err;

    if (e == TARGZ_OK) {
        if (r->gz == GZ_DEFLATE) {
            e = TARGZ_ERR_TRUNCATED;          /* the deflate stream never ended */
        } else if (r->holdLen < GZ_FOOT_LEN) {
            e = TARGZ_ERR_TRUNCATED;          /* fewer bytes than a footer */
        } else if (r->tar == TAR_DATA || r->tar == TAR_PAD || r->hdrHave) {
            e = TARGZ_ERR_TRUNCATED;          /* ended mid-entry */
        } else {
            /* The withheld last eight bytes ARE the footer. */
            uint32_t crc, isize;
            memcpy(&crc,   r->hold,     4);
            memcpy(&isize, r->hold + 4, 4);
            if (crc != r->crc || isize != (uint32_t)r->rawLen)
                e = TARGZ_ERR_CRC;
        }
    }

    if (entries) *entries = r->entries;
    if (bytes)   *bytes   = r->bytes;
    free(r->inf);
    free(r->dict);
    free(r);
    return e;
}
