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
#include "siprec_invite.h"
#include "siprec_media.h"
#include "siprec_sdp.h"

globals_t globals;

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_siprec_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_siprec_load);

SWITCH_MODULE_DEFINITION(mod_siprec, mod_siprec_load, mod_siprec_shutdown, NULL);


static switch_xml_config_item_t general_instructions[] = {
	SWITCH_CONFIG_ITEM("src-enabled", SWITCH_CONFIG_BOOL, CONFIG_RELOADABLE, &globals.src_enabled, SWITCH_TRUE, NULL, "true|false", "Enable/Disable Server Recording Client"),
	SWITCH_CONFIG_ITEM("srs-enabled", SWITCH_CONFIG_BOOL, CONFIG_RELOADABLE, &globals.srs_enabled, SWITCH_FALSE, NULL, "true|false", "Enable/Disable Server Recording Server"),
	SWITCH_CONFIG_ITEM_END()
};

static switch_status_t load_recording_server(switch_xml_t xml)
{
	switch_xml_t param, settings;
	char *name = (char *) switch_xml_attr_soft(xml, "name");
	recording_server_t *recording_server;
	switch_memory_pool_t *recording_server_pool;

	switch_core_new_memory_pool(&recording_server_pool);

	recording_server = (recording_server_t *) switch_core_alloc(recording_server_pool, sizeof(*recording_server));
	recording_server->name = switch_core_strdup(recording_server_pool, switch_str_nil(name));
	recording_server->pool = recording_server_pool;

	if ((settings = switch_xml_child(xml, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *var = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");
			/* Use switch_core_strdup (pool-bound) instead of bare
			 * strdup. The recording_server's pool is destroyed at
			 * module shutdown; strings allocated from the heap
			 * (strdup) leak because nothing tracks their lifetime
			 * — name was already pool-allocated, the others were
			 * inconsistent. switch_atoui returns unsigned; the
			 * cast keeps the signed-int port field tidy.
			 */
			if (!strcmp(var, "host")) {
				recording_server->host = switch_core_strdup(recording_server_pool, val);
			} else if (!strcmp(var, "port")) {
				recording_server->port = (int) switch_atoui(val);
			} else if (!strcmp(var, "register")) {
				recording_server->should_register = switch_true(val);
			} else if (!strcmp(var, "username")) {
				recording_server->username = switch_core_strdup(recording_server_pool, val);
			} else if (!strcmp(var, "password")) {
				recording_server->password = switch_core_strdup(recording_server_pool, val);
			} else if (!strcmp(var, "transport")) {
				/* "udp" (default), "tcp", "tls". TLS implies
				 * the dial URI uses sips:; the sofia profile
				 * MUST have sip-tls-port configured. */
				recording_server->transport =
					switch_core_strdup(recording_server_pool, val);
			}
		}
	}

	switch_mutex_lock(globals.recording_servers_mutex);
	/* If an entry with this name already exists, append the
	 * new one to the end of the failover chain. siprec_invite
	 * walks the chain on dial failure. */
	recording_server_t *existing =
		switch_core_hash_find(globals.recording_servers_hash, recording_server->name);
	if (existing) {
		while (existing->next) existing = existing->next;
		existing->next = recording_server;
	} else {
		switch_core_hash_insert(globals.recording_servers_hash,
			recording_server->name, recording_server);
	}
	switch_mutex_unlock(globals.recording_servers_mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t switch_xml_config_parse_module_recording_servers(const char *file, switch_bool_t reload)
{
	switch_xml_t cfg, xml, servers, xserver;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!(xml = switch_xml_open_cfg(file, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open %s\n", file);
		return SWITCH_STATUS_FALSE;
	}

	if ((servers = switch_xml_child(cfg, "recording-servers"))) {
		for (xserver = switch_xml_child(servers, "recording-server"); xserver; xserver = xserver->next) {
			if (load_recording_server(xserver) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error loading recording server.\n");
			}
		}
	}

	/* Only free the root xml — xserver and servers are children
	 * (returned by switch_xml_child) and live inside the root's
	 * allocation. Freeing them after switch_xml_free(xml) is a
	 * use-after-free / double-free that crashes on module reload.
	 */
	switch_xml_free(xml);

	return status;
}

static switch_status_t do_config(switch_bool_t reload)
{
	if (switch_xml_config_parse_module_settings("siprec.conf", reload, general_instructions) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
			"siprec: could not parse <settings> in siprec.conf\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_xml_config_parse_module_recording_servers("siprec.conf", reload) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
			"siprec: could not parse <recording-servers> in siprec.conf\n");
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(siprec_app_function)
{
	char *argv[4] = { 0 };
	int argc;
	char *mydata = NULL;
	const char *recording_server_name = NULL;

	if (zstr(data)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: no arguments — usage: siprec <recording_server>\n");
		return;
	}

	if (!(mydata = switch_core_session_strdup(session, data))) {
		return;
	}

	/* Original code required argc == 2 to populate the server name —
	 * which meant a single-arg invocation like `siprec default` left
	 * recording_server_name as NULL and crashed inside
	 * start_recording_session. Accept any non-empty first token.
	 */
	argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	if (argc >= 1 && !zstr(argv[0])) {
		recording_server_name = argv[0];
	}

	start_recording_session(session, recording_server_name);
}

/* siprec_pause / siprec_resume: send a re-INVITE on the
 * recording dialog with an updated SDP direction attribute
 * per RFC 7866 §6.4.
 *
 *   pause   →  a=inactive   (SRS stops writing the WAV
 *                            but the dialog stays up)
 *   resume  →  a=sendonly   (SRS resumes writing)
 *
 * The new SDP is built locally using the same parameters
 * the original INVITE used (src_ip, codec, port stable for
 * the dialog's lifetime); only the direction attribute
 * changes.
 *
 * Usage in dialplan:
 *   <action application="siprec_pause"  data="default"/>
 *   <action application="siprec_resume" data="default"/>
 *
 * The recording-server name argument selects which active
 * recording to re-INVITE (one call may have multiple
 * recordings to different SRSes).
 */
static switch_status_t siprec_change_direction(
	switch_core_session_t *session,
	const char *server_name,
	int paused)
{
	if (zstr(server_name)) {
		server_name = "default";
	}

	const char *uuid = switch_core_session_get_uuid(session);
	char *recording_key = switch_mprintf("%s-%s", server_name, uuid);

	switch_mutex_lock(globals.recordings_mutex);
	recording_t *recording = switch_core_hash_find(globals.recordings_hash, recording_key);
	switch_mutex_unlock(globals.recordings_mutex);
	switch_safe_free(recording_key);

	if (!recording) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: no active recording for server '%s' on this leg\n",
			server_name);
		return SWITCH_STATUS_FALSE;
	}
	if (!recording->invite_ctx
		|| !*recording->invite_ctx->recording_uuid) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"siprec: recording '%s' has no live SIP dialog\n",
			server_name);
		return SWITCH_STATUS_FALSE;
	}

	/* RFC 7866 §6.4 pause/resume: re-INVITE on the existing
	 * dialog with the SDP direction flipped. The new SDP MUST
	 * keep the negotiated ports, codec, c= address, and crypto
	 * stable — only the direction attribute and o=session-version
	 * change. Building from scratch would change session-id and
	 * the SRS would treat it as a brand-new session.
	 *
	 * Source the existing local SDP from the recording leg's
	 * sip_local_sdp_str channel variable (mod_sofia populates
	 * it after every successful negotiation), flip the
	 * direction line, bump o=version. Locate-by-uuid so the
	 * read can't UAF on a torn-down recording leg. */
	switch_core_session_t *rs = switch_core_session_locate(
		recording->invite_ctx->recording_uuid);
	if (!rs) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"siprec: recording leg %s is gone — cannot pause/resume\n",
			recording->invite_ctx->recording_uuid);
		return SWITCH_STATUS_FALSE;
	}

	switch_channel_t *rch = switch_core_session_get_channel(rs);
	const char *local_sdp =
		switch_channel_get_variable(rch, "sip_local_sdp_str");

	/* Capture state and produce the rewritten SDP BEFORE
	 * rwunlock — local_sdp is a pointer into the channel's
	 * pool, which can be freed once we drop the read-lock and
	 * sofia / FS-core finish tearing down the session. The
	 * rewritten new_sdp is a fresh malloc, independent of the
	 * channel's lifetime. */
	int   had_local_sdp = !zstr(local_sdp);
	char *new_sdp       = had_local_sdp
		? siprec_sdp_flip_direction(local_sdp, paused)
		: NULL;

	switch_core_session_rwunlock(rs);

	if (!had_local_sdp) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"siprec: recording leg has no sip_local_sdp_str — "
			"cannot pause/resume\n");
		return SWITCH_STATUS_FALSE;
	}
	if (!new_sdp) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"siprec: SDP direction-flip allocation failed\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_status_t st = siprec_invite_reinvite(recording, new_sdp, NULL);
	siprec_sdp_free(new_sdp);

	if (st != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"siprec: re-INVITE for %s failed: %d\n",
			paused ? "pause" : "resume", (int)st);
	}
	return st;
}

SWITCH_STANDARD_APP(siprec_pause_app_function)
{
	siprec_change_direction(session, data, /*paused*/ 1);
}

SWITCH_STANDARD_APP(siprec_resume_app_function)
{
	siprec_change_direction(session, data, /*paused*/ 0);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_siprec_load)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_application_interface_t *app_interface;

	switch_mutex_init(&globals.recording_servers_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&globals.recordings_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_core_hash_init(&globals.recording_servers_hash);
	switch_core_hash_init(&globals.recordings_hash);

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	status = do_config(SWITCH_FALSE);
	if (status == SWITCH_STATUS_FALSE) {
		goto done;
	}

	SWITCH_ADD_APP(app_interface, "siprec",
		"Start a SIPREC recording", "", siprec_app_function,
		"<recording_server>", SAF_NONE);
	SWITCH_ADD_APP(app_interface, "siprec_pause",
		"Pause a SIPREC recording (re-INVITE a=inactive)",
		"", siprec_pause_app_function,
		"<recording_server>", SAF_NONE);
	SWITCH_ADD_APP(app_interface, "siprec_resume",
		"Resume a SIPREC recording (re-INVITE a=sendonly)",
		"", siprec_resume_app_function,
		"<recording_server>", SAF_NONE);

	done:
	return status;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_siprec_shutdown)
{
	switch_hash_index_t *hi;
	void *val;
	const void *vvar;
	recording_t *recording = NULL;
	recording_server_t *recording_server = NULL;

	switch_xml_config_cleanup(general_instructions);

	switch_mutex_lock(globals.recordings_mutex);
	for (hi = switch_core_hash_first(globals.recordings_hash); hi; hi = switch_core_hash_next(&hi)) {
		switch_core_hash_this(hi, &vvar, NULL, &val);
		recording = (recording_t *) val;

		/* Tear down active state before freeing the pool.
		 * Without this, the media-bug callback can still fire
		 * on the FS media thread while user_data
		 * (siprec_media_ctx_t) is allocated from the pool we're
		 * about to destroy → use-after-free on module unload
		 * with active recordings.
		 *
		 * siprec_media_detach calls switch_core_media_bug_remove
		 * which is synchronous — it blocks until any in-flight
		 * callback has returned. siprec_invite_send_bye is
		 * idempotent and best-effort; if the recording leg's
		 * session is already gone the BYE is a no-op. */
		siprec_media_detach(recording);
		siprec_invite_send_bye(recording);

		switch_core_destroy_memory_pool(&recording->pool);
	}
	
	switch_core_hash_destroy(&globals.recordings_hash);
	switch_mutex_unlock(globals.recordings_mutex);

	switch_mutex_lock(globals.recording_servers_mutex);
	for (hi = switch_core_hash_first(globals.recording_servers_hash); hi; hi = switch_core_hash_next(&hi)) {
		switch_core_hash_this(hi, &vvar, NULL, &val);
		recording_server = (recording_server_t *) val;

		switch_core_destroy_memory_pool(&recording_server->pool);
	}

	switch_core_hash_destroy(&globals.recording_servers_hash);
	switch_mutex_unlock(globals.recording_servers_mutex);

	switch_mutex_destroy(globals.recordings_mutex);
	switch_mutex_destroy(globals.recording_servers_mutex);

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
