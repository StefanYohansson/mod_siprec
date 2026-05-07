/*
 * siprec_sdp.h — SDP body builder for SIPREC INVITE.
 *
 * The SDP half of the multipart MIME body sent on the SIP INVITE
 * from the SRC (Session Recording Client) to the SRS (Session
 * Recording Server). Spec: RFC 7866 §7.
 *
 * Pure C, no FreeSWITCH dependency. Unit-testable.
 *
 * Output ownership: the build function returns a heap-allocated
 * char buffer. Caller frees with siprec_sdp_free().
 */
#ifndef SIPREC_SDP_H
#define SIPREC_SDP_H

#include <stddef.h>
#include <stdint.h>

/* siprec_sdp_track: one m=audio line in the SRC's SDP body.
 * Per RFC 7866 §7.2 a recording group uses one stream per
 * participant per direction; for a typical 2-leg call we send
 * two streams (caller-to-callee, callee-to-caller).
 */
typedef struct {
    /* label is the SDP a=label:N value referenced by the
     * metadata XML. RFC 7866 §8.5: MUST be a non-empty token,
     * unique within the offer. */
    const char *label;

    /* RTP port the SRC will send from (and the SRS will
     * receive on). The SRC MUST advertise a port even though
     * it is sendonly — RFC 7866 §7.4. */
    uint16_t port;

    /* Codec payload type and name. Static PT (0=PCMU, 8=PCMA,
     * etc.) is encoded directly in m=. Dynamic PTs (e.g. opus
     * = 96+) need an a=rtpmap line; we always emit one for
     * clarity. */
    uint8_t pt;
    const char *codec_name;
    uint32_t clock_rate;

    /* For PCMU/PCMA the channels count is 1; opus etc. may be
     * 2. RFC 4566 §6 — the trailing /N on rtpmap is omitted
     * when channels == 1. */
    uint8_t channels;

    /* ptime in milliseconds. 20ms is the carrier standard. */
    uint8_t ptime_ms;
} siprec_sdp_track_t;

typedef struct {
    /* Connection / origin IP for the SDP c= and o= lines.
     * Typically the FS host's external IP. */
    const char *src_ip;

    /* o=- session-id session-version IN IP4 src_ip
     * The session-version is bumped on every re-INVITE per
     * RFC 4566 §5.2. session_id stays stable for the life of
     * the recording. */
    uint64_t session_id;
    uint64_t session_version;

    /* Tracks array. The recommended layout is two tracks for a
     * 2-leg call (one per direction). RFC 7866 §7.5 supports
     * up to N participants → N or 2N tracks. */
    const siprec_sdp_track_t *tracks;
    size_t track_count;
} siprec_sdp_options_t;

/* siprec_sdp_build: render the SDP body to a heap buffer.
 * Returns NULL on allocation failure or invalid input
 * (zero tracks, missing src_ip).
 *
 * Caller frees with siprec_sdp_free().
 */
char *siprec_sdp_build(const siprec_sdp_options_t *opts);

/* siprec_sdp_flip_direction: produce a copy of `src_sdp` with
 * its direction attribute swapped to a=inactive (paused != 0)
 * or a=sendonly (paused == 0), and the o= session-version
 * incremented per RFC 4566 §5.2.
 *
 * Used by the pause/resume re-INVITE path: the recording leg
 * already negotiated a complete local SDP (ports, codec, c=)
 * and a re-INVITE for direction change MUST keep all of that
 * stable while only flipping the direction. Rebuilding from
 * scratch would change the session-id and break dialog
 * continuity at the SRS.
 *
 * Returns a heap buffer (caller frees with siprec_sdp_free)
 * or NULL on allocation failure / malformed input.
 */
char *siprec_sdp_flip_direction(const char *src_sdp, int paused);

/* siprec_sdp_inject_labels: produce a copy of `src_sdp` with
 * `a=label:<n>` injected into every m= block that doesn't
 * already carry a label, where <n> is the 1-based index of
 * that m= block within the SDP (first m= → label:1, second →
 * label:2, etc). The o= session-version is incremented per
 * RFC 4566 §5.2.
 *
 * RFC 7866 §8.5 requires every SRC stream to carry an
 * a=label:N attribute for cross-reference from the metadata
 * XML's <stream> entries. mod_sofia's auto-generated offer
 * SDP doesn't emit a=label, so we do a surgical injection on
 * the local SDP after originate succeeds and re-INVITE the
 * SRS with the labelled body. Same low-risk modification
 * pattern as flip_direction: ports, codec, c=, crypto stay
 * untouched; we only add one attribute line per m= block.
 *
 * The label is placed immediately before the direction
 * attribute (a=sendonly / a=inactive) within each m= block,
 * or at the end of the block if no direction attribute is
 * present. m= blocks that already carry a=label:<anything>
 * are left untouched (idempotent — calling this on an
 * already-labelled SDP is safe; the per-block counter still
 * advances on already-labelled blocks so subsequent unlabelled
 * blocks pick up where the existing labels left off).
 *
 * Today mod_sofia's auto-gen produces a single m=audio so
 * the function effectively emits label:1. Once a multi-track
 * offer path lands (either via a "set local SDP before
 * originate" sofia hook OR via siprec_sdp_build's own
 * multi-track output) the same call site picks up label:1 +
 * label:2 + … without code changes.
 *
 * Returns a heap buffer (caller frees with siprec_sdp_free)
 * or NULL on allocation failure / malformed input.
 */
char *siprec_sdp_inject_labels(const char *src_sdp);

/* Free a buffer returned by siprec_sdp_build /
 * siprec_sdp_flip_direction. NULL-safe. */
void siprec_sdp_free(char *buf);

#endif /* SIPREC_SDP_H */
