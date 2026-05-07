/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Stefan Yohansson <stefan.yohansson@agnesit.tech>
 *
 *
 * mod_siprec.c -- SIPRec RFC 7866 implementation
 *
 */
#include <switch.h>

#include <time.h>

#include "mod_siprec.h"
#include "recording_session.h"
#include "siprec_invite.h"
#include "siprec_media.h"
#include "siprec_metadata.h"

static switch_status_t my_on_destroy(switch_core_session_t *session)
{
    switch_assert(session);
    if (stop_recording_session(session) == SWITCH_STATUS_FALSE) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Failed to stop recording session\n");
    }
    return SWITCH_STATUS_SUCCESS;
}

/* state_handlers gets attached to the original call's channel
 * inside start_recording_session via switch_channel_add_state_handler.
 * SSH_FLAG_STICKY keeps the handler bound across dialplan
 * transfers so a recording started during the IVR phase still
 * fires on_destroy when the bridged leg hangs up. */
static switch_state_handler_table_t state_handlers = {
    /*.on_init */ NULL,
    /*.on_routing */ NULL,
    /*.on_execute */ NULL,
    /*.on_hangup */ NULL,
    /*.on_exchange_media */ NULL,
    /*.on_soft_execute */ NULL,
    /*.on_consume_media */ NULL,
    /*.on_hibernate */ NULL,
    /*.on_reset */ NULL,
    /*.on_park */ NULL,
    /*.on_reporting */ NULL,
    /*.on_destroy */ my_on_destroy,
    SSH_FLAG_STICKY
};


switch_status_t stop_recording_session(switch_core_session_t *session)
{
    char *recording_key = NULL;
    recording_t *recording = NULL;
    recording_server_t *recording_server = NULL;
    switch_hash_index_t *hi;
    void *val;
    const void *vvar;

    switch_mutex_lock(globals.recording_servers_mutex);
    for (hi = switch_core_hash_first(globals.recording_servers_hash); hi; hi = switch_core_hash_next(&hi)) {
        switch_core_hash_this(hi, &vvar, NULL, &val);
        recording_server = (recording_server_t *) val;

        recording_key = switch_mprintf("%s-%s", recording_server->name, switch_core_session_get_uuid(session));

        switch_mutex_lock(globals.recordings_mutex);
        recording = switch_core_hash_find(globals.recordings_hash, recording_key);
        switch_mutex_unlock(globals.recordings_mutex);

        switch_safe_free(recording_key);

        if (!recording) {
            continue;
        }

        /* Tear down media + signalling BEFORE removing the
         * hash entry — the bug callback may still fire while
         * the bug is being removed; siprec_media_detach
         * handles that ordering safely.
         *
         * The order matters: detach the media bug first
         * (stops new RTP from being sent), then BYE the
         * recording leg (allows the SRS to flush any pending
         * write before the dialog closes).
         */
        siprec_media_detach(recording);
        siprec_invite_send_bye(recording);

        switch_mutex_lock(globals.recordings_mutex);
        switch_core_hash_delete(globals.recordings_hash, recording->key);
        switch_mutex_unlock(globals.recordings_mutex);

        /* Tear down the recording's pool. The original code
         * removed the hash entry but never destroyed the
         * pool — every <Stop><Siprec/> leaked the recording_t
         * struct and its pool's full allocation arena until
         * module shutdown rolled them up via the shutdown
         * function's hash walk. With per-call dispatch on a
         * multi-tenant box, the leak compounds. */
        switch_core_destroy_memory_pool(&recording->pool);
    }
    switch_mutex_unlock(globals.recording_servers_mutex);

    return SWITCH_STATUS_SUCCESS;
}

/* SDP / negotiated-port allocation for the recording leg's
 * SIP-side media is delegated to mod_sofia: the outbound
 * originate uses the profile's rtp-port-min/-max range, with
 * `local_ip_v4` selecting the bind address.
 *
 * The RTP fork itself opens its own UDP sockets in
 * siprec_media_attach (one per stream, kernel-assigned
 * ephemeral source port) and sends to the SRS-side endpoint
 * parsed out of the 200 OK answer SDP. The recording-leg
 * SIP session and the RTP fork are independent transport
 * channels — sofia owns one, siprec_media owns the other.
 */

/* discard_pending_recording: tear down a half-built recording_t
 * that's already in globals.recordings_hash but hasn't yet had
 * its on_destroy state-handler bound to the original session.
 * Called from every failure path between hash-insert and
 * state-handler-bind in start_recording_session — without it,
 * any failure between those two points leaks the recording_t
 * (and its mutex + pool) until module unload, since nothing
 * else will ever reap it. */
static void discard_pending_recording(recording_t *recording)
{
    if (!recording) return;

    /* Best-effort tear-down of any partially-attached state.
     * Both detach and BYE are NULL-safe and idempotent — they
     * no-op when invite_ctx / media_ctx aren't populated. */
    siprec_media_detach(recording);
    siprec_invite_send_bye(recording);

    switch_mutex_lock(globals.recordings_mutex);
    switch_core_hash_delete(globals.recordings_hash, recording->key);
    switch_mutex_unlock(globals.recordings_mutex);

    switch_core_destroy_memory_pool(&recording->pool);
}

switch_status_t start_recording_session(switch_core_session_t *session, const char *recording_server_name)
{
    recording_server_t *server = NULL;
    recording_t *recording = NULL;
    const char *uuid = switch_core_session_get_uuid(session);
    char *recording_key = NULL;
    switch_memory_pool_t *recording_pool = NULL;

    /* Reject NULL server name early. APR's switch_core_hash_find with
     * APR_HASH_KEY_STRING calls strlen() on the key — passing NULL
     * segfaults FreeSWITCH.
     */
    if (zstr(recording_server_name)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: no recording server specified (usage: siprec <server-name>)\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_mutex_lock(globals.recording_servers_mutex);
    server = switch_core_hash_find(globals.recording_servers_hash, recording_server_name);
    switch_mutex_unlock(globals.recording_servers_mutex);

    if (!server) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: recording server %s not found in siprec.conf\n", recording_server_name);
        return SWITCH_STATUS_FALSE;
    }

    recording_key = switch_mprintf("%s-%s", recording_server_name, uuid);

    /* Duplicate-detect against the recordings hash, NOT the
     * recording_servers hash. The original code looked up the new
     * recording_key in the SERVER hash (a different keyspace), which
     * would never match — the dup check was effectively dead.
     */
    switch_mutex_lock(globals.recordings_mutex);
    recording = switch_core_hash_find(globals.recordings_hash, recording_key);
    switch_mutex_unlock(globals.recordings_mutex);

    if (recording) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: recording %s already exists\n", recording_key);
        switch_safe_free(recording_key);
        return SWITCH_STATUS_FALSE;
    }

    /* CRITICAL FIX: create the memory pool BEFORE allocating from
     * it. The original code did the inverse — `switch_core_alloc(
     * recording_pool, ...)` was called while recording_pool was
     * still uninitialized (declared but never assigned), which
     * dereferenced an indeterminate pointer and segfaulted FS on
     * the first siprec dispatch.
     */
    if (switch_core_new_memory_pool(&recording_pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: failed to allocate recording memory pool\n");
        switch_safe_free(recording_key);
        return SWITCH_STATUS_FALSE;
    }

    recording = (recording_t *) switch_core_alloc(recording_pool, sizeof(*recording));
    recording->pool = recording_pool;

    /* recording_key was returned by switch_mprintf (heap-allocated);
     * copy into the recording's pool so the lifetime is bounded by
     * the recording, not the pool-less malloc. The original key
     * pointer leaks on the success path; guard with the copy here.
     */
    recording->key = switch_core_strdup(recording->pool, recording_key);
    switch_safe_free(recording_key);
    recording->uuid = switch_core_strdup(recording->pool, uuid);
    recording->start_epoch = switch_epoch_time_now(NULL);
    recording->session = session;
    recording->server = server;

    switch_mutex_lock(globals.recordings_mutex);
    switch_core_hash_insert(globals.recordings_hash, recording->key, recording);
    switch_mutex_unlock(globals.recordings_mutex);

    /* ──────────────────────────────────────────────────────── *
     * RFC 7866 INVITE dispatch                                *
     * ──────────────────────────────────────────────────────── */

    /* Model the bridged call as two participants — caller
     * (sip_from_uri) and callee (sip_to_uri / dialed
     * destination_number) — with one stream per direction.
     * The metadata XML carries the per-participant cross-
     * reference structure RFC 7865 §5 recommends; the actual
     * SDP shape (one m=audio mono-mixed vs two labelled m=
     * blocks) depends on whether the SRS-side answer
     * negotiated one or two streams (parsed in siprec_invite.c).
     *
     * Stream mapping:
     *   stream-1  →  audio FROM caller TO callee  (read dir)
     *   stream-2  →  audio FROM callee TO caller  (write dir)
     * The bug's READ callback receives the carrier inbound
     * (caller-spoken); WRITE receives what FS sends back to
     * the carrier (callee-spoken via FS-internal apps).
     */
    switch_channel_t *orig_ch = switch_core_session_get_channel(session);

    const char *caller_aor = switch_channel_get_variable(orig_ch, "sip_from_uri");
    if (!caller_aor) {
        caller_aor = switch_channel_get_variable(orig_ch, "caller_id_number");
    }
    if (!caller_aor) caller_aor = "sip:unknown@unknown";

    const char *callee_aor = switch_channel_get_variable(orig_ch, "sip_to_uri");
    if (!callee_aor) {
        callee_aor = switch_channel_get_variable(orig_ch, "destination_number");
    }
    if (!callee_aor) callee_aor = "sip:unknown@unknown";

    /* participant IDs are derived from the call-uuid +
     * suffix so they're unique within the recording session
     * but stable across re-INVITEs. */
    char p_caller_id[80], p_callee_id[80];
    switch_snprintf(p_caller_id, sizeof(p_caller_id), "%s-caller", uuid);
    switch_snprintf(p_callee_id, sizeof(p_callee_id), "%s-callee", uuid);

    siprec_metadata_participant_t parts[2] = {
        { .participant_id = p_caller_id,
          .aor = caller_aor, .display_name = NULL },
        { .participant_id = p_callee_id,
          .aor = callee_aor, .display_name = NULL },
    };

    /* RFC 7865 §5 + RFC 7866 §8.5: the metadata's <stream>
     * entries cross-reference the SDP offer's a=label:N lines.
     * Claiming N streams in the metadata when the SDP offers
     * fewer — or vice versa — is a conformance error: the SRS
     * has no a=label:K to bind metadata stream label="K" to.
     *
     * Today mod_sofia's auto-generated outbound-leg offer is
     * single-track (one m=audio block). siprec_sdp_inject_labels
     * emits a=label:1 on that block (RFC 7866 §8.5) via the
     * post-originate re-INVITE in siprec_invite_send. That's
     * the only stream the SRS will see RTP for, so the metadata
     * must describe exactly one <stream> with the matching
     * label.
     *
     * Single-stream attribution: the bug is attached to the
     * recording->session leg with SMBF_READ_STREAM | SMBF_WRITE_STREAM,
     * but stream[0] (READ direction) is the only one forwarded
     * to the SRS today (siprec_media.c drops stream[1] when
     * negotiated_count==1). READ on the bug-host leg captures
     * what that leg HEARS — i.e., the FAR participant's voice.
     * We attribute the stream to participant[0] by convention
     * (caller, in <participantstreamassoc participant_id=
     * "...-caller"><send>) because that's the conservative
     * choice for the typical outbound campaign use case where
     * SIPREC starts on the originating leg and the goal is to
     * record the agent ↔ callee conversation as a single audio
     * track. A more precise leg-direction attribution is the
     * v1.4.0 follow-up alongside making both directions
     * actually flow on the wire. */
    siprec_metadata_stream_t streams_arr[1] = {
        { .stream_id = "stream-1", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 0,
          .label = "1" },
    };

    char associate_time[64] = {0};
    {
        time_t now = time(NULL);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        strftime(associate_time, sizeof(associate_time),
            "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    }

    siprec_metadata_options_t mopts = {
        .session_id        = uuid,
        .group_id          = uuid,
        .associate_time_utc = associate_time,
        .datamode          = SIPREC_DATAMODE_COMPLETE,
        .participants      = parts,
        .participant_count = 2,
        .streams           = streams_arr,
        .stream_count      = sizeof(streams_arr) / sizeof(streams_arr[0]),
    };
    char *metadata_body = siprec_metadata_build(&mopts);
    if (!metadata_body) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR, "siprec: metadata build failed\n");
        discard_pending_recording(recording);
        return SWITCH_STATUS_FALSE;
    }

    /* Profile: same one carrying the original call. The
     * channel's `sofia_profile_name` is the canonical source
     * — set automatically by mod_sofia for any channel that
     * arrived through (or was originated against) a profile.
     * Hardcoding deployment-specific names like "voicetel" or
     * even the vanilla-config "internal" / "external" couples
     * the module to one operator's layout; the channel
     * variable lets us run unmodified on any profile name.
     *
     * If sofia_profile_name is absent, the channel almost
     * certainly didn't come from sofia (loopback, mod_dingaling
     * legacy, mod_skinny, …) — SIPREC isn't applicable in
     * those cases. Fail loud rather than guess at a default.
     */
    const char *profile = switch_channel_get_variable(
        switch_core_session_get_channel(session),
        "sofia_profile_name");
    if (zstr(profile)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "siprec: channel has no sofia_profile_name — "
            "SIPREC requires a sofia-backed channel\n");
        siprec_metadata_free(metadata_body);
        discard_pending_recording(recording);
        return SWITCH_STATUS_FALSE;
    }

    /* Send the INVITE without an SDP override and let mod_sofia
     * auto-generate the offer body. The sdp_body argument to
     * siprec_invite_send_failover is therefore NULL — reserved
     * for the future multi-track offer bring-up that needs an
     * offer-time SDP-override hook through mod_sofia. */
    switch_status_t inv = siprec_invite_send_failover(
        recording, profile, server, /*sdp_body*/ NULL, metadata_body);

    siprec_metadata_free(metadata_body);

    if (inv != SWITCH_STATUS_SUCCESS) {
        /* INVITE failed: there is no recording leg, no media
         * bug, and the original session's on_destroy
         * state-handler hasn't been bound yet (we only do
         * that on the success path below), so nobody else
         * will reap this recording_t. */
        discard_pending_recording(recording);
        return inv;
    }

    /* siprec_invite_send populated invite_ctx->negotiated[]
     * by parsing the SRS-side answer SDP. Hand that off to
     * siprec_media_attach to wire the bug + RTP fork.
     *
     * Failing the attach is a hard error — the SIP dialog is
     * up but no audio will reach the SRS. We BYE the dialog
     * to prevent a "ghost" recording session at the SRS that
     * receives no media. discard_pending_recording handles
     * the BYE + hash cleanup. */
    if (siprec_media_attach(recording) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR,
            "siprec: media attach failed; tearing down "
            "recording leg for server '%s'\n",
            recording_server_name);
        discard_pending_recording(recording);
        return SWITCH_STATUS_FALSE;
    }

    /* Bind the on_destroy state-handler so caller-side hangup
     * automatically tears down the recording. Without this
     * the recording_t survives the original call's destroy
     * cycle and only gets reaped at module shutdown. */
    switch_channel_add_state_handler(
        switch_core_session_get_channel(session), &state_handlers);

    return SWITCH_STATUS_SUCCESS;
}


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet
 */
