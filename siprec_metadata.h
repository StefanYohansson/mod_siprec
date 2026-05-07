/*
 * siprec_metadata.h — RFC 7865 recording metadata XML builder.
 *
 * The XML half of the multipart MIME body sent on the SIP INVITE
 * from the SRC to the SRS. Spec: RFC 7865 §5.
 *
 * Pure C, no FreeSWITCH dependency. Heap-allocated string output.
 */
#ifndef SIPREC_METADATA_H
#define SIPREC_METADATA_H

#include <stddef.h>
#include <stdint.h>

/* siprec_metadata_stream: one media stream entry. RFC 7865 §5
 * defines stream as the unit of recordable media — for a 2-leg
 * audio call we typically have 2 streams (one per direction). */
typedef struct {
    /* stream_id is the unique identifier referenced from the
     * SDP's a=label and from the participant binding emitted
     * inside <participantstreamassoc>'s <send>/<recv> child
     * elements. RFC 7865 §5 — a UUID URN is the canonical form,
     * e.g. "urn:uuid:55c93cc4-...". */
    const char *stream_id;

    /* Mode determines the XML element under <participant> that
     * cross-references this stream — <send> for SRC-to-SRS,
     * <recv> for SRS-to-SRC (currently never; SRC is sendonly).
     * For our SRC-only implementation every stream is "send". */
    enum {
        SIPREC_STREAM_SEND,
        SIPREC_STREAM_RECV,
    } mode;

    /* The participant this stream belongs to. Index into the
     * participants array. */
    size_t participant_idx;

    /* RFC 7865 Appendix A streamtype: only <label> is a typed
     * in-namespace child. The label MUST match the corresponding
     * SDP a=label attribute on the SRC INVITE so the SRS can
     * correlate the metadata stream with the actual RTP session
     * (RFC 7866 §8.5). NULL omits the element (the stream
     * becomes self-closing). */
    const char *label;
} siprec_metadata_stream_t;

typedef struct {
    /* participant_id — UUID URN. RFC 7865 §5. */
    const char *participant_id;

    /* nameID/aor — the SIP AOR ("Address of Record") of the
     * participant. RFC 7865 §5: "sip:caller@example.com". */
    const char *aor;

    /* Optional display name (RFC 7865 §5 nameID/name).
     * NULL omits the element. */
    const char *display_name;
} siprec_metadata_participant_t;

/* siprec_metadata_datamode: RFC 7865 §5.1 <datamode>.
 *   COMPLETE — full snapshot; the SRS replaces any prior state.
 *   PARTIAL  — delta update (re-INVITE); the SRS merges with
 *              the existing state.
 * v1 sends COMPLETE on initial INVITE and PARTIAL only when
 * driving a re-INVITE that carries a participant change.
 */
typedef enum {
    SIPREC_DATAMODE_COMPLETE = 0,
    SIPREC_DATAMODE_PARTIAL  = 1,
} siprec_metadata_datamode_t;

typedef struct {
    /* session_id — UUID URN identifying the recording session.
     * Stays stable across re-INVITEs. RFC 7865 §5. */
    const char *session_id;

    /* group_id — RFC 7865 Appendix A grouptype — groups
     * participants of the same logical conversation. For a
     * 1-call recording, all participants share one group.
     * NULL omits the <group>. When set, <session> also gains
     * a <group-ref> child (RFC 7865 sessiontype) binding the
     * session to its group. */
    const char *group_id;

    /* ISO-8601 timestamp, e.g. "2026-05-06T03:00:00Z".
     * Emitted as:
     *   <session>'s <start-time> child
     *   <group>'s <associate-time> child (when group_id set)
     *   <participantsessionassoc>'s <associate-time> child
     * NULL omits everywhere. */
    const char *associate_time_utc;

    /* RFC 7865 §5.1 <datamode>. Default (zero value) is
     * COMPLETE. Re-INVITE updates that carry only changed
     * fields should set this to PARTIAL. */
    siprec_metadata_datamode_t datamode;

    /* Optional session-level <reason> (RFC 7865 Appendix A
     * sessiontype). Surfaces inside <session>; common values
     * are "terminated", "paused", "resumed". NULL omits.
     *
     * <group> has no <reason> in the schema (grouptype only
     * declares associate-time / disassociate-time / extension
     * children) so a group-level reason field would be
     * non-conformant — there is none here. */
    const char *session_reason;

    /* Participants array. */
    const siprec_metadata_participant_t *participants;
    size_t participant_count;

    /* Streams array. Cross-references participants by index. */
    const siprec_metadata_stream_t *streams;
    size_t stream_count;
} siprec_metadata_options_t;

/* siprec_metadata_build: render the XML to a heap buffer.
 * Returns NULL on allocation failure or invalid input.
 *
 * Caller frees with siprec_metadata_free().
 */
char *siprec_metadata_build(const siprec_metadata_options_t *opts);

/* Free a buffer returned by siprec_metadata_build. NULL-safe. */
void siprec_metadata_free(char *buf);

#endif /* SIPREC_METADATA_H */
