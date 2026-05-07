/*
 * siprec_sdp.c — SDP body builder for SIPREC INVITE (RFC 7866 §7).
 *
 * Pure C, no FreeSWITCH dependency. Heap-allocated string output.
 *
 * Implementation strategy: a small growable string buffer (sb_t)
 * accumulates the SDP lines with realloc-on-overflow, then we
 * dup the final buffer trimmed to length. The grow policy
 * doubles capacity to keep amortized cost O(n).
 */
#include "siprec_sdp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────── *
 * Internal: growable string buffer.                           *
 * ──────────────────────────────────────────────────────────── */

/* Hard ceiling for a single SDP body. RFC 4566 doesn't impose
 * one, but real-world SIP UDP messages cap at the path MTU
 * (~1500) and large TCP-fragmented bodies still rarely exceed
 * a few KB. 64 MB is six orders of magnitude beyond anything
 * legitimate; an SDP that hits this is either a memory-pressure
 * attack or a code bug. Failing the build with an error is
 * preferable to OOMing the host. */
#define SB_MAX_CAP ((size_t)64 * 1024 * 1024)

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    err; /* sticky: any allocation failure latches here */
} sb_t;

static void sb_init(sb_t *sb) {
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
    sb->err  = 0;
}

static int sb_reserve(sb_t *sb, size_t want) {
    if (sb->err) {
        return -1;
    }
    if (want <= sb->cap) {
        return 0;
    }
    /* Reject overflow / pathological growth before realloc. */
    if (want > SB_MAX_CAP) {
        sb->err = 1;
        return -1;
    }
    size_t new_cap = sb->cap ? sb->cap : 256;
    /* Guard against new_cap *= 2 wrapping past SIZE_MAX/2 — at
     * that point new_cap stops increasing, which would loop
     * forever. SB_MAX_CAP < SIZE_MAX/2 by construction so the
     * cap ceiling check above already prevents this loop from
     * entering the danger zone, but the explicit check makes
     * the safety property visible to readers and analyzers. */
    while (new_cap < want) {
        if (new_cap > SB_MAX_CAP / 2) {
            new_cap = want;
            break;
        }
        new_cap *= 2;
    }
    char *p = realloc(sb->data, new_cap);
    if (!p) {
        sb->err = 1;
        return -1;
    }
    sb->data = p;
    sb->cap  = new_cap;
    return 0;
}

/* sb_can_grow_by: true iff sb->len + extra + 1 doesn't
 * overflow size_t and stays below SB_MAX_CAP. Callers use
 * this before passing a derived want into sb_reserve. */
static int sb_can_grow_by(const sb_t *sb, size_t extra) {
    /* extra + 1 overflow */
    if (extra > SIZE_MAX - 1) return 0;
    /* sb->len + extra + 1 overflow */
    if (sb->len > SIZE_MAX - extra - 1) return 0;
    /* exceeds SB_MAX_CAP */
    if (sb->len + extra + 1 > SB_MAX_CAP) return 0;
    return 1;
}

static void sb_appendf(sb_t *sb, const char *fmt, ...) {
    if (sb->err) {
        return;
    }
    /* Two-pass: first vsnprintf with len 0 to learn the size,
     * then reserve + format into the real buffer. */
    /* vsnprintf with size=0 is the standard C99 sizing pass;
     * the function is bounded by definition (writes nothing,
     * returns the would-have-been length). Annex K's
     * vsnprintf_s rejects size=0 so cannot replace this. */
    va_list ap;
    va_start(ap, fmt);
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        sb->err = 1;
        return;
    }
    /* Reject before the addition can overflow size_t / breach
     * SB_MAX_CAP. */
    if (!sb_can_grow_by(sb, (size_t)n)) {
        sb->err = 1;
        return;
    }

    size_t need = sb->len + (size_t)n + 1; /* +1 for NUL */
    if (sb_reserve(sb, need) != 0) {
        return;
    }

    /* Bounded: dst capacity is the explicit second argument
     * (sb->cap - sb->len), and sb_reserve above guarantees
     * it covers n+1 bytes. */
    va_start(ap, fmt);
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    int written = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);

    if (written < 0 || (size_t)written >= sb->cap - sb->len) {
        sb->err = 1;
        return;
    }

    sb->len += (size_t)written;
}

static char *sb_take(sb_t *sb) {
    if (sb->err || !sb->data) {
        free(sb->data);
        return NULL;
    }
    /* Shrink-to-fit so the caller doesn't carry a 2x oversized
     * buffer for the lifetime of the SDP. realloc-failure path
     * keeps the original (oversized) buffer per C11 §7.22.3.5;
     * cppcheck's "memleak" hint doesn't grok that. */
    /* cppcheck-suppress memleak */
    char *p = realloc(sb->data, sb->len + 1);
    if (!p) {
        return sb->data;
    }
    return p;
}

/* ──────────────────────────────────────────────────────────── *
 * SDP rendering.                                              *
 * ──────────────────────────────────────────────────────────── */

/* CRLF terminators per RFC 4566 §6 — "lines are terminated by
 * CRLF, but parsers SHOULD be tolerant and also accept lines
 * terminated with a single newline character". We emit CRLF to
 * be conservative; downstream parsers all handle it. */
#define EOL "\r\n"

static int validate_track(const siprec_sdp_track_t *t) {
    if (!t->label || !*t->label) return 0;
    if (!t->codec_name || !*t->codec_name) return 0;
    if (t->port == 0) return 0;
    if (t->clock_rate == 0) return 0;
    return 1;
}

static int validate_options(const siprec_sdp_options_t *opts) {
    if (!opts) return 0;
    if (!opts->src_ip || !*opts->src_ip) return 0;
    if (opts->track_count == 0 || !opts->tracks) return 0;
    for (size_t i = 0; i < opts->track_count; i++) {
        if (!validate_track(&opts->tracks[i])) return 0;
    }
    return 1;
}

char *siprec_sdp_build(const siprec_sdp_options_t *opts) {
    if (!validate_options(opts)) {
        return NULL;
    }

    sb_t sb;
    sb_init(&sb);

    /* v=0 — RFC 4566 §5.1 */
    sb_appendf(&sb, "v=0" EOL);

    /* o=<user> <session-id> <session-version> <nettype>
     *   <addrtype> <unicast-address>
     * RFC 4566 §5.2. We use "-" for the user (Twilio convention,
     * SRC identity is in the SIP From: header anyway). */
    sb_appendf(&sb, "o=- %llu %llu IN IP4 %s" EOL,
        (unsigned long long)opts->session_id,
        (unsigned long long)opts->session_version,
        opts->src_ip);

    /* s=- — session name; RFC 4566 §5.3 requires a non-empty s=
     * line. "-" is the conventional placeholder. */
    sb_appendf(&sb, "s=-" EOL);

    /* c=IN IP4 <src_ip> — session-level connection. RFC 7866
     * §7.2 examples put it at session level so each m=audio
     * inherits unless overridden. */
    sb_appendf(&sb, "c=IN IP4 %s" EOL, opts->src_ip);

    /* t=0 0 — unbounded session lifetime. SIPREC sessions live
     * as long as the original call; no NTP timing info applies. */
    sb_appendf(&sb, "t=0 0" EOL);

    /* No a=group:DUP. RFC 5888 §5 defines DUP as duplication
     * semantics — packets of the same content carried over
     * multiple transports for redundancy. SIPREC's per-direction
     * streams carry DISTINCT audio (caller-side vs callee-side),
     * not duplicates, so DUP would mislead the SRS. RFC 7866
     * does not mandate any specific group attribute; the
     * <participantstreamassoc> entries in the metadata XML
     * already correlate streams to participants. */

    /* One m=audio block per track. */
    for (size_t i = 0; i < opts->track_count; i++) {
        const siprec_sdp_track_t *t = &opts->tracks[i];

        /* m=audio <port> RTP/AVP <pt>
         * RFC 4566 §5.14. SRC always offers a single PT per
         * stream (per RFC 7866 examples) — no codec list. */
        sb_appendf(&sb, "m=audio %u RTP/AVP %u" EOL,
            (unsigned)t->port, (unsigned)t->pt);

        /* a=rtpmap:<pt> <name>/<clock>[/<channels>]
         * RFC 4566 §6. Channels suffix omitted when channels==1
         * (the default for telephony codecs). */
        if (t->channels > 1) {
            sb_appendf(&sb, "a=rtpmap:%u %s/%u/%u" EOL,
                (unsigned)t->pt, t->codec_name,
                (unsigned)t->clock_rate, (unsigned)t->channels);
        } else {
            sb_appendf(&sb, "a=rtpmap:%u %s/%u" EOL,
                (unsigned)t->pt, t->codec_name,
                (unsigned)t->clock_rate);
        }

        /* a=ptime:<ms> — RFC 4566 §6. */
        if (t->ptime_ms > 0) {
            sb_appendf(&sb, "a=ptime:%u" EOL, (unsigned)t->ptime_ms);
        }

        /* a=label:<token> — RFC 7866 §8.5 — REQUIRED on every
         * SRC stream. Cross-referenced from the metadata XML. */
        sb_appendf(&sb, "a=label:%s" EOL, t->label);

        /* a=sendonly — RFC 7866 §7.4. The SRC only sends; the
         * SRS records but does not send media back. */
        sb_appendf(&sb, "a=sendonly" EOL);
    }

    return sb_take(&sb);
}

/* ──────────────────────────────────────────────────────────── *
 * SDP direction-flip                                          *
 * ──────────────────────────────────────────────────────────── */

/* sb_append: raw byte append (no formatting). Used by the
 * direction-flip helper to copy unmodified lines straight
 * through.
 *
 * Early-return on n == 0: ISO C 7.24.2.1 makes memcpy with a
 * NULL src or dst undefined behavior even when count is zero.
 * The flip-direction walker legitimately calls sb_append with
 * n == 0 when src_sdp contains adjacent line terminators
 * ("\r\n\r\n"), and on the very first call sb->data is still
 * NULL because sb_reserve hasn't run yet. Skip the memcpy in
 * that path so we never construct the UB even though no
 * implementation actually crashes on it. */
static void sb_append(sb_t *sb, const char *s, size_t n) {
    if (sb->err) return;
    if (n == 0) return;
    /* Reject before the addition can overflow size_t / breach
     * SB_MAX_CAP. */
    if (!sb_can_grow_by(sb, n)) { sb->err = 1; return; }
    if (sb_reserve(sb, sb->len + n + 1) != 0) return;
    /* sb_reserve only returns 0 on a successful allocation, so
     * sb->data is non-NULL here. The redundant check exists for
     * flow-sensitive analyzers that don't propagate
     * sb_reserve's post-condition. */
    if (!sb->data) { sb->err = 1; return; }
    /* Bounded: n is the explicit length, dst tail capacity
     * was just guaranteed by sb_reserve(len+n+1). */
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

/* siprec_sdp_inject_labels: walk src_sdp line by line,
 * emitting each line verbatim except:
 *   - o= line: bump session-version per RFC 4566 §5.2
 *   - within each m= block, inject "a=label:<n>\r\n" if the
 *     block doesn't already carry one, where <n> is the
 *     1-based ordinal of the m= block. Placed immediately
 *     before the direction attribute (a=sendonly etc) for
 *     ordering parity with siprec_sdp_build's output.
 *
 * The block counter advances on EVERY m= block — including
 * blocks that already carry their own a=label — so a partially
 * labelled SDP gets unique sequential labels for the missing
 * positions. (Worth noting: an already-labelled "label:1" on
 * the first block followed by an unlabelled second block ends
 * up with label:2 on the second; we never collide.)
 *
 * Idempotent: m= blocks that already have an a=label line are
 * passed through unchanged (a second call adds no second
 * label, only bumps the version).
 *
 * Walk loop is parallel to siprec_sdp_flip_direction's; the
 * two functions intentionally don't share a callback-based
 * helper because the inject path needs cross-line state
 * (block_index / current_block_has_label / label_emitted) that
 * doesn't fit a stateless filter cleanly. */
char *siprec_sdp_inject_labels(const char *src_sdp) {
    if (!src_sdp || !*src_sdp) return NULL;

    sb_t sb;
    sb_init(&sb);

    /* block_index counts m= blocks observed so far (1-based at
     * emission time — incremented when we enter a block, so
     * the first m= block has block_index == 1). */
    int block_index             = 0;
    int current_block_has_label = 0;
    int label_emitted_for_block = 0;

    const char *p = src_sdp;
    while (*p) {
        const char *eol = strpbrk(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        if (line_len >= 2 && p[0] == 'o' && p[1] == '=') {
            char     user[64], id[64], net[16], at[16], addr[64];
            unsigned long long ver = 0;
            /* Bounded by %63s / %15s widths. */
            /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
            int n = sscanf(p, "o=%63s %63s %llu %15s %15s %63s",
                user, id, &ver, net, at, addr);
            if (n == 6) {
                sb_appendf(&sb, "o=%s %s %llu %s %s %s",
                    user, id, ver + 1ULL, net, at, addr);
            } else {
                sb_append(&sb, p, line_len);
            }
        } else if (line_len >= 2 && p[0] == 'm' && p[1] == '=') {
            /* New media block. If the previous block was
             * unlabelled and we hadn't emitted yet, do it now
             * (covers the corner case of an m= block with no
             * direction attribute — terminator is the next m=
             * line). The label uses the PREVIOUS block_index
             * because we haven't bumped for this new block yet. */
            if (block_index > 0 && !current_block_has_label && !label_emitted_for_block) {
                sb_appendf(&sb, "a=label:%d\r\n", block_index);
            }
            block_index++;
            current_block_has_label = 0;
            label_emitted_for_block = 0;
            sb_append(&sb, p, line_len);
        } else if (block_index > 0 && line_len >= 8 && memcmp(p, "a=label:", 8) == 0) {
            current_block_has_label = 1;
            sb_append(&sb, p, line_len);
        } else if (block_index > 0 && !current_block_has_label && !label_emitted_for_block
                   && line_len == 10 &&
                   (memcmp(p, "a=sendonly", 10) == 0 ||
                    memcmp(p, "a=inactive", 10) == 0 ||
                    memcmp(p, "a=recvonly", 10) == 0 ||
                    memcmp(p, "a=sendrecv", 10) == 0)) {
            sb_appendf(&sb, "a=label:%d\r\n", block_index);
            label_emitted_for_block = 1;
            sb_append(&sb, p, line_len);
        } else {
            sb_append(&sb, p, line_len);
        }

        if (!eol) break;
        if (eol[0] == '\r' && eol[1] == '\n') {
            sb_append(&sb, "\r\n", 2);
            p = eol + 2;
        } else {
            sb_append(&sb, eol, 1);
            p = eol + 1;
        }
    }

    /* Trailing m= block with neither a=label nor a direction
     * attribute — append the label at end-of-SDP. */
    if (block_index > 0 && !current_block_has_label && !label_emitted_for_block) {
        sb_appendf(&sb, "a=label:%d\r\n", block_index);
    }

    return sb_take(&sb);
}

char *siprec_sdp_flip_direction(const char *src_sdp, int paused) {
    if (!src_sdp || !*src_sdp) return NULL;

    const char *target = paused ? "a=inactive" : "a=sendonly";
    const size_t target_len = strlen(target);

    sb_t sb;
    sb_init(&sb);

    const char *p = src_sdp;
    while (*p) {
        /* Locate end-of-line. SDP lines are CRLF-terminated
         * per RFC 4566 §6 but accept LF-only for tolerance. */
        const char *eol = strpbrk(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        if (line_len >= 2 && p[0] == 'o' && p[1] == '=') {
            /* o=<user> <session-id> <session-version>
             *   <nettype> <addrtype> <unicast-address>
             *
             * RFC 4566 §5.2: session-version MUST be incremented
             * on every modification. We bump by 1; sscanf with
             * %llu handles values up to 64-bit per the RFC. */
            char     user[64], id[64], net[16], at[16], addr[64];
            unsigned long long ver = 0;
            /* Bounded: every %s specifier carries an explicit
             * width (%63s / %15s) that fits its destination
             * buffer with room for the trailing NUL. */
            /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
            int n = sscanf(p, "o=%63s %63s %llu %15s %15s %63s",
                user, id, &ver, net, at, addr);
            if (n == 6) {
                sb_appendf(&sb, "o=%s %s %llu %s %s %s",
                    user, id, ver + 1ULL, net, at, addr);
            } else {
                /* Malformed o= — pass through unchanged rather
                 * than guess at a fix. */
                sb_append(&sb, p, line_len);
            }
        } else if (line_len == 10 &&
                   (memcmp(p, "a=sendonly", 10) == 0 ||
                    memcmp(p, "a=recvonly", 10) == 0 ||
                    memcmp(p, "a=sendrecv", 10) == 0 ||
                    memcmp(p, "a=inactive", 10) == 0)) {
            /* RFC 4566 §6: direction attributes are exactly
             * 10 chars on a line of their own (no parameters).
             * Matching on exact length avoids accidentally
             * rewriting "a=sendonlysomething". */
            sb_append(&sb, target, target_len);
        } else {
            sb_append(&sb, p, line_len);
        }

        if (!eol) break;

        if (eol[0] == '\r' && eol[1] == '\n') {
            sb_append(&sb, "\r\n", 2);
            p = eol + 2;
        } else {
            sb_append(&sb, eol, 1);
            p = eol + 1;
        }
    }

    return sb_take(&sb);
}

void siprec_sdp_free(char *buf) {
    free(buf);
}
