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

    /* RFC 7866 §11.3: the SRC-to-SRS hop SHOULD use SIPS for
     * confidentiality on hostile-network deployments. When
     * `transport` is "tls" the dial-string uses sips:..;transport=tls
     * and the sofia profile MUST have a TLS port configured.
     * Default ("udp" / NULL) keeps the v1 plain-UDP path. */
    char *transport;

    switch_memory_pool_t *pool;

    /* Forward-pointer for the next entry in the failover
     * chain. Multiple recording-server entries with the same
     * name form an ordered list; siprec_invite_send walks
     * them in order on connect / 4xx-5xx failure until one
     * accepts. NULL on the last entry. */
    struct recording_server *next;
};
typedef struct recording_server recording_server_t;

/* Forward declarations for components defined elsewhere. The
 * recording_t holds opaque pointers to them so the header
 * doesn't pull in unrelated headers. */
struct siprec_invite_ctx;
struct siprec_media_ctx;

struct recording {
    char *key;
    char *uuid;
    int start_epoch;
    switch_core_session_t *session;
    recording_server_t *server;
    switch_memory_pool_t *pool;

    /* SIP signalling context — populated by siprec_invite_send.
     * Owns the recording-leg session pointer + dialog
     * negotiation state. NULL until siprec_invite_send fires. */
    struct siprec_invite_ctx *invite_ctx;

    /* Media-bug + RTP-fork context — populated by
     * siprec_media_attach. NULL until media is wired. */
    struct siprec_media_ctx *media_ctx;
};
typedef struct recording recording_t;

typedef struct {
    int src_enabled;
    int srs_enabled;
    switch_hash_t *recording_servers_hash;
    switch_mutex_t *recording_servers_mutex;
    switch_hash_t *recordings_hash;
    switch_mutex_t *recordings_mutex;
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
