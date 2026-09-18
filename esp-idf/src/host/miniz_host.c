/**
 * miniz_host.c — the deflate/inflate entry points the firmware calls, on a
 * host where the chip's ROM does not supply them.
 *
 * The firmware uses exactly three of miniz's low-level functions —
 * tdefl_init, tdefl_compress and tinfl_decompress — and drives both with an
 * explicit input and output buffer. That is a shape zlib answers directly, so
 * each one is a thin translation onto zlib's raw-deflate streams, and the
 * bytes on both sides are ordinary RFC 1951 deflate either way.
 *
 * Each compressor and decompressor carries its zlib stream inside the miniz
 * state structure the caller allocated, so ownership and lifetime stay exactly
 * as they are on the chip: the caller allocates it, hands it in, and frees it.
 * tinfl_init is a macro that zeroes m_state, which is this file's signal that
 * a decompressor is fresh and its stream has to be opened.
 *
 * zlib is declared here rather than included: the host carries the library but
 * not its headers, and the handful of declarations below are the frozen part
 * of its interface.
 */

/* miniz.h renames the whole zlib API onto its own by default, which would
 * rewrite the calls below into calls to itself. */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES 1
#include "miniz.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

/* ---- zlib, as much of it as this file uses ---- */

typedef void* (*hostz_alloc_fn)(void* opaque, unsigned items, unsigned size);
typedef void  (*hostz_free_fn)(void* opaque, void* address);

typedef struct {
    const unsigned char* next_in;
    unsigned             avail_in;
    unsigned long        total_in;
    unsigned char*       next_out;
    unsigned             avail_out;
    unsigned long        total_out;
    const char*          msg;
    void*                state;
    hostz_alloc_fn       zalloc;
    hostz_free_fn        zfree;
    void*                opaque;
    int                  data_type;
    unsigned long        adler;
    unsigned long        reserved;
} hostz_stream;

#define HOSTZ_OK            0
#define HOSTZ_STREAM_END    1
#define HOSTZ_DEFLATED      8
#define HOSTZ_VERSION       "1.3"
/* Negative window bits select a raw deflate stream: no zlib wrapper and no
 * trailing check value, which is what miniz produces and consumes here. */
#define HOSTZ_RAW_WINDOW    (-15)

extern int deflateInit2_(hostz_stream*, int level, int method, int windowBits,
                         int memLevel, int strategy, const char* version,
                         int stream_size);
extern int deflate(hostz_stream*, int flush);
extern int deflateEnd(hostz_stream*);
extern int inflateInit2_(hostz_stream*, int windowBits, const char* version,
                         int stream_size);
extern int inflate(hostz_stream*, int flush);
extern int inflateEnd(hostz_stream*);

/* ---- Compression ---- */

/* The caller allocates the state structure and does not clear it, so "is there
 * a zlib stream in here already?" has to be asked of a value that cannot occur
 * by accident. */
#define HOST_DEFLATE_OPEN  0x7a6c69624445464cULL

typedef struct {
    tdefl_put_buf_func_ptr put;
    void*                  put_user;
    unsigned long long     open;
    hostz_stream           zs;
} host_deflate_t;

_Static_assert(sizeof(host_deflate_t) <= sizeof(tdefl_compressor),
               "the zlib compressor state must fit the structure the caller allocated");

tdefl_status tdefl_init(tdefl_compressor* d, tdefl_put_buf_func_ptr put,
                        void* put_user, int flags)
{
    host_deflate_t* h = (host_deflate_t*)d;
    (void)flags;   /* probe count and parse strategy are miniz's own knobs */

    if (!d) return TDEFL_STATUS_BAD_PARAM;
    if (h->open == HOST_DEFLATE_OPEN) deflateEnd(&h->zs);

    memset(h, 0, sizeof(*h));
    h->put = put;
    h->put_user = put_user;
    if (deflateInit2_(&h->zs, 6, HOSTZ_DEFLATED, HOSTZ_RAW_WINDOW, 8, 0,
                      HOSTZ_VERSION, (int)sizeof(hostz_stream)) != HOSTZ_OK)
        return TDEFL_STATUS_BAD_PARAM;
    h->open = HOST_DEFLATE_OPEN;
    return TDEFL_STATUS_OKAY;
}

tdefl_status tdefl_compress(tdefl_compressor* d, const void* in, size_t* inLen,
                            void* out, size_t* outLen, tdefl_flush flush)
{
    host_deflate_t* h = (host_deflate_t*)d;
    size_t inAvail, outAvail;
    int rc;

    if (!d || h->open != HOST_DEFLATE_OPEN) return TDEFL_STATUS_BAD_PARAM;

    inAvail  = inLen  ? *inLen  : 0;
    outAvail = outLen ? *outLen : 0;

    h->zs.next_in   = (const unsigned char*)in;
    h->zs.avail_in  = (unsigned)inAvail;
    h->zs.next_out  = (unsigned char*)out;
    h->zs.avail_out = (unsigned)outAvail;

    /* miniz's flush values are zlib's: NO_FLUSH 0, SYNC 2, FULL 3, FINISH 4. */
    rc = deflate(&h->zs, (int)flush);
    if (rc != HOSTZ_OK && rc != HOSTZ_STREAM_END)
        return TDEFL_STATUS_PUT_BUF_FAILED;

    if (inLen)  *inLen  = inAvail  - h->zs.avail_in;
    if (outLen) *outLen = outAvail - h->zs.avail_out;

    if (rc == HOSTZ_STREAM_END) {
        deflateEnd(&h->zs);
        h->open = 0;
        return TDEFL_STATUS_DONE;
    }
    return TDEFL_STATUS_OKAY;
}

/* ---- Decompression ---- */

typedef struct {
    mz_uint32    m_state;   /* tinfl_init's macro zeroes this: 0 means fresh */
    hostz_stream zs;
} host_inflate_t;

_Static_assert(sizeof(host_inflate_t) <= sizeof(tinfl_decompressor),
               "the zlib decompressor state must fit the structure the caller allocated");

tinfl_status tinfl_decompress(tinfl_decompressor* r, const mz_uint8* in,
                              size_t* inLen, mz_uint8* outStart,
                              mz_uint8* outNext, size_t* outLen,
                              const mz_uint32 flags)
{
    host_inflate_t* h = (host_inflate_t*)r;
    size_t inAvail, outAvail;
    int rc;

    (void)outStart;   /* zlib keeps its own window; the caller's is just output */

    if (!r) return TINFL_STATUS_BAD_PARAM;

    if (h->m_state == 0) {
        memset(&h->zs, 0, sizeof(h->zs));
        if (inflateInit2_(&h->zs, HOSTZ_RAW_WINDOW, HOSTZ_VERSION,
                          (int)sizeof(hostz_stream)) != HOSTZ_OK)
            return TINFL_STATUS_FAILED;
        h->m_state = 1;
    }

    inAvail  = inLen  ? *inLen  : 0;
    outAvail = outLen ? *outLen : 0;

    h->zs.next_in   = in;
    h->zs.avail_in  = (unsigned)inAvail;
    h->zs.next_out  = outNext;
    h->zs.avail_out = (unsigned)outAvail;

    rc = inflate(&h->zs, 0);

    if (inLen)  *inLen  = inAvail  - h->zs.avail_in;
    if (outLen) *outLen = outAvail - h->zs.avail_out;

    if (rc == HOSTZ_STREAM_END) {
        inflateEnd(&h->zs);
        h->m_state = 0;
        return TINFL_STATUS_DONE;
    }
    /* Z_BUF_ERROR only says no progress was possible with what was handed in;
     * the caller's loop is what decides whether that is the end of the road. */
    if (rc != HOSTZ_OK && rc != -5) {
        inflateEnd(&h->zs);
        h->m_state = 0;
        return TINFL_STATUS_FAILED;
    }
    if (h->zs.avail_out == 0) return TINFL_STATUS_HAS_MORE_OUTPUT;
    (void)flags;
    return TINFL_STATUS_NEEDS_MORE_INPUT;
}
