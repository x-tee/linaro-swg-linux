/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Private teelog decision helpers (production + KUnit). Not a UAPI /
 * driver-export header — do not include from outside drivers/tee/optee/.
 */
#ifndef OPTEE_TEELOG_INTERNAL_H
#define OPTEE_TEELOG_INTERNAL_H

#include <linux/types.h>

enum teelog_comm_result {
	TEELOG_COMM_OK = 0,
	TEELOG_COMM_REJECTED,
	TEELOG_COMM_UNKNOWN,
};

/* GP Internal Core / Client API shared codes used by teelog PTA. */
#define TEELOG_TEE_ERROR_BAD_STATE		0xFFFF0007U
#define TEELOG_TEE_ERROR_COMMUNICATION		0xFFFF000EU

/**
 * teelog_classify_invoke() - Map Linux + PTA status to the U5 triad
 * @linux_rc: return from tee_client_invoke_func / open_session
 * @tee_ret:  invoke arg.ret / session arg.ret when linux_rc == 0
 */
static inline enum teelog_comm_result
teelog_classify_invoke(int linux_rc, u32 tee_ret)
{
	if (linux_rc < 0)
		return TEELOG_COMM_UNKNOWN;
	if (tee_ret == 0)
		return TEELOG_COMM_OK;
	if (tee_ret == TEELOG_TEE_ERROR_COMMUNICATION)
		return TEELOG_COMM_UNKNOWN;

	switch (tee_ret) {
	case 0xFFFF0001U: /* ACCESS_DENIED */
	case 0xFFFF0006U: /* BAD_PARAMETERS */
	case TEELOG_TEE_ERROR_BAD_STATE:
	case 0xFFFF0008U: /* ITEM_NOT_FOUND */
	case 0xFFFF000AU: /* NOT_SUPPORTED */
	case 0xFFFF000CU: /* OUT_OF_MEMORY */
		return TEELOG_COMM_REJECTED;
	default:
		return TEELOG_COMM_UNKNOWN;
	}
}

enum teelog_recover_action {
	TEELOG_RECOVER_RELEASE = 0,
	TEELOG_RECOVER_UNREGISTER,
	TEELOG_RECOVER_ABORT,
};

static inline enum teelog_recover_action
teelog_recover_decide(bool query_ok, bool active, bool matched)
{
	if (!query_ok)
		return TEELOG_RECOVER_ABORT;
	if (!active)
		return TEELOG_RECOVER_RELEASE;
	if (matched)
		return TEELOG_RECOVER_UNREGISTER;
	return TEELOG_RECOVER_ABORT;
}

static inline bool teelog_reg_fields_match(u64 ledger_pa, u32 ledger_size,
					   u32 ledger_gen, u64 tee_pa,
					   u32 tee_size, u32 tee_gen)
{
	return ledger_pa == tee_pa && ledger_size == tee_size &&
	       ledger_gen == tee_gen &&
	       ledger_gen != 0;
}

enum teelog_page_fate {
	TEELOG_PAGE_KEEP = 0,
	TEELOG_PAGE_RELEASE,
	TEELOG_PAGE_ISOLATE,
};

static inline enum teelog_page_fate
teelog_pages_after_register(enum teelog_comm_result cls)
{
	switch (cls) {
	case TEELOG_COMM_OK:
		return TEELOG_PAGE_KEEP;
	case TEELOG_COMM_REJECTED:
		return TEELOG_PAGE_RELEASE;
	case TEELOG_COMM_UNKNOWN:
	default:
		return TEELOG_PAGE_ISOLATE;
	}
}

static inline enum teelog_page_fate
teelog_pages_after_unregister(enum teelog_comm_result cls)
{
	return cls == TEELOG_COMM_OK ? TEELOG_PAGE_RELEASE : TEELOG_PAGE_ISOLATE;
}

/*
 * Fault-inject policy (CONFIG_OPTEE_TEELOG_FAULT_INJECT):
 * REJECTED / alloc-fail / recover-mismatch are decided before the affected
 * TEE or allocator call. Only UNKNOWN may be forced after a real invoke.
 */
enum teelog_inject_reg {
	TEELOG_INJECT_NONE = 0,
	TEELOG_INJECT_REG_REJECT_PRE = 1,
	TEELOG_INJECT_REG_UNKNOWN_POST = 2,
	TEELOG_INJECT_UNREG_UNKNOWN_POST = 3,
	TEELOG_INJECT_ALLOC_FAIL = 4,
	/* When TEE registration is active, force gen mismatch on recover. */
	TEELOG_INJECT_RECOVER_MISMATCH = 5,
};

static inline bool teelog_inject_skip_register_call(int inject)
{
	return inject == TEELOG_INJECT_REG_REJECT_PRE;
}

static inline bool teelog_inject_alloc_fail(int inject)
{
	return inject == TEELOG_INJECT_ALLOC_FAIL;
}

static inline bool teelog_inject_recover_mismatch(int inject)
{
	return inject == TEELOG_INJECT_RECOVER_MISMATCH;
}

/*
 * Corrupt a local copy of GET_REGISTRATION fields so teelog_reg_fields_match()
 * sees a real PA/generation mismatch. Does not alter TEE state.
 */
static inline void teelog_inject_corrupt_reg_reply(int inject, bool active,
						  u64 *tee_pa, u32 *tee_gen)
{
	if (!teelog_inject_recover_mismatch(inject) || !active ||
	    !tee_pa || !tee_gen)
		return;
	/*
	 * Prefer generation bump so size/PA stay plausible; wrap 0 away
	 * because generation 0 is never a valid match key.
	 */
	*tee_gen = *tee_gen + 1U;
	if (!*tee_gen)
		*tee_gen = 1U;
	/* Also flip a low PA bit so both compared fields diverge. */
	*tee_pa ^= 0x1000ULL;
}

static inline enum teelog_comm_result
teelog_inject_after_register(int inject, enum teelog_comm_result cls)
{
	if (inject == TEELOG_INJECT_REG_UNKNOWN_POST)
		return TEELOG_COMM_UNKNOWN;
	return cls;
}

static inline enum teelog_comm_result
teelog_inject_after_unregister(int inject, enum teelog_comm_result cls)
{
	if (inject == TEELOG_INJECT_UNREG_UNKNOWN_POST)
		return TEELOG_COMM_UNKNOWN;
	return cls;
}

#endif /* OPTEE_TEELOG_INTERNAL_H */
