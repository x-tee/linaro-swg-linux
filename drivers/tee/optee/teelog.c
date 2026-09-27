// SPDX-License-Identifier: GPL-2.0-only
/*
 * OP-TEE TEE log lifecycle: REE page alloc, ledger, PTA register/stop/recover.
 *
 * Design §5–§6 / plan U5. Failures never fail OP-TEE probe. Page free requires
 * resident ledger match plus stop-write confirmation — never GET_REGISTRATION
 * or __va(pa) alone. Cross-module rmmod recovery gate is U8.
 *
 * Lock / ownership notes:
 * - teelog_ledger_begin/end: serialises recover→register and stop→release.
 * - optee->teelog_slot_mu: publish/clear/get of optee->teelog; get() takes
 *   kref under this lock so it cannot race free after clear.
 * - tl->state_lock: state + ctx/session fields for U6; detach clears ctx
 *   pointers under the lock, then closes TEE outside (may sleep).
 */
#define pr_fmt(fmt) "optee teelog: " fmt

#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/tee_core.h>
#include <linux/tee_drv.h>
#include <linux/types.h>
#include <linux/uuid.h>

#include "optee_private.h"
#include "teelog.h"
#include "teelog_internal.h"
#include "teelog_protocol.h"

/*
 * Boot roundtrip is opt-in only. This QEMU U-Boot ignores QEMU -append; enable
 * with bootargs optee.teelog_roundtrip=1 (rebuild U-Boot env) or the module
 * param after boot when supported.
 */
static bool teelog_roundtrip;
module_param_named(teelog_roundtrip, teelog_roundtrip, bool, 0444);
MODULE_PARM_DESC(teelog_roundtrip,
		 "After first REGISTER, UNREGISTER and REGISTER again (test only)");

#if IS_ENABLED(CONFIG_OPTEE_TEELOG_FAULT_INJECT)
/* See teelog_inject_reg in teelog_internal.h — REJECTED is pre-call only. */
static int teelog_inject;
module_param_named(inject, teelog_inject, int, 0644);
MODULE_PARM_DESC(inject,
		 "1=REG reject pre, 2=REG unknown post, 3=UNREG unknown post, 4=alloc fail, 5=recover field mismatch when TEE active");

/* Host for fault_action; set in init, cleared in stop. */
static struct optee *teelog_fault_host;
static DEFINE_MUTEX(teelog_fault_mu);
#else
static const int teelog_inject;
#endif

enum optee_teelog_state {
	OPTEE_TEELOG_OFF = 0,
	OPTEE_TEELOG_ACTIVE,
	OPTEE_TEELOG_STOPPING,
};

struct optee_teelog {
	struct kref kref;
	/* state + ctx/session identity for U6; see file comment. */
	struct mutex state_lock;
	enum optee_teelog_state state;
	struct optee *optee;
	struct tee_context *ctx;
	u32 session;
	bool session_open;
	u64 pa;
	u32 size;
	u32 order;
	u32 generation;
	u32 notif_id;
};

static const uuid_t teelog_pta_uuid =
	UUID_INIT(0xb5a1c0de, 0x7e31, 0x4a9c,
		  0x9f, 0x2e, 0x41, 0x58, 0x6d, 0x73, 0xa0, 0x11);

static void teelog_release(struct kref *kref)
{
	struct optee_teelog *tl = container_of(kref, struct optee_teelog, kref);

	mutex_destroy(&tl->state_lock);
	kfree(tl);
}

struct optee_teelog *optee_teelog_get(struct optee *optee)
{
	struct optee_teelog *tl;

	if (!optee)
		return NULL;

	mutex_lock(&optee->teelog_slot_mu);
	tl = optee->teelog;
	if (tl && !kref_get_unless_zero(&tl->kref))
		tl = NULL;
	mutex_unlock(&optee->teelog_slot_mu);
	return tl;
}
EXPORT_SYMBOL_GPL(optee_teelog_get);

void optee_teelog_put(struct optee_teelog *tl)
{
	if (tl)
		kref_put(&tl->kref, teelog_release);
}
EXPORT_SYMBOL_GPL(optee_teelog_put);

static void teelog_slot_publish(struct optee *optee, struct optee_teelog *tl)
{
	mutex_lock(&optee->teelog_slot_mu);
	WRITE_ONCE(optee->teelog, tl);
	mutex_unlock(&optee->teelog_slot_mu);
}

static struct optee_teelog *teelog_slot_take(struct optee *optee)
{
	struct optee_teelog *tl;

	mutex_lock(&optee->teelog_slot_mu);
	tl = optee->teelog;
	optee->teelog = NULL;
	mutex_unlock(&optee->teelog_slot_mu);
	return tl;
}

#if IS_ENABLED(CONFIG_OPTEE_TEELOG_FAULT_INJECT)
static bool teelog_slot_busy(struct optee *optee)
{
	bool busy;

	mutex_lock(&optee->teelog_slot_mu);
	busy = READ_ONCE(optee->teelog) != NULL;
	mutex_unlock(&optee->teelog_slot_mu);
	return busy;
}
#endif

static struct optee_teelog *teelog_alloc_inst(struct optee *optee)
{
	struct optee_teelog *tl;

	tl = kzalloc(sizeof(*tl), GFP_KERNEL);
	if (!tl)
		return NULL;
	kref_init(&tl->kref);
	mutex_init(&tl->state_lock);
	tl->state = OPTEE_TEELOG_OFF;
	tl->optee = optee;
	return tl;
}

static void teelog_close_tee(struct tee_context *ctx, u32 session,
			     bool session_open)
{
	if (session_open && ctx)
		/* close_session return is not stop-write evidence. */
		tee_client_close_session(ctx, session);
	if (ctx)
		tee_client_close_context(ctx);
}

static void teelog_detach_tee(struct optee_teelog *tl)
{
	struct tee_context *ctx;
	u32 session;
	bool session_open;

	mutex_lock(&tl->state_lock);
	ctx = tl->ctx;
	session = tl->session;
	session_open = tl->session_open;
	tl->ctx = NULL;
	tl->session = 0;
	tl->session_open = false;
	mutex_unlock(&tl->state_lock);

	teelog_close_tee(ctx, session, session_open);
}

/*
 * Open a REE_KERNEL session on this OP-TEE instance's client teedev.
 * Sets supp_nowait like tee_client_open_context() for in-kernel clients.
 */
static int teelog_open_session(struct optee *optee, struct tee_context **ctx_out,
			       u32 *session_out)
{
	struct tee_ioctl_open_session_arg sess_arg = {};
	struct tee_context *ctx;
	int rc;
	enum teelog_comm_result cls;

	if (!optee || !optee->teedev)
		return -ENODEV;

	ctx = teedev_open(optee->teedev);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->supp_nowait = true;

	export_uuid(sess_arg.uuid, &teelog_pta_uuid);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_REE_KERNEL;
	sess_arg.num_params = 0;

	rc = tee_client_open_session(ctx, &sess_arg, NULL);
	cls = teelog_classify_invoke(rc, sess_arg.ret);
	if (cls != TEELOG_COMM_OK) {
		tee_client_close_context(ctx);
		if (cls == TEELOG_COMM_UNKNOWN)
			return rc < 0 ? rc : -EIO;
		return -ENODEV;
	}

	*ctx_out = ctx;
	*session_out = sess_arg.session;
	return 0;
}

static enum teelog_comm_result
teelog_invoke(struct tee_context *ctx, u32 session, u32 cmd,
	      struct tee_param *params, u32 num_params, u32 *tee_ret_out)
{
	struct tee_ioctl_invoke_arg inv = {};
	int rc;
	u32 tee_ret;
	enum teelog_comm_result cls;

	/*
	 * REGISTER reject inject is pre-call only: must not hit TEE, or the
	 * caller would release_once while secure world still owns the pages.
	 */
	if (cmd == PTA_TEELOG_CMD_REGISTER &&
	    teelog_inject_skip_register_call(teelog_inject))
		return TEELOG_COMM_REJECTED;

	inv.func = cmd;
	inv.session = session;
	inv.num_params = num_params;

	rc = tee_client_invoke_func(ctx, &inv, params);
	tee_ret = inv.ret;
	cls = teelog_classify_invoke(rc, tee_ret);

	if (cmd == PTA_TEELOG_CMD_REGISTER)
		cls = teelog_inject_after_register(teelog_inject, cls);
	else if (cmd == PTA_TEELOG_CMD_UNREGISTER)
		cls = teelog_inject_after_unregister(teelog_inject, cls);

	if (tee_ret_out)
		*tee_ret_out = tee_ret;
	return cls;
}

static enum teelog_comm_result
teelog_cmd_get_version(struct tee_context *ctx, u32 session, u32 *version,
		       u32 *caps)
{
	struct tee_param param[4] = {};
	enum teelog_comm_result cls;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
	cls = teelog_invoke(ctx, session, PTA_TEELOG_CMD_GET_VERSION, param, 4,
			    NULL);
	if (cls != TEELOG_COMM_OK)
		return cls;
	*version = param[0].u.value.a;
	*caps = param[0].u.value.b;
	return TEELOG_COMM_OK;
}

static enum teelog_comm_result
teelog_cmd_get_registration(struct tee_context *ctx, u32 session, u64 *pa,
			    u32 *size, u32 *generation, u32 *notif_id,
			    bool *active)
{
	struct tee_param param[4] = {};
	enum teelog_comm_result cls;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
	param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	cls = teelog_invoke(ctx, session, PTA_TEELOG_CMD_GET_REGISTRATION,
			    param, 4, NULL);
	if (cls != TEELOG_COMM_OK)
		return cls;

	*active = param[2].u.value.b != 0;
	if (!*active) {
		*pa = 0;
		*size = 0;
		*generation = 0;
		*notif_id = 0;
		return TEELOG_COMM_OK;
	}

	*pa = ((u64)param[0].u.value.b << 32) | param[0].u.value.a;
	*size = param[1].u.value.a;
	*generation = param[1].u.value.b;
	*notif_id = param[2].u.value.a;
	return TEELOG_COMM_OK;
}

static enum teelog_comm_result
teelog_cmd_register(struct tee_context *ctx, u32 session, u64 pa, u32 size,
		    u32 generation, u32 *notif_id)
{
	struct tee_param param[4] = {};
	enum teelog_comm_result cls;

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = lower_32_bits(pa);
	param[0].u.value.b = upper_32_bits(pa);
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[1].u.value.a = size;
	param[1].u.value.b = PTA_TEELOG_PROTOCOL_VERSION;
	param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[2].u.value.a = generation;
	param[2].u.value.b = PTA_TEELOG_REG_F_NONE;
	param[3].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	cls = teelog_invoke(ctx, session, PTA_TEELOG_CMD_REGISTER, param, 4,
			    NULL);
	if (cls == TEELOG_COMM_OK)
		*notif_id = param[3].u.value.a;
	return cls;
}

static enum teelog_comm_result
teelog_cmd_unregister(struct tee_context *ctx, u32 session, u32 generation)
{
	struct tee_param param[4] = {};

	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = generation;
	param[0].u.value.b = 0;

	return teelog_invoke(ctx, session, PTA_TEELOG_CMD_UNREGISTER, param, 4,
			     NULL);
}

/*
 * Caller holds teelog_ledger_begin(). Opens a temporary session on @optee,
 * recovers any prior ledger slot, then returns with the lock still held.
 */
static int teelog_recover_locked(struct optee *optee)
{
	struct teelog_ledger_entry entry;
	struct tee_context *ctx = NULL;
	u32 session = 0;
	u64 tee_pa = 0;
	u32 tee_size = 0, tee_gen = 0, notif_id = 0;
	bool active = false;
	bool matched;
	enum teelog_comm_result cls;
	enum teelog_recover_action action;
	enum teelog_page_fate fate;
	int rc;

	rc = teelog_ledger_snapshot(&entry);
	if (rc)
		return rc;
	if (!entry.occupied)
		return 0;

	pr_info("recover: ledger gen=%u pa=0x%llx size=%u isolated=%d\n",
		entry.generation, entry.pa, entry.size, entry.isolated);

	rc = teelog_open_session(optee, &ctx, &session);
	if (rc) {
		pr_warn("recover: cannot open PTA session (%d); abort\n", rc);
		return -EAGAIN;
	}

	cls = teelog_cmd_get_registration(ctx, session, &tee_pa, &tee_size,
					  &tee_gen, &notif_id, &active);
	if (cls != TEELOG_COMM_OK) {
		pr_warn("recover: GET_REGISTRATION not confirmed; abort\n");
		teelog_close_tee(ctx, session, true);
		return -EAGAIN;
	}

	/*
	 * inject=5: mutate local copies of the TEE reply, then run the real
	 * field compare. Proves PA/generation mismatch detection — not a
	 * forced matched=false bypass.
	 */
	if (teelog_inject_recover_mismatch(teelog_inject) && active) {
		u64 tee_pa_obs = tee_pa;
		u32 tee_gen_obs = tee_gen;

		teelog_inject_corrupt_reg_reply(teelog_inject, active,
						&tee_pa_obs, &tee_gen_obs);
		pr_info("TEELOG_RECOVER_MISMATCH_INJECT pa=0x%llx->0x%llx gen=%u->%u ledger_gen=%u\n",
			tee_pa, tee_pa_obs, tee_gen, tee_gen_obs,
			entry.generation);
		matched = teelog_reg_fields_match(entry.pa, entry.size,
						  entry.generation, tee_pa_obs,
						  tee_size, tee_gen_obs);
	} else {
		matched = active &&
			  teelog_reg_fields_match(entry.pa, entry.size,
						  entry.generation, tee_pa,
						  tee_size, tee_gen);
	}
	action = teelog_recover_decide(true, active, matched);

	switch (action) {
	case TEELOG_RECOVER_RELEASE:
		rc = teelog_ledger_release_once(entry.generation);
		if (rc)
			pr_warn("recover: release_once failed (%d)\n", rc);
		else
			pr_info("TEELOG_RECOVER_RELEASE gen=%u\n",
				entry.generation);
		break;
	case TEELOG_RECOVER_UNREGISTER:
		cls = teelog_cmd_unregister(ctx, session, entry.generation);
		fate = teelog_pages_after_unregister(cls);
		if (fate == TEELOG_PAGE_RELEASE) {
			rc = teelog_ledger_release_once(entry.generation);
			if (rc)
				pr_warn("recover: release after UNREGISTER failed (%d)\n",
					rc);
			else
				pr_info("TEELOG_RECOVER_UNREGISTER_OK gen=%u\n",
					entry.generation);
		} else {
			teelog_ledger_mark_isolated(entry.generation);
			pr_warn("TEELOG_UNREGISTER_ISOLATED gen=%u (recover)\n",
				entry.generation);
			rc = -EAGAIN;
		}
		break;
	case TEELOG_RECOVER_ABORT:
	default:
		pr_warn("TEELOG_RECOVER_ABORT active=%d match=%d\n",
			active, matched);
		rc = -EAGAIN;
		break;
	}

	teelog_close_tee(ctx, session, true);
	return rc;
}

static int teelog_alloc_pages(u32 size, unsigned long *va_out, u32 *order_out,
			      u64 *pa_out)
{
	unsigned long va;
	u32 order;

	if (size < TEELOG_MIN_SIZE || size > TEELOG_MAX_SIZE ||
	    !IS_ALIGNED(size, PAGE_SIZE))
		return -EINVAL;

	if (teelog_inject_alloc_fail(teelog_inject)) {
		pr_info("TEELOG_ALLOC_FAIL (inject)\n");
		return -ENOMEM;
	}

	order = get_order(size);
	va = __get_free_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!va)
		return -ENOMEM;

	*va_out = va;
	*order_out = order;
	*pa_out = __pa(va);
	return 0;
}

/*
 * Caller holds teelog_ledger_begin() and an empty ledger slot.
 * Allocates the instance before REGISTER to avoid post-success ENOMEM unwind.
 */
static int teelog_register_new_locked(struct optee *optee)
{
	struct optee_teelog *tl;
	struct tee_context *ctx = NULL;
	u32 session = 0;
	bool session_open = false;
	unsigned long va = 0;
	u64 pa = 0;
	u32 size = TEELOG_DEFAULT_SIZE;
	u32 order = 0;
	u32 generation = 0;
	u32 version = 0, caps = 0, notif_id = PTA_TEELOG_NOTIF_ID_NONE;
	enum teelog_comm_result cls;
	enum teelog_page_fate fate;
	int rc;

	tl = teelog_alloc_inst(optee);
	if (!tl)
		return -ENOMEM;

	rc = teelog_open_session(optee, &ctx, &session);
	if (rc) {
		pr_info("PTA session unavailable (%d); teelog disabled\n", rc);
		goto err_put;
	}
	session_open = true;

	cls = teelog_cmd_get_version(ctx, session, &version, &caps);
	if (cls != TEELOG_COMM_OK || version != PTA_TEELOG_PROTOCOL_VERSION) {
		pr_info("GET_VERSION failed or mismatch (cls=%d ver=%u)\n",
			cls, version);
		rc = -ENODEV;
		goto err_close;
	}

	rc = teelog_alloc_pages(size, &va, &order, &pa);
	if (rc)
		goto err_close;

	rc = teelog_ledger_record(pa, size, order, &generation);
	if (rc)
		goto err_free_pages;

	cls = teelog_cmd_register(ctx, session, pa, size, generation, &notif_id);
	fate = teelog_pages_after_register(cls);
	switch (fate) {
	case TEELOG_PAGE_KEEP:
		break;
	case TEELOG_PAGE_RELEASE:
		pr_info("TEELOG_REGISTER_REJECTED gen=%u\n", generation);
		teelog_ledger_release_once(generation);
		rc = -ECONNREFUSED;
		goto err_close;
	case TEELOG_PAGE_ISOLATE:
	default:
		teelog_ledger_mark_isolated(generation);
		pr_warn("TEELOG_REGISTER_ISOLATED gen=%u pa=0x%llx\n",
			generation, pa);
		rc = -EIO;
		goto err_close;
	}

	mutex_lock(&tl->state_lock);
	tl->ctx = ctx;
	tl->session = session;
	tl->session_open = true;
	tl->pa = pa;
	tl->size = size;
	tl->order = order;
	tl->generation = generation;
	tl->notif_id = notif_id;
	tl->state = OPTEE_TEELOG_ACTIVE;
	mutex_unlock(&tl->state_lock);

	teelog_slot_publish(optee, tl);
	pr_info("TEELOG_REGISTER_OK pa=0x%llx size=%u gen=%u notif=0x%x\n",
		pa, size, generation, notif_id);
	return 0;

err_free_pages:
	free_pages(va, order);
err_close:
	teelog_close_tee(ctx, session, session_open);
err_put:
	optee_teelog_put(tl);
	return rc;
}

static int teelog_stop_instance_locked(struct optee *optee,
				       struct optee_teelog *tl)
{
	struct tee_context *ctx;
	u32 session, generation;
	bool session_open;
	enum teelog_comm_result cls;
	enum teelog_page_fate fate;
	int rc = 0;

	mutex_lock(&tl->state_lock);
	if (tl->state != OPTEE_TEELOG_ACTIVE) {
		mutex_unlock(&tl->state_lock);
		return 0;
	}
	tl->state = OPTEE_TEELOG_STOPPING;
	ctx = tl->ctx;
	session = tl->session;
	session_open = tl->session_open;
	generation = tl->generation;
	/* Keep ctx pointers until after UNREGISTER; cleared in detach. */
	mutex_unlock(&tl->state_lock);

	if (!session_open || !ctx) {
		teelog_ledger_mark_isolated(generation);
		pr_warn("TEELOG_UNREGISTER_ISOLATED gen=%u (no session)\n",
			generation);
		goto detach;
	}

	cls = teelog_cmd_unregister(ctx, session, generation);
	fate = teelog_pages_after_unregister(cls);
	if (fate == TEELOG_PAGE_RELEASE) {
		rc = teelog_ledger_release_once(generation);
		if (rc)
			pr_warn("stop: release_once failed (%d)\n", rc);
		else
			pr_info("TEELOG_UNREGISTER_OK gen=%u\n", generation);
	} else {
		teelog_ledger_mark_isolated(generation);
		pr_warn("TEELOG_UNREGISTER_ISOLATED gen=%u\n", generation);
		rc = -EIO;
	}

detach:
	teelog_detach_tee(tl);
	mutex_lock(&tl->state_lock);
	tl->state = OPTEE_TEELOG_OFF;
	tl->optee = NULL;
	mutex_unlock(&tl->state_lock);
	optee_teelog_put(tl);
	return rc;
}

static void teelog_try_roundtrip(struct optee *optee)
{
	struct optee_teelog *tl;
	int rc;

	if (!teelog_roundtrip)
		return;

	tl = teelog_slot_take(optee);
	if (!tl)
		return;

	pr_info("roundtrip: stopping first registration\n");
	rc = teelog_ledger_begin();
	if (rc) {
		/* Put the instance back so normal stop can reclaim. */
		teelog_slot_publish(optee, tl);
		pr_warn("roundtrip: begin failed (%d)\n", rc);
		return;
	}
	teelog_stop_instance_locked(optee, tl);
	rc = teelog_register_new_locked(optee);
	teelog_ledger_end();
	if (!rc)
		pr_info("TEELOG_ROUNDTRIP_OK\n");
	else
		pr_warn("roundtrip: re-REGISTER failed (%d)\n", rc);
}

#if IS_ENABLED(CONFIG_OPTEE_TEELOG_FAULT_INJECT)
enum {
	TEELOG_FAULT_ACT_STOP = 1,   /* stop published instance (honor inject) */
	TEELOG_FAULT_ACT_INIT = 2,   /* recover + register_new; -EBUSY if slot busy */
	TEELOG_FAULT_ACT_ORPHAN = 3, /* crash-sim: drop publish, keep TEE registration */
};

/*
 * Orphaned PTA session left open so recover can observe an active TEE
 * registration without a published Linux instance. Closed after the next
 * successful recover/init or on STOP/host teardown.
 */
static struct tee_context *teelog_fault_orphan_ctx;
static u32 teelog_fault_orphan_session;
static bool teelog_fault_orphan_open;

static void teelog_fault_orphan_close(void)
{
	if (!teelog_fault_orphan_ctx)
		return;
	teelog_close_tee(teelog_fault_orphan_ctx, teelog_fault_orphan_session,
			 teelog_fault_orphan_open);
	teelog_fault_orphan_ctx = NULL;
	teelog_fault_orphan_session = 0;
	teelog_fault_orphan_open = false;
}

static int teelog_fault_orphan_locked(struct optee *optee)
{
	struct optee_teelog *tl;
	u32 generation;

	tl = teelog_slot_take(optee);
	if (!tl)
		return -ENOENT;

	mutex_lock(&tl->state_lock);
	if (tl->state != OPTEE_TEELOG_ACTIVE) {
		mutex_unlock(&tl->state_lock);
		teelog_slot_publish(optee, tl);
		return -EINVAL;
	}
	/* Replace any prior orphan before abandoning this session. */
	teelog_fault_orphan_close();
	teelog_fault_orphan_ctx = tl->ctx;
	teelog_fault_orphan_session = tl->session;
	teelog_fault_orphan_open = tl->session_open;
	tl->ctx = NULL;
	tl->session = 0;
	tl->session_open = false;
	generation = tl->generation;
	tl->state = OPTEE_TEELOG_OFF;
	tl->optee = NULL;
	mutex_unlock(&tl->state_lock);

	/* Keep ledger occupied; TEE registration stays with the orphaned session. */
	teelog_ledger_mark_isolated(generation);
	optee_teelog_put(tl);
	pr_info("TEELOG_FAULT_ACT_ORPHAN gen=%u\n", generation);
	return 0;
}

static int teelog_fault_action_set(const char *val,
				   const struct kernel_param *kp)
{
	int act;
	int rc = 0;
	struct optee *optee;
	struct optee_teelog *tl;

	if (kstrtoint(val, 0, &act))
		return -EINVAL;
	if (act < TEELOG_FAULT_ACT_STOP || act > TEELOG_FAULT_ACT_ORPHAN)
		return -EINVAL;

	mutex_lock(&teelog_fault_mu);
	optee = READ_ONCE(teelog_fault_host);
	if (!optee) {
		mutex_unlock(&teelog_fault_mu);
		return -ENODEV;
	}

	rc = teelog_ledger_begin();
	if (rc) {
		mutex_unlock(&teelog_fault_mu);
		return rc;
	}

	if (act == TEELOG_FAULT_ACT_STOP) {
		tl = teelog_slot_take(optee);
		if (tl)
			rc = teelog_stop_instance_locked(optee, tl);
		teelog_fault_orphan_close();
		pr_info("TEELOG_FAULT_ACT_STOP rc=%d inject=%d\n", rc,
			teelog_inject);
		/* Marker path: do not fail the sysfs write on isolate. */
		rc = 0;
	} else if (act == TEELOG_FAULT_ACT_ORPHAN) {
		rc = teelog_fault_orphan_locked(optee);
		pr_info("TEELOG_FAULT_ACT_ORPHAN rc=%d\n", rc);
	} else {
		/* INIT: never overwrite a live published instance. */
		if (teelog_slot_busy(optee)) {
			pr_info("TEELOG_FAULT_ACT_INIT_BUSY\n");
			rc = -EBUSY;
		} else {
			rc = teelog_recover_locked(optee);
			if (!rc)
				rc = teelog_register_new_locked(optee);
			if (!rc)
				teelog_fault_orphan_close();
			pr_info("TEELOG_FAULT_ACT_INIT rc=%d inject=%d\n", rc,
				teelog_inject);
			/* Injected failures use dmesg markers; keep write OK. */
			if (rc != -EBUSY)
				rc = 0;
		}
	}

	teelog_ledger_end();
	mutex_unlock(&teelog_fault_mu);
	return rc;
}

static const struct kernel_param_ops teelog_fault_action_ops = {
	.set = teelog_fault_action_set,
};

static int teelog_fault_action;
module_param_cb(fault_action, &teelog_fault_action_ops, &teelog_fault_action,
		0200);
MODULE_PARM_DESC(fault_action,
		 "1=stop, 2=recover+REGISTER (-EBUSY if active), 3=orphan crash-sim");
#endif /* CONFIG_OPTEE_TEELOG_FAULT_INJECT */

void optee_teelog_init(struct optee *optee)
{
	int rc;

	if (!optee)
		return;

#if IS_ENABLED(CONFIG_OPTEE_TEELOG_FAULT_INJECT)
	WRITE_ONCE(teelog_fault_host, optee);
#endif

	rc = teelog_ledger_begin();
	if (rc) {
		pr_warn("init: ledger begin failed (%d); skip\n", rc);
		return;
	}

	rc = teelog_recover_locked(optee);
	if (rc) {
		teelog_ledger_end();
		pr_warn("init: recovery incomplete; skip new REGISTER\n");
		return;
	}

	rc = teelog_register_new_locked(optee);
	teelog_ledger_end();

	if (rc)
		pr_info("init: REGISTER path ended (%d); OP-TEE continues\n",
			rc);
	else
		teelog_try_roundtrip(optee);
}

void optee_teelog_stop(struct optee *optee)
{
	struct optee_teelog *tl;
	int rc;

	if (!optee)
		return;

#if IS_ENABLED(CONFIG_OPTEE_TEELOG_FAULT_INJECT)
	mutex_lock(&teelog_fault_mu);
	if (READ_ONCE(teelog_fault_host) == optee) {
		WRITE_ONCE(teelog_fault_host, NULL);
		teelog_fault_orphan_close();
	}
	mutex_unlock(&teelog_fault_mu);
#endif

	tl = teelog_slot_take(optee);
	if (!tl)
		return;

	rc = teelog_ledger_begin();
	if (rc) {
		/* Slot already cleared; republish so a later stop can retry. */
		teelog_slot_publish(optee, tl);
		pr_warn("stop: ledger begin failed (%d)\n", rc);
		return;
	}
	teelog_stop_instance_locked(optee, tl);
	teelog_ledger_end();
}
