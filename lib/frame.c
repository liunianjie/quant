#include <inttypes.h>
#include <sys/param.h>

#include "frame.h"
#include "pkt.h"
#include "util.h"


// Convert stream ID length encoded in flags to bytes
static uint8_t __attribute__((const)) dec_sid_len(const uint8_t flags)
{
    const uint8_t l = flags & 0x03;
    assert(/*l >= 0 && */ l <= 3, "cannot decode stream ID length %d", l);
    static const uint8_t dec[] = {1, 2, 3, 4};
    return dec[l];
}


// Convert stream ID length encoded in bytes to flags
static uint8_t __attribute__((const)) enc_sid_len(const uint8_t n)
{
    assert(n >= 1 && n <= 4, "cannot decode stream ID length %d", n);
    static const uint8_t enc[] = {0xFF, 0, 1, 2, 3}; // 0xFF invalid
    return enc[n];
}


// Convert stream offset length encoded in flags to bytes
static uint8_t __attribute__((const)) dec_off_len(const uint8_t flags)
{
    const uint8_t l = (flags & 0x1C) >> 2;
    assert(/* l >= 0 && */ l <= 7, "cannot decode stream offset length %d", l);
    static const uint8_t dec[] = {0, 2, 3, 4, 5, 6, 7, 8};
    return dec[l];
}


// Convert stream offset length encoded in bytes to flags
static uint8_t __attribute__((const)) enc_off_len(const uint8_t n)
{
    assert(n != 1 && n <= 8, "cannot stream encode offset length %d", n);
    static const uint8_t enc[] = {0, 0xFF, 1, 2, 3, 4, 5, 6, 7}; // 0xFF invalid
    return (uint8_t)(enc[n] << 2);
}


// Convert largest ACK length encoded in flags to bytes
static uint8_t __attribute__((const)) dec_lg_ack_len(const uint8_t flags)
{
    const uint8_t l = (flags & 0x0C) >> 2;
    assert(/* l >= 0 && */ l <= 3, "cannot decode largest ACK length %d", l);
    static const uint8_t dec[] = {1, 2, 3, 4};
    return dec[l];
}


// Convert ACK block length encoded in flags to bytes
static uint8_t __attribute__((const)) dec_ack_block_len(const uint8_t flags)
{
    const uint8_t l = flags & 0x03;
    assert(/*l >= 0 && */ l <= 3, "cannot decode largest ACK length %d", l);
    static const uint8_t dec[] = {1, 2, 4, 6};
    return dec[l];
}


static int __attribute__((nonnull))
cmp_q_stream(const void * restrict const arg, const void * restrict const obj)
{
    return *(const uint64_t *)arg != ((const struct q_stream *)obj)->id;
}


static uint16_t __attribute__((nonnull))
dec_stream_frame(struct q_conn * restrict const c,
                 const uint8_t * restrict const buf,
                 const uint16_t len)
{
    uint16_t i = 0;
    uint8_t type;
    decode(type, buf, len, i, 0, "0x%02x");
    warn(debug, "fin = %d", type & F_STREAM_FIN);

    const uint8_t sid_len = dec_sid_len(type);
    uint32_t sid = 0;
    decode(sid, buf, len, i, sid_len, "%d");
    struct q_stream * restrict s =
        hash_search(&c->streams, cmp_q_stream, &sid, sid);
    if (s == 0) {
        s = calloc(1, sizeof(*s));
        s->id = sid;
        hash_insert(&c->streams, &s->node, s, s->id);
        warn(info, "new stream %" PRIu64 " on connection %" PRIu64, s->id,
             c->id);
    }

    const uint8_t off_len = dec_off_len(type);
    uint64_t off = 0;
    if (off_len)
        decode(off, buf, len, i, off_len, "%" PRIu64);

    if (type & F_STREAM_DATA_LEN) {
        uint16_t dlen = 0;
        decode(dlen, buf, len, i, 0, "%d");
        // TODO: handle data
        i += dlen;
    }

    return i;
}


static uint16_t __attribute__((nonnull))
dec_ack_frame(struct q_conn * restrict const c __attribute__((unused)),
              const uint8_t * restrict const buf,
              const uint16_t len)
{
    uint16_t i = 0;
    uint8_t type;
    decode(type, buf, len, i, 0, "0x%02x");

    assert((type & F_ACK_UNUSED) == 0, "unused ACK frame bit set");

    const uint8_t lg_ack_len = dec_lg_ack_len(type);
    uint64_t lg_ack = 0;
    decode(lg_ack, buf, len, i, lg_ack_len, "%" PRIu64);

    uint16_t lg_ack_delta_t;
    decode(lg_ack_delta_t, buf, len, i, 0, "%d");

    const uint8_t ack_block_len = dec_ack_block_len(type);
    warn(debug, "%d-byte ACK block length", ack_block_len);

    uint8_t ack_blocks;
    if (type & F_ACK_N) {
        decode(ack_blocks, buf, len, i, 0, "%d");
        ack_blocks++; // NOTE: draft-hamilton says +1
    } else {
        ack_blocks = 1;
        warn(debug, "F_ACK_N unset; one ACK block present");
    }

    for (uint8_t b = 0; b < ack_blocks; b++) {
        warn(debug, "decoding ACK block #%d", b);
        uint64_t l = 0;
        decode(l, buf, len, i, ack_block_len, "%" PRIu64);
        // XXX: assume that the gap is not present for the very last ACK block
        if (b < ack_blocks - 1) {
            uint8_t gap;
            decode(gap, buf, len, i, 0, "%d");
        }
    }

    uint8_t ts_blocks;
    decode(ts_blocks, buf, len, i, 0, "%d");
    for (uint8_t b = 0; b < ts_blocks; b++) {
        warn(debug, "decoding timestamp block #%d", b);
        uint8_t delta_lg_obs;
        decode(delta_lg_obs, buf, len, i, 0, "%d");
        uint32_t ts;
        decode(ts, buf, len, i, 0, "%d");
    }

    return i;
}


static uint16_t __attribute__((nonnull))
dec_stop_waiting_frame(struct q_conn * restrict const c __attribute__((unused)),
                       const struct q_pub_hdr * restrict const p,
                       const uint8_t * restrict const buf,
                       const uint16_t len)
{
    uint16_t i = 0;
    uint8_t type;
    decode(type, buf, len, i, 0, "0x%02x");

    uint64_t lst_unacked = 0;
    decode(lst_unacked, buf, len, i, p->nr_len, "%" PRIu64);
    return i;
}


static uint16_t __attribute__((nonnull))
dec_conn_close_frame(struct q_conn * restrict const c __attribute__((unused)),
                     const uint8_t * restrict const buf,
                     const uint16_t len)
{
    uint16_t i = 0;
    uint8_t type;
    decode(type, buf, len, i, 0, "0x%02x");

    uint32_t err;
    decode(err, buf, len, i, 0, "%d");

    uint16_t reason_len;
    decode(reason_len, buf, len, i, 0, "%d");

    if (reason_len) {
        uint8_t * reason = 0;
        decode(*reason, buf, len, i, reason_len, "%d"); // XXX: ugly
        warn(err, "%s", reason);
    }

    return i;
}


uint16_t __attribute__((nonnull))
dec_frames(struct q_conn * restrict const c,
           const struct q_pub_hdr * restrict const p,
           const uint8_t * restrict const buf,
           const uint16_t len)
{
    uint16_t i = 0;

    while (i < len) {
        const uint8_t flags = buf[i];
        warn(debug, "frame type 0x%02x, start pos %d", flags, i);

        if (flags & F_STREAM) {
            i += dec_stream_frame(c, &buf[i], len - i);
            continue;
        }
        if (flags & F_ACK) {
            i += dec_ack_frame(c, &buf[i], len - i);
            continue;
        }

        switch (flags) {
        case T_PADDING:
            warn(debug, "%d-byte padding frame", len - i);
            static const uint8_t zero[MAX_PKT_LEN] = {0};
            assert(memcmp(&buf[i], zero, len - i) == 0,
                   "%d-byte padding not zero", len - i);
            i = len;
            break;

        case T_RST_STREAM:
            die("rst_stream frame");
            break;
        case T_CONNECTION_CLOSE:
            i += dec_conn_close_frame(c, &buf[i], len - i);
            break;
        case T_GOAWAY:
            die("goaway frame");
            break;
        case T_WINDOW_UPDATE:
            die("window_update frame");
            break;
        case T_BLOCKED:
            die("blocked frame");
            break;
        case T_STOP_WAITING:
            i += dec_stop_waiting_frame(c, p, &buf[i], len - i);
            break;
        case T_PING:
            die("ping frame");
            break;
        default:
            die("unknown frame type 0x%02x", buf[0]);
        }
    }
    return i;
}


uint16_t __attribute__((nonnull))
enc_ack_frame(uint8_t * restrict const buf, const uint16_t len)
{
    uint16_t i = 0;
    static const uint8_t type = F_ACK;
    encode(buf, len, i, type, 0, "%d");
    return i;
}


uint16_t __attribute__((nonnull))
enc_conn_close_frame(uint8_t * restrict const buf, const uint16_t len)
{
    uint16_t i = 0;
    static const uint8_t type = T_CONNECTION_CLOSE;
    encode(buf, len, i, type, 0, "%d");
    static const uint32_t err = QUIC_INVALID_VERSION;
    encode(buf, len, i, err, 0, "%d");
    static const char reason[] = "Because I don't like you.";
    static const uint16_t reason_len = sizeof(reason);
    encode(buf, len, i, reason, reason_len, "%s");
    return i;
}


uint16_t __attribute__((nonnull))
enc_stream_frame(uint8_t * restrict const buf, const uint16_t len)
{
    buf[0] = F_STREAM;
    uint16_t i = 1;
    assert(i < len, "buf too short");

    const uint32_t dummy_id = 1;
    memcpy(&buf[i], &dummy_id, sizeof(dummy_id));
    warn(debug, "%zu-byte id %d", sizeof(dummy_id), dummy_id);
    buf[0] |= enc_sid_len(sizeof(dummy_id));
    i += sizeof(dummy_id);
    assert(i < len, "buf too short");

    const uint16_t dummy_dl = 0;
    memcpy(&buf[i], &dummy_dl, sizeof(dummy_dl));
    warn(debug, "%zu-byte dl %d", sizeof(dummy_dl), dummy_dl);
    buf[0] |= F_STREAM_DATA_LEN;
    i += sizeof(dummy_dl);
    assert(i < len, "buf too short");

    const uint64_t dummy_off = 0;
    memcpy(&buf[i], &dummy_off, sizeof(dummy_off));
    const uint8_t off_len = enc_off_len(sizeof(dummy_off));
    warn(debug, "%zu-byte off %" PRIu64 " encoded as 0x%0x", sizeof(dummy_off),
         dummy_off, off_len);
    buf[0] |= off_len;
    i += sizeof(dummy_off);
    assert(i < len, "buf too short");

    buf[0] |= F_STREAM_FIN;

    // TODO: FIN bit and offset

    return i;
}


uint16_t __attribute__((nonnull))
enc_padding_frame(uint8_t * restrict const buf, const uint16_t len)
{
    buf[0] = T_PADDING;
    memset(&buf[1], 0, len - 1);
    warn(debug, "inserting %d bytes of zero padding", len - 1);
    return len;
}
