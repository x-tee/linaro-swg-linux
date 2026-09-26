// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the resident teelog page ledger in tee_core.c (U2).
 * Does not talk to a real TEE.
 *
 * Cleanup must run in the same task that called teelog_ledger_begin():
 * life_mu is owned by current, and KUnit deferred actions run in a
 * separate try-catch kthread (see kunit_try_catch_run).
 */
#include <kunit/test.h>
#include <asm/io.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/tee_drv.h>

#include "teelog_ledger_internal.h"

/* Each case gets a fresh private state. Racers share only this test instance. */
static struct teelog_ledger_state test_ledger;

#define teelog_ledger_begin() teelog_ledger_do_begin(&test_ledger)
#define teelog_ledger_end() teelog_ledger_do_end(&test_ledger)
#define teelog_ledger_record(...) teelog_ledger_do_record(&test_ledger, __VA_ARGS__)
#define teelog_ledger_lookup(...) teelog_ledger_do_lookup(&test_ledger, __VA_ARGS__)
#define teelog_ledger_snapshot(...) teelog_ledger_do_snapshot(&test_ledger, __VA_ARGS__)
#define teelog_ledger_mark_isolated(...) \
	teelog_ledger_do_mark_isolated(&test_ledger, __VA_ARGS__)
#define teelog_ledger_release_once(...) \
	teelog_ledger_do_release_once(&test_ledger, __VA_ARGS__)

static int teelog_test_init(struct kunit *test)
{
	bool dirty = false;

	/*
	 * A previous case must leave the private ledger empty. If not, reclaim
	 * orphaned pages before wiping metadata (memset would lose them).
	 */
	if (test_ledger.slot.occupied) {
		u64 pa = test_ledger.slot.pa;
		u32 order = test_ledger.slot.order;
		u32 gen = test_ledger.slot.generation;

		kunit_err(test,
			  "previous case left occupied gen=%u; reclaiming pages\n",
			  gen);
		free_pages((unsigned long)__va(pa), order);
		memset(&test_ledger.slot, 0, sizeof(test_ledger.slot));
		dirty = true;
	}
	if (READ_ONCE(test_ledger.life_owner)) {
		kunit_err(test, "previous case left life lock held by %ps\n",
			  READ_ONCE(test_ledger.life_owner));
		dirty = true;
	}
	if (dirty)
		KUNIT_FAIL(test, "previous case left dirty private ledger");

	memset(&test_ledger, 0, sizeof(test_ledger));
	mutex_init(&test_ledger.life_mu);
	mutex_init(&test_ledger.ledger_mu);
	test_ledger.next_gen = 1;
	return 0;
}

struct teelog_ctx {
	bool life_held;
	u32 owned_gen; /* non-zero => ledger still owns this generation */
	unsigned long pending_va; /* caller-owned page not yet recorded */
	u32 pending_order;
};

static void teelog_ctx_unwind(struct teelog_ctx *c)
{
	if (c->life_held) {
		if (c->owned_gen) {
			(void)teelog_ledger_release_once(c->owned_gen);
			c->owned_gen = 0;
		}
		teelog_ledger_end();
		c->life_held = false;
	} else if (c->owned_gen) {
		if (!teelog_ledger_begin()) {
			if (teelog_ledger_lookup(c->owned_gen, NULL, NULL, NULL,
						 NULL) == 0)
				(void)teelog_ledger_release_once(c->owned_gen);
			teelog_ledger_end();
		}
		c->owned_gen = 0;
	}

	if (c->pending_va) {
		free_pages(c->pending_va, c->pending_order);
		c->pending_va = 0;
	}
}

static int teelog_ctx_begin(struct teelog_ctx *c)
{
	int rc = teelog_ledger_begin();

	if (!rc)
		c->life_held = true;
	return rc;
}

static void teelog_ctx_note_pending(struct teelog_ctx *c, unsigned long va,
				    u32 order)
{
	if (c->pending_va)
		free_pages(c->pending_va, c->pending_order);
	c->pending_va = va;
	c->pending_order = order;
}

static void teelog_ctx_clear_pending(struct teelog_ctx *c)
{
	c->pending_va = 0;
	c->pending_order = 0;
}

static void teelog_ctx_note_owned(struct teelog_ctx *c, u32 gen)
{
	c->owned_gen = gen;
	teelog_ctx_clear_pending(c);
}

static void teelog_ctx_note_released(struct teelog_ctx *c)
{
	c->owned_gen = 0;
}

/* EXPECT is non-fatal: only drop credentials after a successful release. */
static void teelog_ctx_expect_release(struct kunit *test, struct teelog_ctx *c,
				      u32 gen)
{
	int rc = teelog_ledger_release_once(gen);

	KUNIT_EXPECT_EQ(test, 0, rc);
	if (!rc)
		teelog_ctx_note_released(c);
}

static void teelog_ctx_end_life(struct teelog_ctx *c)
{
	if (!c->life_held)
		return;
	teelog_ledger_end();
	c->life_held = false;
}

static unsigned long test_alloc_pages(u32 *order_out, u64 *pa_out)
{
	unsigned long va;
	u32 order = get_order(PAGE_SIZE);

	va = __get_free_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!va)
		return 0;

	*order_out = order;
	*pa_out = (u64)virt_to_phys((void *)va);
	return va;
}

static void test_begin_nested(struct kunit *test)
{
	struct teelog_ctx c = {};

	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "begin failed");
		return;
	}
	KUNIT_EXPECT_EQ(test, -EBUSY, teelog_ledger_begin());
	teelog_ctx_end_life(&c);
	teelog_ctx_unwind(&c);
}

static void test_ops_require_begin(struct kunit *test)
{
	u32 gen = 1;
	u64 pa;
	u32 size, order;
	bool isolated;

	KUNIT_EXPECT_EQ(test, -EPERM,
			teelog_ledger_record(0x1000, PAGE_SIZE, 0, &gen));
	KUNIT_EXPECT_EQ(test, -EPERM,
			teelog_ledger_lookup(1, &pa, &size, &order, &isolated));
	KUNIT_EXPECT_EQ(test, -EPERM, teelog_ledger_mark_isolated(1));
	KUNIT_EXPECT_EQ(test, -EPERM, teelog_ledger_release_once(1));
}

static void test_record_lookup_release(struct kunit *test)
{
	struct teelog_ctx c = {};
	unsigned long va;
	u64 pa;
	u32 order, size, gen = 0, gen2 = 0;
	bool isolated = true;

	va = test_alloc_pages(&order, &pa);
	if (!va) {
		KUNIT_FAIL(test, "page alloc failed");
		return;
	}
	teelog_ctx_note_pending(&c, va, order);

	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "begin failed");
		goto out;
	}
	if (teelog_ledger_record(pa, PAGE_SIZE, order, &gen)) {
		KUNIT_FAIL(test, "record failed");
		goto out;
	}
	teelog_ctx_note_owned(&c, gen);
	KUNIT_EXPECT_NE(test, 0U, gen);

	KUNIT_EXPECT_EQ(test, 0,
			teelog_ledger_lookup(gen, &pa, &size, &order, &isolated));
	KUNIT_EXPECT_EQ(test, PAGE_SIZE, size);
	KUNIT_EXPECT_FALSE(test, isolated);

	KUNIT_EXPECT_EQ(test, -EBUSY,
			teelog_ledger_record(pa, PAGE_SIZE, order, &gen2));

	teelog_ctx_expect_release(test, &c, gen);
	KUNIT_EXPECT_EQ(test, -ENOENT, teelog_ledger_release_once(gen));
	KUNIT_EXPECT_EQ(test, -ENOENT,
			teelog_ledger_lookup(gen, NULL, NULL, NULL, NULL));
	teelog_ctx_end_life(&c);
out:
	teelog_ctx_unwind(&c);
}

static void test_wrong_generation(struct kunit *test)
{
	struct teelog_ctx c = {};
	unsigned long va;
	u64 pa;
	u32 order, gen = 0;

	va = test_alloc_pages(&order, &pa);
	if (!va) {
		KUNIT_FAIL(test, "page alloc failed");
		return;
	}
	teelog_ctx_note_pending(&c, va, order);

	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "begin failed");
		goto out;
	}
	if (teelog_ledger_record(pa, PAGE_SIZE, order, &gen)) {
		KUNIT_FAIL(test, "record failed");
		goto out;
	}
	teelog_ctx_note_owned(&c, gen);

	KUNIT_EXPECT_EQ(test, -ENOENT, teelog_ledger_release_once(gen + 1));
	KUNIT_EXPECT_EQ(test, -ENOENT, teelog_ledger_mark_isolated(gen + 1));
	teelog_ctx_expect_release(test, &c, gen);
	teelog_ctx_end_life(&c);
out:
	teelog_ctx_unwind(&c);
}

static void test_isolated_preserved(struct kunit *test)
{
	struct teelog_ctx c = {};
	unsigned long va;
	u64 pa;
	u32 order, gen = 0;
	bool isolated = false;

	va = test_alloc_pages(&order, &pa);
	if (!va) {
		KUNIT_FAIL(test, "page alloc failed");
		return;
	}
	teelog_ctx_note_pending(&c, va, order);

	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "begin failed");
		goto out;
	}
	if (teelog_ledger_record(pa, PAGE_SIZE, order, &gen)) {
		KUNIT_FAIL(test, "record failed");
		goto out;
	}
	teelog_ctx_note_owned(&c, gen);

	KUNIT_EXPECT_EQ(test, 0, teelog_ledger_mark_isolated(gen));
	KUNIT_EXPECT_EQ(test, 0,
			teelog_ledger_lookup(gen, &pa, NULL, &order, &isolated));
	KUNIT_EXPECT_TRUE(test, isolated);
	KUNIT_EXPECT_EQ(test, (u64)virt_to_phys((void *)va), pa);
	teelog_ctx_expect_release(test, &c, gen);
	teelog_ctx_end_life(&c);
out:
	teelog_ctx_unwind(&c);
}

/*
 * Inject an early-exit unwind while life_mu is held and a page is recorded.
 * Proves same-task cleanup frees the slot and drops the lock (unlike a
 * deferred kunit action running in another try-catch kthread).
 */
static void test_unwind_while_life_held(struct kunit *test)
{
	struct teelog_ctx c = {};
	unsigned long va;
	u64 pa;
	u32 order, gen = 0;

	va = test_alloc_pages(&order, &pa);
	if (!va) {
		KUNIT_FAIL(test, "page alloc failed");
		return;
	}
	teelog_ctx_note_pending(&c, va, order);

	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "begin failed");
		goto out;
	}
	if (teelog_ledger_record(pa, PAGE_SIZE, order, &gen)) {
		KUNIT_FAIL(test, "record failed");
		goto out;
	}
	teelog_ctx_note_owned(&c, gen);

	/* Simulate fatal-assert early exit: unwind in this task. */
	teelog_ctx_unwind(&c);

	KUNIT_EXPECT_EQ(test, 0, teelog_ledger_begin());
	KUNIT_EXPECT_EQ(test, -ENOENT,
			teelog_ledger_lookup(gen, NULL, NULL, NULL, NULL));
	KUNIT_EXPECT_EQ(test, -ENOENT, teelog_ledger_release_once(gen));
	KUNIT_EXPECT_EQ(test, -EBUSY, teelog_ledger_begin());
	teelog_ledger_end();

	/* Pending-only: alloc under begin, unwind before record. */
	va = test_alloc_pages(&order, &pa);
	if (!va) {
		KUNIT_FAIL(test, "second page alloc failed");
		return;
	}
	teelog_ctx_note_pending(&c, va, order);
	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "second begin failed");
		goto out;
	}
	teelog_ctx_unwind(&c);

	KUNIT_EXPECT_EQ(test, 0, teelog_ledger_begin());
	KUNIT_EXPECT_EQ(test, -ENOENT,
			teelog_ledger_lookup(1, NULL, NULL, NULL, NULL));
	teelog_ledger_end();
	return;
out:
	teelog_ctx_unwind(&c);
}

struct release_race_ctx {
	struct completion ready[2];
	struct completion done[2];
	struct completion go;
	atomic_t next_id;
	u32 generation;
	int results[2];
	atomic_t free_ok;
};

static int release_racer(void *data)
{
	struct release_race_ctx *ctx = data;
	int id = atomic_fetch_inc(&ctx->next_id);
	int rc;

	if (id < 0 || id > 1)
		return 0;

	complete(&ctx->ready[id]);
	wait_for_completion(&ctx->go);

	rc = teelog_ledger_begin();
	if (rc) {
		ctx->results[id] = rc;
		complete(&ctx->done[id]);
		return 0;
	}
	rc = teelog_ledger_release_once(ctx->generation);
	ctx->results[id] = rc;
	if (rc == 0)
		atomic_inc(&ctx->free_ok);
	teelog_ledger_end();
	complete(&ctx->done[id]);
	return 0;
}

static void test_concurrent_release_once(struct kunit *test)
{
	struct teelog_ctx c = {};
	struct release_race_ctx ctx;
	unsigned long va;
	u64 pa;
	u32 order, gen = 0;
	struct task_struct *t0, *t1;
	int n_ok = 0, n_enoent = 0, i;

	memset(&ctx, 0, sizeof(ctx));
	init_completion(&ctx.ready[0]);
	init_completion(&ctx.ready[1]);
	init_completion(&ctx.done[0]);
	init_completion(&ctx.done[1]);
	init_completion(&ctx.go);
	atomic_set(&ctx.next_id, 0);
	atomic_set(&ctx.free_ok, 0);
	ctx.results[0] = ctx.results[1] = -EINVAL;

	va = test_alloc_pages(&order, &pa);
	if (!va) {
		KUNIT_FAIL(test, "page alloc failed");
		return;
	}
	teelog_ctx_note_pending(&c, va, order);

	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "begin failed");
		goto out;
	}
	if (teelog_ledger_record(pa, PAGE_SIZE, order, &gen)) {
		KUNIT_FAIL(test, "record failed");
		goto out;
	}
	teelog_ctx_note_owned(&c, gen);
	teelog_ctx_end_life(&c);
	ctx.generation = gen;

	t0 = kthread_run(release_racer, &ctx, "teelog_racer0");
	if (IS_ERR_OR_NULL(t0)) {
		KUNIT_FAIL(test, "failed to start first racer thread");
		goto out;
	}
	t1 = kthread_run(release_racer, &ctx, "teelog_racer1");
	if (IS_ERR_OR_NULL(t1)) {
		wait_for_completion(&ctx.ready[0]);
		complete_all(&ctx.go);
		wait_for_completion(&ctx.done[0]);
		/* Keep owned_gen; unwind looks up the real slot state. */
		KUNIT_FAIL(test, "failed to start second racer thread");
		goto out;
	}

	wait_for_completion(&ctx.ready[0]);
	wait_for_completion(&ctx.ready[1]);
	complete_all(&ctx.go);
	wait_for_completion(&ctx.done[0]);
	wait_for_completion(&ctx.done[1]);

	for (i = 0; i < 2; i++) {
		if (ctx.results[i] == 0)
			n_ok++;
		else if (ctx.results[i] == -ENOENT)
			n_enoent++;
	}

	KUNIT_EXPECT_EQ(test, 1, n_ok);
	KUNIT_EXPECT_EQ(test, 1, n_enoent);
	KUNIT_EXPECT_EQ(test, 1, atomic_read(&ctx.free_ok));
	/*
	 * Do not clear owned_gen here: EXPECT is non-fatal. If racers failed
	 * to free, unwind must still see the generation and reclaim the slot.
	 */
out:
	teelog_ctx_unwind(&c);
}

/*
 * Exhaust a private instance; the production generation counter is untouched.
 */
static void test_generation_monotonic_and_exhaustion(struct kunit *test)
{
	struct teelog_ctx c = {};
	unsigned long va1, va2, va_fail;
	u64 pa1, pa2, pa_fail;
	u32 order1, order2, order_fail, gen1 = 0, gen2 = 0, gen3 = 0;

	va1 = test_alloc_pages(&order1, &pa1);
	if (!va1) {
		KUNIT_FAIL(test, "page1 alloc failed");
		return;
	}
	teelog_ctx_note_pending(&c, va1, order1);

	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "begin failed");
		goto out;
	}
	if (teelog_ledger_record(pa1, PAGE_SIZE, order1, &gen1)) {
		KUNIT_FAIL(test, "record1 failed");
		goto out;
	}
	teelog_ctx_note_owned(&c, gen1);
	if (teelog_ledger_release_once(gen1)) {
		KUNIT_FAIL(test, "release1 failed");
		goto out;
	}
	teelog_ctx_note_released(&c);

	va2 = test_alloc_pages(&order2, &pa2);
	if (!va2) {
		KUNIT_FAIL(test, "page2 alloc failed");
		goto out;
	}
	teelog_ctx_note_pending(&c, va2, order2);
	if (teelog_ledger_record(pa2, PAGE_SIZE, order2, &gen2)) {
		KUNIT_FAIL(test, "record2 failed");
		goto out;
	}
	teelog_ctx_note_owned(&c, gen2);
	KUNIT_EXPECT_GT(test, gen2, gen1);
	if (teelog_ledger_release_once(gen2)) {
		KUNIT_FAIL(test, "release2 failed");
		goto out;
	}
	teelog_ctx_note_released(&c);

	test_ledger.next_gen = U32_MAX;
	va1 = test_alloc_pages(&order1, &pa1);
	if (!va1) {
		KUNIT_FAIL(test, "page for U32_MAX alloc failed");
		goto out;
	}
	teelog_ctx_note_pending(&c, va1, order1);
	if (teelog_ledger_record(pa1, PAGE_SIZE, order1, &gen3)) {
		KUNIT_FAIL(test, "record U32_MAX failed");
		goto out;
	}
	teelog_ctx_note_owned(&c, gen3);
	KUNIT_EXPECT_EQ(test, U32_MAX, gen3);
	KUNIT_EXPECT_EQ(test, 0U, test_ledger.next_gen);

	va_fail = test_alloc_pages(&order_fail, &pa_fail);
	if (!va_fail) {
		KUNIT_FAIL(test, "fail-path page alloc failed");
		goto out;
	}
	teelog_ctx_note_pending(&c, va_fail, order_fail);

	KUNIT_EXPECT_EQ(test, -EBUSY,
			teelog_ledger_record(pa_fail, PAGE_SIZE, order_fail,
					     &gen1));

	if (teelog_ledger_release_once(gen3)) {
		KUNIT_FAIL(test, "release3 failed");
		goto out;
	}
	teelog_ctx_note_released(&c);

	KUNIT_EXPECT_EQ(test, -ENOSPC,
			teelog_ledger_record(pa_fail, PAGE_SIZE, order_fail,
					     &gen1));
	/* ENOSPC: page stays caller-owned in pending; unwind frees it. */
	teelog_ctx_end_life(&c);
out:
	teelog_ctx_unwind(&c);
}

/* Simulate module-local generation loss while the resident slot survives. */
static void test_snapshot_without_generation(struct kunit *test)
{
	struct teelog_ctx c = {};
	struct teelog_ledger_entry entry;
	unsigned long va;
	u64 pa;
	u32 order, gen = 0;

	KUNIT_EXPECT_EQ(test, -EPERM, teelog_ledger_snapshot(&entry));
	va = test_alloc_pages(&order, &pa);
	if (!va) {
		KUNIT_FAIL(test, "page alloc failed");
		return;
	}
	teelog_ctx_note_pending(&c, va, order);
	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "begin failed");
		goto out;
	}
	KUNIT_EXPECT_EQ(test, -EINVAL, teelog_ledger_snapshot(NULL));
	if (teelog_ledger_record(pa, PAGE_SIZE, order, &gen)) {
		KUNIT_FAIL(test, "record failed");
		goto out;
	}
	teelog_ctx_note_owned(&c, gen);
	KUNIT_EXPECT_EQ(test, 0, teelog_ledger_mark_isolated(gen));
	teelog_ctx_end_life(&c);
	gen = 0; /* The next caller has no generation and no TEE active record. */
	if (teelog_ctx_begin(&c)) {
		KUNIT_FAIL(test, "recovery begin failed");
		goto out;
	}
	if (teelog_ledger_snapshot(&entry)) {
		KUNIT_FAIL(test, "snapshot failed");
		goto out;
	}
	KUNIT_EXPECT_TRUE(test, entry.occupied);
	KUNIT_EXPECT_TRUE(test, entry.isolated);
	KUNIT_EXPECT_EQ(test, pa, entry.pa);
	KUNIT_EXPECT_EQ(test, (u32)PAGE_SIZE, entry.size);
	KUNIT_EXPECT_EQ(test, order, entry.order);
	KUNIT_EXPECT_NE(test, 0U, entry.generation);
	gen = entry.generation;
	if (teelog_ledger_release_once(gen)) {
		KUNIT_FAIL(test, "release failed");
		goto out;
	}
	teelog_ctx_note_released(&c);
	memset(&entry, 0xff, sizeof(entry));
	KUNIT_EXPECT_EQ(test, 0, teelog_ledger_snapshot(&entry));
	KUNIT_EXPECT_FALSE(test, entry.occupied);
	KUNIT_EXPECT_FALSE(test, entry.isolated);
	KUNIT_EXPECT_EQ(test, 0ULL, entry.pa);
	KUNIT_EXPECT_EQ(test, 0U, entry.generation);
	KUNIT_EXPECT_EQ(test, 0U, entry.size);
	KUNIT_EXPECT_EQ(test, 0U, entry.order);
	KUNIT_EXPECT_EQ(test, -ENOENT, teelog_ledger_release_once(gen));
out:
	teelog_ctx_unwind(&c);
}

/* Parenthesized calls bypass the private-instance macros above. */
static void test_exhaustion_leaves_resident_usable(struct kunit *test)
{
	struct teelog_ledger_entry entry;
	unsigned long va;
	u64 pa;
	u32 order, gen = 0;
	int rc;

	test_generation_monotonic_and_exhaustion(test);
	KUNIT_ASSERT_EQ(test, 0U, test_ledger.next_gen);
	KUNIT_ASSERT_FALSE(test, test_ledger.slot.occupied);
	KUNIT_ASSERT_EQ(test, 0, (teelog_ledger_begin)());
	rc = (teelog_ledger_snapshot)(&entry);
	KUNIT_EXPECT_EQ(test, 0, rc);
	if (rc || entry.occupied) {
		(teelog_ledger_end)();
		if (!rc)
			kunit_skip(test, "resident allocation active; do not touch it");
		return;
	}
	va = test_alloc_pages(&order, &pa);
	if (!va) {
		(teelog_ledger_end)();
		KUNIT_FAIL(test, "page alloc failed");
		return;
	}
	rc = (teelog_ledger_record)(pa, PAGE_SIZE, order, &gen);
	KUNIT_EXPECT_EQ(test, 0, rc);
	if (rc)
		free_pages(va, order);
	else
		KUNIT_EXPECT_EQ(test, 0, (teelog_ledger_release_once)(gen));
	(teelog_ledger_end)();
}

static struct kunit_case teelog_ledger_test_cases[] = {
	KUNIT_CASE(test_begin_nested),
	KUNIT_CASE(test_ops_require_begin),
	KUNIT_CASE(test_record_lookup_release),
	KUNIT_CASE(test_wrong_generation),
	KUNIT_CASE(test_isolated_preserved),
	KUNIT_CASE(test_unwind_while_life_held),
	KUNIT_CASE(test_concurrent_release_once),
	KUNIT_CASE(test_snapshot_without_generation),
	KUNIT_CASE(test_generation_monotonic_and_exhaustion),
	KUNIT_CASE(test_exhaustion_leaves_resident_usable),
	{}
};

static struct kunit_suite teelog_ledger_test_suite = {
	.name = "teelog_ledger",
	.init = teelog_test_init,
	.test_cases = teelog_ledger_test_cases,
};

kunit_test_suite(teelog_ledger_test_suite);

MODULE_DESCRIPTION("KUnit tests for teelog resident ledger");
MODULE_LICENSE("GPL");
