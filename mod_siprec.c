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
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");
			if (!strcmp(var, "host")) {
				recording_server->host = strdup(val);
			} else if (!strcmp(var, "port")) {
				recording_server->port = switch_atoui(val);
			} else if (!strcmp(var, "register")) {
				recording_server->should_register = switch_true(val);
			} else if (!strcmp(var, "username")) {
				recording_server->username = strdup(val);
			}  else if (!strcmp(var, "password")) {
				recording_server->password = strdup(val);
			}
		}
	}

	switch_mutex_lock(globals.recording_servers_mutex);
	switch_core_hash_insert(globals.recording_servers_hash, recording_server->name, recording_server);
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

	switch_xml_free(xml);
	switch_xml_free(xserver);
	switch_xml_free(servers);

	return status;
}

static switch_status_t do_config(switch_bool_t reload)
{
	if (switch_xml_config_parse_module_settings("siprec.conf", reload, general_instructions) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not open siprec.conf\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_xml_config_parse_module_recording_servers("siprec.conf", reload) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Could not open siprec.conf\n");
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

	if (!(mydata = switch_core_session_strdup(session, data))) {
		return;
	}

	if ((argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
		if (argc == 2) {
			recording_server_name = switch_core_session_strdup(session, argv[0]);
		}
	}

	start_recording_session(session, recording_server_name);
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

	SWITCH_ADD_APP(app_interface, "siprec", "Start RS for provided RCS", "", siprec_app_function, "<recording_server>", SAF_NONE);

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

		switch_mutex_destroy(recording->mutex);
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
