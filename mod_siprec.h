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
#ifndef MOD_SIPREC_H
#define MOD_SIPREC_H

#include <switch.h>
#include <switch_curl.h>
#include <switch_types.h>

struct recording_server {
    char *name;
    char *host;
    int port;
    int should_register;
    char *username;
    char *password;
    switch_memory_pool_t *pool;
};
typedef struct recording_server recording_server_t;

struct recording {
    char *key;
    char *uuid;
    int start_epoch;
    switch_mutex_t *mutex;
    switch_core_session_t *session;
    switch_media_bug_t *new_bug;
    recording_server_t *server;
    switch_memory_pool_t *pool;
    int running;
};
typedef struct recording recording_t;

typedef struct {
    char *odbc_dsn;
    char *dbname;
    int src_enabled;
    int srs_enabled;
    switch_hash_t *recording_servers_hash;
    switch_mutex_t *recording_servers_mutex;
    switch_hash_t *recordings_hash;
    switch_mutex_t *recordings_mutex;
    switch_mutex_t *db_mutex;
    switch_bool_t global_database_lock;
} globals_t;

extern globals_t globals;

#endif


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
