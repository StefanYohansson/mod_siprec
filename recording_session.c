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
#include "mod_siprec.h"
#include "recording_session.h"

static switch_status_t my_on_destroy(switch_core_session_t *session)
{
    switch_assert(session);
    if (stop_recording_session(session) == SWITCH_STATUS_FALSE) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARN, "Failed to stop recording session\n");
    }
    return SWITCH_STATUS_SUCCESS;
}

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

        if (!recording) {
            continue;
        }

        switch_mutex_lock(globals.recordings_mutex);
        switch_core_hash_delete(globals.recordings_hash, recording->key);
        switch_mutex_unlock(globals.recordings_mutex);
    }
    switch_mutex_unlock(globals.recording_servers_mutex);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t start_recording_session(switch_core_session_t *session, const char *recording_server_name)
{
    recording_server_t *server = NULL;
    recording_t *recording = NULL;
    const char *uuid = switch_core_session_get_uuid(session);
    char *recording_key = NULL;
    switch_memory_pool_t *recording_pool;

    switch_mutex_lock(globals.recording_servers_mutex);
    server = switch_core_hash_find(globals.recording_servers_hash, recording_server_name);
    switch_mutex_unlock(globals.recording_servers_mutex);

    if (!server) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Recording server %s not found\n", recording_server_name);
        return SWITCH_STATUS_FALSE;
    }

    recording_key = switch_mprintf("%s-%s", recording_server_name, uuid);

    switch_mutex_lock(globals.recordings_mutex);
    recording = switch_core_hash_find(globals.recording_servers_hash, recording_key);
    switch_mutex_unlock(globals.recordings_mutex);

    if (recording) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Recording %s already exists\n", recording_key);
        return SWITCH_STATUS_FALSE;
    }

    recording = (recording_t *) switch_core_alloc(recording_pool, sizeof(*recording));

    switch_core_new_memory_pool(recording_pool);
    recording->pool = recording_pool;
    switch_mutex_init(&recording->mutex, SWITCH_MUTEX_NESTED, recording->pool);

    recording->key = recording_key;
    recording->uuid = switch_core_session_get_uuid(session);
    recording->start_epoch = switch_epoch_time_now(NULL);
    recording->session = session;
    recording->server = server;
    recording->running = 1;

    switch_mutex_lock(globals.recordings_mutex);
    switch_core_hash_insert(globals.recordings_hash, recording->key, recording);
    switch_mutex_unlock(globals.recordings_mutex);

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
