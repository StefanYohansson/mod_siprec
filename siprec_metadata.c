/*
 * siprec_metadata.c — RFC 7865 recording metadata XML builder.
 *
 * Pure C. The output document conforms to the schema in
 * RFC 7865 §5 (urn:ietf:params:xml:ns:recording:1).
 *
 * Element order matches the IANA-registered schema's
 * complexType <recording>. Attribute names are case-sensitive.
 *
 * XML content is escaped via xml_escape_into() to keep
 * caller-provided strings (display names, AORs, etc.) from
 * breaking the parse on the SRS side. The escaping covers the
 * RFC 4627-style five entities (& < > " ').
 */
#include "siprec_metadata.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Re-use the same string-builder pattern as siprec_sdp.c. The
 * code is small enough that duplicating it keeps the two
 * compilation units independent (no shared header dependency).
 *
 * SB_MAX_CAP / sb_can_grow_by — same rationale as siprec_sdp.c.
 * 64 MB ceiling is a memory-pressure / DoS guard. The
 * sb_can_grow_by helper keeps the size_t arithmetic
 * overflow-safe at the call sites. */
#define SB_MAX_CAP ((size_t)64 * 1024 * 1024)

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    err;
} sb_t;

static void sb_init(sb_t *sb) {
    sb->data = NULL; sb->len = 0; sb->cap = 0; sb->err = 0;
}

static int sb_reserve(sb_t *sb, size_t want) {
    if (sb->err) return -1;
    if (want <= sb->cap) return 0;
    if (want > SB_MAX_CAP) { sb->err = 1; return -1; }
    size_t new_cap = sb->cap ? sb->cap : 512;
    while (new_cap < want) {
        if (new_cap > SB_MAX_CAP / 2) {
            new_cap = want;
            break;
        }
        new_cap *= 2;
    }
    char *p = realloc(sb->data, new_cap);
    if (!p) { sb->err = 1; return -1; }
    sb->data = p; sb->cap = new_cap;
    return 0;
}

static int sb_can_grow_by(const sb_t *sb, size_t extra) {
    if (extra > SIZE_MAX - 1) return 0;
    if (sb->len > SIZE_MAX - extra - 1) return 0;
    if (sb->len + extra + 1 > SB_MAX_CAP) return 0;
    return 1;
}

static void sb_append(sb_t *sb, const char *s, size_t n) {
    if (sb->err) return;
    /* ISO C 7.24.2.1: memcpy with a NULL pointer is undefined
     * behavior even when count is zero. Short-circuit before
     * we can construct the call. xml_escape_into can call us
     * with n == 0 in degenerate paths, and on the very first
     * call sb->data is still NULL. */
    if (n == 0) return;
    /* Reject overflowing / over-cap appends. */
    if (!sb_can_grow_by(sb, n)) { sb->err = 1; return; }
    if (sb_reserve(sb, sb->len + n + 1) != 0) return;
    /* sb_reserve only returns 0 with sb->data set; redundant
     * re-check for flow-sensitive analyzers. */
    if (!sb->data) { sb->err = 1; return; }
    /* Bounded: n is the explicit length, sb_reserve above
     * guaranteed at least n+1 bytes of tail capacity. */
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(sb_t *sb, const char *fmt, ...) {
    if (sb->err) return;
    /* Sizing pass — vsnprintf(NULL, 0, ...) is the canonical
     * C99 way to learn the formatted length without writing
     * anything. Bounded by definition. */
    va_list ap;
    va_start(ap, fmt);
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { sb->err = 1; return; }
    /* Reject overflowing / over-cap formatted appends. */
    if (!sb_can_grow_by(sb, (size_t)n)) { sb->err = 1; return; }
    if (sb_reserve(sb, sb->len + (size_t)n + 1) != 0) return;
    /* Bounded: dst capacity is the explicit second argument
     * and sb_reserve above guaranteed it covers n+1 bytes. */
    va_start(ap, fmt);
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    int written = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= sb->cap - sb->len) {
        sb->err = 1; return;
    }
    sb->len += (size_t)written;
}

static char *sb_take(sb_t *sb) {
    if (sb->err || !sb->data) {
        free(sb->data);
        return NULL;
    }
    /* shrink-to-fit. On realloc failure (rare) the original
     * buffer is still valid per C11 §7.22.3.5 — we just keep
     * the oversized one. cppcheck flags this as a leak; it
     * isn't, since the failure branch never frees. */
    char *p = realloc(sb->data, sb->len + 1);
    /* cppcheck-suppress memleak */
    return p ? p : sb->data;
}

/* xml_escape_into: append `s` to `sb`, replacing the five XML
 * entities with their numeric/named references. Used wherever
 * caller-supplied strings (AORs, display names, IDs) land in
 * element content or attribute values. */
static void xml_escape_into(sb_t *sb, const char *s) {
    if (!s) return;
    while (*s) {
        switch (*s) {
            case '&':  sb_append(sb, "&amp;",  5); break;
            case '<':  sb_append(sb, "&lt;",   4); break;
            case '>':  sb_append(sb, "&gt;",   4); break;
            case '"':  sb_append(sb, "&quot;", 6); break;
            case '\'': sb_append(sb, "&apos;", 6); break;
            default:   sb_append(sb, s, 1);        break;
        }
        s++;
    }
}

/* ──────────────────────────────────────────────────────────── *
 * Validation                                                  *
 * ──────────────────────────────────────────────────────────── */

static int validate_options(const siprec_metadata_options_t *opts) {
    if (!opts) return 0;
    if (!opts->session_id || !*opts->session_id) return 0;
    if (opts->participant_count == 0 || !opts->participants) return 0;

    for (size_t i = 0; i < opts->participant_count; i++) {
        const siprec_metadata_participant_t *p = &opts->participants[i];
        if (!p->participant_id || !*p->participant_id) return 0;
        if (!p->aor || !*p->aor) return 0;
    }

    /* Streams may legitimately be zero (a session with declared
     * participants but no media yet — re-INVITE will add them
     * later). When present, each stream's participant_idx must
     * be in range. */
    for (size_t i = 0; i < opts->stream_count; i++) {
        const siprec_metadata_stream_t *s = &opts->streams[i];
        if (!s->stream_id || !*s->stream_id) return 0;
        if (s->participant_idx >= opts->participant_count) return 0;
    }

    return 1;
}

/* ──────────────────────────────────────────────────────────── *
 * Build                                                       *
 * ──────────────────────────────────────────────────────────── */

char *siprec_metadata_build(const siprec_metadata_options_t *opts) {
    if (!validate_options(opts)) {
        return NULL;
    }

    sb_t sb;
    sb_init(&sb);

    /* XML prolog. RFC 7865 examples use UTF-8. */
    sb_appendf(&sb,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<recording xmlns=\"urn:ietf:params:xml:ns:recording:1\">\r\n");

    /* <datamode> — RFC 7865 §5.1. "complete" means this body is
     * a full snapshot; "partial" is a delta update typically
     * carried on a re-INVITE so the SRS merges rather than
     * replaces existing state. */
    const char *dm_str =
        (opts->datamode == SIPREC_DATAMODE_PARTIAL)
            ? "partial" : "complete";
    sb_appendf(&sb, "  <datamode>%s</datamode>\r\n", dm_str);

    /* <group group_id="..."> — RFC 7865 Appendix A grouptype.
     *
     *   <xs:complexType name="group">
     *     <xs:sequence>
     *       <xs:element name="associate-time" type="dateTime"/>
     *       <xs:element name="disassociate-time" type="dateTime"/>
     *       <xs:any namespace='##other' .../>
     *     </xs:sequence>
     *     <xs:attribute name="group_id" use="required"/>
     *   </xs:complexType>
     *
     * No <reason> child in the schema; only the timestamps
     * and extension elements. */
    int group_has_body =
        opts->group_id && *opts->group_id &&
        opts->associate_time_utc && *opts->associate_time_utc;

    if (opts->group_id && *opts->group_id) {
        sb_appendf(&sb, "  <group group_id=\"");
        xml_escape_into(&sb, opts->group_id);
        sb_appendf(&sb, "\"");

        if (group_has_body) {
            sb_appendf(&sb, ">\r\n");
            sb_appendf(&sb, "    <associate-time>");
            xml_escape_into(&sb, opts->associate_time_utc);
            sb_appendf(&sb, "</associate-time>\r\n");
            sb_appendf(&sb, "  </group>\r\n");
        } else {
            sb_appendf(&sb, "/>\r\n");
        }
    }

    /* <session session_id="..."> — only attribute is
     * session_id (RFC 7865 Appendix A sessiontype).
     *
     * Children appear in schema order:
     *   <sipSessionID>     — omitted (we don't track it).
     *   <reason>           — opts->session_reason.
     *   <group-ref>        — opts->group_id (NB: child element
     *                        with hyphen, NOT a group_ref
     *                        attribute).
     *   <start-time>       — opts->associate_time_utc.
     *
     * The session has no <associate-time> element in the
     * schema; that name belongs to <group> and the assoc
     * elements. <session> uses <start-time> / <stop-time>
     * instead. */
    sb_appendf(&sb, "  <session session_id=\"");
    xml_escape_into(&sb, opts->session_id);
    sb_appendf(&sb, "\"");

    int session_has_body =
        (opts->session_reason && *opts->session_reason) ||
        (opts->group_id && *opts->group_id) ||
        (opts->associate_time_utc && *opts->associate_time_utc);

    if (!session_has_body) {
        sb_appendf(&sb, "/>\r\n");
    } else {
        sb_appendf(&sb, ">\r\n");
        if (opts->session_reason && *opts->session_reason) {
            sb_appendf(&sb, "    <reason>");
            xml_escape_into(&sb, opts->session_reason);
            sb_appendf(&sb, "</reason>\r\n");
        }
        if (opts->group_id && *opts->group_id) {
            sb_appendf(&sb, "    <group-ref>");
            xml_escape_into(&sb, opts->group_id);
            sb_appendf(&sb, "</group-ref>\r\n");
        }
        if (opts->associate_time_utc && *opts->associate_time_utc) {
            sb_appendf(&sb, "    <start-time>");
            xml_escape_into(&sb, opts->associate_time_utc);
            sb_appendf(&sb, "</start-time>\r\n");
        }
        sb_appendf(&sb, "  </session>\r\n");
    }

    /* <participant> entries. Per RFC 7865 Appendix A
     * (participanttype):
     *
     *   <xs:complexType name="participant">
     *     <xs:sequence>
     *       <xs:element name="nameID" .../>
     *       <xs:any namespace='##other' .../>
     *     </xs:sequence>
     *     <xs:attribute name="participant_id" use="required"/>
     *   </xs:complexType>
     *
     * Only `participant_id` is a valid attribute, and the only
     * in-namespace child is <nameID>. The participant→stream
     * mapping lives exclusively in <participantstreamassoc>;
     * <reason> is a session-level element. */
    for (size_t i = 0; i < opts->participant_count; i++) {
        const siprec_metadata_participant_t *p = &opts->participants[i];

        sb_appendf(&sb, "  <participant participant_id=\"");
        xml_escape_into(&sb, p->participant_id);
        sb_appendf(&sb, "\">\r\n");

        sb_appendf(&sb, "    <nameID aor=\"");
        xml_escape_into(&sb, p->aor);
        sb_appendf(&sb, "\"");
        if (p->display_name && *p->display_name) {
            sb_appendf(&sb, ">\r\n      <name>");
            xml_escape_into(&sb, p->display_name);
            sb_appendf(&sb, "</name>\r\n    </nameID>\r\n");
        } else {
            sb_appendf(&sb, "/>\r\n");
        }

        sb_appendf(&sb, "  </participant>\r\n");
    }

    /* <stream> entries — RFC 7865 Appendix A streamtype:
     *
     *   <xs:complexType name="stream">
     *     <xs:sequence>
     *       <xs:element name="label" minOccurs="0" maxOccurs="1"
     *                   type="xs:string"/>
     *       <xs:any namespace='##other' .../>
     *     </xs:sequence>
     *     <xs:attribute name="stream_id" use="required"/>
     *     <xs:attribute name="session_id"/>
     *   </xs:complexType>
     *
     * Only <label> is a typed in-namespace child; media type
     * is conveyed by the SDP, not by a recording: element. A
     * stream with no label collapses to a self-closing tag.
     *
     * Schema sequence: <stream> sits between <participant>
     * and the assoc elements. */
    for (size_t i = 0; i < opts->stream_count; i++) {
        const siprec_metadata_stream_t *s = &opts->streams[i];

        sb_appendf(&sb, "  <stream stream_id=\"");
        xml_escape_into(&sb, s->stream_id);
        sb_appendf(&sb, "\" session_id=\"");
        xml_escape_into(&sb, opts->session_id);
        sb_appendf(&sb, "\"");

        if (s->label && *s->label) {
            sb_appendf(&sb, ">\r\n    <label>");
            xml_escape_into(&sb, s->label);
            sb_appendf(&sb, "</label>\r\n  </stream>\r\n");
        } else {
            sb_appendf(&sb, "/>\r\n");
        }
    }

    /* <participantsessionassoc> — RFC 7865 Appendix A
     * participantsessionassoc complexType. Both participant_id
     * and session_id are required. Each declared participant
     * gets one binding so an SRS that prefers explicit
     * associations over schema implication has a definitive
     * source. Schema sequence: comes after <stream>. */
    for (size_t i = 0; i < opts->participant_count; i++) {
        const siprec_metadata_participant_t *p = &opts->participants[i];

        sb_appendf(&sb, "  <participantsessionassoc participant_id=\"");
        xml_escape_into(&sb, p->participant_id);
        sb_appendf(&sb, "\" session_id=\"");
        xml_escape_into(&sb, opts->session_id);
        sb_appendf(&sb, "\"");

        if (opts->associate_time_utc && *opts->associate_time_utc) {
            sb_appendf(&sb, ">\r\n    <associate-time>");
            xml_escape_into(&sb, opts->associate_time_utc);
            sb_appendf(&sb,
                "</associate-time>\r\n  </participantsessionassoc>\r\n");
        } else {
            sb_appendf(&sb, "/>\r\n");
        }
    }

    /* <participantstreamassoc> — RFC 7865 Appendix A
     * participantstreamassoc complexType. Carries the
     * participant→stream mapping (this is the ONLY place
     * <send>/<recv> appear; they are not allowed inside
     * <participant>). Schema declares only participant_id
     * as a required attribute. */
    for (size_t i = 0; i < opts->stream_count; i++) {
        const siprec_metadata_stream_t *s = &opts->streams[i];
        if (s->participant_idx >= opts->participant_count) continue;
        const siprec_metadata_participant_t *p =
            &opts->participants[s->participant_idx];

        sb_appendf(&sb, "  <participantstreamassoc participant_id=\"");
        xml_escape_into(&sb, p->participant_id);
        sb_appendf(&sb, "\">\r\n");

        const char *tag =
            (s->mode == SIPREC_STREAM_RECV) ? "recv" : "send";
        sb_appendf(&sb, "    <%s>", tag);
        xml_escape_into(&sb, s->stream_id);
        sb_appendf(&sb, "</%s>\r\n", tag);

        sb_appendf(&sb, "  </participantstreamassoc>\r\n");
    }

    sb_appendf(&sb, "</recording>\r\n");

    return sb_take(&sb);
}

void siprec_metadata_free(char *buf) {
    free(buf);
}
