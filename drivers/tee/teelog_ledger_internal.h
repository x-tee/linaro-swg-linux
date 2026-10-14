/* SPDX-License-Identifier: GPL-2.0-only */
/* Private ledger engine. KUnit uses a separate instance, never the resident slot. */
#ifndef __TEELOG_LEDGER_INTERNAL_H
#define __TEELOG_LEDGER_INTERNAL_H

#include <asm/io.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tee_drv.h>

struct teelog_ledger_slot {
	bool occupied;
	bool isolated;
	/*
	 * live: a published ACTIVE instance still maps these pages.
	 * Recovery/query/free is forbidden while live. Cleared on stop,
	 * orphan hand-off, or release_once — never inferred from TEE state.
	 */
	bool live;
	/* Stable backend identity (SMC/FF-A/…); not a struct optee *. */
	u64 owner_key;
	u64 pa;
	u32 size;
	u32 order;
	u32 generation;
};

struct teelog_ledger_state {
	struct mutex life_mu;
	struct mutex ledger_mu;
	struct task_struct *life_owner;
	/* Next generation to hand out; 0 means exhausted after UINT32_MAX. */
	u32 next_gen;
	struct teelog_ledger_slot slot;
	/*
	 * Buddy pages recorded vs pages passed to free_pages(). Both
	 * counters use 1<<order, not size>>PAGE_SHIFT: get_order() rounds
	 * a non-power-of-two page count up to the next buddy block.
	 */
	u64 pages_recorded;
	u64 pages_freed;
};

static inline bool teelog_ledger_life_held(struct teelog_ledger_state *ledger)
{
	return READ_ONCE(ledger->life_owner) == current;
}

static inline int teelog_ledger_do_begin(struct teelog_ledger_state *ledger)
{
	if (teelog_ledger_life_held(ledger))
		return -EBUSY;

	mutex_lock(&ledger->life_mu);
	WRITE_ONCE(ledger->life_owner, current);
	return 0;
}

static inline void teelog_ledger_do_end(struct teelog_ledger_state *ledger)
{
	if (!teelog_ledger_life_held(ledger)) {
		WARN(1, "teelog_ledger_end without matching begin\n");
		return;
	}

	WRITE_ONCE(ledger->life_owner, NULL);
	mutex_unlock(&ledger->life_mu);
}

static inline int teelog_ledger_do_record(struct teelog_ledger_state *ledger,
					  u64 pa, u32 size, u32 order,
					  u64 owner_key, u32 *generation)
{
	u32 gen;
	int rc = 0;

	if (!generation || !owner_key || !size || !IS_ALIGNED(pa, PAGE_SIZE) ||
	    !IS_ALIGNED(size, PAGE_SIZE) || order != get_order(size))
		return -EINVAL;
	if (!teelog_ledger_life_held(ledger))
		return -EPERM;

	mutex_lock(&ledger->ledger_mu);

	if (ledger->slot.occupied) {
		rc = -EBUSY;
		goto out;
	}
	if (ledger->next_gen == 0) {
		rc = -ENOSPC;
		goto out;
	}

	gen = ledger->next_gen;
	if (ledger->next_gen == U32_MAX)
		ledger->next_gen = 0;
	else
		ledger->next_gen++;

	ledger->slot.occupied = true;
	ledger->slot.isolated = false;
	ledger->slot.live = false;
	ledger->slot.owner_key = owner_key;
	ledger->slot.pa = pa;
	ledger->slot.size = size;
	ledger->slot.order = order;
	ledger->slot.generation = gen;
	*generation = gen;
	if (check_add_overflow(ledger->pages_recorded, 1ULL << order,
			       &ledger->pages_recorded))
		ledger->pages_recorded = U64_MAX;

out:
	mutex_unlock(&ledger->ledger_mu);
	return rc;
}

/*
 * Mark the matching slot live after /dev/teelog publish. Rejects a second
 * live claim even with the same generation — only one ACTIVE mapper.
 */
static inline int teelog_ledger_do_claim_live(struct teelog_ledger_state *ledger,
					     u32 generation, u64 owner_key)
{
	int rc = -ENOENT;

	if (!generation || !owner_key)
		return -EINVAL;
	if (!teelog_ledger_life_held(ledger))
		return -EPERM;

	mutex_lock(&ledger->ledger_mu);
	if (ledger->slot.occupied &&
	    ledger->slot.generation == generation &&
	    ledger->slot.owner_key == owner_key) {
		if (ledger->slot.live)
			rc = -EBUSY;
		else {
			ledger->slot.live = true;
			rc = 0;
		}
	}
	mutex_unlock(&ledger->ledger_mu);
	return rc;
}

/* Clear live so a later same-owner probe may recover; keeps the credential. */
static inline int teelog_ledger_do_drop_live(struct teelog_ledger_state *ledger,
					    u32 generation, u64 owner_key)
{
	int rc = -ENOENT;

	if (!generation || !owner_key)
		return -EINVAL;
	if (!teelog_ledger_life_held(ledger))
		return -EPERM;

	mutex_lock(&ledger->ledger_mu);
	if (ledger->slot.occupied &&
	    ledger->slot.generation == generation &&
	    ledger->slot.owner_key == owner_key) {
		ledger->slot.live = false;
		rc = 0;
	}
	mutex_unlock(&ledger->ledger_mu);
	return rc;
}

static inline int teelog_ledger_do_lookup(struct teelog_ledger_state *ledger,
					  u32 generation, u64 *pa, u32 *size,
					  u32 *order, bool *isolated)
{
	int rc = -ENOENT;

	if (!generation)
		return -EINVAL;
	if (!teelog_ledger_life_held(ledger))
		return -EPERM;

	mutex_lock(&ledger->ledger_mu);
	if (ledger->slot.occupied &&
	    ledger->slot.generation == generation) {
		if (pa)
			*pa = ledger->slot.pa;
		if (size)
			*size = ledger->slot.size;
		if (order)
			*order = ledger->slot.order;
		if (isolated)
			*isolated = ledger->slot.isolated;
		rc = 0;
	}
	mutex_unlock(&ledger->ledger_mu);
	return rc;
}

static inline int
teelog_ledger_do_mark_isolated(struct teelog_ledger_state *ledger,
				 u32 generation)
{
	int rc = -ENOENT;

	if (!generation)
		return -EINVAL;
	if (!teelog_ledger_life_held(ledger))
		return -EPERM;

	mutex_lock(&ledger->ledger_mu);
	if (ledger->slot.occupied &&
	    ledger->slot.generation == generation) {
		ledger->slot.isolated = true;
		rc = 0;
	}
	mutex_unlock(&ledger->ledger_mu);
	return rc;
}

static inline int
teelog_ledger_do_release_once(struct teelog_ledger_state *ledger,
				 u32 generation)
{
	int rc = -ENOENT;

	if (!generation)
		return -EINVAL;
	if (!teelog_ledger_life_held(ledger))
		return -EPERM;

	/*
	 * Design §6: match, free_pages, and clear the slot under the same
	 * ledger mutex (caller already holds life_mu).
	 */
	mutex_lock(&ledger->ledger_mu);
	if (ledger->slot.occupied &&
	    ledger->slot.generation == generation) {
		u64 pa = ledger->slot.pa;
		u32 order = ledger->slot.order;
		u64 npages = 1ULL << order;

		free_pages((unsigned long)__va(pa), order);
		if (check_add_overflow(ledger->pages_freed, npages,
				       &ledger->pages_freed))
			ledger->pages_freed = U64_MAX;
		memset(&ledger->slot, 0, sizeof(ledger->slot));
		rc = 0;
	}
	mutex_unlock(&ledger->ledger_mu);
	return rc;
}

static inline int teelog_ledger_do_snapshot(struct teelog_ledger_state *ledger,
					  struct teelog_ledger_entry *entry)
{
	if (!entry)
		return -EINVAL;
	if (!teelog_ledger_life_held(ledger))
		return -EPERM;

	mutex_lock(&ledger->ledger_mu);
	memset(entry, 0, sizeof(*entry));
	if (ledger->slot.occupied) {
		entry->occupied = true;
		entry->isolated = ledger->slot.isolated;
		entry->live = ledger->slot.live;
		entry->owner_key = ledger->slot.owner_key;
		entry->pa = ledger->slot.pa;
		entry->size = ledger->slot.size;
		entry->order = ledger->slot.order;
		entry->generation = ledger->slot.generation;
	}
	mutex_unlock(&ledger->ledger_mu);
	return 0;
}

#endif /* __TEELOG_LEDGER_INTERNAL_H */
