// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2020, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#if !defined(PARTICLE) && !defined(RIOT_VERSION)
#include <netinet/ip.h>
#endif

#include <picotls.h>
#include <quant/quant.h>
#include <timeout.h>
#include <warpcore/warpcore.h>

#include "bitset.h"
#include "conn.h"
#include "diet.h"
#include "frame.h"
#include "marshall.h"
#include "pkt.h"
#include "pn.h"
#include "qlog.h"
#include "quic.h"
#include "recovery.h"
#include "stream.h"
#include "tls.h"


#define MAX_PKT_NR_LEN 4 ///< Maximum packet number length allowed by spec.


#ifndef NDEBUG

void log_pkt(const char * const dir,
             const struct w_iov * const v,
             const struct w_sockaddr * const saddr
#ifdef FUZZING
             __attribute__((unused))
#endif
             ,
             const uint8_t * const tok,
             const uint16_t tok_len,
             const uint8_t * const rit)
{
    char ip[IP_STRLEN];
    w_ntop(&saddr->addr, ip);
    const uint16_t port = bswap16(saddr->port);
    const struct pkt_meta * const m = &meta(v);
    const char * const pts = pkt_type_str(m->hdr.flags, &m->hdr.vers);

    mk_cid_str(NTE, &m->hdr.dcid, dcid_str);
    mk_cid_str(NTE, &m->hdr.scid, scid_str);
    const char * const tok_str = tok_len ? tok_str(tok, tok_len) : "";
    const char * const rit_str = rit ? rit_str(rit) : "";

    if (*dir == 'R') {
        if (is_lh(m->hdr.flags)) {
            if (m->hdr.vers == 0)
                twarn(NTE,
                      BLD BLU "RX" NRM " from=%s%s%s:%u len=%u 0x%02x=" BLU
                              "%s " NRM "vers=0x%0" PRIx32 " dcid=%s scid=%s",
                      v->wv_af == AF_INET6 ? "[" : "", ip,
                      v->wv_af == AF_INET6 ? "]" : "", port, v->len,
                      m->hdr.flags, pts, m->hdr.vers, dcid_str, scid_str);
            else if (m->hdr.type == LH_RTRY)
                twarn(NTE,
                      BLD BLU "RX" NRM " from=%s%s%s:%u len=%u 0x%02x=" BLU
                              "%s " NRM "vers=0x%0" PRIx32
                              " dcid=%s scid=%s tok=%s rit=%s",
                      v->wv_af == AF_INET6 ? "[" : "", ip,
                      v->wv_af == AF_INET6 ? "]" : "", port, v->len,
                      m->hdr.flags, pts, m->hdr.vers, dcid_str, scid_str,
                      tok_str, rit_str);
            else if (m->hdr.type == LH_INIT)
                twarn(NTE,
                      BLD BLU "RX" NRM " from=%s%s%s:%u len=%u 0x%02x=" BLU
                              "%s " NRM "vers=0x%0" PRIx32
                              " dcid=%s scid=%s tok=%s len=%u nr=" BLU
                              "%" PRIu NRM,
                      v->wv_af == AF_INET6 ? "[" : "", ip,
                      v->wv_af == AF_INET6 ? "]" : "", port, v->len,
                      m->hdr.flags, pts, m->hdr.vers, dcid_str, scid_str,
                      tok_str, m->hdr.len, m->hdr.nr);
            else
                twarn(NTE,
                      BLD BLU "RX" NRM " from=%s%s%s:%u len=%u 0x%02x=" BLU
                              "%s " NRM "vers=0x%0" PRIx32
                              " dcid=%s scid=%s len=%u nr=" BLU "%" PRIu NRM,
                      v->wv_af == AF_INET6 ? "[" : "", ip,
                      v->wv_af == AF_INET6 ? "]" : "", port, v->len,
                      m->hdr.flags, pts, m->hdr.vers, dcid_str, scid_str,
                      m->hdr.len, m->hdr.nr);
        } else
            twarn(NTE,
                  BLD BLU "RX" NRM " from=%s%s%s:%u len=%u 0x%02x=" BLU
                          "%s " NRM "kyph=%u spin=%u dcid=%s nr=" BLU
                          "%" PRIu NRM,
                  v->wv_af == AF_INET6 ? "[" : "", ip,
                  v->wv_af == AF_INET6 ? "]" : "", port, v->len, m->hdr.flags,
                  pts, is_set(SH_KYPH, m->hdr.flags),
                  is_set(SH_SPIN, m->hdr.flags), dcid_str, m->hdr.nr);

    } else {
        // on TX, v->len is not yet final/correct, so don't print it
        if (is_lh(m->hdr.flags)) {
            if (m->hdr.vers == 0)
                twarn(NTE,
                      BLD GRN "TX" NRM " to=%s%s%s:%u 0x%02x=" GRN "%s " NRM
                              "vers=0x%0" PRIx32 " dcid=%s scid=%s",
                      v->wv_af == AF_INET6 ? "[" : "", ip,
                      v->wv_af == AF_INET6 ? "]" : "", port, m->hdr.flags, pts,
                      m->hdr.vers, dcid_str, scid_str);
            else if (m->hdr.type == LH_RTRY)
                twarn(NTE,
                      BLD GRN "TX" NRM " to=%s%s%s:%u 0x%02x=" GRN "%s " NRM
                              "vers=0x%0" PRIx32
                              " dcid=%s scid=%s tok=%s rit=%s",
                      v->wv_af == AF_INET6 ? "[" : "", ip,
                      v->wv_af == AF_INET6 ? "]" : "", port, m->hdr.flags, pts,
                      m->hdr.vers, dcid_str, scid_str, tok_str, rit_str);
            else if (m->hdr.type == LH_INIT)
                twarn(NTE,
                      BLD GRN "TX" NRM " to=%s%s%s:%u 0x%02x=" GRN "%s " NRM
                              "vers=0x%0" PRIx32
                              " dcid=%s scid=%s tok=%s len=%u nr=" GRN
                              "%" PRIu NRM,
                      v->wv_af == AF_INET6 ? "[" : "", ip,
                      v->wv_af == AF_INET6 ? "]" : "", port, m->hdr.flags, pts,
                      m->hdr.vers, dcid_str, scid_str, tok_str, m->hdr.len,
                      m->hdr.nr);
            else
                twarn(NTE,
                      BLD GRN "TX" NRM " to=%s%s%s:%u 0x%02x=" GRN "%s " NRM
                              "vers=0x%0" PRIx32
                              " dcid=%s scid=%s len=%u nr=" GRN "%" PRIu NRM,
                      v->wv_af == AF_INET6 ? "[" : "", ip,
                      v->wv_af == AF_INET6 ? "]" : "", port, m->hdr.flags, pts,
                      m->hdr.vers, dcid_str, scid_str, m->hdr.len, m->hdr.nr);
        } else
            twarn(NTE,
                  BLD GRN "TX" NRM " to=%s%s%s:%u 0x%02x=" GRN "%s " NRM
                          "kyph=%u spin=%u dcid=%s nr=" GRN "%" PRIu NRM,
                  v->wv_af == AF_INET6 ? "[" : "", ip,
                  v->wv_af == AF_INET6 ? "]" : "", port, m->hdr.flags, pts,
                  is_set(SH_KYPH, m->hdr.flags), is_set(SH_SPIN, m->hdr.flags),
                  dcid_str, m->hdr.nr);
    }
}
#endif


static bool __attribute__((const))
can_coalesce_pkt_types(const uint8_t a, const uint8_t b)
{
    return (a == LH_INIT && (b == LH_0RTT || b == LH_HSHK || b == SH)) ||
           (a == LH_HSHK && (b == LH_HSHK || b == SH)) ||
           (a == LH_0RTT && b == LH_HSHK);
}


void coalesce(struct w_iov_sq * const q, const uint16_t max_pkt_size)
{
    struct w_iov * v = sq_first(q);
    while (v) {
        struct w_iov * next = sq_next(v, next);
        struct w_iov * prev = v;
        const uint8_t * inner_buf = v->buf;
        while (next) {
            struct w_iov * const next_next = sq_next(next, next);
            // do we have space? do the packet types make sense to coalesce?
            if (v->len + next->len > max_pkt_size) {
                warn(DBG,
                     "cannot coalesce %u-byte pkt behind %u-byte pkt, limit %u",
                     next->len, v->len, max_pkt_size);
                prev = next;
            } else if (can_coalesce_pkt_types(pkt_type(*inner_buf),
                                              pkt_type(*next->buf)) == false) {
                warn(DBG, "cannot coalesce %s pkt behind %s pkt",
                     pkt_type_str(*next->buf, next->buf + 1),
                     pkt_type_str(*inner_buf, inner_buf + 1));
                prev = next;
            } else {
                // we can coalesce
                warn(INF,
                     "coalescing %u-byte %s pkt behind %u-byte pkt, outermost "
                     "%s, innermost %s",
                     next->len, pkt_type_str(*next->buf, next->buf + 1), v->len,
                     pkt_type_str(*v->buf, v->buf + 1),
                     pkt_type_str(*inner_buf, inner_buf + 1));
                inner_buf = next->buf;
                memcpy(v->buf + v->len, next->buf, next->len);
                v->len += next->len;
                sq_remove_after(q, prev, next);
                sq_next(next, next) = 0; // must unlink
                w_free_iov(next);
            }
            next = next_next;
        }
        v = sq_next(v, next);
    }
}


static inline uint8_t __attribute__((const))
needed_pkt_nr_len(const uint_t lg_acked, const uint64_t n)
{
    const uint64_t d =
        (n - (unlikely(lg_acked == UINT_T_MAX) ? 0 : lg_acked)) * 2;
    if (d <= UINT8_MAX)
        return 1;
    if (d <= UINT16_MAX)
        return 2;
    if (d <= (UINT32_MAX >> 8))
        return 3;
    return 4;
}


void enc_lh_cids(uint8_t ** pos,
                 const uint8_t * const end,
                 struct pkt_meta * const m,
                 const struct cid * const dcid,
                 const struct cid * const scid)
{
    cid_cpy(&m->hdr.dcid, dcid);
    if (scid)
        cid_cpy(&m->hdr.scid, scid);
    enc1(pos, end, m->hdr.dcid.len);
    if (m->hdr.dcid.len)
        encb(pos, end, m->hdr.dcid.id, m->hdr.dcid.len);
    enc1(pos, end, m->hdr.scid.len);
    if (m->hdr.scid.len)
        encb(pos, end, m->hdr.scid.id, m->hdr.scid.len);
}


static bool __attribute__((nonnull)) can_enc(uint8_t ** const pos,
                                             const uint8_t * const end,
                                             const struct pkt_meta * const m,
                                             const uint8_t type,
                                             const bool one_per_pkt)
{
    const bool has_space = *pos + max_frame_len(type) <= end;
    return (one_per_pkt && has_frm(m->frms, type)) == false && has_space;
}


static void __attribute__((nonnull
#ifdef NO_QINFO
                           (2, 3, 4)
#endif
                               ))
enc_other_frames(
#ifndef NO_QINFO
    struct q_conn_info
#else
    void
#endif
        * const ci,
    uint8_t ** pos,
    const uint8_t * const end,
    struct pkt_meta * const m)
{
    struct q_conn * const c = m->pn->c;

    // encode connection control frames
    if (c->tx_hshk_done)
        enc_hshk_done_frame(ci, pos, end, m);

    if (!is_clnt(c) && c->tok_len && can_enc(pos, end, m, FRM_TOK, true)) {
        enc_new_token_frame(ci, pos, end, m);
        c->tok_len = 0;
    }

#ifndef NO_MIGRATION
    if (c->tx_path_resp && can_enc(pos, end, m, FRM_PRP, true)) {
        enc_path_response_frame(ci, pos, end, m);
        c->tx_path_resp = false;
    }

    if (c->tx_retire_cid && can_enc(pos, end, m, FRM_RTR, true)) {
        struct cid * rcid = splay_min(cids_by_seq, &c->dcids_by_seq);
        while (rcid && rcid->seq < c->dcid->seq) {
            struct cid * const next =
                splay_next(cids_by_seq, &c->dcids_by_seq, rcid);
            if (rcid->retired) {
                enc_retire_cid_frame(ci, pos, end, m, rcid);
                free_dcid(c, rcid);
            }
            rcid = next;
        }
    }

    if (c->tx_path_chlg && can_enc(pos, end, m, FRM_PCL, true))
        enc_path_challenge_frame(ci, pos, end, m);

    while (c->tx_ncid && can_enc(pos, end, m, FRM_CID, false)) {
        enc_new_cid_frame(ci, pos, end, m);
        c->tx_ncid = needs_more_ncids(c);
    }
#endif

    if (c->blocked && can_enc(pos, end, m, FRM_CDB, true))
        enc_data_blocked_frame(ci, pos, end, m);

    if (c->tx_max_data && can_enc(pos, end, m, FRM_MCD, true))
        enc_max_data_frame(ci, pos, end, m);

    if (c->sid_blocked_bidi && can_enc(pos, end, m, FRM_SBB, true))
        enc_streams_blocked_frame(ci, pos, end, m, true);

    if (c->sid_blocked_uni && can_enc(pos, end, m, FRM_SBU, true))
        enc_streams_blocked_frame(ci, pos, end, m, false);

    if (c->tx_max_sid_bidi && can_enc(pos, end, m, FRM_MSB, true))
        enc_max_strms_frame(ci, pos, end, m, true);

    if (c->tx_max_sid_uni && can_enc(pos, end, m, FRM_MSU, true))
        enc_max_strms_frame(ci, pos, end, m, false);

    while (!sl_empty(&c->need_ctrl)) {
        // XXX this assumes we can encode all the ctrl frames
        struct q_stream * const s = sl_first(&c->need_ctrl);
        sl_remove_head(&c->need_ctrl, node_ctrl);
        s->in_ctrl = false;
        // encode stream control frames
        if (s->blocked && can_enc(pos, end, m, FRM_SDB, true))
            enc_strm_data_blocked_frame(ci, pos, end, m, s);
        if (s->tx_max_strm_data && can_enc(pos, end, m, FRM_MSD, true))
            enc_max_strm_data_frame(ci, pos, end, m, s);
    }
}


bool enc_pkt(struct q_stream * const s,
             const bool rtx,
             const bool enc_data,
             const bool tx_ack_eliciting,
             const bool pmtud,
             struct w_iov * const v,
             struct pkt_meta * const m)
{
    if (likely(enc_data))
        // prepend the header by adjusting the buffer offset
        adj_iov_to_start(v, m);

    struct q_conn * const c = s->c;
    uint8_t * len_pos = 0;
#ifndef NO_QINFO
    struct q_conn_info * const ci = &c->i;
#else
    void * const ci = 0;
#endif

    const epoch_t epoch = unlikely(pmtud) ? ep_hshk : strm_epoch(s);
    struct pn_space * const pn = m->pn = pn_for_epoch(c, epoch);

    m->txed = true;
#ifndef NO_SERVER
    if (unlikely(c->tx_rtry))
        m->hdr.nr = 0;
    else
#endif
        if (unlikely(pn->lg_sent == UINT_T_MAX))
        // next pkt nr
        m->hdr.nr = pn->lg_sent = 0;
    else
        m->hdr.nr = ++pn->lg_sent;

    switch (epoch) {
    case ep_init:
#ifndef NO_SERVER
        if (unlikely(c->tx_rtry))
            m->hdr.type = LH_RTRY;
        else
#endif
            m->hdr.type = LH_INIT;
        m->hdr.flags = LH | m->hdr.type;
#ifndef NO_SERVER
        if (unlikely(c->tx_rtry))
            m->hdr.flags |= (uint8_t)w_rand_uniform32(0x0f);
#endif
        break;
    case ep_0rtt:
        if (is_clnt(c)) {
            m->hdr.type = LH_0RTT;
            m->hdr.flags = LH | m->hdr.type;
        } else
            m->hdr.type = m->hdr.flags = SH;
        break;
    case ep_hshk:
        m->hdr.type = LH_HSHK;
        m->hdr.flags = LH | m->hdr.type;
        break;
    case ep_data:
        m->hdr.type = m->hdr.flags = SH;
        m->hdr.flags |= pn->data.out_kyph ? SH_KYPH : 0;
        if (c->spin_enabled && c->spin)
            m->hdr.flags |= SH_SPIN;
        break;
    }

    const uint8_t pnl = needed_pkt_nr_len(pn->lg_acked, m->hdr.nr);
    m->hdr.flags |= (pnl - 1);

    uint8_t * pos = v->buf;
    const uint8_t * const end = v->buf + (enc_data ? m->strm_data_pos : v->len);
    enc1(&pos, end, m->hdr.flags);

    if (unlikely(is_lh(m->hdr.flags))) {
        m->hdr.vers = c->vers;
        enc4(&pos, end, m->hdr.vers);
        enc_lh_cids(&pos, end, m, c->dcid, c->scid);

        if (m->hdr.type == LH_INIT)
            encv(&pos, end, is_clnt(c) ? c->tok_len : 0);

        if (((is_clnt(c) && m->hdr.type == LH_INIT) ||
             m->hdr.type == LH_RTRY) &&
            c->tok_len)
            encb(&pos, end, c->tok, c->tok_len);

        if (m->hdr.type != LH_RTRY) {
            // leave space for length field (2 bytes is enough)
            len_pos = pos;
            pos += 2;
        }

    } else {
        cid_cpy(&m->hdr.dcid, c->dcid);
        encb(&pos, end, m->hdr.dcid.id, m->hdr.dcid.len);
    }

    uint8_t * pkt_nr_pos = 0;
    if (likely(m->hdr.type != LH_RTRY)) {
        pkt_nr_pos = pos;
        switch ((pnl - 1) & HEAD_PNRL_MASK) {
        case 0:
            enc1(&pos, end, m->hdr.nr & UINT64_C(0xff));
            break;
        case 1:
            enc2(&pos, end, m->hdr.nr & UINT64_C(0xffff));
            break;
        case 2:
            enc3(&pos, end, m->hdr.nr & UINT64_C(0xffffff));
            break;
        case 3:
            enc4(&pos, end, m->hdr.nr & UINT64_C(0xffffffff));
            break;
        }
    }

    m->hdr.hdr_len = (uint16_t)(pos - v->buf);
    v->saddr =
#ifndef NO_MIGRATION
        unlikely(c->tx_path_chlg) ? c->migr_peer :
#endif
                                  c->peer;

#ifndef NDEBUG
    // sanity check
    if (unlikely(m->hdr.hdr_len >=
                 DATA_OFFSET + (is_lh(m->hdr.flags) ? c->tok_len + 16 : 0))) {
        warn(ERR, "pkt header %u >= offset %u", m->hdr.hdr_len,
             DATA_OFFSET + (is_lh(m->hdr.flags) ? c->tok_len + 16 : 0));
        return false;
    }
#endif

    if (unlikely(m->hdr.type == LH_RTRY)) {
        uint8_t rit[RIT_LEN];
        make_rit(c, m->hdr.flags, &m->hdr.dcid, &m->hdr.scid, c->tok,
                 c->tok_len, rit);
        encb(&pos, end, rit, RIT_LEN);
        log_pkt("TX", v, &v->saddr, c->tok, c->tok_len, rit);
        goto tx;
    }

    log_pkt("TX", v, &v->saddr, c->tok, c->tok_len, 0);

    if (unlikely(pmtud)) {
        enc_ping_frame(ci, &pos, end, m);
        enc_padding_frame(ci, &pos, end, m, (uint16_t)(end - pos - AEAD_LEN));
        goto tx;
    }

    if (needs_ack(pn) != no_ack) {
        // FIXME: 8 is an arbitrary value
        if (enc_data == false || diet_cnt(&pn->recv) <= 8)
            enc_ack_frame(ci, &pos, v->buf, end, m, pn);
        else
            timeouts_add(ped(c->w)->wheel, &c->ack_alarm, 0);
    }

    if (unlikely(c->state == conn_clsg))
        enc_close_frame(ci, &pos, end, m);
    else if (epoch == ep_data || (!is_clnt(c) && epoch == ep_0rtt))
        // TODO calc stream hdr len and subtract
        enc_other_frames(ci, &pos, end, m);

    if (unlikely(rtx)) {
        // this is a RTX, pad out until beginning of stream header
        enc_padding_frame(ci, &pos, end, m,
                          m->strm_frm_pos - (uint16_t)(pos - v->buf));
        pos = v->buf + m->strm_data_pos + m->strm_data_len;
        log_stream_or_crypto_frame(
            true, m, v->buf[m->strm_frm_pos], s->id, false,
            m->strm_off + m->strm_data_len < s->out_data ? sdt_ooo : sdt_seq);

    } else if (likely(enc_data)) {
        // this is a fresh data/crypto or pure stream FIN packet
        uint16_t hlen;
        uint16_t dlen;
        calc_lens_of_stream_or_crypto_frame(m, v, s, &hlen, &dlen);
        if (unlikely(pos + hlen >= v->buf + m->strm_data_pos))
            // if the stream header would overflow a previous frame, kill *all*
            pos = v->buf + m->hdr.hdr_len;
        // pad out any remaining space before stream header
        enc_padding_frame(ci, &pos, end, m,
                          m->strm_data_pos - hlen - (uint16_t)(pos - v->buf));
        enc_stream_or_crypto_frame(&pos, end, m, v, s, dlen);
    }

    // TODO: include more frames when c->rec.max_pkt_size < max_pkt_len TP
    if (unlikely((pos - v->buf) < c->rec.max_pkt_size - AEAD_LEN &&
                 (enc_data || rtx) &&
                 (epoch == ep_data || (!is_clnt(c) && epoch == ep_0rtt))))
        // we can try to stick some more frames in after the stream frame
        enc_other_frames(ci, &pos, v->buf + c->rec.max_pkt_size - AEAD_LEN, m);

    if (is_clnt(c) && enc_data) {
        if (unlikely(c->try_0rtt == false && m->hdr.type == LH_INIT)) {
            const uint8_t * const min_len = v->buf + MIN_INI_LEN - AEAD_LEN;
            if (pos < min_len)
                enc_padding_frame(ci, &pos, min_len, m,
                                  (uint16_t)(min_len - pos));
        }
        if (unlikely(c->try_0rtt == true && m->hdr.type == LH_0RTT &&
                     s->id >= 0)) {
            // if we pad the first 0-RTT pkt, peek at txq to get the CI length
            const uint8_t * const min_len =
                v->buf + MIN_INI_LEN - AEAD_LEN -
                (sq_first(&c->txq) ? sq_first(&c->txq)->len : 0);
            if (pos < min_len)
                enc_padding_frame(ci, &pos, min_len, m,
                                  (uint16_t)(min_len - pos));
        }
    }

    m->ack_eliciting = is_ack_eliciting(&m->frms);
    if (unlikely(tx_ack_eliciting) && m->ack_eliciting == false &&
        m->hdr.type == SH) {
        enc_ping_frame(ci, &pos, end, m);
        m->ack_eliciting = true;
    }

    // gotta send something, anything
    if (unlikely(pos - v->buf == m->hdr.hdr_len)) {
        if (diet_empty(&pn->recv) == false)
            enc_ack_frame(ci, &pos, v->buf, end, m, pn);
        else
            enc_ping_frame(ci, &pos, end, m);
    }

tx:;
    // make sure we have enough frame bytes for the header protection sample
    const uint16_t pnp_dist = (uint16_t)(pos - pkt_nr_pos);
    if (unlikely(pnp_dist < 4))
        enc_padding_frame(ci, &pos, end, m, 4 - pnp_dist);

    // for LH pkts, now encode the length
    m->hdr.len = (uint16_t)(pos - pkt_nr_pos) + AEAD_LEN;
    if (unlikely(len_pos))
        encvl(&len_pos, len_pos + 2, m->hdr.len, 2);

    v->len = (uint16_t)(pos - v->buf);

    // alloc directly from warpcore for crypto TX - no need for metadata alloc
    struct w_iov * const xv = w_alloc_iov(c->w, q_conn_af(c), 0, 0);
    ensure(xv, "w_alloc_iov failed");

    if (unlikely(m->hdr.type == LH_RTRY)) {
        memcpy(xv->buf, v->buf, v->len); // copy data
        xv->len = v->len;
    } else {
        const uint16_t ret =
            enc_aead(v, m, xv, (uint16_t)(pkt_nr_pos - v->buf));
        if (unlikely(ret == 0)) {
            adj_iov_to_start(v, m);
            return false;
        }
    }

    if (!is_clnt(c))
        xv->saddr = v->saddr;

    // track the flags manually, since warpcore sets them on the xv and it'd
    // require another loop to copy them over
    xv->flags = v->flags |= likely(c->sockopt.enable_ecn) ? IPTOS_ECN_ECT0 : 0;

#ifndef NO_MIGRATION
    if (unlikely(c->tx_path_chlg))
        sq_insert_tail(&c->migr_txq, xv, next);
    else
#endif
        sq_insert_tail(&c->txq, xv, next);

    m->udp_len = xv->len;
    c->out_data += m->udp_len;

    if (unlikely(m->hdr.type == LH_INIT && is_clnt(c) && m->strm_data_len))
        // adjust v->len to exclude the post-stream padding for CI
        v->len = m->strm_data_pos + m->strm_data_len;

    if (likely(enc_data)) {
        adj_iov_to_data(v, m);
        // XXX not clear if changing the len before calling on_pkt_sent is ok
        v->len = m->strm_data_len;
    }

    if (unlikely(rtx && m->lost)) {
        // we did an RTX and this is no longer lost
        m->lost = false;
        m->strm->lost_cnt--;
    }

    on_pkt_sent(m);
    qlog_transport(pkt_tx, "DEFAULT", ped(c->w), v, m, &c->odcid);
    bit_or(FRM_MAX, &pn->tx_frames, &m->frms);

    if (is_clnt(c)) {
        if (is_lh(m->hdr.flags) == false)
            maybe_flip_keys(c, true);
        if (unlikely(m->hdr.type == LH_HSHK && c->cstrms[ep_init]))
            abandon_pn(&c->pns[ep_init]);
    }

    return true;
}


#define dec1_chk(val, pos, end)                                                \
    do {                                                                       \
        if (unlikely(dec1((val), (pos), (end)) == false))                      \
            return false;                                                      \
    } while (0)


#define dec2_chk(val, pos, end)                                                \
    do {                                                                       \
        if (unlikely(dec2((val), (pos), (end)) == false))                      \
            return false;                                                      \
    } while (0)


#define dec3_chk(val, pos, end)                                                \
    do {                                                                       \
        if (unlikely(dec3((val), (pos), (end)) == false))                      \
            return false;                                                      \
    } while (0)


#define dec4_chk(val, pos, end)                                                \
    do {                                                                       \
        if (unlikely(dec4((val), (pos), (end)) == false))                      \
            return false;                                                      \
    } while (0)


#define decv_chk(val, pos, end)                                                \
    do {                                                                       \
        if (unlikely(decv((val), (pos), (end)) == false))                      \
            return false;                                                      \
    } while (0)


#define decb_chk(val, pos, end, len)                                           \
    do {                                                                       \
        if (unlikely(decb((val), (pos), (end), (len)) == false))               \
            return false;                                                      \
    } while (0)


bool dec_pkt_hdr_beginning(struct w_iov * const xv,
                           struct w_iov * const v,
                           struct pkt_meta * const m,
                           const bool is_clnt,
                           uint8_t * const tok,
                           uint16_t * const tok_len,
                           uint8_t * const rit,
                           const uint8_t dcid_len)

{
    const uint8_t * pos = xv->buf;
    const uint8_t * const end = xv->buf + xv->len;

    m->udp_len = xv->len;

    dec1_chk(&m->hdr.flags, &pos, end);
    m->hdr.type = pkt_type(m->hdr.flags);

    if (unlikely(is_lh(m->hdr.flags))) {
        dec4_chk(&m->hdr.vers, &pos, end);
        dec1_chk(&m->hdr.dcid.len, &pos, end);

        if (unlikely(m->hdr.vers && m->hdr.dcid.len > CID_LEN_MAX)) {
            warn(DBG, "illegal dcid len %u", m->hdr.dcid.len);
            m->hdr.dcid.len = 0;
            return false;
        }

        if (m->hdr.dcid.len)
            decb_chk(m->hdr.dcid.id, &pos, end, m->hdr.dcid.len);

        dec1_chk(&m->hdr.scid.len, &pos, end);
        if (unlikely(m->hdr.vers && m->hdr.scid.len > CID_LEN_MAX)) {
            warn(DBG, "illegal scid len %u", m->hdr.scid.len);
            m->hdr.dcid.len = 0;
            return false;
        }

        if (m->hdr.scid.len)
            decb_chk(m->hdr.scid.id, &pos, end, m->hdr.scid.len);

        if (m->hdr.vers == 0) {
            // version negotiation packet - copy raw
            memcpy(v->buf, xv->buf, xv->len);
            v->len = xv->len;
            goto done;
        }

        if (m->hdr.type == LH_INIT) {
            // decode token
            uint64_t tmp = 0;
            decv_chk(&tmp, &pos, end);
            *tok_len = (uint16_t)tmp;
            if (is_clnt && *tok_len) {
                // server initial pkts must have no tokens
                warn(ERR, "tok (len %u) present in serv initial", *tok_len);
                return false;
            }
        } else if (m->hdr.type == LH_RTRY) {
            *tok_len = (uint16_t)(end - pos);
            if (unlikely(*tok_len <= RIT_LEN)) {
                warn(DBG, "tok_len %u too short", *tok_len);
                return false;
            }
            *tok_len -= RIT_LEN;
        }

        if (*tok_len) {
            if (unlikely(*tok_len >= MAX_TOK_LEN ||
                         *tok_len + m->hdr.hdr_len > xv->len)) {
                // corrupt token len
                warn(DBG, "tok_len %u invalid (max %u)", *tok_len, MAX_TOK_LEN);
                return false;
            }
            decb_chk(tok, &pos, end, *tok_len);
            if (m->hdr.type == LH_RTRY)
                decb_chk(rit, &pos, end, RIT_LEN);
        }

        if (m->hdr.type != LH_RTRY) {
            uint64_t tmp = 0;
            decv_chk(&tmp, &pos, end);
            m->hdr.len = (uint16_t)tmp;
            // sanity check len
            if (unlikely(m->hdr.len + m->hdr.hdr_len > xv->len)) {
                warn(DBG, "len %u invalid", m->hdr.len);
                return false;
            }
        }

    } else {
        // this logic depends on picking a SCID w/known length during handshake
        m->hdr.dcid.len = dcid_len;
        decb_chk(m->hdr.dcid.id, &pos, end, m->hdr.dcid.len);
    }

done:
    m->hdr.hdr_len = (uint16_t)(pos - xv->buf);
    return true;
}


bool xor_hp(struct w_iov * const xv,
            const struct pkt_meta * const m,
            const struct cipher_ctx * const ctx,
            const uint16_t pkt_nr_pos,
            const bool is_enc)
{
    const uint16_t off = pkt_nr_pos + MAX_PKT_NR_LEN;
    const uint16_t len =
        is_lh(m->hdr.flags) ? pkt_nr_pos + m->hdr.len : xv->len;
    if (unlikely(off + AEAD_LEN > len))
        return false;

    ptls_cipher_init(ctx->header_protection, &xv->buf[off]);
    uint8_t mask[MAX_PKT_NR_LEN + 1] = {0};
    ptls_cipher_encrypt(ctx->header_protection, mask, mask, sizeof(mask));

    const uint8_t orig_flags = xv->buf[0];
    xv->buf[0] ^= mask[0] & (unlikely(is_lh(m->hdr.flags)) ? 0x0f : 0x1f);
    const uint8_t pnl = pkt_nr_len(is_enc ? orig_flags : xv->buf[0]);
    for (uint8_t i = 0; i < pnl; i++)
        xv->buf[pkt_nr_pos + i] ^= mask[1 + i];

#ifdef DEBUG_PROT
    warn(DBG, "%s HP over [0, %u..%u] w/sample off %u",
         is_enc ? "apply" : "undo", pkt_nr_pos, pkt_nr_pos + pnl - 1, off);
#endif

    return true;
}


static bool undo_hp(struct w_iov * const xv,
                    struct pkt_meta * const m,
                    const struct cipher_ctx * const ctx)
{
    // undo HP and update meta; m->hdr.hdr_len holds the offset of the pnr field
    if (unlikely(xor_hp(xv, m, ctx, m->hdr.hdr_len, false) == false))
        return false;

    m->hdr.flags = xv->buf[0];
    m->hdr.type = pkt_type(xv->buf[0]);
    const uint8_t pnl = pkt_nr_len(xv->buf[0]);
    struct pn_space * const pn = pn_for_pkt_type(m->pn->c, m->hdr.type);
    const uint8_t * pnp = xv->buf + m->hdr.hdr_len;

    switch (xv->buf[0] & HEAD_PNRL_MASK) {
    case 0:;
        uint8_t tmp1;
        dec1_chk(&tmp1, &pnp, pnp + pnl);
        m->hdr.nr = tmp1;
        break;
    case 1:;
        uint16_t tmp2;
        dec2_chk(&tmp2, &pnp, pnp + pnl);
        m->hdr.nr = tmp2;
        break;
    case 2:;
        uint32_t tmp34;
        dec3_chk(&tmp34, &pnp, pnp + pnl);
        m->hdr.nr = tmp34;
        break;
    case 3:
        dec4_chk(&tmp34, &pnp, pnp + pnl);
        m->hdr.nr = tmp34;
        break;
    }
    m->hdr.hdr_len += pnl;

    const uint64_t expected_pn = diet_max(&pn->recv) + 1;
    const uint64_t pn_win = UINT64_C(1) << (pnl * 8);
    const uint64_t pn_hwin = pn_win / 2;
    const uint64_t pn_mask = pn_win - 1;

    m->hdr.nr |= (expected_pn & ~pn_mask);
    if (m->hdr.nr + pn_hwin <= expected_pn &&
        likely(m->hdr.nr + pn_hwin < (UINT64_C(1) << 62)))
        m->hdr.nr += pn_win;
    else if (m->hdr.nr > expected_pn + pn_hwin && m->hdr.nr >= pn_win)
        m->hdr.nr -= pn_win;

    return true;
}


static const struct cipher_ctx * __attribute__((nonnull))
which_cipher_ctx_in(struct q_conn * const c,
                    struct pkt_meta * const m,
                    const bool kyph)
{
    switch (m->hdr.type) {
    case LH_INIT:
    case LH_RTRY:
        m->pn = &c->pns[pn_init];
        return &m->pn->early.in;
    case LH_0RTT:
        m->pn = &c->pns[pn_data];
        return &m->pn->data.in_0rtt;
    case LH_HSHK:
        m->pn = &c->pns[pn_hshk];
        return &m->pn->early.in;
    default:
        m->pn = &c->pns[pn_data];
        return &m->pn->data.in_1rtt[kyph ? is_set(SH_KYPH, m->hdr.flags) : 0];
    }
}


struct q_conn * is_srt(const struct w_iov * const xv
#ifdef NO_SRT_MATCHING
                       __attribute__((unused))
#endif
                       ,
                       struct pkt_meta * const m
#ifdef NO_SRT_MATCHING
                       __attribute__((unused))
#endif
)
{
#ifndef NO_SRT_MATCHING
    if ((m->hdr.flags & LH) != HEAD_FIXD || xv->len < MIN_SRT_PKT_LEN)
        return 0;

    uint8_t * const srt = &xv->buf[xv->len - SRT_LEN];
    struct q_conn * const c = get_conn_by_srt(srt);

    if (c && c->state != conn_drng) {
        m->is_reset = true;
        warn(DBG, "stateless reset for %s conn %s", conn_type(c),
             cid_str(c->scid));
        conn_to_state(c, conn_drng);
        enter_closing(c);
        return c;
    }
#endif
    return 0;
}


bool dec_pkt_hdr_remainder(struct w_iov * const xv,
                           struct w_iov * const v,
                           struct pkt_meta * const m,
                           struct q_conn * const c,
                           struct w_iov_sq * const x,
                           bool * const decoal)
{
    *decoal = false;
    const struct cipher_ctx * ctx = which_cipher_ctx_in(c, m, false);
    if (unlikely(ctx->header_protection == 0))
        return false;

    // we can now undo the packet protection
    if (unlikely(undo_hp(xv, m, ctx) == false))
        return is_srt(xv, m);

    // we can now try and decrypt the packet
    struct pn_space * const pn = &c->pns[pn_data];
    struct pn_data * const pnd = &pn->data;
    if (likely(is_lh(m->hdr.flags) == false) &&
        unlikely(is_set(SH_KYPH, m->hdr.flags) != pnd->in_kyph)) {
        if (pnd->out_kyph == pnd->in_kyph)
            // this is a peer-initiated key phase flip
            flip_keys(c, false);
        else
            // the peer switched to a key phase that we flipped
            pnd->in_kyph = pnd->out_kyph;
    }

    ctx = which_cipher_ctx_in(c, m, true);
    if (unlikely(ctx->aead == 0))
        return is_srt(xv, m);

    const uint16_t pkt_len = is_lh(m->hdr.flags) ? m->hdr.hdr_len + m->hdr.len -
                                                       pkt_nr_len(m->hdr.flags)
                                                 : xv->len;
    const uint16_t ret = dec_aead(xv, v, m, pkt_len, ctx);
    if (unlikely(ret == 0))
        return is_srt(xv, m);

    const uint8_t rsvd_bits =
        m->hdr.flags & (is_lh(m->hdr.flags) ? LH_RSVD_MASK : SH_RSVD_MASK);
    if (unlikely(rsvd_bits)) {
        err_close(c, ERR_PROTOCOL_VIOLATION, 0,
                  "reserved %s bits are 0x%02x (= non-zero)",
                  is_lh(m->hdr.flags) ? "LH" : "SH", rsvd_bits);
        return false;
    }

    if (unlikely(is_lh(m->hdr.flags))) {
        // check for coalesced packet
        if (unlikely(pkt_len < xv->len)) {
            *decoal = true;
            // allocate new w_iov for coalesced packet and copy it over
            struct w_iov * const dup = dup_iov(xv, 0, pkt_len);
            // adjust length of first packet
            xv->len = pkt_len;
            // rx() has already removed xv from x, so just insert dup at head
            sq_insert_head(x, dup, next);
            warn(DBG, "split out coalesced %u-byte %s pkt", dup->len,
                 pkt_type_str(*dup->buf, &dup->buf[1]));
        }

    } else {
        // check if a key phase flip has been verified
        const bool v_kyph = is_set(SH_KYPH, m->hdr.flags);
        if (unlikely(v_kyph != pnd->in_kyph))
            pnd->in_kyph = v_kyph;

        if (c->spin_enabled && m->hdr.nr > diet_max(&pn->recv_all))
            // short header, spin the bit
            c->spin = (is_set(SH_SPIN, m->hdr.flags) == !is_clnt(c));
    }

    v->len = xv->len - AEAD_LEN;

    if (!is_clnt(c) && unlikely(m->hdr.type == LH_HSHK && c->cstrms[ep_init])) {
        abandon_pn(&c->pns[pn_init]);

        // server can assume path is validated
        warn(DBG, "clnt path validated");
        c->path_val_win = UINT_T_MAX;
    }

    // packet protection verified OK
    if (diet_find(&pn_for_pkt_type(c, m->hdr.type)->recv_all, m->hdr.nr))
        return is_srt(xv, m);

    // check if we need to send an immediate ACK
    if ((unlikely(diet_empty(&m->pn->recv_all) == false &&
                  m->hdr.nr < diet_max(&m->pn->recv_all)) ||
         (xv->flags & IPTOS_ECN_MASK) == IPTOS_ECN_CE))
        // XXX: this also sends an imm_ack if the reor is "fixed" within a burst
        m->pn->imm_ack = true;

    return true;
}
