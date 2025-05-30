/*
 * Copyright (C) 2014 Daniel-Constantin Mierla (asipto.com)
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/** Kamailio core :: Aync tasks
 * @ingroup core
 * Module: core
 */

#ifndef _ASYNC_TASK_H_
#define _ASYNC_TASK_H_

typedef void (*async_cbe_t)(void *p);

typedef struct _async_task
{
	async_cbe_t exec;
	void *param;
} async_task_t;

typedef struct _async_wgroup
{
	str name;
	int workers;
	int sockets[2];
	int usleep;
	int nonblock;
	struct _async_wgroup *next;
} async_wgroup_t;

int async_task_init(void);
int async_task_child_init(int rank);
int async_task_initialized(void);
int async_task_set_workers(int n);
int async_task_set_nonblock(int n);
int async_task_set_workers_group(char *data);
int async_task_push(async_task_t *task);
int async_task_set_usleep(int n);
int async_task_workers_get(void);
int async_task_workers_active(void);
async_wgroup_t *async_task_workers_get_crt(void);
async_wgroup_t *async_task_group_find(str *gname);

int async_task_group_push(str *gname, async_task_t *task);
int async_task_group_send(async_wgroup_t *awg, async_task_t *task);

typedef struct async_tkv_param
{
	int dtype;
	str skey;
	str sval;
} async_tkv_param_t;

#define KSR_ASYNC_TKV_SIZE 1024
void async_tkv_init(void);
int async_tkv_emit(int dtype, char *pkey, char *fmt, ...);
async_tkv_param_t *ksr_async_tkv_param_get(void);
void async_tkv_gname_set(char *gname);
void async_tkv_evcb_set(char *evcb);

#endif
