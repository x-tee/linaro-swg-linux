/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Private teelog instance / device glue shared by teelog.c and teelog_dev.c.
 * Not a UAPI header — do not include outside drivers/tee/optee/.
 */
#ifndef OPTEE_TEELOG_PRIV_H
#define OPTEE_TEELOG_PRIV_H

#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "teelog_protocol.h"

struct optee;
struct tee_context;
struct file;

enum optee_teelog_state {
	OPTEE_TEELOG_OFF = 0,
	OPTEE_TEELOG_ACTIVE,
	OPTEE_TEELOG_STOPPING,
};

struct optee_teelog {
	struct kref kref;
	/* state + ctx/session/va for readers; see teelog.c file comment. */
	struct mutex state_lock;
	enum optee_teelog_state state;
	struct optee *optee;
	struct tee_context *ctx;
	u32 session;
	bool session_open;
	u64 pa;
	unsigned long va;	/* __get_free_pages VA; 0 after pages freed */
	u32 size;
	u32 order;
	u32 generation;
	u32 notif_id;
	u32 tee_caps;		/* from GET_VERSION at register */

	wait_queue_head_t wq;
	struct delayed_work poll_work;
	bool dwork_inited;	/* INIT_DELAYED_WORK done; cancel is safe */
	u64 worker_seen_seq;	/* last seq observed by the 100 ms worker */
	struct miscdevice misc;
	bool misc_registered;
	/* Single-instance tlogcat -f claim; protected by state_lock. */
	struct file *tlogcat_owner;
};

/* Device / reader / notif / worker (teelog_dev.c) */
int teelog_dev_start(struct optee_teelog *tl);
void teelog_dev_shutdown(struct optee_teelog *tl);
void teelog_dev_kick(struct optee_teelog *tl);

#endif /* OPTEE_TEELOG_PRIV_H */
