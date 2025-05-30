/*
 * dialog module - basic support for dialog tracking
 *
 * Copyright (C) 2006 Voice Sistem SRL
 * Copyright (C) 2011 Carsten Bock, carsten@ng-voice.com
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

/*!
 * \file
 * \brief Module interface
 * \ingroup dialog
 * Module: \ref dialog
 */

/**
 * @defgroup dialog dialog :: Kamailio dialog module
 * @brief Kamailio dialog module
 *
 * The dialog module provides dialog awareness to the Kamailio proxy. Its
 * functionality is to keep track of the current dialogs, to offer
 * information about them (like how many dialogs are active) or to manage
 * them. The module exports several functions that could be used directly
 * from scripts.
 * The module, via an internal API, also provide the foundation to build
 * on top of it more complex dialog-based functionalities via other
 * Kamailio modules.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

#include "../../core/sr_module.h"
#include "../../lib/srdb1/db.h"
#include "../../core/dprint.h"
#include "../../core/error.h"
#include "../../core/ut.h"
#include "../../core/pvar.h"
#include "../../core/mod_fix.h"
#include "../../core/script_cb.h"
#include "../../core/kemi.h"
#include "../../core/fmsg.h"
#include "../../core/hashes.h"
#include "../../core/counters.h"
#include "../../core/mem/mem.h"
#include "../../core/timer_proc.h"
#include "../../core/lvalue.h"
#include "../../core/globals.h"
#include "../../core/parser/parse_to.h"
#include "../../modules/tm/tm_load.h"
#include "../../core/rpc_lookup.h"
#include "../../core/srapi.h"
#include "../../core/events.h"
#include "../rr/api.h"
#include "dlg_hash.h"
#include "dlg_timer.h"
#include "dlg_handlers.h"
#include "dlg_load.h"
#include "dlg_cb.h"
#include "dlg_db_handler.h"
#include "dlg_req_within.h"
#include "dlg_profile.h"
#include "dlg_var.h"
#include "dlg_transfer.h"
#include "dlg_cseq.h"
#include "dlg_dmq.h"

MODULE_VERSION


#define RPC_DATE_BUF_LEN 21

static int mod_init(void);
static int child_init(int rank);
static void mod_destroy(void);

/* module parameter */
static int dlg_hash_size = 4096;
static char *rr_param = "did";
static str timeout_spec = {NULL, 0};
static int default_timeout = 60 * 60 * 12; /* 12 hours */
static int seq_match_mode = SEQ_MATCH_STRICT_ID;
static char *profiles_wv_s = NULL;
static char *profiles_nv_s = NULL;
str dlg_extra_hdrs = {NULL, 0};
static int db_fetch_rows = 200;
static int db_skip_load = 0;
static int dlg_keep_proxy_rr = 0;
int dlg_filter_mode = 0;
int initial_cbs_inscript = 1;
int dlg_wait_ack = 1;
static int dlg_timer_procs = 0;
static int _dlg_track_cseq_updates = 0;
int dlg_ka_failed_limit = 1;
int dlg_early_timeout = 300;
int dlg_noack_timeout = 60;
int dlg_end_timeout = 300;

int dlg_enable_dmq = 0;

int dlg_event_rt[DLG_EVENTRT_MAX];
str dlg_event_callback = STR_NULL;

str dlg_bridge_controller = str_init("sip:controller@kamailio.org");

str dlg_bridge_contact = str_init("sip:controller@kamailio.org:5060");

int bye_early_code = 480;
str bye_early_reason = str_init("Temporarily Unavailable");

str ruri_pvar_param = str_init("$ru");
pv_elem_t *ruri_param_model = NULL;
str empty_str = STR_NULL;

int dlg_h_id_start = 0;
int dlg_h_id_step = 1;

/* statistic variables */
int dlg_enable_stats = 1;
int detect_spirals = 1;
int dlg_send_bye = 0;
int dlg_timeout_noreset = 0;
stat_var *active_dlgs = 0;
stat_var *processed_dlgs = 0;
stat_var *expired_dlgs = 0;
stat_var *failed_dlgs = 0;
stat_var *early_dlgs = 0;

int debug_variables_list = 0;

struct tm_binds d_tmb;
struct rr_binds d_rrb;
pv_spec_t timeout_avp;

int dlg_db_mode_param = DB_MODE_NONE;

str dlg_xavp_cfg = {0};
int dlg_ka_timer = 0;
int dlg_ka_interval = 0;
int dlg_clean_timer = 90;
int dlg_ctxiuid_mode = 0;
int dlg_process_mode = 0;

str dlg_lreq_callee_headers = {0};

/* db stuff */
static str db_url = str_init(DEFAULT_DB_URL);
static unsigned int db_update_period = DB_DEFAULT_UPDATE_PERIOD;

static int pv_get_dlg_count(
		struct sip_msg *msg, pv_param_t *param, pv_value_t *res);

void dlg_ka_timer_exec(unsigned int ticks, void *param);
void dlg_clean_timer_exec(unsigned int ticks, void *param);

/* commands wrappers and fixups */
static int fixup_profile(void **param, int param_no);
static int fixup_get_profile2(void **param, int param_no);
static int fixup_get_profile3(void **param, int param_no);
static int w_set_dlg_profile(struct sip_msg *, char *, char *);
static int w_unset_dlg_profile(struct sip_msg *, char *, char *);
static int w_is_in_profile(struct sip_msg *, char *, char *);
static int w_get_profile_size2(struct sip_msg *, char *, char *);
static int w_get_profile_size3(struct sip_msg *, char *, char *, char *);
static int w_dlg_isflagset(struct sip_msg *msg, char *flag, str *s2);
static int w_dlg_resetflag(struct sip_msg *msg, char *flag, str *s2);
static int w_dlg_setflag(struct sip_msg *msg, char *flag, char *s2);
static int w_dlg_set_property(struct sip_msg *msg, char *prop, char *s2);
static int w_dlg_reset_property(struct sip_msg *msg, char *prop, char *s2);
static int w_dlg_manage(struct sip_msg *, char *, char *);
static int w_dlg_bye(struct sip_msg *, char *, char *);
static int w_dlg_refer(struct sip_msg *, char *, char *);
static int w_dlg_bridge(struct sip_msg *, char *, char *, char *);
static int w_dlg_set_timeout(struct sip_msg *, char *, char *, char *);
static int w_dlg_set_timeout_by_profile2(struct sip_msg *, char *, char *);
static int w_dlg_set_timeout_by_profile3(
		struct sip_msg *, char *, char *, char *);
static int fixup_dlg_bye(void **param, int param_no);
static int fixup_dlg_refer(void **param, int param_no);
static int fixup_dlg_bridge(void **param, int param_no);
static int w_dlg_get(struct sip_msg *, char *, char *, char *);
static int w_is_known_dlg(struct sip_msg *);
static int w_dlg_set_ruri(sip_msg_t *, char *, char *);
static int w_dlg_db_load_callid(sip_msg_t *msg, char *ci, char *p2);
static int w_dlg_db_load_extra(sip_msg_t *msg, char *p1, char *p2);
static int fixup_dlg_get_var(void **param, int param_no);
static int fixup_dlg_get_var_free(void **param, int param_no);
static int w_dlg_get_var(
		struct sip_msg *, char *, char *, char *, char *, char *);
static int fixup_dlg_set_var(void **param, int param_no);
static int fixup_dlg_set_var_free(void **param, int param_no);
static int w_dlg_set_var(
		struct sip_msg *, char *, char *, char *, char *, char *);
static int w_dlg_remote_profile(sip_msg_t *msg, char *cmd, char *pname,
		char *pval, char *puid, char *expires);
static int fixup_dlg_remote_profile(void **param, int param_no);

static int w_dlg_req_with_headers_and_content(
		struct sip_msg *, char *, char *, char *, char *, char *);
static int w_dlg_req_with_content(
		struct sip_msg *, char *, char *, char *, char *);
static int w_dlg_req_with_headers(struct sip_msg *, char *, char *, char *);
static int w_dlg_req_within(struct sip_msg *, char *, char *);
static int w_dlg_set_state(sip_msg_t *, char *, char *);
static int w_dlg_update_state(sip_msg_t *, char *, char *);

static int fixup_dlg_dlg_req_within(void **, int);
static int fixup_dlg_req_with_headers(void **, int);
static int fixup_dlg_req_with_content(void **, int);
static int fixup_dlg_req_with_headers_and_content(void **, int);

static int dlg_sip_reply_out(sr_event_param_t *evp);

/* clang-format off */
static cmd_export_t cmds[]={
	{"dlg_manage", (cmd_function)w_dlg_manage,            0,0,
			0, REQUEST_ROUTE },
	{"dlg_set_state", (cmd_function)w_dlg_set_state,      1,fixup_spve_null,
			fixup_free_spve_null, ANY_ROUTE },
	{"dlg_update_state", (cmd_function)w_dlg_update_state, 0,0,
			0, REQUEST_ROUTE| FAILURE_ROUTE | ONREPLY_ROUTE | BRANCH_ROUTE },
	{"set_dlg_profile", (cmd_function)w_set_dlg_profile,  1,fixup_profile,
			0, ANY_ROUTE },
	{"set_dlg_profile", (cmd_function)w_set_dlg_profile,  2,fixup_profile,
			0, ANY_ROUTE },
	{"unset_dlg_profile", (cmd_function)w_unset_dlg_profile,  1,fixup_profile,
			0, REQUEST_ROUTE| FAILURE_ROUTE | ONREPLY_ROUTE | BRANCH_ROUTE },
	{"unset_dlg_profile", (cmd_function)w_unset_dlg_profile,  2,fixup_profile,
			0, REQUEST_ROUTE| FAILURE_ROUTE | ONREPLY_ROUTE | BRANCH_ROUTE },
	{"is_in_profile", (cmd_function)w_is_in_profile,      1,fixup_profile,
			0, ANY_ROUTE },
	{"is_in_profile", (cmd_function)w_is_in_profile,      2,fixup_profile,
			0, ANY_ROUTE },
	{"get_profile_size",(cmd_function)w_get_profile_size2, 2,fixup_get_profile2,
			0, ANY_ROUTE },
	{"get_profile_size",(cmd_function)w_get_profile_size3, 3,fixup_get_profile3,
			0, ANY_ROUTE },
	{"dlg_setflag", (cmd_function)w_dlg_setflag,          1,fixup_igp_null,
			0, ANY_ROUTE },
	{"dlg_resetflag", (cmd_function)w_dlg_resetflag,      1,fixup_igp_null,
			0, ANY_ROUTE },
	{"dlg_isflagset", (cmd_function)w_dlg_isflagset,      1,fixup_igp_null,
			0, ANY_ROUTE },
	{"dlg_bye",(cmd_function)w_dlg_bye,                   1,fixup_dlg_bye,
			0, ANY_ROUTE },
	{"dlg_refer",(cmd_function)w_dlg_refer,               2,fixup_dlg_refer,
			0, ANY_ROUTE },
	{"dlg_bridge",(cmd_function)w_dlg_bridge,             3,fixup_dlg_bridge,
			0, ANY_ROUTE },
	{"dlg_get",(cmd_function)w_dlg_get,                   3,fixup_dlg_bridge,
			0, ANY_ROUTE },
	{"is_known_dlg", (cmd_function)w_is_known_dlg,        0, NULL,
			0, ANY_ROUTE },
	{"dlg_set_timeout", (cmd_function)w_dlg_set_timeout,  1,fixup_igp_null,
			0, ANY_ROUTE },
	{"dlg_set_timeout", (cmd_function)w_dlg_set_timeout,  3,fixup_igp_all,
			0, ANY_ROUTE },
	{"dlg_set_timeout_by_profile",
		(cmd_function) w_dlg_set_timeout_by_profile2, 2, fixup_profile,
			0, ANY_ROUTE },
	{"dlg_set_timeout_by_profile",
		(cmd_function) w_dlg_set_timeout_by_profile3, 3, fixup_profile,
			0, ANY_ROUTE },
	{"dlg_set_property", (cmd_function)w_dlg_set_property,1,fixup_spve_null,
			0, ANY_ROUTE },
	{"dlg_reset_property", (cmd_function)w_dlg_reset_property,1,fixup_spve_null,
			0, ANY_ROUTE },
	{"dlg_remote_profile", (cmd_function)w_dlg_remote_profile, 5, fixup_dlg_remote_profile,
			0, ANY_ROUTE },
	{"dlg_set_ruri",       (cmd_function)w_dlg_set_ruri,  0, NULL,
			0, ANY_ROUTE },
	{"dlg_db_load_callid", (cmd_function)w_dlg_db_load_callid, 1, fixup_spve_null,
			0, ANY_ROUTE },
	{"dlg_db_load_extra", (cmd_function)w_dlg_db_load_extra, 0, 0,
			0, ANY_ROUTE },
	{"dlg_get_var",(cmd_function)w_dlg_get_var, 5, fixup_dlg_get_var,
			fixup_dlg_get_var_free, ANY_ROUTE },
	{"dlg_set_var",(cmd_function)w_dlg_set_var, 5, fixup_dlg_set_var,
			fixup_dlg_set_var_free, ANY_ROUTE },
	{"dlg_req_within",  (cmd_function)w_dlg_req_within, 2, fixup_dlg_dlg_req_within,
			0, ANY_ROUTE},
	{"dlg_req_within",  (cmd_function)w_dlg_req_with_headers, 3, fixup_dlg_req_with_headers,
			0, ANY_ROUTE},
	{"dlg_req_within",  (cmd_function)w_dlg_req_with_content, 4, fixup_dlg_req_with_content,
			0, ANY_ROUTE},
	{"dlg_req_within",  (cmd_function)w_dlg_req_with_headers_and_content, 5,
			fixup_dlg_req_with_headers_and_content, 0, ANY_ROUTE},

	{"load_dlg",  (cmd_function)load_dlg,   0, 0, 0, 0},
	{0,0,0,0,0,0}
};

static param_export_t mod_params[]={
	{ "enable_stats",          PARAM_INT, &dlg_enable_stats         },
	{ "hash_size",             PARAM_INT, &dlg_hash_size            },
	{ "rr_param",              PARAM_STRING, &rr_param                 },
	{ "timeout_avp",           PARAM_STR, &timeout_spec           },
	{ "default_timeout",       PARAM_INT, &default_timeout          },
	{ "dlg_extra_hdrs",        PARAM_STR, &dlg_extra_hdrs         },
	{ "dlg_match_mode",        PARAM_INT, &seq_match_mode           },
	{ "detect_spirals",        PARAM_INT, &detect_spirals,          },
	{ "db_url",                PARAM_STR, &db_url                 },
	{ "db_mode",               PARAM_INT, &dlg_db_mode_param        },
	{ "table_name",            PARAM_STR, &dialog_table_name        },
	{ "call_id_column",        PARAM_STR, &call_id_column         },
	{ "from_uri_column",       PARAM_STR, &from_uri_column        },
	{ "from_tag_column",       PARAM_STR, &from_tag_column        },
	{ "to_uri_column",         PARAM_STR, &to_uri_column          },
	{ "to_tag_column",         PARAM_STR, &to_tag_column          },
	{ "h_id_column",           PARAM_STR, &h_id_column            },
	{ "h_entry_column",        PARAM_STR, &h_entry_column         },
	{ "state_column",          PARAM_STR, &state_column           },
	{ "start_time_column",     PARAM_STR, &start_time_column      },
	{ "timeout_column",        PARAM_STR, &timeout_column         },
	{ "to_cseq_column",        PARAM_STR, &to_cseq_column         },
	{ "from_cseq_column",      PARAM_STR, &from_cseq_column       },
	{ "to_route_column",       PARAM_STR, &to_route_column        },
	{ "from_route_column",     PARAM_STR, &from_route_column      },
	{ "to_contact_column",     PARAM_STR, &to_contact_column      },
	{ "from_contact_column",   PARAM_STR, &from_contact_column    },
	{ "to_sock_column",        PARAM_STR, &to_sock_column         },
	{ "from_sock_column",      PARAM_STR, &from_sock_column       },
	{ "sflags_column",         PARAM_STR, &sflags_column          },
	{ "toroute_name_column",   PARAM_STR, &toroute_name_column    },

	{ "vars_table_name",       PARAM_STR, &dialog_vars_table_name   },
	{ "vars_h_id_column",      PARAM_STR, &vars_h_id_column       },
	{ "vars_h_entry_column",   PARAM_STR, &vars_h_entry_column    },
	{ "vars_key_column",       PARAM_STR, &vars_key_column        },
	{ "vars_value_column",     PARAM_STR, &vars_value_column      },

	{ "db_update_period",      PARAM_INT, &db_update_period         },
	{ "db_fetch_rows",         PARAM_INT, &db_fetch_rows            },
	{ "profiles_with_value",   PARAM_STRING, &profiles_wv_s            },
	{ "profiles_no_value",     PARAM_STRING, &profiles_nv_s            },
	{ "bridge_controller",     PARAM_STR, &dlg_bridge_controller  },
	{ "bridge_contact",        PARAM_STR, &dlg_bridge_contact       },
	{ "ruri_pvar",             PARAM_STR, &ruri_pvar_param        },
	{ "initial_cbs_inscript",  PARAM_INT, &initial_cbs_inscript     },
	{ "send_bye",              PARAM_INT, &dlg_send_bye             },
	{ "wait_ack",              PARAM_INT, &dlg_wait_ack             },
	{ "xavp_cfg",              PARAM_STR, &dlg_xavp_cfg           },
	{ "ka_timer",              PARAM_INT, &dlg_ka_timer             },
	{ "ka_interval",           PARAM_INT, &dlg_ka_interval          },
	{ "timeout_noreset",       PARAM_INT, &dlg_timeout_noreset      },
	{ "timer_procs",           PARAM_INT, &dlg_timer_procs          },
	{ "track_cseq_updates",    PARAM_INT, &_dlg_track_cseq_updates  },
	{ "lreq_callee_headers",   PARAM_STR, &dlg_lreq_callee_headers  },
	{ "db_skip_load",          PARAM_INT, &db_skip_load             },
	{ "ka_failed_limit",       PARAM_INT, &dlg_ka_failed_limit      },
	{ "enable_dmq",            PARAM_INT, &dlg_enable_dmq           },
	{ "event_callback",        PARAM_STR, &dlg_event_callback       },
	{ "early_timeout",         PARAM_INT, &dlg_early_timeout        },
	{ "noack_timeout",         PARAM_INT, &dlg_noack_timeout        },
	{ "end_timeout",           PARAM_INT, &dlg_end_timeout          },
	{ "h_id_start",            PARAM_INT, &dlg_h_id_start           },
	{ "h_id_step",             PARAM_INT, &dlg_h_id_step            },
	{ "keep_proxy_rr",         PARAM_INT, &dlg_keep_proxy_rr        },
	{ "dlg_filter_mode",       PARAM_INT, &dlg_filter_mode          },
	{ "bye_early_code",        PARAM_INT, &bye_early_code           },
	{ "bye_early_reason",      PARAM_STR, &bye_early_reason         },
	{ "dlg_ctxiuid_mode",      PARAM_INT, &dlg_ctxiuid_mode         },
	{ "debug_variables",       PARAM_INT, &debug_variables_list     },
	{ "dlg_mode",              PARAM_INT, &dlg_process_mode         },

	{ 0,0,0 }
};


static stat_export_t mod_stats[] = {
	{"active_dialogs" ,     STAT_NO_RESET,  &active_dlgs       },
	{"early_dialogs",       STAT_NO_RESET,  &early_dlgs        },
	{"processed_dialogs" ,  0,              &processed_dlgs    },
	{"expired_dialogs" ,    0,              &expired_dlgs      },
	{"failed_dialogs",      0,              &failed_dlgs       },
	{0,0,0}
};

static rpc_export_t rpc_methods[];

static pv_export_t mod_items[] = {
	{ {"DLG_count",  sizeof("DLG_count")-1}, PVT_OTHER,  pv_get_dlg_count,    0,
		0, 0, 0, 0 },
	{ {"DLG_lifetime",sizeof("DLG_lifetime")-1}, PVT_OTHER, pv_get_dlg_lifetime, 0,
		0, 0, 0, 0 },
	{ {"DLG_status",  sizeof("DLG_status")-1}, PVT_OTHER, pv_get_dlg_status, 0,
		0, 0, 0, 0 },
	{ {"dlg_ctx",  sizeof("dlg_ctx")-1}, PVT_OTHER, pv_get_dlg_ctx,
		pv_set_dlg_ctx, pv_parse_dlg_ctx_name, 0, 0, 0 },
	{ {"dlg",  sizeof("dlg")-1}, PVT_OTHER, pv_get_dlg,
		0, pv_parse_dlg_name, 0, 0, 0 },
	{ {"dlg_var", sizeof("dlg_var")-1}, PVT_OTHER, pv_get_dlg_variable,
		pv_set_dlg_variable,    pv_parse_dialog_var_name, 0, 0, 0},
	{ {0, 0}, 0, 0, 0, 0, 0, 0, 0 }
};

struct module_exports exports= {
	"dialog",        /* module's name */
	DEFAULT_DLFLAGS, /* dlopen flags */
	cmds,            /* exported functions */
	mod_params,      /* param exports */
	0,               /* exported RPC methods */
	mod_items,       /* exported pseudo-variables */
	0,               /* reply processing function */
	mod_init,        /* module initialization function */
	child_init,      /* per-child init function */
	mod_destroy
};
/* clang-format on */

static int fixup_profile(void **param, int param_no)
{
	struct dlg_profile_table *profile;
	pv_elem_t *model = NULL;
	str s;

	s.s = (char *)(*param);
	s.len = strlen(s.s);
	if(s.len == 0) {
		LM_ERR("param %d is empty string!\n", param_no);
		return E_CFG;
	}

	if(param_no == 1) {
		profile = search_dlg_profile(&s);
		if(profile == NULL) {
			LM_CRIT("profile <%s> not defined\n", s.s);
			return E_CFG;
		}
		pkg_free(*param);
		*param = (void *)profile;
		return 0;
	} else if(param_no == 2) {
		if(pv_parse_format(&s, &model) || model == NULL) {
			LM_ERR("wrong format [%s] for value param!\n", s.s);
			return E_CFG;
		}
		*param = (void *)model;
	}
	return 0;
}


static int fixup_get_profile2(void **param, int param_no)
{
	pv_spec_t *sp;
	int ret;

	if(param_no == 1) {
		return fixup_profile(param, 1);
	} else if(param_no == 2) {
		ret = fixup_pvar_null(param, 1);
		if(ret < 0)
			return ret;
		sp = (pv_spec_t *)(*param);
		if(sp->type != PVT_AVP && sp->type != PVT_SCRIPTVAR) {
			LM_ERR("return must be an AVP or SCRIPT VAR!\n");
			return E_SCRIPT;
		}
	}
	return 0;
}


static int fixup_get_profile3(void **param, int param_no)
{
	if(param_no == 1) {
		return fixup_profile(param, 1);
	} else if(param_no == 2) {
		return fixup_profile(param, 2);
	} else if(param_no == 3) {
		return fixup_get_profile2(param, 2);
	}
	return 0;
}


int load_dlg(struct dlg_binds *dlgb)
{
	dlgb->register_dlgcb = register_dlgcb;
	dlgb->terminate_dlg = dlg_bye_all;
	dlgb->set_dlg_var = set_dlg_variable;
	dlgb->get_dlg_varref = get_dlg_varref;
	dlgb->get_dlg_varval = get_dlg_varval;
	dlgb->get_dlg_vardup = get_dlg_vardup;
	dlgb->get_dlg_varstatus = get_dlg_varstatus;
	dlgb->get_dlg = dlg_get_msg_dialog;
	dlgb->release_dlg = dlg_release;
	return 1;
}


static int pv_get_dlg_count(
		struct sip_msg *msg, pv_param_t *param, pv_value_t *res)
{
	int n;
	int l;
	char *ch;

	if(msg == NULL || res == NULL)
		return -1;

	n = active_dlgs ? get_stat_val(active_dlgs) : 0;
	l = 0;
	ch = int2str(n, &l);

	res->rs.s = ch;
	res->rs.len = l;

	res->ri = n;
	res->flags = PV_VAL_STR | PV_VAL_INT | PV_TYPE_INT;

	return 0;
}


static int mod_init(void)
{
	unsigned int n;
	sr_cfgenv_t *cenv = NULL;

	if(dlg_h_id_start == -1) {
		dlg_h_id_start = server_id;
	} else if(dlg_h_id_start < 0) {
		dlg_h_id_start = 0;
	}

	if(dlg_h_id_step < 1) {
		dlg_h_id_step = 1;
	}

	if(dlg_ka_interval != 0 && dlg_ka_interval < 30) {
		LM_ERR("ka interval too low (%d), has to be at least 30\n",
				dlg_ka_interval);
		return -1;
	}

	dlg_event_rt[DLG_EVENTRT_START] = route_lookup(&event_rt, "dialog:start");
	dlg_event_rt[DLG_EVENTRT_END] = route_lookup(&event_rt, "dialog:end");
	dlg_event_rt[DLG_EVENTRT_FAILED] = route_lookup(&event_rt, "dialog:failed");

#ifdef STATISTICS
	/* register statistics */
	if(dlg_enable_stats && (register_module_stats("dialog", mod_stats) != 0)) {
		LM_ERR("failed to register statistics\n");
		return -1;
	}
#endif

	if(rpc_register_array(rpc_methods) != 0) {
		LM_ERR("failed to register RPC commands\n");
		return -1;
	}

	if(faked_msg_init() < 0)
		return -1;

	if(dlg_bridge_init_hdrs() < 0)
		return -1;

	if(rr_param == 0 || rr_param[0] == 0) {
		LM_ERR("empty rr_param!!\n");
		return -1;
	} else if(strlen(rr_param) > MAX_DLG_RR_PARAM_NAME) {
		LM_ERR("rr_param too long (max=%d)!!\n", MAX_DLG_RR_PARAM_NAME);
		return -1;
	}

	if(dlg_keep_proxy_rr < 0 || dlg_keep_proxy_rr > 3) {
		LM_ERR("invalid value for keep_proxy_rr\n");
		return -1;
	}

	if(timeout_spec.s) {
		if(pv_parse_spec(&timeout_spec, &timeout_avp) == 0
				&& (timeout_avp.type != PVT_AVP)) {
			LM_ERR("malformed or non AVP timeout "
				   "AVP definition in '%.*s'\n",
					timeout_spec.len, timeout_spec.s);
			return -1;
		}
	}

	if(default_timeout <= 0) {
		LM_ERR("0 default_timeout not accepted!!\n");
		return -1;
	}

	if(ruri_pvar_param.s == NULL || ruri_pvar_param.len <= 0) {
		LM_ERR("invalid r-uri PV string\n");
		return -1;
	}

	if(pv_parse_format(&ruri_pvar_param, &ruri_param_model) < 0
			|| ruri_param_model == NULL) {
		LM_ERR("malformed r-uri PV string: %s\n", ruri_pvar_param.s);
		return -1;
	}

	if(initial_cbs_inscript != 0 && initial_cbs_inscript != 1) {
		LM_ERR("invalid parameter for running initial callbacks in-script"
			   " (must be either 0 or 1)\n");
		return -1;
	}

	if(seq_match_mode != SEQ_MATCH_NO_ID && seq_match_mode != SEQ_MATCH_FALLBACK
			&& seq_match_mode != SEQ_MATCH_STRICT_ID) {
		LM_ERR("invalid value %d for seq_match_mode param!!\n", seq_match_mode);
		return -1;
	}

	if(detect_spirals != 0 && detect_spirals != 1) {
		LM_ERR("invalid value %d for detect_spirals param!!\n", detect_spirals);
		return -1;
	}

	if(dlg_timeout_noreset != 0 && dlg_timeout_noreset != 1) {
		LM_ERR("invalid value %d for timeout_noreset param!!\n",
				dlg_timeout_noreset);
		return -1;
	}

	/* create profile hashes */
	if(add_profile_definitions(profiles_nv_s, 0) != 0) {
		LM_ERR("failed to add profiles without value\n");
		return -1;
	}
	if(add_profile_definitions(profiles_wv_s, 1) != 0) {
		LM_ERR("failed to add profiles with value\n");
		return -1;
	}

	/* load the TM API */
	if(load_tm_api(&d_tmb) != 0) {
		LM_ERR("can't load TM API\n");
		return -1;
	}

	/* load RR API also */
	if(load_rr_api(&d_rrb) != 0) {
		LM_ERR("can't load RR API\n");
		return -1;
	}

	/* register callbacks*/
	/* listen for all incoming requests  */
	if(dlg_process_mode == 0) {
		if(d_tmb.register_tmcb(0, 0, TMCB_REQUEST_IN, dlg_onreq, 0, 0) <= 0) {
			LM_ERR("cannot register TMCB_REQUEST_IN callback\n");
			return -1;
		}
	}

	/* listen for all routed requests  */
	if(d_rrb.register_rrcb(dlg_onroute, 0) < 0) {
		LM_ERR("cannot register RR callback\n");
		return -1;
	}

	if(register_script_cb(profile_cleanup, POST_SCRIPT_CB | REQUEST_CB, 0)
			< 0) {
		LM_ERR("cannot register script callback");
		return -1;
	}
	if(register_script_cb(dlg_cfg_cb, PRE_SCRIPT_CB | REQUEST_CB, 0) < 0) {
		LM_ERR("cannot register pre-script ctx callback\n");
		return -1;
	}
	if(register_script_cb(dlg_cfg_cb, POST_SCRIPT_CB | REQUEST_CB, 0) < 0) {
		LM_ERR("cannot register post-script ctx callback\n");
		return -1;
	}

	if(register_script_cb(spiral_detect_reset, POST_SCRIPT_CB | REQUEST_CB, 0)
			< 0) {
		LM_ERR("cannot register req pre-script spiral detection reset "
			   "callback\n");
		return -1;
	}

	if(register_script_cb(cb_dlg_locals_reset, POST_SCRIPT_CB | ONREPLY_CB, 0)
			< 0) {
		LM_ERR("cannot register reply post-script dlg locals reset callback\n");
		return -1;
	}

	if(register_script_cb(cb_dlg_locals_reset, POST_SCRIPT_CB | FAILURE_CB, 0)
			< 0) {
		LM_ERR("cannot register failure post-script dlg locals reset "
			   "callback\n");
		return -1;
	}

	if(dlg_timer_procs <= 0) {
		if(register_timer(dlg_timer_routine, 0, 1) < 0) {
			LM_ERR("failed to register timer \n");
			return -1;
		}
	} else {
		register_sync_timers(1);
	}

	/* init handlers */
	init_dlg_handlers(rr_param, timeout_spec.s ? &timeout_avp : 0,
			default_timeout, seq_match_mode, dlg_keep_proxy_rr);

	/* init timer */
	if(init_dlg_timer(dlg_ontimeout) != 0) {
		LM_ERR("cannot init timer list\n");
		return -1;
	}

	/* sanitize dlg_hash_zie */
	if(dlg_hash_size < 1) {
		LM_WARN("hash_size is smaller "
				"then 1  -> rounding from %d to 1\n",
				dlg_hash_size);
		dlg_hash_size = 1;
	}
	/* initialized the hash table */
	for(n = 0; n < (8 * sizeof(n)); n++) {
		if(dlg_hash_size == (1 << n))
			break;
		if(n && dlg_hash_size < (1 << n)) {
			LM_WARN("hash_size is not a power "
					"of 2 as it should be -> rounding from %d to %d\n",
					dlg_hash_size, 1 << (n - 1));
			dlg_hash_size = 1 << (n - 1);
		}
	}

	if(init_dlg_table(dlg_hash_size) < 0) {
		LM_ERR("failed to create hash table\n");
		return -1;
	}

	/* if a database should be used to store the dialogs' information */
	dlg_db_mode = dlg_db_mode_param;
	if(dlg_db_mode == DB_MODE_NONE) {
		db_url.s = 0;
		db_url.len = 0;
	} else {
		if(dlg_db_mode != DB_MODE_REALTIME && dlg_db_mode != DB_MODE_DELAYED
				&& dlg_db_mode != DB_MODE_SHUTDOWN) {
			LM_ERR("unsupported db_mode %d\n", dlg_db_mode);
			return -1;
		}
		if(!db_url.s || db_url.len == 0) {
			LM_ERR("db_url not configured for db_mode %d\n", dlg_db_mode);
			return -1;
		}
		if(init_dlg_db(&db_url, dlg_hash_size, db_update_period, db_fetch_rows,
				   db_skip_load)
				!= 0) {
			LM_ERR("failed to initialize the DB support\n");
			return -1;
		}
	}

	/* timer process to send keep alive requests */
	if(dlg_ka_timer > 0 && dlg_ka_interval > 0)
		register_sync_timers(1);

	/* timer process to clean old unconfirmed dialogs */
	register_sync_timers(1);

	if(_dlg_track_cseq_updates != 0) {
		cenv = sr_cfgenv_get();
		cenv->cb_cseq_update = dlg_cseq_update;
		dlg_register_cseq_callbacks();
	}

	if(dlg_enable_dmq > 0 && dlg_dmq_initialize() != 0) {
		LM_ERR("failed to initialize dmq integration\n");
		return -1;
	}

	if(dlg_db_mode == DB_MODE_SHUTDOWN) {
		ksr_module_set_flag(KSRMOD_FLAG_POSTCHILDINIT);
	}

	if(dlg_process_mode != 0) {
		sr_event_register_cb(SREV_SIP_REPLY_OUT, dlg_sip_reply_out);
	}
	return 0;
}


static int child_init(int rank)
{
	dlg_db_mode = dlg_db_mode_param;


	if(rank == PROC_INIT) {
		if(dlg_db_mode != DB_MODE_NONE) {
			run_load_callbacks();
		}
	}

	if(rank == PROC_MAIN) {
		if(dlg_timer_procs > 0) {
			if(fork_sync_timer(PROC_TIMER, "Dialog Main Timer",
					   1 /*socks flag*/, dlg_timer_routine, NULL,
					   1 /*every sec*/)
					< 0) {
				LM_ERR("failed to start main timer routine as process\n");
				return -1; /* error */
			}
		}

		if(dlg_ka_timer > 0 && dlg_ka_interval > 0) {
			if(fork_sync_timer(PROC_TIMER, "Dialog KA Timer", 1 /*socks flag*/,
					   dlg_ka_timer_exec, NULL, dlg_ka_timer /*sec*/)
					< 0) {
				LM_ERR("failed to start ka timer routine as process\n");
				return -1; /* error */
			}
		}

		if(fork_sync_timer(PROC_TIMER, "Dialog Clean Timer", 1 /*socks flag*/,
				   dlg_clean_timer_exec, NULL, dlg_clean_timer /*sec*/)
				< 0) {
			LM_ERR("failed to start clean timer routine as process\n");
			return -1; /* error */
		}
	}

	if(((dlg_db_mode == DB_MODE_REALTIME || dlg_db_mode == DB_MODE_DELAYED)
			   && (rank > 0 || rank == PROC_TIMER || rank == PROC_RPC))
			|| (dlg_db_mode == DB_MODE_SHUTDOWN
					&& (rank == PROC_POSTCHILDINIT))) {
		if(dlg_connect_db(&db_url)) {
			LM_ERR("failed to connect to database (rank=%d)\n", rank);
			return -1;
		}
	}

	/* in DB_MODE_SHUTDOWN only PROC_MAIN will do a DB dump at the end, so
	 * for the rest of the processes will be the same as DB_MODE_NONE */
	if(dlg_db_mode == DB_MODE_SHUTDOWN && rank != PROC_POSTCHILDINIT)
		dlg_db_mode = DB_MODE_NONE;
	/* in DB_MODE_REALTIME and DB_MODE_DELAYED the PROC_MAIN have no DB handle */
	if((dlg_db_mode == DB_MODE_REALTIME || dlg_db_mode == DB_MODE_DELAYED)
			&& rank == PROC_MAIN)
		dlg_db_mode = DB_MODE_NONE;

	return 0;
}


static void mod_destroy(void)
{
	if(dlg_db_mode == DB_MODE_DELAYED || dlg_db_mode == DB_MODE_SHUTDOWN) {
		dialog_update_db(0, 0);
		destroy_dlg_db();
	}
}


/**
 *
 */
static int dlg_sip_reply_out(sr_event_param_t *evp)
{
	LM_DBG("handling sip response\n");
	dlg_update_state(evp->rpl);
	return 0;
}


static int w_set_dlg_profile_helper(
		sip_msg_t *msg, struct dlg_profile_table *profile, str *value)
{
	if(profile->has_value) {
		if(value == NULL || value->len <= 0) {
			LM_ERR("invalid value parameter\n");
			return -1;
		}
		if(set_dlg_profile(msg, value, profile) < 0) {
			LM_ERR("failed to set profile with key\n");
			return -1;
		}
	} else {
		if(set_dlg_profile(msg, NULL, profile) < 0) {
			LM_ERR("failed to set profile\n");
			return -1;
		}
	}
	return 1;
}

static int w_set_dlg_profile(struct sip_msg *msg, char *profile, char *value)
{
	pv_elem_t *pve = NULL;
	str val_s = STR_NULL;

	pve = (pv_elem_t *)value;
	if(pve != NULL) {
		if(pv_printf_s(msg, pve, &val_s) != 0 || val_s.len <= 0
				|| val_s.s == NULL) {
			LM_WARN("cannot get string for value\n");
			return -1;
		}
	}

	return w_set_dlg_profile_helper(
			msg, (struct dlg_profile_table *)profile, &val_s);
}

static int w_unset_dlg_profile_helper(
		sip_msg_t *msg, struct dlg_profile_table *profile, str *value)
{
	if(profile->has_value) {
		if(value == NULL || value->len <= 0) {
			LM_ERR("invalid value parameter\n");
			return -1;
		}
		if(unset_dlg_profile(msg, value, profile) < 0) {
			LM_ERR("failed to unset profile with key\n");
			return -1;
		}
	} else {
		if(unset_dlg_profile(msg, NULL, profile) < 0) {
			LM_ERR("failed to unset profile\n");
			return -1;
		}
	}
	return 1;
}

static int w_unset_dlg_profile(struct sip_msg *msg, char *profile, char *value)
{
	pv_elem_t *pve = NULL;
	str val_s = STR_NULL;

	pve = (pv_elem_t *)value;
	if(pve != NULL) {
		if(pv_printf_s(msg, pve, &val_s) != 0 || val_s.len <= 0
				|| val_s.s == NULL) {
			LM_WARN("cannot get string for value\n");
			return -1;
		}
	}

	return w_unset_dlg_profile_helper(
			msg, (struct dlg_profile_table *)profile, &val_s);
}

static int w_is_in_profile_helper(
		sip_msg_t *msg, struct dlg_profile_table *profile, str *value)
{
	if(profile->has_value) {
		if(value == NULL || value->len <= 0) {
			LM_ERR("invalid value parameter\n");
			return -1;
		}
		return is_dlg_in_profile(msg, profile, value);
	} else {
		return is_dlg_in_profile(msg, profile, NULL);
	}
}

static int w_is_in_profile(struct sip_msg *msg, char *profile, char *value)
{
	pv_elem_t *pve = NULL;
	str val_s = STR_NULL;

	pve = (pv_elem_t *)value;
	if(pve != NULL) {
		if(pv_printf_s(msg, pve, &val_s) != 0 || val_s.len <= 0
				|| val_s.s == NULL) {
			LM_WARN("cannot get string for value\n");
			return -1;
		}
	}

	return w_is_in_profile_helper(
			msg, (struct dlg_profile_table *)profile, &val_s);
}

/**
 * get dynamic name profile size
 */
static int w_get_profile_size_helper(sip_msg_t *msg,
		struct dlg_profile_table *profile, str *value, pv_spec_t *spd)
{
	unsigned int size;
	pv_value_t val;

	if(profile->has_value) {
		if(value == NULL || value->s == NULL || value->len <= 0) {
			LM_ERR("invalid value parameter\n");
			return -1;
		}
		size = get_profile_size(profile, value);
	} else {
		size = get_profile_size(profile, NULL);
	}

	memset(&val, 0, sizeof(pv_value_t));
	val.flags = PV_VAL_INT | PV_TYPE_INT;
	val.ri = (int)size;

	if(spd->setf(msg, &spd->pvp, (int)EQ_T, &val) < 0) {
		LM_ERR("setting profile PV failed\n");
		return -1;
	}

	return 1;
}

static int w_get_profile_size3(
		struct sip_msg *msg, char *profile, char *value, char *result)
{
	pv_elem_t *pve = NULL;
	str val_s = STR_NULL;
	pv_spec_t *spd = NULL;

	if(result != NULL) {
		pve = (pv_elem_t *)value;
		spd = (pv_spec_t *)result;
	} else {
		pve = NULL;
		spd = (pv_spec_t *)value;
	}
	if(pve != NULL) {
		if(pv_printf_s(msg, pve, &val_s) != 0 || val_s.len == 0
				|| val_s.s == NULL) {
			LM_WARN("cannot get string for value\n");
			return -1;
		}
	}

	return w_get_profile_size_helper(msg, (struct dlg_profile_table *)profile,
			(pve) ? &val_s : NULL, spd);
}

/**
 * get static name profile size
 */
static int w_get_profile_size2(struct sip_msg *msg, char *profile, char *result)
{
	return w_get_profile_size3(msg, profile, result, NULL);
}


static int ki_dlg_setflag(struct sip_msg *msg, int val)
{
	dlg_ctx_t *dctx;
	dlg_cell_t *d;

	if(val < 0 || val > 31)
		return -1;
	if((dctx = dlg_get_dlg_ctx()) == NULL)
		return -1;

	dctx->flags |= 1 << val;
	d = dlg_get_by_iuid(&dctx->iuid);
	if(d != NULL) {
		d->sflags |= 1 << val;
		dlg_release(d);
	}
	return 1;
}

static int w_dlg_setflag(struct sip_msg *msg, char *flag, char *s2)
{
	int val;

	if(fixup_get_ivalue(msg, (gparam_p)flag, &val) != 0) {
		LM_ERR("no flag value\n");
		return -1;
	}

	return ki_dlg_setflag(msg, val);
}

static int ki_dlg_resetflag(struct sip_msg *msg, int val)
{
	dlg_ctx_t *dctx;
	dlg_cell_t *d;

	if(val < 0 || val > 31)
		return -1;

	if((dctx = dlg_get_dlg_ctx()) == NULL)
		return -1;

	dctx->flags &= ~(1 << val);
	d = dlg_get_by_iuid(&dctx->iuid);
	if(d != NULL) {
		d->sflags &= ~(1 << val);
		dlg_release(d);
	}
	return 1;
}

static int w_dlg_resetflag(struct sip_msg *msg, char *flag, str *s2)
{
	int val;

	if(fixup_get_ivalue(msg, (gparam_p)flag, &val) != 0) {
		LM_ERR("no flag value\n");
		return -1;
	}
	return ki_dlg_resetflag(msg, val);
}

static int ki_dlg_isflagset(struct sip_msg *msg, int val)
{
	dlg_ctx_t *dctx;
	dlg_cell_t *d;
	int ret;

	if(val < 0 || val > 31)
		return -1;

	if((dctx = dlg_get_dlg_ctx()) == NULL)
		return -1;

	d = dlg_get_by_iuid(&dctx->iuid);
	if(d != NULL) {
		ret = (d->sflags & (1 << val)) ? 1 : -1;
		dlg_release(d);
		return ret;
	}
	return (dctx->flags & (1 << val)) ? 1 : -1;
}

static int w_dlg_isflagset(struct sip_msg *msg, char *flag, str *s2)
{
	int val;

	if(fixup_get_ivalue(msg, (gparam_p)flag, &val) != 0) {
		LM_ERR("no flag value\n");
		return -1;
	}
	return ki_dlg_isflagset(msg, val);
}

/**
 *
 */
static int w_dlg_manage(struct sip_msg *msg, char *s1, char *s2)
{
	return dlg_manage(msg);
}

/**
 *
 */
static int ki_dlg_set_state(sip_msg_t *msg, str *state)
{
	int istate = 0;

	if(state == NULL || state->s == NULL || state->len <= 0) {
		LM_ERR("invalid state value\n");
		return -1;
	}
	switch(state->s[0]) {
		case 'u':
		case 'U':
			istate = DLG_STATE_UNCONFIRMED;
			break;
		case 'e':
		case 'E':
			istate = DLG_STATE_EARLY;
			break;
		case 'a':
		case 'A':
			istate = DLG_STATE_CONFIRMED_NA;
			break;
		case 'c':
		case 'C':
			istate = DLG_STATE_CONFIRMED;
			break;
		case 'd':
		case 'D':
			istate = DLG_STATE_DELETED;
			break;
		default:
			LM_ERR("unknown state value: %.*s\n", state->len, state->s);
			return -1;
	}
	if(dlg_set_state(msg, istate) < 0) {
		return -1;
	}
	return 1;
}

/**
 *
 */
static int w_dlg_set_state(sip_msg_t *msg, char *pstate, char *p2)
{
	str state = STR_NULL;

	if(fixup_get_svalue(msg, (gparam_t *)pstate, &state) != 0) {
		LM_ERR("unable to get Method\n");
		return -1;
	}
	return ki_dlg_set_state(msg, &state);
}

/**
 *
 */
static int ki_dlg_update_state(sip_msg_t *msg)
{
	return dlg_update_state(msg);
}

/**
 *
 */
static int w_dlg_update_state(sip_msg_t *msg, char *pstate, char *p2)
{
	return dlg_update_state(msg);
}

static int fixup_dlg_dlg_req_within(void **param, int param_no)
{
	char *val;
	int n = 0;

	if(param_no == 1) {
		val = (char *)*param;
		if(strcasecmp(val, "all") == 0) {
			n = 0;
		} else if(strcasecmp(val, "caller") == 0) {
			n = 1;
		} else if(strcasecmp(val, "callee") == 0) {
			n = 2;
		} else {
			LM_ERR("invalid param \"%s\"\n", val);
			return E_CFG;
		}
		pkg_free(*param);
		*param = (void *)(long)n;
	} else if(param_no == 2) {
		return fixup_spve_null(param, 1);
	} else {
		LM_ERR("called with parameter != 1\n");
		return E_BUG;
	}
	return 0;
}

static int fixup_dlg_req_with_headers(void **param, int param_no)
{
	char *val;
	int n = 0;

	if(param_no == 1) {
		val = (char *)*param;
		if(strcasecmp(val, "all") == 0) {
			n = 0;
		} else if(strcasecmp(val, "caller") == 0) {
			n = 1;
		} else if(strcasecmp(val, "callee") == 0) {
			n = 2;
		} else {
			LM_ERR("invalid param \"%s\"\n", val);
			return E_CFG;
		}
		pkg_free(*param);
		*param = (void *)(long)n;
	} else if(param_no == 2) {
		return fixup_spve_null(param, 1);
	} else if(param_no == 3) {
		return fixup_spve_null(param, 1);
	} else {
		LM_ERR("called with parameter != 1\n");
		return E_BUG;
	}
	return 0;
}


static int fixup_dlg_req_with_content(void **param, int param_no)
{
	char *val;
	int n = 0;

	if(param_no == 1) {
		val = (char *)*param;
		if(strcasecmp(val, "all") == 0) {
			n = 0;
		} else if(strcasecmp(val, "caller") == 0) {
			n = 1;
		} else if(strcasecmp(val, "callee") == 0) {
			n = 2;
		} else {
			LM_ERR("invalid param \"%s\"\n", val);
			return E_CFG;
		}
		pkg_free(*param);
		*param = (void *)(long)n;
	} else if(param_no == 2) {
		return fixup_spve_null(param, 1);
	} else if(param_no == 3) {
		return fixup_spve_null(param, 1);
	} else if(param_no == 4) {
		return fixup_spve_null(param, 1);
	} else {
		LM_ERR("called with parameter != 1\n");
		return E_BUG;
	}
	return 0;
}

static int fixup_dlg_req_with_headers_and_content(void **param, int param_no)
{
	char *val;
	int n = 0;

	if(param_no == 1) {
		val = (char *)*param;
		if(strcasecmp(val, "all") == 0) {
			n = 0;
		} else if(strcasecmp(val, "caller") == 0) {
			n = 1;
		} else if(strcasecmp(val, "callee") == 0) {
			n = 2;
		} else {
			LM_ERR("invalid param \"%s\"\n", val);
			return E_CFG;
		}
		pkg_free(*param);
		*param = (void *)(long)n;
	} else if(param_no == 2) {
		return fixup_spve_null(param, 1);
	} else if(param_no == 3) {
		return fixup_spve_null(param, 1);
	} else if(param_no == 4) {
		return fixup_spve_null(param, 1);
	} else if(param_no == 5) {
		return fixup_spve_null(param, 1);
	} else {
		LM_ERR("called with parameter != 1\n");
		return E_BUG;
	}
	return 0;
}

static int ki_dlg_req_with_headers_and_content(struct sip_msg *msg, int nside,
		str *smethod, str *sheaders, str *scontent_type, str *scontent)
{
	dlg_cell_t *dlg = NULL;

	dlg = dlg_get_ctx_dialog();
	if(dlg == NULL)
		return -1;

	if(nside == 1) {
		if(dlg_request_within(msg, dlg, DLG_CALLER_LEG, smethod, sheaders,
				   scontent_type, scontent)
				!= 0)
			goto error;
		goto done;
	} else if(nside == 2) {
		if(dlg_request_within(msg, dlg, DLG_CALLEE_LEG, smethod, sheaders,
				   scontent_type, scontent)
				!= 0)
			goto error;
		goto done;
	} else {
		if(dlg_request_within(msg, dlg, DLG_CALLER_LEG, smethod, sheaders,
				   scontent_type, scontent)
				!= 0)
			goto error;
		if(dlg_request_within(msg, dlg, DLG_CALLEE_LEG, smethod, sheaders,
				   scontent_type, scontent)
				!= 0)
			goto error;
		goto done;
	}

done:
	dlg_release(dlg);
	return 1;

error:
	dlg_release(dlg);
	return -1;
}

static int w_dlg_req_with_headers_and_content(struct sip_msg *msg, char *side,
		char *method, char *headers, char *content_type, char *content)
{
	int n;
	str str_method = {0, 0};
	str str_headers = {0, 0};
	str str_content_type = {0, 0};
	str str_content = {0, 0};


	if(fixup_get_svalue(msg, (gparam_p)method, &str_method) != 0) {
		LM_ERR("unable to get Method\n");
		goto error;
	}
	if(str_method.s == NULL || str_method.len == 0) {
		LM_ERR("invalid Method parameter\n");
		goto error;
	}

	if(headers) {
		if(fixup_get_svalue(msg, (gparam_p)headers, &str_headers) != 0) {
			LM_ERR("unable to get Method\n");
			goto error;
		}
		if(str_headers.s == NULL || str_headers.len == 0) {
			LM_ERR("invalid Headers parameter\n");
			goto error;
		}
	}
	if(content_type && content) {
		if(fixup_get_svalue(msg, (gparam_p)content_type, &str_content_type)
				!= 0) {
			LM_ERR("unable to get Content-Type\n");
			goto error;
		}
		if(str_content_type.s == NULL || str_content_type.len == 0) {
			LM_ERR("invalid Headers parameter\n");
			goto error;
		}
		if(fixup_get_svalue(msg, (gparam_p)content, &str_content) != 0) {
			LM_ERR("unable to get Content\n");
			goto error;
		}
		if(str_content.s == NULL || str_content.len == 0) {
			LM_ERR("invalid Content parameter\n");
			goto error;
		}
	}

	n = (int)(long)side;

	return ki_dlg_req_with_headers_and_content(
			msg, n, &str_method, &str_headers, &str_content_type, &str_content);

error:
	return -1;
}

static int w_dlg_req_with_content(struct sip_msg *msg, char *side, char *method,
		char *content_type, char *content)
{
	return w_dlg_req_with_headers_and_content(
			msg, side, method, NULL, content_type, content);
}

static int w_dlg_req_with_headers(
		struct sip_msg *msg, char *side, char *method, char *headers)
{
	return w_dlg_req_with_headers_and_content(
			msg, side, method, headers, NULL, NULL);
}

static int w_dlg_req_within(struct sip_msg *msg, char *side, char *method)
{
	return w_dlg_req_with_headers_and_content(
			msg, side, method, NULL, NULL, NULL);
}

static int w_dlg_bye(struct sip_msg *msg, char *side, char *s2)
{
	dlg_cell_t *dlg = NULL;
	int n;

	dlg = dlg_get_ctx_dialog();
	if(dlg == NULL)
		return -1;

	n = (int)(long)side;
	if(n == 1) {
		if(dlg_bye(dlg, NULL, DLG_CALLER_LEG) != 0)
			goto error;
		goto done;
	} else if(n == 2) {
		if(dlg_bye(dlg, NULL, DLG_CALLEE_LEG) != 0)
			goto error;
		goto done;
	} else {
		if(dlg_bye_all(dlg, NULL) != 0)
			goto error;
		goto done;
	}

done:
	dlg_release(dlg);
	return 1;

error:
	dlg_release(dlg);
	return -1;
}

static int w_dlg_refer(struct sip_msg *msg, char *side, char *to)
{
	dlg_cell_t *dlg;
	int n;
	str st = {0, 0};

	dlg = dlg_get_ctx_dialog();
	if(dlg == NULL)
		return -1;

	n = (int)(long)side;

	if(fixup_get_svalue(msg, (gparam_p)to, &st) != 0) {
		LM_ERR("unable to get To\n");
		goto error;
	}
	if(st.s == NULL || st.len == 0) {
		LM_ERR("invalid To parameter\n");
		goto error;
	}
	if(n == 1) {
		if(dlg_transfer(dlg, &st, DLG_CALLER_LEG) != 0)
			goto error;
	} else {
		if(dlg_transfer(dlg, &st, DLG_CALLEE_LEG) != 0)
			goto error;
	}

	dlg_release(dlg);
	return 1;

error:
	dlg_release(dlg);
	return -1;
}

static int w_dlg_bridge(struct sip_msg *msg, char *from, char *to, char *op)
{
	str sf = {0, 0};
	str st = {0, 0};
	str so = {0, 0};

	if(from == 0 || to == 0 || op == 0) {
		LM_ERR("invalid parameters\n");
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_p)from, &sf) != 0) {
		LM_ERR("unable to get From\n");
		return -1;
	}
	if(sf.s == NULL || sf.len == 0) {
		LM_ERR("invalid From parameter\n");
		return -1;
	}
	if(fixup_get_svalue(msg, (gparam_p)to, &st) != 0) {
		LM_ERR("unable to get To\n");
		return -1;
	}
	if(st.s == NULL || st.len == 0) {
		LM_ERR("invalid To parameter\n");
		return -1;
	}
	if(fixup_get_svalue(msg, (gparam_p)op, &so) != 0) {
		LM_ERR("unable to get OP\n");
		return -1;
	}

	if(dlg_bridge(&sf, &st, &so, NULL) != 0)
		return -1;
	return 1;
}

static int ki_dlg_bridge(sip_msg_t *msg, str *sfrom, str *sto, str *soproxy)
{
	if(dlg_bridge(sfrom, sto, soproxy, NULL) != 0)
		return -1;
	return 1;
}

/**
 *
 */
static int w_dlg_set_timeout(
		struct sip_msg *msg, char *pto, char *phe, char *phi)
{
	int to = 0;
	unsigned int he = 0;
	unsigned int hi = 0;
	dlg_cell_t *dlg = NULL;

	if(fixup_get_ivalue(msg, (gparam_p)pto, &to) != 0) {
		LM_ERR("no timeout value\n");
		return -1;
	}
	if(to <= 0) {
		LM_ERR("invalid timeout value: %d\n", to);
		return -1;
	}
	if(phe != NULL) {
		if(phi == NULL) {
			LM_ERR("invalid number of parameters\n");
			return -1;
		}
		if(fixup_get_ivalue(msg, (gparam_p)phe, (int *)&he) != 0) {
			LM_ERR("no hash entry value value\n");
			return -1;
		}
		if(fixup_get_ivalue(msg, (gparam_p)phi, (int *)&hi) != 0) {
			LM_ERR("no hash id value value\n");
			return -1;
		}
		dlg = dlg_lookup(he, hi);
	} else {
		dlg = dlg_get_msg_dialog(msg);
	}

	if(dlg == NULL) {
		LM_DBG("no dialog found\n");
		return -1;
	}

	if(update_dlg_timeout(dlg, to) != 0)
		return -1;

	return 1;
}

/**
 *
 */
static int ki_dlg_set_property(sip_msg_t *msg, str *pval)
{
	dlg_ctx_t *dctx;
	dlg_cell_t *d;

	if(pval->len <= 0) {
		LM_ERR("empty property value\n");
		return -1;
	}
	if((dctx = dlg_get_dlg_ctx()) == NULL)
		return -1;

	if(pval->len == 6 && strncmp(pval->s, "ka-src", 6) == 0) {
		dctx->iflags |= DLG_IFLAG_KA_SRC;
		d = dlg_get_by_iuid(&dctx->iuid);
		if(d != NULL) {
			d->iflags |= DLG_IFLAG_KA_SRC;
			dlg_release(d);
		}
	} else if(pval->len == 6 && strncmp(pval->s, "ka-dst", 6) == 0) {
		dctx->iflags |= DLG_IFLAG_KA_DST;
		d = dlg_get_by_iuid(&dctx->iuid);
		if(d != NULL) {
			d->iflags |= DLG_IFLAG_KA_DST;
			dlg_release(d);
		}
	} else if(pval->len == 15 && strncmp(pval->s, "timeout-noreset", 15) == 0) {
		dctx->iflags |= DLG_IFLAG_TIMER_NORESET;
		d = dlg_get_by_iuid(&dctx->iuid);
		if(d != NULL) {
			d->iflags |= DLG_IFLAG_TIMER_NORESET;
			dlg_release(d);
		}
	} else {
		LM_ERR("unknown property value [%.*s]\n", pval->len, pval->s);
		return -1;
	}

	return 1;
}

/**
 *
 */
static int w_dlg_set_property(struct sip_msg *msg, char *prop, char *s2)
{
	str val;

	if(fixup_get_svalue(msg, (gparam_t *)prop, &val) != 0) {
		LM_ERR("no property value\n");
		return -1;
	}

	return ki_dlg_set_property(msg, &val);
}

/**
 *
 */
static int ki_dlg_reset_property(sip_msg_t *msg, str *pval)
{
	dlg_ctx_t *dctx;
	dlg_cell_t *d;

	if(pval->len <= 0) {
		LM_ERR("empty property value\n");
		return -1;
	}
	if((dctx = dlg_get_dlg_ctx()) == NULL)
		return -1;

	if(pval->len == 6 && strncmp(pval->s, "ka-src", 6) == 0) {
		dctx->iflags &= ~(DLG_IFLAG_KA_SRC);
		d = dlg_get_by_iuid(&dctx->iuid);
		if(d != NULL) {
			d->iflags &= ~(DLG_IFLAG_KA_SRC);
			dlg_release(d);
		}
	} else if(pval->len == 6 && strncmp(pval->s, "ka-dst", 6) == 0) {
		dctx->iflags &= ~(DLG_IFLAG_KA_DST);
		d = dlg_get_by_iuid(&dctx->iuid);
		if(d != NULL) {
			d->iflags &= ~(DLG_IFLAG_KA_DST);
			dlg_release(d);
		}
	} else if(pval->len == 15 && strncmp(pval->s, "timeout-noreset", 15) == 0) {
		dctx->iflags &= ~(DLG_IFLAG_TIMER_NORESET);
		d = dlg_get_by_iuid(&dctx->iuid);
		if(d != NULL) {
			d->iflags &= ~(DLG_IFLAG_TIMER_NORESET);
			dlg_release(d);
		}
	} else {
		LM_ERR("unknown property value [%.*s]\n", pval->len, pval->s);
		return -1;
	}

	return 1;
}

/**
 *
 */
static int w_dlg_reset_property(struct sip_msg *msg, char *prop, char *s2)
{
	str val;

	if(fixup_get_svalue(msg, (gparam_t *)prop, &val) != 0) {
		LM_ERR("no property value\n");
		return -1;
	}

	return ki_dlg_reset_property(msg, &val);
}

static int w_dlg_set_timeout_by_profile3(
		struct sip_msg *msg, char *profile, char *value, char *timeout_str)
{
	pv_elem_t *pve = NULL;
	str val_s;

	pve = (pv_elem_t *)value;

	if(pve != NULL && ((struct dlg_profile_table *)profile)->has_value) {
		if(pv_printf_s(msg, pve, &val_s) != 0 || !val_s.s || val_s.len == 0) {
			LM_WARN("cannot get string for value\n");
			return -1;
		}
	}

	if(dlg_set_timeout_by_profile(
			   (struct dlg_profile_table *)profile, &val_s, atoi(timeout_str))
			!= 0)
		return -1;

	return 1;
}

static int w_dlg_set_timeout_by_profile2(
		struct sip_msg *msg, char *profile, char *timeout_str)
{
	return w_dlg_set_timeout_by_profile3(msg, profile, NULL, timeout_str);
}

void dlg_ka_timer_exec(unsigned int ticks, void *param)
{
	dlg_ka_run(ticks);
}

void dlg_clean_timer_exec(unsigned int ticks, void *param)
{
	dlg_clean_run(ticks);
	remove_expired_remote_profiles(time(NULL));
}

static int fixup_dlg_bye(void **param, int param_no)
{
	char *val;
	int n = 0;

	if(param_no == 1) {
		val = (char *)*param;
		if(strcasecmp(val, "all") == 0) {
			n = 0;
		} else if(strcasecmp(val, "caller") == 0) {
			n = 1;
		} else if(strcasecmp(val, "callee") == 0) {
			n = 2;
		} else {
			LM_ERR("invalid param \"%s\"\n", val);
			return E_CFG;
		}
		pkg_free(*param);
		*param = (void *)(long)n;
	} else {
		LM_ERR("called with parameter != 1\n");
		return E_BUG;
	}
	return 0;
}

static int fixup_dlg_refer(void **param, int param_no)
{
	char *val;
	int n = 0;

	if(param_no == 1) {
		val = (char *)*param;
		if(strcasecmp(val, "caller") == 0) {
			n = 1;
		} else if(strcasecmp(val, "callee") == 0) {
			n = 2;
		} else {
			LM_ERR("invalid param \"%s\"\n", val);
			return E_CFG;
		}
		pkg_free(*param);
		*param = (void *)(long)n;
	} else if(param_no == 2) {
		return fixup_spve_null(param, 1);
	} else {
		LM_ERR("called with parameter idx %d\n", param_no);
		return E_BUG;
	}
	return 0;
}

static int fixup_dlg_bridge(void **param, int param_no)
{
	if(param_no >= 1 && param_no <= 3) {
		return fixup_spve_null(param, 1);
	} else {
		LM_ERR("called with parameter idx %d\n", param_no);
		return E_BUG;
	}
	return 0;
}

static int ki_dlg_get_var_helper(
		sip_msg_t *msg, str *sc, str *sf, str *st, str *key, str *val)
{
	dlg_cell_t *dlg = NULL;
	unsigned int dir = 0;

	if(sc == NULL || sc->s == NULL || sc->len == 0) {
		LM_ERR("invalid Call-ID parameter\n");
		return -1;
	}
	if(sf == NULL || sf->s == NULL || sf->len == 0) {
		LM_ERR("invalid From tag parameter\n");
		return -1;
	}
	if(st == NULL) {
		LM_ERR("invalid To tag parameter\n");
		return -1;
	}

	dlg = get_dlg(sc, sf, st, &dir);
	if(dlg == NULL) {
		LM_DBG("dialog not found for call-id: %.*s\n", sc->len, sc->s);
		return -1;
	}
	if(get_dlg_varval(dlg, key, val) != 0) {
		dlg_release(dlg);
		return -1;
	}
	dlg_release(dlg);
	return 0;
}

/**
 *
 */
static sr_kemi_xval_t _sr_kemi_dialog_xval = {0};

static sr_kemi_xval_t *ki_dlg_get_var(
		sip_msg_t *msg, str *sc, str *sf, str *st, str *key)
{
	memset(&_sr_kemi_dialog_xval, 0, sizeof(sr_kemi_xval_t));

	if(ki_dlg_get_var_helper(msg, sc, sf, st, key, &_sr_kemi_dialog_xval.v.s)
			< 0) {
		sr_kemi_xval_null(&_sr_kemi_dialog_xval, SR_KEMI_XVAL_NULL_NONE);
		return &_sr_kemi_dialog_xval;
	}

	_sr_kemi_dialog_xval.vtype = SR_KEMIP_STR;

	return &_sr_kemi_dialog_xval;
}

static int w_dlg_get_var(
		struct sip_msg *msg, char *ci, char *ft, char *tt, char *key, char *pv)
{
	str sc = STR_NULL;
	str sf = STR_NULL;
	str st = STR_NULL;
	str k = STR_NULL;
	sr_kemi_xval_t *val = NULL;
	pv_value_t dst_val;
	pv_spec_t *dst_pv = (pv_spec_t *)pv;

	if(ci == 0 || ft == 0 || tt == 0) {
		LM_ERR("invalid parameters\n");
		goto error;
	}

	if(fixup_get_svalue(msg, (gparam_p)ci, &sc) != 0) {
		LM_ERR("unable to get Call-ID\n");
		goto error;
	}

	if(fixup_get_svalue(msg, (gparam_p)ft, &sf) != 0) {
		LM_ERR("unable to get From tag\n");
		goto error;
	}

	if(fixup_get_svalue(msg, (gparam_p)tt, &st) != 0) {
		LM_ERR("unable to get To Tag\n");
		goto error;
	}
	if(fixup_get_svalue(msg, (gparam_p)key, &k) != 0) {
		LM_ERR("unable to get key name\n");
		goto error;
	}
	val = ki_dlg_get_var(msg, &sc, &sf, &st, &k);
	if(val && val->vtype == SR_KEMIP_STR) {
		memset(&dst_val, 0, sizeof(pv_value_t));
		dst_val.flags |= PV_VAL_STR;
		dst_val.rs.s = val->v.s.s;
		dst_val.rs.len = val->v.s.len;
	} else {
		pv_get_null(msg, NULL, &dst_val);
	}
	if(pv_set_spec_value(msg, dst_pv, 0, &dst_val) != 0) {
		LM_ERR("unable to set value to dst_pv\n");
		if(val)
			goto error;
		else
			return -1;
	}
	return 1;

error:
	pv_get_null(msg, NULL, &dst_val);
	if(pv_set_spec_value(msg, dst_pv, 0, &dst_val) != 0) {
		LM_ERR("unable to set null value to dst_pv\n");
	}
	return -1;
}

static int fixup_dlg_get_var(void **param, int param_no)
{
	if(param_no >= 1 && param_no <= 4)
		return fixup_spve_null(param, 1);
	if(param_no == 5)
		return fixup_pvar_all(param, 1);
	return 0;
}

static int fixup_dlg_get_var_free(void **param, int param_no)
{
	if(param_no <= 4)
		return fixup_free_spve_null(param, 1);
	if(param_no == 5)
		return fixup_free_pvar_all(param, 1);
	return -1;
}

static int ki_dlg_set_var(
		sip_msg_t *msg, str *sc, str *sf, str *st, str *key, str *val)
{
	dlg_cell_t *dlg = NULL;
	unsigned int dir = 0;
	int ret = 1;

	if(sc == NULL || sc->s == NULL || sc->len == 0) {
		LM_ERR("invalid Call-ID parameter\n");
		return -1;
	}
	if(sf == NULL || sf->s == NULL || sf->len == 0) {
		LM_ERR("invalid From tag parameter\n");
		return -1;
	}
	if(st == NULL) {
		LM_ERR("invalid To tag parameter\n");
		return -1;
	}

	dlg = get_dlg(sc, sf, st, &dir);
	if(dlg == NULL)
		return -1;
	if(set_dlg_variable(dlg, key, val) != 0)
		ret = -1;
	dlg_release(dlg);
	return ret;
}

static int w_dlg_set_var(
		struct sip_msg *msg, char *ci, char *ft, char *tt, char *key, char *val)
{
	str sc = STR_NULL;
	str sf = STR_NULL;
	str st = STR_NULL;
	str k = STR_NULL;
	str v = STR_NULL;

	if(ci == 0 || ft == 0 || tt == 0) {
		LM_ERR("invalid parameters\n");
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_p)ci, &sc) != 0) {
		LM_ERR("unable to get Call-ID\n");
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_p)ft, &sf) != 0) {
		LM_ERR("unable to get From tag\n");
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_p)tt, &st) != 0) {
		LM_ERR("unable to get To Tag\n");
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_p)key, &k) != 0) {
		LM_ERR("unable to get key name\n");
		return -1;
	}
	if(fixup_get_svalue(msg, (gparam_p)val, &v) != 0) {
		LM_ERR("unable to get value\n");
		return -1;
	}
	return ki_dlg_set_var(msg, &sc, &sf, &st, &k, &v);
}

static int fixup_dlg_set_var(void **param, int param_no)
{
	if(param_no >= 1 && param_no <= 5)
		return fixup_spve_null(param, 1);
	return 0;
}

static int fixup_dlg_set_var_free(void **param, int param_no)
{
	if(param_no <= 5)
		return fixup_free_spve_null(param, 1);
	return -1;
}

static int ki_dlg_get(sip_msg_t *msg, str *sc, str *sf, str *st)
{
	dlg_cell_t *dlg = NULL;
	unsigned int dir = 0;

	if(sc == NULL || sc->s == NULL || sc->len == 0) {
		LM_ERR("invalid Call-ID parameter\n");
		return -1;
	}
	if(sf == NULL || sf->s == NULL || sf->len == 0) {
		LM_ERR("invalid From tag parameter\n");
		return -1;
	}
	if(st == NULL || st->s == NULL || st->len == 0) {
		LM_ERR("invalid To tag parameter\n");
		return -1;
	}

	dlg = get_dlg(sc, sf, st, &dir);
	if(dlg == NULL)
		return -1;
	/* set shortcut to dialog internal unique id */
	_dlg_ctx.iuid.h_entry = dlg->h_entry;
	_dlg_ctx.iuid.h_id = dlg->h_id;
	_dlg_ctx.dir = dir;
	dlg_release(dlg);
	return 1;
}

static int w_dlg_get(struct sip_msg *msg, char *ci, char *ft, char *tt)
{
	str sc = {0, 0};
	str sf = {0, 0};
	str st = {0, 0};

	if(ci == 0 || ft == 0 || tt == 0) {
		LM_ERR("invalid parameters\n");
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_p)ci, &sc) != 0) {
		LM_ERR("unable to get Call-ID\n");
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_p)ft, &sf) != 0) {
		LM_ERR("unable to get From tag\n");
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_p)tt, &st) != 0) {
		LM_ERR("unable to get To Tag\n");
		return -1;
	}
	if(st.s == NULL || st.len == 0) {
		LM_ERR("invalid To tag parameter\n");
		return -1;
	}

	return ki_dlg_get(msg, &sc, &sf, &st);
}

/**
 *
 */
static int w_dlg_remote_profile(sip_msg_t *msg, char *cmd, char *pname,
		char *pval, char *puid, char *expires)
{
	str scmd;
	str sname;
	str sval;
	str suid;
	int ival;
	int ret;

	if(fixup_get_svalue(msg, (gparam_t *)cmd, &scmd) != 0) {
		LM_ERR("unable to get command\n");
		return -1;
	}
	if(fixup_get_svalue(msg, (gparam_t *)pname, &sname) != 0) {
		LM_ERR("unable to get profile name\n");
		return -1;
	}
	if(fixup_get_svalue(msg, (gparam_t *)pval, &sval) != 0) {
		LM_ERR("unable to get profile value\n");
		return -1;
	}
	if(fixup_get_svalue(msg, (gparam_t *)puid, &suid) != 0) {
		LM_ERR("unable to get profile uid\n");
		return -1;
	}
	if(fixup_get_ivalue(msg, (gparam_t *)expires, &ival) != 0) {
		LM_ERR("no hash entry value value\n");
		return -1;
	}

	ret = dlg_cmd_remote_profile(&scmd, &sname, &sval, &suid, (time_t)ival, 0);
	if(ret == 0)
		return 1;
	return ret;
}

static int fixup_dlg_remote_profile(void **param, int param_no)
{
	if(param_no >= 1 && param_no <= 4)
		return fixup_spve_null(param, 1);
	if(param_no == 5)
		return fixup_igp_null(param, 1);
	return 0;
}

/**
 *
 */
static int ki_dlg_bye(sip_msg_t *msg, str *side)
{
	dlg_cell_t *dlg = NULL;

	dlg = dlg_get_ctx_dialog();
	if(dlg == NULL)
		return -1;

	if(side->len == 6 && strncasecmp(side->s, "caller", 6) == 0) {
		if(dlg_bye(dlg, NULL, DLG_CALLER_LEG) != 0)
			goto error;
		goto done;
	} else if(side->len == 6 && strncasecmp(side->s, "callee", 6) == 0) {
		if(dlg_bye(dlg, NULL, DLG_CALLEE_LEG) != 0)
			goto error;
		goto done;
	} else {
		if(dlg_bye_all(dlg, NULL) != 0)
			goto error;
		goto done;
	}

done:
	dlg_release(dlg);
	return 1;

error:
	dlg_release(dlg);
	return -1;
}

/**
 *
 */
static int ki_dlg_set_timeout_id(sip_msg_t *msg, int to, int he, int hi)
{
	dlg_cell_t *dlg = NULL;

	dlg = dlg_lookup(he, hi);
	if(dlg == NULL) {
		LM_DBG("no dialog found\n");
		return -1;
	}

	/* update_dlg_timeout() does dlg_release() */
	if(update_dlg_timeout(dlg, to) != 0)
		return -1;

	return 1;
}

/**
 *
 */
static int ki_dlg_set_timeout(sip_msg_t *msg, int to)
{
	dlg_cell_t *dlg = NULL;

	dlg = dlg_get_msg_dialog(msg);
	if(dlg == NULL) {
		LM_DBG("no dialog found\n");
		return -1;
	}

	/* update_dlg_timeout() does dlg_release() */
	if(update_dlg_timeout(dlg, to) != 0)
		return -1;

	return 1;
}

/**
 *
 */
static int ki_set_dlg_profile_static(sip_msg_t *msg, str *sprofile)
{
	struct dlg_profile_table *profile = NULL;

	if(sprofile == NULL || sprofile->s == NULL || sprofile->len <= 0) {
		LM_ERR("invalid profile identifier\n");
		return -1;
	}
	profile = search_dlg_profile(sprofile);
	if(profile == NULL) {
		LM_CRIT("profile <%.*s> not defined\n", sprofile->len, sprofile->s);
		return -1;
	}

	return w_set_dlg_profile_helper(msg, profile, NULL);
}

/**
 *
 */
static int ki_set_dlg_profile(sip_msg_t *msg, str *sprofile, str *svalue)
{
	struct dlg_profile_table *profile = NULL;

	if(sprofile == NULL || sprofile->s == NULL || sprofile->len <= 0) {
		LM_ERR("invalid profile identifier\n");
		return -1;
	}
	profile = search_dlg_profile(sprofile);
	if(profile == NULL) {
		LM_CRIT("profile <%.*s> not defined\n", sprofile->len, sprofile->s);
		return -1;
	}

	return w_set_dlg_profile_helper(msg, profile, svalue);
}

/**
 *
 */
static int ki_unset_dlg_profile_static(sip_msg_t *msg, str *sprofile)
{
	struct dlg_profile_table *profile = NULL;

	if(sprofile == NULL || sprofile->s == NULL || sprofile->len <= 0) {
		LM_ERR("invalid profile identifier\n");
		return -1;
	}
	profile = search_dlg_profile(sprofile);
	if(profile == NULL) {
		LM_CRIT("profile <%.*s> not defined\n", sprofile->len, sprofile->s);
		return -1;
	}

	return w_unset_dlg_profile_helper(msg, profile, NULL);
}

/**
 *
 */
static int ki_unset_dlg_profile(sip_msg_t *msg, str *sprofile, str *svalue)
{
	struct dlg_profile_table *profile = NULL;

	if(sprofile == NULL || sprofile->s == NULL || sprofile->len <= 0) {
		LM_ERR("invalid profile identifier\n");
		return -1;
	}
	profile = search_dlg_profile(sprofile);
	if(profile == NULL) {
		LM_CRIT("profile <%.*s> not defined\n", sprofile->len, sprofile->s);
		return -1;
	}

	return w_unset_dlg_profile_helper(msg, profile, svalue);
}

/**
 *
 */
static int ki_is_in_profile_static(sip_msg_t *msg, str *sprofile)
{
	struct dlg_profile_table *profile = NULL;

	if(sprofile == NULL || sprofile->s == NULL || sprofile->len <= 0) {
		LM_ERR("invalid profile identifier\n");
		return -1;
	}
	profile = search_dlg_profile(sprofile);
	if(profile == NULL) {
		LM_CRIT("profile <%.*s> not defined\n", sprofile->len, sprofile->s);
		return -1;
	}

	return w_is_in_profile_helper(msg, profile, NULL);
}

/**
 *
 */
static int ki_is_in_profile(sip_msg_t *msg, str *sprofile, str *svalue)
{
	struct dlg_profile_table *profile = NULL;

	if(sprofile == NULL || sprofile->s == NULL || sprofile->len <= 0) {
		LM_ERR("invalid profile identifier\n");
		return -1;
	}
	profile = search_dlg_profile(sprofile);
	if(profile == NULL) {
		LM_CRIT("profile <%.*s> not defined\n", sprofile->len, sprofile->s);
		return -1;
	}

	return w_is_in_profile_helper(msg, profile, svalue);
}

/**
 *
 */
static int ki_get_profile_size_static(sip_msg_t *msg, str *sprofile, str *spv)
{
	struct dlg_profile_table *profile = NULL;
	pv_spec_t *pvs = NULL;

	if(sprofile == NULL || sprofile->s == NULL || sprofile->len <= 0) {
		LM_ERR("invalid profile identifier\n");
		return -1;
	}
	if(spv == NULL || spv->s == NULL || spv->len <= 0) {
		LM_ERR("invalid destination var name\n");
		return -1;
	}
	profile = search_dlg_profile(sprofile);
	if(profile == NULL) {
		LM_CRIT("profile <%.*s> not defined\n", sprofile->len, sprofile->s);
		return -1;
	}
	pvs = pv_cache_get(spv);
	if(pvs == NULL) {
		LM_ERR("cannot get pv spec for [%.*s]\n", spv->len, spv->s);
		return -1;
	}
	if(pvs->type != PVT_AVP && pvs->type != PVT_SCRIPTVAR) {
		LM_ERR("return must be an AVP or SCRIPT VAR!\n");
		return -1;
	}

	return w_get_profile_size_helper(msg, profile, NULL, pvs);
}

/**
 *
 */
static int ki_get_profile_size(
		sip_msg_t *msg, str *sprofile, str *svalue, str *spv)
{
	struct dlg_profile_table *profile = NULL;
	pv_spec_t *pvs = NULL;

	if(sprofile == NULL || sprofile->s == NULL || sprofile->len <= 0) {
		LM_ERR("invalid profile identifier\n");
		return -1;
	}
	if(spv == NULL || spv->s == NULL || spv->len <= 0) {
		LM_ERR("invalid destination var name\n");
		return -1;
	}
	profile = search_dlg_profile(sprofile);
	if(profile == NULL) {
		LM_CRIT("profile <%.*s> not defined\n", sprofile->len, sprofile->s);
		return -1;
	}
	pvs = pv_cache_get(spv);
	if(pvs == NULL) {
		LM_ERR("cannot get pv spec for [%.*s]\n", spv->len, spv->s);
		return -1;
	}
	if(pvs->type != PVT_AVP && pvs->type != PVT_SCRIPTVAR) {
		LM_ERR("return must be an AVP or SCRIPT VAR!\n");
		return -1;
	}

	return w_get_profile_size_helper(msg, profile, svalue, pvs);
}

/**
 *
 */
static int ki_dlg_db_load_callid(sip_msg_t *msg, str *callid)
{
	int ret;

	ret = load_dialog_info_from_db(dlg_hash_size, db_fetch_rows, 1, callid);

	if(ret == 0)
		return 1;
	return ret;
}

/**
 *
 */
static int w_dlg_db_load_callid(sip_msg_t *msg, char *ci, char *p2)
{
	str sc = {0, 0};

	if(ci == 0) {
		LM_ERR("invalid parameters\n");
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_t *)ci, &sc) != 0) {
		LM_ERR("unable to get Call-ID\n");
		return -1;
	}

	return ki_dlg_db_load_callid(msg, &sc);
}

/**
 *
 */
static int ki_dlg_db_load_extra(sip_msg_t *msg)
{
	int ret;

	ret = load_dialog_info_from_db(dlg_hash_size, db_fetch_rows, 2, NULL);

	if(ret == 0)
		return 1;
	return ret;
}

/**
 *
 */
static int w_dlg_db_load_extra(sip_msg_t *msg, char *p1, char *p2)
{
	return ki_dlg_db_load_extra(msg);
}

/**
 *
 */
static int ki_dlg_var_sets(sip_msg_t *msg, str *name, str *val)
{
	dlg_cell_t *dlg;
	int ret;

	dlg = dlg_get_msg_dialog(msg);
	if(dlg) {
		dlg_cell_lock(dlg);
	}
	ret = set_dlg_variable_unsafe(dlg, name, val);
	if(dlg) {
		dlg_cell_unlock(dlg);
		dlg_release(dlg);
	}

	return (ret == 0) ? 1 : ret;
}

/**
 *
 */
static sr_kemi_xval_t *ki_dlg_var_get_mode(sip_msg_t *msg, str *name, int rmode)
{
	dlg_cell_t *dlg;

	memset(&_sr_kemi_dialog_xval, 0, sizeof(sr_kemi_xval_t));

	dlg = dlg_get_msg_dialog(msg);
	if(dlg == NULL) {
		sr_kemi_xval_null(&_sr_kemi_dialog_xval, rmode);
		return &_sr_kemi_dialog_xval;
	}
	if(get_dlg_varval(dlg, name, &_sr_kemi_dialog_xval.v.s) < 0) {
		sr_kemi_xval_null(&_sr_kemi_dialog_xval, rmode);
		goto done;
	}

	_sr_kemi_dialog_xval.vtype = SR_KEMIP_STR;

done:
	dlg_release(dlg);
	return &_sr_kemi_dialog_xval;
}

/**
 *
 */
static sr_kemi_xval_t *ki_dlg_var_get(sip_msg_t *msg, str *name)
{
	return ki_dlg_var_get_mode(msg, name, SR_KEMI_XVAL_NULL_NONE);
}

/**
 *
 */
static sr_kemi_xval_t *ki_dlg_var_gete(sip_msg_t *msg, str *name)
{
	return ki_dlg_var_get_mode(msg, name, SR_KEMI_XVAL_NULL_EMPTY);
}
/**
 *
 */
static sr_kemi_xval_t *ki_dlg_var_getw(sip_msg_t *msg, str *name)
{
	return ki_dlg_var_get_mode(msg, name, SR_KEMI_XVAL_NULL_PRINT);
}

/**
 *
 */
static int ki_dlg_var_rm(sip_msg_t *msg, str *name)
{
	dlg_cell_t *dlg;

	dlg = dlg_get_msg_dialog(msg);
	if(dlg) {
		dlg_cell_lock(dlg);
		set_dlg_variable_unsafe(dlg, name, NULL);
		dlg_cell_unlock(dlg);
		dlg_release(dlg);
	}
	return 1;
}

/**
 *
 */
static int ki_dlg_var_is_null(sip_msg_t *msg, str *name)
{
	dlg_cell_t *dlg;
	int ret;

	dlg = dlg_get_msg_dialog(msg);
	if(dlg == NULL) {
		return 1;
	}
	ret = get_dlg_varstatus(dlg, name);
	if(ret == 1) {
		return 1;
	}
	return -1;
}

/**
 *
 */
/* clang-format off */
static sr_kemi_t sr_kemi_dialog_exports[] = {
	{ str_init("dialog"), str_init("dlg_manage"),
		SR_KEMIP_INT, dlg_manage,
		{ SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_set_state"),
		SR_KEMIP_INT, ki_dlg_set_state,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_update_state"),
		SR_KEMIP_INT, ki_dlg_update_state,
		{ SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_bye"),
		SR_KEMIP_INT, ki_dlg_bye,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("is_known_dlg"),
		SR_KEMIP_INT, is_known_dlg,
		{ SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_set_timeout"),
		SR_KEMIP_INT, ki_dlg_set_timeout,
		{ SR_KEMIP_INT, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_set_timeout_id"),
		SR_KEMIP_INT, ki_dlg_set_timeout_id,
		{ SR_KEMIP_INT, SR_KEMIP_INT, SR_KEMIP_INT,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_set_property"),
		SR_KEMIP_INT, ki_dlg_set_property,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_reset_property"),
		SR_KEMIP_INT, ki_dlg_reset_property,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_get"),
		SR_KEMIP_INT, ki_dlg_get,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_get_var"),
		SR_KEMIP_XVAL, ki_dlg_get_var,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR,
			SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_set_var"),
		SR_KEMIP_INT, ki_dlg_set_var,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR,
			SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("set_dlg_profile_static"),
		SR_KEMIP_INT, ki_set_dlg_profile_static,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("set_dlg_profile"),
		SR_KEMIP_INT, ki_set_dlg_profile,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("unset_dlg_profile_static"),
		SR_KEMIP_INT, ki_unset_dlg_profile_static,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("unset_dlg_profile"),
		SR_KEMIP_INT, ki_unset_dlg_profile,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("is_in_profile_static"),
		SR_KEMIP_INT, ki_is_in_profile_static,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("is_in_profile"),
		SR_KEMIP_INT, ki_is_in_profile,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("get_profile_size_static"),
		SR_KEMIP_INT, ki_get_profile_size_static,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("get_profile_size"),
		SR_KEMIP_INT, ki_get_profile_size,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_setflag"),
		SR_KEMIP_INT, ki_dlg_setflag,
		{ SR_KEMIP_INT, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_resetflag"),
		SR_KEMIP_INT, ki_dlg_resetflag,
		{ SR_KEMIP_INT, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_isflagset"),
		SR_KEMIP_INT, ki_dlg_isflagset,
		{ SR_KEMIP_INT, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_db_load_callid"),
		SR_KEMIP_INT, ki_dlg_db_load_callid,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_db_load_extra"),
		SR_KEMIP_INT, ki_dlg_db_load_extra,
		{ SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("var_sets"),
		SR_KEMIP_INT, ki_dlg_var_sets,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("var_get"),
		SR_KEMIP_XVAL, ki_dlg_var_get,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("var_gete"),
		SR_KEMIP_XVAL, ki_dlg_var_gete,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("var_getw"),
		SR_KEMIP_XVAL, ki_dlg_var_getw,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("var_rm"),
		SR_KEMIP_INT, ki_dlg_var_rm,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("var_is_null"),
		SR_KEMIP_INT, ki_dlg_var_is_null,
		{ SR_KEMIP_STR, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("dialog"), str_init("dlg_bridge"),
		SR_KEMIP_INT, ki_dlg_bridge,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},

	{ {0, 0}, {0, 0}, 0, NULL, { 0, 0, 0, 0, 0, 0 } }
};
/* clang-format on */

/**
 *
 */
int mod_register(char *path, int *dlflags, void *p1, void *p2)
{
	sr_kemi_modules_add(sr_kemi_dialog_exports);
	return 0;
}

/**************************** RPC functions ******************************/
/*!
 * \brief Helper method that outputs a dialog in a file
 * \see rpc_dump_file_dlg
 * \param dlg printed dialog
 * \param output file descriptor
 * \return 0 on success, -1 on failure
 */
static inline void internal_rpc_dump_file_dlg(dlg_cell_t *dlg, FILE *dialogf)
{
	dlg_profile_link_t *pl;
	dlg_var_t *var;
	srjson_doc_t jdoc;
	srjson_t *jdoc_caller = NULL;
	srjson_t *jdoc_callee = NULL;
	srjson_t *jdoc_profiles = NULL;
	srjson_t *jdoc_variables = NULL;

	srjson_InitDoc(&jdoc, NULL);
	jdoc.root = srjson_CreateObject(&jdoc);
	if(!jdoc.root) {
		LM_ERR("cannot create json\n");
		goto clear;
	}
	srjson_AddNumberToObject(&jdoc, jdoc.root, "h_entry", dlg->h_entry);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "h_id", dlg->h_id);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "ref", dlg->ref);
	srjson_AddStrToObject(
			&jdoc, jdoc.root, "call_id", dlg->callid.s, dlg->callid.len);
	srjson_AddStrToObject(
			&jdoc, jdoc.root, "from_uri", dlg->from_uri.s, dlg->from_uri.len);
	srjson_AddStrToObject(
			&jdoc, jdoc.root, "to_uri", dlg->to_uri.s, dlg->to_uri.len);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "state", dlg->state);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "start_ts", dlg->start_ts);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "init_ts", dlg->init_ts);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "end_ts", dlg->end_ts);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "timeout",
			dlg->tl.timeout ? time(0) + dlg->tl.timeout - get_ticks() : 0);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "lifetime", dlg->lifetime);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "dflags", dlg->dflags);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "sflags", dlg->sflags);
	srjson_AddNumberToObject(&jdoc, jdoc.root, "iflags", dlg->iflags);

	jdoc_caller = srjson_CreateObject(&jdoc);
	if(!jdoc_caller) {
		LM_ERR("cannot create json caller\n");
		goto clear;
	}
	srjson_AddStrToObject(&jdoc, jdoc_caller, "tag", dlg->tag[DLG_CALLER_LEG].s,
			dlg->tag[DLG_CALLER_LEG].len);
	srjson_AddStrToObject(&jdoc, jdoc_caller, "contact",
			dlg->contact[DLG_CALLER_LEG].s, dlg->contact[DLG_CALLER_LEG].len);
	srjson_AddStrToObject(&jdoc, jdoc_caller, "cseq",
			dlg->cseq[DLG_CALLER_LEG].s, dlg->cseq[DLG_CALLER_LEG].len);
	srjson_AddStrToObject(&jdoc, jdoc_caller, "route_set",
			dlg->route_set[DLG_CALLER_LEG].s,
			dlg->route_set[DLG_CALLER_LEG].len);
	srjson_AddStrToObject(&jdoc, jdoc_caller, "socket",
			dlg->bind_addr[DLG_CALLER_LEG]
					? dlg->bind_addr[DLG_CALLER_LEG]->sock_str.s
					: empty_str.s,
			dlg->bind_addr[DLG_CALLER_LEG]
					? dlg->bind_addr[DLG_CALLER_LEG]->sock_str.len
					: empty_str.len);
	srjson_AddItemToObject(&jdoc, jdoc.root, "caller", jdoc_caller);

	jdoc_callee = srjson_CreateObject(&jdoc);
	if(!jdoc_callee) {
		LM_ERR("cannot create json callee\n");
		goto clear;
	}
	srjson_AddStrToObject(&jdoc, jdoc_callee, "tag", dlg->tag[DLG_CALLEE_LEG].s,
			dlg->tag[DLG_CALLEE_LEG].len);
	srjson_AddStrToObject(&jdoc, jdoc_callee, "contact",
			dlg->contact[DLG_CALLEE_LEG].s, dlg->contact[DLG_CALLEE_LEG].len);
	srjson_AddStrToObject(&jdoc, jdoc_callee, "cseq",
			dlg->cseq[DLG_CALLEE_LEG].s, dlg->cseq[DLG_CALLEE_LEG].len);
	srjson_AddStrToObject(&jdoc, jdoc_callee, "route_set",
			dlg->route_set[DLG_CALLEE_LEG].s,
			dlg->route_set[DLG_CALLEE_LEG].len);
	srjson_AddStrToObject(&jdoc, jdoc_callee, "socket",
			dlg->bind_addr[DLG_CALLEE_LEG]
					? dlg->bind_addr[DLG_CALLEE_LEG]->sock_str.s
					: empty_str.s,
			dlg->bind_addr[DLG_CALLEE_LEG]
					? dlg->bind_addr[DLG_CALLEE_LEG]->sock_str.len
					: empty_str.len);
	srjson_AddItemToObject(&jdoc, jdoc.root, "callee", jdoc_callee);

	// profiles section
	jdoc_profiles = srjson_CreateObject(&jdoc);
	if(!jdoc_profiles) {
		LM_ERR("cannot create json profiles\n");
		goto clear;
	}
	for(pl = dlg->profile_links; pl && (dlg->state < DLG_STATE_DELETED);
			pl = pl->next) {
		if(pl->profile->has_value) {
			srjson_AddStrToObject(&jdoc, jdoc_profiles, pl->profile->name.s,
					pl->hash_linker.value.s, pl->hash_linker.value.len);
		} else {
			srjson_AddStrToObject(&jdoc, jdoc_profiles, pl->profile->name.s,
					empty_str.s, empty_str.len);
		}
	}
	srjson_AddItemToObject(&jdoc, jdoc.root, "profiles", jdoc_profiles);

	// variables section
	jdoc_variables = srjson_CreateObject(&jdoc);
	if(!jdoc_variables) {
		LM_ERR("cannot create json variables\n");
		goto clear;
	}
	for(var = dlg->vars; var && (dlg->state < DLG_STATE_DELETED);
			var = var->next) {
		srjson_AddStrToObject(&jdoc, jdoc_variables, var->key.s, var->value.s,
				var->value.len);
	}
	srjson_AddItemToObject(&jdoc, jdoc.root, "variables", jdoc_variables);

	// serialize and print to file
	jdoc.buf.s = srjson_PrintUnformatted(&jdoc, jdoc.root);
	if(!jdoc.buf.s) {
		LM_ERR("unable to serialize data\n");
		goto clear;
	}
	jdoc.buf.len = strlen(jdoc.buf.s);
	LM_DBG("sending serialized data %.*s\n", jdoc.buf.len, jdoc.buf.s);
	fprintf(dialogf, "%s\n", jdoc.buf.s);

clear:
	if(jdoc.buf.s) {
		jdoc.free_fn(jdoc.buf.s);
		jdoc.buf.s = NULL;
	}
	srjson_DestroyDoc(&jdoc);
	return;
}

/*!
 * \brief Helper method that outputs a dialog via the RPC interface
 * \see rpc_print_dlg
 * \param rpc RPC node that should be filled
 * \param c RPC void pointer
 * \param dlg printed dialog
 * \param with_context if 1 then the dialog context will be also printed
 * \return 0 on success, -1 on failure
 */
static inline void internal_rpc_print_dlg(
		rpc_t *rpc, void *c, dlg_cell_t *dlg, int with_context)
{
	rpc_cb_ctx_t rpc_cb;
	void *h, *sh, *ssh;
	dlg_profile_link_t *pl;
	dlg_var_t *var;
	time_t tnow;
	int tdur;

	if(rpc->add(c, "{", &h) < 0)
		goto error;

	tnow = time(NULL);
	if(dlg->end_ts) {
		tdur = (int)(dlg->end_ts - dlg->start_ts);
	} else if(dlg->start_ts) {
		tdur = (int)(tnow - dlg->start_ts);
	} else {
		tdur = 0;
	}
	rpc->struct_add(h, "dddSSSdddddddddd", "h_entry", dlg->h_entry, "h_id",
			dlg->h_id, "ref", dlg->ref, "call-id", &dlg->callid, "from_uri",
			&dlg->from_uri, "to_uri", &dlg->to_uri, "state", dlg->state,
			"start_ts", dlg->start_ts, "init_ts", dlg->init_ts, "end_ts",
			dlg->end_ts, "duration", tdur, "timeout",
			dlg->tl.timeout ? tnow + dlg->tl.timeout - get_ticks() : 0,
			"lifetime", dlg->lifetime, "dflags", dlg->dflags, "sflags",
			dlg->sflags, "iflags", dlg->iflags);

	if(rpc->struct_add(h, "{", "caller", &sh) < 0)
		goto error;
	rpc->struct_add(sh, "SSSSS", "tag", &dlg->tag[DLG_CALLER_LEG], "contact",
			&dlg->contact[DLG_CALLER_LEG], "cseq", &dlg->cseq[DLG_CALLER_LEG],
			"route_set", &dlg->route_set[DLG_CALLER_LEG], "socket",
			dlg->bind_addr[DLG_CALLER_LEG]
					? &dlg->bind_addr[DLG_CALLER_LEG]->sock_str
					: &empty_str);

	if(rpc->struct_add(h, "{", "callee", &sh) < 0)
		goto error;
	rpc->struct_add(sh, "SSSSS", "tag", &dlg->tag[DLG_CALLEE_LEG], "contact",
			&dlg->contact[DLG_CALLEE_LEG], "cseq", &dlg->cseq[DLG_CALLEE_LEG],
			"route_set", &dlg->route_set[DLG_CALLEE_LEG], "socket",
			dlg->bind_addr[DLG_CALLEE_LEG]
					? &dlg->bind_addr[DLG_CALLEE_LEG]->sock_str
					: &empty_str);

	if(rpc->struct_add(h, "[", "profiles", &sh) < 0)
		goto error;
	for(pl = dlg->profile_links; pl && (dlg->state < DLG_STATE_DELETED);
			pl = pl->next) {
		if(pl->profile->has_value) {
			rpc->array_add(sh, "{", &ssh);
			rpc->struct_add(
					ssh, "S", pl->profile->name.s, &pl->hash_linker.value);
		} else {
			rpc->array_add(sh, "S", &pl->profile->name);
		}
	}

	if(rpc->struct_add(h, "[", "variables", &sh) < 0)
		goto error;
	for(var = dlg->vars; var && (dlg->state < DLG_STATE_DELETED);
			var = var->next) {
		rpc->array_add(sh, "{", &ssh);
		rpc->struct_add(ssh, "S", var->key.s, &var->value);
	}

	if(with_context) {
		rpc_cb.rpc = rpc;
		rpc_cb.c = h;
		run_dlg_callbacks(DLGCB_RPC_CONTEXT, dlg, NULL, NULL, DLG_DIR_NONE,
				(void *)&rpc_cb);
	}

	return;
error:
	LM_ERR("Failed to add item to RPC response\n");
	return;
}

/*!
 * \brief Helper function that outputs all dialogs via the RPC interface
 * \see rpc_dump_file_dlgs
 * \param rpc RPC node that should be filled
 * \param c RPC void pointer
 * \param with_context if 1 then the dialog context will be also printed
 */
static void internal_rpc_dump_file_dlgs(rpc_t *rpc, void *c, int with_context)
{
	dlg_cell_t *dlg;
	str output_file_name;
	FILE *dialogf;
	unsigned int i;
	if(rpc->scan(c, ".S", &output_file_name) < 1)
		return;

	dialogf = fopen(output_file_name.s, "a+");
	if(!dialogf) {
		LM_ERR("failed to open output file: %s\n", output_file_name.s);
		return;
	}

	for(i = 0; i < d_table->size; i++) {
		dlg_lock(d_table, &(d_table->entries[i]));

		for(dlg = d_table->entries[i].first; dlg; dlg = dlg->next) {
			internal_rpc_dump_file_dlg(dlg, dialogf);
		}
		dlg_unlock(d_table, &(d_table->entries[i]));
	}
	fclose(dialogf);
}

/*!
 * \brief Helper function that outputs all dialogs via the RPC interface
 * \see rpc_print_dlgs
 * \param rpc RPC node that should be filled
 * \param c RPC void pointer
 * \param with_context if 1 then the dialog context will be also printed
 */
static void internal_rpc_print_dlgs(rpc_t *rpc, void *c, int with_context)
{
	dlg_cell_t *dlg;
	unsigned int i;

	for(i = 0; i < d_table->size; i++) {
		dlg_lock(d_table, &(d_table->entries[i]));

		for(dlg = d_table->entries[i].first; dlg; dlg = dlg->next) {
			internal_rpc_print_dlg(rpc, c, dlg, with_context);
		}
		dlg_unlock(d_table, &(d_table->entries[i]));
	}
}

/*!
 * \brief Helper function that outputs a dialog via the RPC interface
 * \see rpc_print_dlgs
 * \param rpc RPC node that should be filled
 * \param c RPC void pointer
 * \param with_context if 1 then the dialog context will be also printed
 */
static void internal_rpc_print_single_dlg(rpc_t *rpc, void *c, int with_context)
{
	str callid, ft;
	str *from_tag = NULL;
	dlg_entry_t *d_entry;
	dlg_cell_t *dlg;
	unsigned int h_entry;

	if(rpc->scan(c, ".S", &callid) < 1)
		return;

	h_entry = core_hash(&callid, 0, d_table->size);
	d_entry = &(d_table->entries[h_entry]);

	if(rpc->scan(c, "*.S", &ft) == 1) {
		from_tag = &ft;
	}

	dlg_lock(d_table, d_entry);
	for(dlg = d_entry->first; dlg; dlg = dlg->next) {
		if(match_downstream_dialog(dlg, &callid, from_tag) == 1) {
			internal_rpc_print_dlg(rpc, c, dlg, with_context);
		}
	}
	dlg_unlock(d_table, d_entry);
}

/*!
 * \brief Helper function that outputs the size of a given profile via the RPC interface
 * \see rpc_profile_get_size
 * \see rpc_profile_w_value_get_size
 * \param rpc RPC node that should be filled
 * \param c RPC void pointer
 * \param profile_name the given profile
 * \param value the given profile value
 */
static void internal_rpc_profile_get_size(
		rpc_t *rpc, void *c, str *profile_name, str *value)
{
	unsigned int size;
	dlg_profile_table_t *profile;

	profile = search_dlg_profile(profile_name);
	if(!profile) {
		rpc->fault(c, 404, "Profile not found: %.*s", profile_name->len,
				profile_name->s);
		return;
	}
	size = get_profile_size(profile, value);
	rpc->add(c, "d", size);
	return;
}

/*!
 * \brief Helper function that outputs the dialogs belonging to a given profile via the RPC interface
 * \see rpc_profile_print_dlgs
 * \see rpc_profile_w_value_print_dlgs
 * \param rpc RPC node that should be filled
 * \param c RPC void pointer
 * \param profile_name the given profile
 * \param value the given profile value
 * \param with_context if 1 then the dialog context will be also printed
 */
static void internal_rpc_profile_print_dlgs(
		rpc_t *rpc, void *c, str *profile_name, str *value)
{
	dlg_profile_table_t *profile;
	dlg_profile_hash_t *ph;
	unsigned int i;

	profile = search_dlg_profile(profile_name);
	if(!profile) {
		rpc->fault(c, 404, "Profile not found: %.*s", profile_name->len,
				profile_name->s);
		return;
	}

	/* go through the hash and print the dialogs */
	if(profile->has_value == 0)
		value = NULL;

	lock_get(&profile->lock);
	for(i = 0; i < profile->size; i++) {
		ph = profile->entries[i].first;
		if(ph) {
			do {
				if((!value || (STR_EQ(*value, ph->value))) && ph->dlg) {
					/* print dialog */
					internal_rpc_print_dlg(rpc, c, ph->dlg, 0);
				}
				/* next */
				ph = ph->next;
			} while(ph != profile->entries[i].first);
		}
	}
	lock_release(&profile->lock);
}

/*
 * Wrapper around is_known_dlg().
 */

static int w_is_known_dlg(sip_msg_t *msg)
{
	return is_known_dlg(msg);
}

static int w_dlg_set_ruri(sip_msg_t *msg, char *p1, char *p2)
{
	return dlg_set_ruri(msg);
}

static const char *rpc_print_dlgs_doc[2] = {"Print all dialogs", 0};
static const char *rpc_dump_file_dlgs_doc[2] = {
		"Print all dialogs to json file", 0};
static const char *rpc_print_dlgs_ctx_doc[2] = {
		"Print all dialogs with associated context", 0};
static const char *rpc_dlg_list_match_doc[2] = {"Print matching dialogs", 0};
static const char *rpc_dlg_list_match_ctx_doc[2] = {
		"Print matching dialogs with associated context", 0};
static const char *rpc_print_dlg_doc[2] = {
		"Print dialog based on callid and optionally fromtag", 0};
static const char *rpc_print_dlg_ctx_doc[2] = {
		"Print dialog with associated context based on callid and optionally "
		"fromtag",
		0};
static const char *rpc_end_dlg_entry_id_doc[2] = {
		"End a given dialog based on [h_entry] [h_id]", 0};
static const char *rpc_dlg_terminate_dlg_doc[2] = {
		"End a given dialog based on callid", 0};
static const char *rpc_dlg_set_state_doc[3] = {
		"Set state for a dialog based on callid and tags",
		"It is targeting the need to update from state 4 (confirmed) to 5 "
		"(terminated)",
		0};
static const char *rpc_profile_get_size_doc[2] = {
		"Returns the number of dialogs belonging to a profile", 0};
static const char *rpc_profile_print_dlgs_doc[2] = {
		"Lists all the dialogs belonging to a profile", 0};
static const char *rpc_dlg_bridge_doc[2] = {
		"Bridge two SIP addresses in a call using INVITE(hold)-REFER-BYE "
		"mechanism:"
		" to, from, [outbound SIP proxy]",
		0};
static const char *rpc_dlg_is_alive_doc[2] = {
		"Check whether dialog is alive or not", 0};


static void rpc_print_dlgs(rpc_t *rpc, void *c)
{
	internal_rpc_print_dlgs(rpc, c, 0);
}
static void rpc_dump_file_dlgs(rpc_t *rpc, void *c)
{
	internal_rpc_dump_file_dlgs(rpc, c, 0);
}
static void rpc_print_dlgs_ctx(rpc_t *rpc, void *c)
{
	internal_rpc_print_dlgs(rpc, c, 1);
}
static void rpc_print_dlg(rpc_t *rpc, void *c)
{
	internal_rpc_print_single_dlg(rpc, c, 0);
}
static void rpc_print_dlg_ctx(rpc_t *rpc, void *c)
{
	internal_rpc_print_single_dlg(rpc, c, 1);
}

static void rpc_dlg_terminate_dlg(rpc_t *rpc, void *c)
{
	str callid = {NULL, 0};
	str ftag = {NULL, 0};
	str ttag = {NULL, 0};

	dlg_cell_t *dlg = NULL;
	unsigned int dir;
	int ret = 0;
	dir = 0;


	if(rpc->scan(c, ".S.S.S", &callid, &ftag, &ttag) < 3) {
		LM_ERR("Unable to read the parameters dlg_terminate_dlg \n");
		rpc->fault(c, 400, "Need a Callid ,from tag ,to tag");
		return;
	}

	dlg = get_dlg(&callid, &ftag, &ttag, &dir);

	if(dlg == NULL) {
		LM_ERR("Couldnt find callid in dialog '%.*s' \n", callid.len, callid.s);
		rpc->fault(c, 500, "Couldnt find callid in dialog");
		return;
	}

	LM_DBG("Dialog is found with callid  for terminate rpc '%.*s' \n",
			callid.len, callid.s);

	ret = dlg_bye_all(dlg, NULL);

	LM_DBG("Dialog bye return code %d \n", ret);

	if(ret >= 0) {
		LM_WARN("Dialog is terminated callid: '%.*s' \n", callid.len, callid.s);
		dlg_release(dlg);
	}
}

static void rpc_dlg_set_state(rpc_t *rpc, void *c)
{
	str callid = {NULL, 0};
	str ftag = {NULL, 0};
	str ttag = {NULL, 0};
	int sval = DLG_STATE_DELETED;
	int ostate = 0;

	dlg_cell_t *dlg = NULL;
	unsigned int dir;
	int unref = 1;
	dir = 0;

	if(rpc->scan(c, ".S.S.Sd", &callid, &ftag, &ttag, &sval) < 3) {
		LM_ERR("unable to read the parameters\n");
		rpc->fault(c, 400, "Need the callid, from tag,to tag and state");
		return;
	}
	if(sval < DLG_STATE_UNCONFIRMED || sval > DLG_STATE_DELETED) {
		LM_ERR("invalid new state value: %d\n", sval);
		rpc->fault(c, 500, "Invalid state value");
		return;
	}

	dlg = get_dlg(&callid, &ftag, &ttag, &dir);

	if(dlg == NULL) {
		LM_ERR("dialog not found - callid '%.*s' \n", callid.len, callid.s);
		rpc->fault(c, 500, "Dialog not found");
		return;
	}

	LM_DBG("dialog found - callid '%.*s'\n", callid.len, callid.s);

	if(dlg->state != DLG_STATE_CONFIRMED || sval != DLG_STATE_DELETED) {
		LM_WARN("updating states for not confirmed dialogs not properly "
				"supported yet,"
				" use at own risk: '%.*s'\n",
				callid.len, callid.s);
	}

	/* setting new state for this dialog */
	ostate = dlg->state;
	dlg->state = sval;

	/* updates for terminated dialogs */
	if(ostate == DLG_STATE_CONFIRMED && sval == DLG_STATE_DELETED) {
		/* updating timestamps, flags, dialog stats */
		dlg->init_ts = ksr_time_uint(NULL, NULL);
		dlg->end_ts = ksr_time_uint(NULL, NULL);
	}
	dlg->dflags |= DLG_FLAG_CHANGED;

	dlg_unref(dlg, unref);

	if(ostate == DLG_STATE_CONFIRMED && sval == DLG_STATE_DELETED) {
		if_update_stat(dlg_enable_stats, active_dlgs, -1);
	}

	/* dlg_clean_run called by timer execution will handle timers deletion and all that stuff */
	LM_NOTICE(
			"dialog callid '%.*s' - state change forced - old: %d - new: %d\n",
			callid.len, callid.s, ostate, sval);

	rpc->add(c, "s", "Done");
}

static void rpc_dlg_is_alive(rpc_t *rpc, void *c)
{
	str callid = {NULL, 0};
	str ftag = {NULL, 0};
	str ttag = {NULL, 0};

	dlg_cell_t *dlg = NULL;
	unsigned int dir = 0;
	unsigned int state = 0;

	if(rpc->scan(c, ".S.S.S", &callid, &ftag, &ttag) < 3) {
		LM_DBG("Unable to read expected parameters\n");
		rpc->fault(c, 400,
				"Too few parameters (required callid, from-tag, to-tag)");
		return;
	}

	dlg = get_dlg(&callid, &ftag, &ttag, &dir);

	if(dlg == NULL) {
		LM_DBG("Couldnt find dialog with callid: '%.*s'\n", callid.len,
				callid.s);
		rpc->fault(c, 404, "Dialog not found");
		return;
	}
	state = dlg->state;
	dlg_release(dlg);
	if(state != DLG_STATE_CONFIRMED) {
		LM_DBG("Dialog with Call-ID '%.*s' is in state: %d (confirmed: %d)\n",
				callid.len, callid.s, state, DLG_STATE_CONFIRMED);
		rpc->fault(c, 500, "Dialog not in confirmed state");
		return;
	} else {
		rpc->add(c, "s", "Alive");
	}
}

static void rpc_end_dlg_entry_id(rpc_t *rpc, void *c)
{
	unsigned int h_entry, h_id;
	dlg_cell_t *dlg = NULL;
	str rpc_extra_hdrs = {NULL, 0};
	int n;

	n = rpc->scan(c, "dd", &h_entry, &h_id);
	if(n < 2) {
		LM_ERR("unable to read the parameters (%d)\n", n);
		rpc->fault(c, 500, "Invalid parameters");
		return;
	}
	if(rpc->scan(c, "*S", &rpc_extra_hdrs) < 1) {
		rpc_extra_hdrs.s = NULL;
		rpc_extra_hdrs.len = 0;
	}

	dlg = dlg_lookup(h_entry, h_id);
	if(dlg == NULL) {
		rpc->fault(c, 404, "Dialog not found");
		return;
	}

	dlg_bye_all(dlg, (rpc_extra_hdrs.len > 0) ? &rpc_extra_hdrs : NULL);
	dlg_release(dlg);
}
static void rpc_profile_get_size(rpc_t *rpc, void *c)
{
	str profile_name = {NULL, 0};
	str value = {NULL, 0};

	if(rpc->scan(c, ".S", &profile_name) < 1)
		return;
	if(rpc->scan(c, "*.S", &value) > 0) {
		internal_rpc_profile_get_size(rpc, c, &profile_name, &value);
	} else {
		internal_rpc_profile_get_size(rpc, c, &profile_name, NULL);
	}
	return;
}
static void rpc_profile_print_dlgs(rpc_t *rpc, void *c)
{
	str profile_name = {NULL, 0};
	str value = {NULL, 0};

	if(rpc->scan(c, ".S", &profile_name) < 1)
		return;
	if(rpc->scan(c, "*.S", &value) > 0) {
		internal_rpc_profile_print_dlgs(rpc, c, &profile_name, &value);
	} else {
		internal_rpc_profile_print_dlgs(rpc, c, &profile_name, NULL);
	}
	return;
}

static void rpc_dlg_bridge(rpc_t *rpc, void *c)
{
	str from = {NULL, 0};
	str to = {NULL, 0};
	str op = {NULL, 0};
	str bd = {NULL, 0};
	int n;

	n = rpc->scan(c, "SS", &from, &to);
	if(n < 2) {
		LM_ERR("unable to read the parameters (%d)\n", n);
		rpc->fault(c, 500, "Invalid parameters");
		return;
	}
	if(rpc->scan(c, "*S", &op) < 1) {
		op.s = NULL;
		op.len = 0;
	} else {
		if(op.len == 1 && *op.s == '.') {
			op.s = NULL;
			op.len = 0;
		}
		if(rpc->scan(c, "*S", &bd) < 1) {
			bd.s = NULL;
			bd.len = 0;
		} else {
			if(bd.len == 1 && *bd.s == '.') {
				bd.s = NULL;
				bd.len = 0;
			} else if(bd.len == 1 && *bd.s == '_') {
				bd.s = "";
				bd.len = 0;
			}
		}
	}

	dlg_bridge(&from, &to, &op, &bd);
}

static const char *rpc_dlg_stats_active_doc[2] = {
		"Get stats about active dialogs", 0};

/*!
 * \brief Print stats of active dialogs
 */
static void rpc_dlg_stats_active(rpc_t *rpc, void *c)
{
	dlg_cell_t *dlg;
	unsigned int i;
	int dlg_own = 0;
	int dlg_starting = 0;
	int dlg_connecting = 0;
	int dlg_answering = 0;
	int dlg_ongoing = 0;
	void *h;

	if(rpc->scan(c, "*d", &dlg_own) < 1)
		dlg_own = 0;
	for(i = 0; i < d_table->size; i++) {
		dlg_lock(d_table, &(d_table->entries[i]));

		for(dlg = d_table->entries[i].first; dlg; dlg = dlg->next) {
			if(dlg_own != 0 && dlg->bind_addr[0] == NULL)
				continue;
			switch(dlg->state) {
				case DLG_STATE_UNCONFIRMED:
					dlg_starting++;
					break;
				case DLG_STATE_EARLY:
					dlg_connecting++;
					break;
				case DLG_STATE_CONFIRMED_NA:
					dlg_answering++;
					break;
				case DLG_STATE_CONFIRMED:
					dlg_ongoing++;
					break;
				default:
					LM_DBG("not active - state: %d\n", dlg->state);
			}
		}
		dlg_unlock(d_table, &(d_table->entries[i]));
	}

	if(rpc->add(c, "{", &h) < 0) {
		rpc->fault(c, 500, "Server failure");
		return;
	}

	rpc->struct_add(h, "ddddd", "starting", dlg_starting, "connecting",
			dlg_connecting, "answering", dlg_answering, "ongoing", dlg_ongoing,
			"all", dlg_starting + dlg_connecting + dlg_answering + dlg_ongoing);
}

/*!
 * \brief Helper function that outputs matching dialogs via the RPC interface
 *
 * \param rpc RPC node that should be filled
 * \param c RPC void pointer
 * \param with_context if 1 then the dialog context will be also printed
 */
static void rpc_dlg_list_match_ex(rpc_t *rpc, void *c, int with_context)
{
	dlg_cell_t *dlg = NULL;
	int i = 0;
	str mkey = {NULL, 0};
	str mop = {NULL, 0};
	str mval = {NULL, 0};
	str sval = {NULL, 0};
	unsigned int ival = 0;
	unsigned int mival = 0;
	int n = 0;
	int m = 0;
	int vkey = 0;
	int vop = 0;
	int matched = 0;
	regex_t mre;
	regmatch_t pmatch;

	i = rpc->scan(c, "SSS", &mkey, &mop, &mval);
	if(i < 3) {
		LM_ERR("unable to read required parameters (%d)\n", i);
		rpc->fault(c, 500, "Invalid parameters");
		return;
	}
	if(mkey.s == NULL || mkey.len <= 0 || mop.s == NULL || mop.len <= 0
			|| mval.s == NULL || mval.len <= 0) {
		LM_ERR("invalid parameters (%d)\n", i);
		rpc->fault(c, 500, "Invalid parameters");
		return;
	}
	if(mkey.len == 4 && strncmp(mkey.s, "ruri", mkey.len) == 0) {
		vkey = 0;
	} else if(mkey.len == 4 && strncmp(mkey.s, "furi", mkey.len) == 0) {
		vkey = 1;
	} else if(mkey.len == 4 && strncmp(mkey.s, "turi", mkey.len) == 0) {
		vkey = 2;
	} else if(mkey.len == 6 && strncmp(mkey.s, "callid", mkey.len) == 0) {
		vkey = 3;
	} else if(mkey.len == 8 && strncmp(mkey.s, "start_ts", mkey.len) == 0) {
		vkey = 4;
	} else {
		LM_ERR("invalid key %.*s\n", mkey.len, mkey.s);
		rpc->fault(c, 500, "Invalid matching key parameter");
		return;
	}
	if(mop.len != 2) {
		LM_ERR("invalid matching operator %.*s\n", mop.len, mop.s);
		rpc->fault(c, 500, "Invalid matching operator parameter");
		return;
	}
	if(strncmp(mop.s, "eq", 2) == 0) {
		vop = 0;
	} else if(strncmp(mop.s, "re", 2) == 0) {
		vop = 1;
		memset(&mre, 0, sizeof(regex_t));
		if(regcomp(&mre, mval.s, REG_EXTENDED | REG_ICASE | REG_NEWLINE) != 0) {
			LM_ERR("failed to compile regex: %.*s\n", mval.len, mval.s);
			rpc->fault(c, 500, "Invalid matching value parameter");
			return;
		}
	} else if(strncmp(mop.s, "sw", 2) == 0) {
		vop = 2;
	} else if(strncmp(mop.s, "gt", 2) == 0) {
		vop = 3;
	} else if(strncmp(mop.s, "lt", 2) == 0) {
		vop = 4;
	} else {
		LM_ERR("invalid matching operator %.*s\n", mop.len, mop.s);
		rpc->fault(c, 500, "Invalid matching operator parameter");
		return;
	}
	if(rpc->scan(c, "*d", &n) < 1) {
		n = 0;
	}

	if(vkey == 4 && vop <= 2) {
		LM_ERR("Matching operator %.*s not supported with start_ts key\n",
				mop.len, mop.s);
		rpc->fault(c, 500, "Matching operator not supported with start_ts key");
		return;
	}

	if(vkey != 4 && vop >= 3) {
		LM_ERR("Matching operator %.*s not supported with key %.*s\n", mop.len,
				mop.s, mkey.len, mkey.s);
		rpc->fault(c, 500, "Matching operator not supported");
		return;
	}

	for(i = 0; i < d_table->size; i++) {
		dlg_lock(d_table, &(d_table->entries[i]));
		for(dlg = d_table->entries[i].first; dlg != NULL; dlg = dlg->next) {
			matched = 0;
			switch(vkey) {
				case 0:
					sval = dlg->req_uri;
					break;
				case 1:
					sval = dlg->from_uri;
					break;
				case 2:
					sval = dlg->to_uri;
					break;
				case 3:
					sval = dlg->callid;
					break;
				case 4:
					ival = dlg->start_ts;
					break;
			}
			switch(vop) {
				case 0:
					/* string comparison */
					if(mval.len == sval.len
							&& strncmp(mval.s, sval.s, mval.len) == 0) {
						matched = 1;
					}
					break;
				case 1:
					/* regexp matching */
					if(regexec(&mre, sval.s, 1, &pmatch, 0) == 0) {
						matched = 1;
					}
					break;
				case 2:
					/* starts with */
					if(mval.len <= sval.len
							&& strncmp(mval.s, sval.s, mval.len) == 0) {
						matched = 1;
					}
					break;
				case 3:
					/* greater than */
					if(str2int(&mval, &mival) == 0 && ival > mival) {
						matched = 1;
					}
					break;
				case 4:
					if(str2int(&mval, &mival) == 0 && ival < mival) {
						matched = 1;
					}
					break;
			}
			if(matched == 1) {
				m++;
				internal_rpc_print_dlg(rpc, c, dlg, with_context);
				if(n > 0 && m == n) {
					break;
				}
			}
		}
		dlg_unlock(d_table, &(d_table->entries[i]));
		if(n > 0 && m == n) {
			break;
		}
	}
	if(vop == 1) {
		regfree(&mre);
	}

	if(m == 0) {
		rpc->fault(c, 404, "Not found");
		return;
	}
}

/*!
 * \brief Print matching dialogs
 */
static void rpc_dlg_list_match(rpc_t *rpc, void *c)
{
	rpc_dlg_list_match_ex(rpc, c, 0);
}

/*!
 * \brief Print matching dialogs with context
 */
static void rpc_dlg_list_match_ctx(rpc_t *rpc, void *c)
{
	rpc_dlg_list_match_ex(rpc, c, 1);
}

static const char *rpc_dlg_briefing_doc[2] = {
		"List the summary of dialog records in memory", 0};

/*!
 * \brief List summary of active calls
 */
static void rpc_dlg_briefing(rpc_t *rpc, void *c)
{
	dlg_cell_t *dlg = NULL;
	unsigned int i = 0;
	int n = 0;
	str fmt = STR_NULL;
	void *h = NULL;

	n = rpc->scan(c, "S", &fmt);
	if(n < 1) {
		fmt.s = "ftcFT";
		fmt.len = 5;
	}

	for(i = 0; i < d_table->size; i++) {
		dlg_lock(d_table, &(d_table->entries[i]));
		for(dlg = d_table->entries[i].first; dlg; dlg = dlg->next) {
			if(rpc->add(c, "{", &h) < 0) {
				rpc->fault(c, 500, "Failed to create the structure");
				return;
			}
			if(rpc->struct_add(
					   h, "dd", "h_entry", dlg->h_entry, "h_id", dlg->h_id)
					< 0) {
				rpc->fault(c, 500, "Failed to add fields");
				return;
			}
			for(n = 0; n < fmt.len; n++) {
				switch(fmt.s[n]) {
					case 'f':
						if(rpc->struct_add(h, "S", "from_uri", &dlg->from_uri)
								< 0) {
							rpc->fault(c, 500, "Failed to add fields");
							return;
						}
						break;
					case 't':
						if(rpc->struct_add(h, "S", "to_uri", &dlg->to_uri)
								< 0) {
							rpc->fault(c, 500, "Failed to add fields");
							return;
						}
						break;
					case 'c':
						if(rpc->struct_add(h, "S", "call-id", &dlg->callid)
								< 0) {
							rpc->fault(c, 500, "Failed to add fields");
							return;
						}
						break;
					case 'F':
						if(rpc->struct_add(h, "S", "from_tag",
								   &dlg->tag[DLG_CALLER_LEG])
								< 0) {
							rpc->fault(c, 500, "Failed to add fields");
							return;
						}
						break;
					case 'T':
						if(rpc->struct_add(
								   h, "S", "to_tag", &dlg->tag[DLG_CALLER_LEG])
								< 0) {
							rpc->fault(c, 500, "Failed to add fields");
							return;
						}
						break;
					case 'I':
						if(rpc->struct_add(h, "d", "init_ts", dlg->init_ts)
								< 0) {
							rpc->fault(c, 500, "Failed to add fields");
							return;
						}
						break;
					case 'S':
						if(rpc->struct_add(h, "d", "start_ts", dlg->start_ts)
								< 0) {
							rpc->fault(c, 500, "Failed to add fields");
							return;
						}
						break;
					case 'E':
						if(rpc->struct_add(h, "d", "end_ts", dlg->end_ts) < 0) {
							rpc->fault(c, 500, "Failed to add fields");
							return;
						}
						break;
					case 's':
						if(rpc->struct_add(h, "d", "state", dlg->state) < 0) {
							rpc->fault(c, 500, "Failed to add fields");
							return;
						}
						break;
				}
			}
		}
		dlg_unlock(d_table, &(d_table->entries[i]));
	}
}

static rpc_export_t rpc_methods[] = {
		{"dlg.briefing", rpc_dlg_briefing, rpc_dlg_briefing_doc, RET_ARRAY},
		{"dlg.list", rpc_print_dlgs, rpc_print_dlgs_doc, RET_ARRAY},
		{"dlg.dump_file", rpc_dump_file_dlgs, rpc_dump_file_dlgs_doc, 0},
		{"dlg.list_ctx", rpc_print_dlgs_ctx, rpc_print_dlgs_ctx_doc, RET_ARRAY},
		{"dlg.list_match", rpc_dlg_list_match, rpc_dlg_list_match_doc,
				RET_ARRAY},
		{"dlg.list_match_ctx", rpc_dlg_list_match_ctx,
				rpc_dlg_list_match_ctx_doc, RET_ARRAY},
		{"dlg.dlg_list", rpc_print_dlg, rpc_print_dlg_doc, 0},
		{"dlg.dlg_list_ctx", rpc_print_dlg_ctx, rpc_print_dlg_ctx_doc, 0},
		{"dlg.end_dlg", rpc_end_dlg_entry_id, rpc_end_dlg_entry_id_doc, 0},
		{"dlg.profile_get_size", rpc_profile_get_size, rpc_profile_get_size_doc,
				0},
		{"dlg.profile_list", rpc_profile_print_dlgs, rpc_profile_print_dlgs_doc,
				RET_ARRAY},
		{"dlg.bridge_dlg", rpc_dlg_bridge, rpc_dlg_bridge_doc, 0},
		{"dlg.terminate_dlg", rpc_dlg_terminate_dlg, rpc_dlg_terminate_dlg_doc,
				0},
		{"dlg.set_state", rpc_dlg_set_state, rpc_dlg_set_state_doc, 0},
		{"dlg.stats_active", rpc_dlg_stats_active, rpc_dlg_stats_active_doc, 0},
		{"dlg.is_alive", rpc_dlg_is_alive, rpc_dlg_is_alive_doc, 0},
		{0, 0, 0, 0}};
