// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit coverage for teelog lifecycle helpers (U5).
 * Pure decision / inject-policy tests — no live TEE, no ledger, no free_pages.
 * Asserts UNKNOWN never maps to release; REJECTED inject is pre-call only.
 * These cases do not close the fault-path runtime ticket; that still needs
 * CONFIG_OPTEE_TEELOG_FAULT_INJECT + observed register/stop resource state.
 */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include "teelog_internal.h"
#include "teelog_protocol.h"
#include "teelog_ring_read.h"

#define TEEC_SUCCESS			0x00000000U
#define TEEC_ERROR_ACCESS_DENIED	0xFFFF0001U
#define TEEC_ERROR_BAD_PARAMETERS	0xFFFF0006U
#define TEEC_ERROR_ITEM_NOT_FOUND	0xFFFF0008U
#define TEEC_ERROR_NOT_SUPPORTED	0xFFFF000AU
#define TEEC_ERROR_OUT_OF_MEMORY	0xFFFF000CU
#define TEEC_ERROR_BUSY			0xFFFF000DU

static void test_classify_ok(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_OK,
			teelog_classify_invoke(0, TEEC_SUCCESS));
}

static void test_classify_linux_error_unknown(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_UNKNOWN,
			teelog_classify_invoke(-EIO, TEEC_SUCCESS));
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_UNKNOWN,
			teelog_classify_invoke(-ETIMEDOUT, 0));
}

static void test_classify_communication_unknown(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_UNKNOWN,
			teelog_classify_invoke(0, TEELOG_TEE_ERROR_COMMUNICATION));
}

static void test_classify_explicit_reject(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_REJECTED,
			teelog_classify_invoke(0, TEEC_ERROR_BAD_PARAMETERS));
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_REJECTED,
			teelog_classify_invoke(0, TEEC_ERROR_ACCESS_DENIED));
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_REJECTED,
			teelog_classify_invoke(0, TEELOG_TEE_ERROR_BAD_STATE));
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_REJECTED,
			teelog_classify_invoke(0, TEEC_ERROR_ITEM_NOT_FOUND));
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_REJECTED,
			teelog_classify_invoke(0, TEEC_ERROR_OUT_OF_MEMORY));
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_REJECTED,
			teelog_classify_invoke(0, TEEC_ERROR_NOT_SUPPORTED));
}

static void test_classify_busy_unknown(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_UNKNOWN,
			teelog_classify_invoke(0, TEEC_ERROR_BUSY));
}

static void test_recover_owner_gate(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_GATE_PROCEED,
			teelog_recover_owner_gate(false, false, 0, 1));
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_GATE_SKIP_LIVE,
			teelog_recover_owner_gate(true, true, 1, 1));
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_GATE_SKIP_LIVE,
			teelog_recover_owner_gate(true, true, 1, 2));
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_GATE_SKIP_OWNER,
			teelog_recover_owner_gate(true, false, 1, 2));
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_GATE_SKIP_OWNER,
			teelog_recover_owner_gate(true, false, 1, 0));
	/* Unknown slot owner fails closed. */
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_GATE_SKIP_OWNER,
			teelog_recover_owner_gate(true, false, 0, 1));
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_GATE_PROCEED,
			teelog_recover_owner_gate(true, false, 1, 1));
}

static void test_recover_inactive_releases(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_RELEASE,
			teelog_recover_decide(true, false, false));
}

static void test_recover_active_match_unregisters(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_UNREGISTER,
			teelog_recover_decide(true, true, true));
}

static void test_recover_active_mismatch_aborts(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_ABORT,
			teelog_recover_decide(true, true, false));
}

static void test_recover_query_fail_aborts(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_ABORT,
			teelog_recover_decide(false, false, false));
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_ABORT,
			teelog_recover_decide(false, true, true));
}

static void test_reg_fields_match(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  teelog_reg_fields_match(0x1000, 262144, 3,
						  0x1000, 262144, 3));
	KUNIT_EXPECT_FALSE(test,
			   teelog_reg_fields_match(0x1000, 262144, 3,
						   0x2000, 262144, 3));
	KUNIT_EXPECT_FALSE(test,
			   teelog_reg_fields_match(0x1000, 262144, 3,
						   0x1000, 262144, 4));
	KUNIT_EXPECT_FALSE(test,
			   teelog_reg_fields_match(0x1000, 262144, 0,
						   0x1000, 262144, 0));
}

static void test_pages_after_register_fate(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_PAGE_KEEP,
			teelog_pages_after_register(TEELOG_COMM_OK));
	KUNIT_EXPECT_EQ(test, TEELOG_PAGE_RELEASE,
			teelog_pages_after_register(TEELOG_COMM_REJECTED));
	KUNIT_EXPECT_EQ(test, TEELOG_PAGE_ISOLATE,
			teelog_pages_after_register(TEELOG_COMM_UNKNOWN));
}

static void test_pages_after_unregister_fate(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TEELOG_PAGE_RELEASE,
			teelog_pages_after_unregister(TEELOG_COMM_OK));
	/* Confirmed stop only; UNKNOWN/REJECTED must not free. */
	KUNIT_EXPECT_EQ(test, TEELOG_PAGE_ISOLATE,
			teelog_pages_after_unregister(TEELOG_COMM_UNKNOWN));
	KUNIT_EXPECT_EQ(test, TEELOG_PAGE_ISOLATE,
			teelog_pages_after_unregister(TEELOG_COMM_REJECTED));
}

static void test_inject_reject_is_pre_call(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  teelog_inject_skip_register_call(
				  TEELOG_INJECT_REG_REJECT_PRE));
	KUNIT_EXPECT_FALSE(test,
			   teelog_inject_skip_register_call(
				   TEELOG_INJECT_NONE));
	KUNIT_EXPECT_FALSE(test,
			   teelog_inject_skip_register_call(
				   TEELOG_INJECT_REG_UNKNOWN_POST));
	KUNIT_EXPECT_FALSE(test,
			   teelog_inject_skip_register_call(
				   TEELOG_INJECT_UNREG_UNKNOWN_POST));
	KUNIT_EXPECT_TRUE(test,
			  teelog_inject_alloc_fail(TEELOG_INJECT_ALLOC_FAIL));
	KUNIT_EXPECT_FALSE(test,
			   teelog_inject_alloc_fail(TEELOG_INJECT_NONE));
	KUNIT_EXPECT_TRUE(test,
			  teelog_inject_recover_mismatch(
				  TEELOG_INJECT_RECOVER_MISMATCH));
	KUNIT_EXPECT_FALSE(test,
			   teelog_inject_recover_mismatch(TEELOG_INJECT_NONE));
}

static void test_inject_corrupt_reg_reply_breaks_match(struct kunit *test)
{
	u64 tee_pa = 0x1000;
	u32 tee_gen = 3;
	u64 ledger_pa = 0x1000;
	u32 ledger_size = 262144;
	u32 ledger_gen = 3;

	KUNIT_ASSERT_TRUE(test,
			  teelog_reg_fields_match(ledger_pa, ledger_size,
						  ledger_gen, tee_pa,
						  ledger_size, tee_gen));

	teelog_inject_corrupt_reg_reply(TEELOG_INJECT_RECOVER_MISMATCH, true,
					&tee_pa, &tee_gen);
	KUNIT_EXPECT_FALSE(test,
			   teelog_reg_fields_match(ledger_pa, ledger_size,
						   ledger_gen, tee_pa,
						   ledger_size, tee_gen));
	KUNIT_EXPECT_EQ(test, TEELOG_RECOVER_ABORT,
			teelog_recover_decide(true, true, false));

	/* Inactive TEE: corrupt helper is a no-op. */
	tee_pa = 0x1000;
	tee_gen = 3;
	teelog_inject_corrupt_reg_reply(TEELOG_INJECT_RECOVER_MISMATCH, false,
					&tee_pa, &tee_gen);
	KUNIT_EXPECT_EQ(test, 0x1000ULL, tee_pa);
	KUNIT_EXPECT_EQ(test, 3U, tee_gen);
}

static void test_inject_post_only_unknown(struct kunit *test)
{
	/* Post inject must not turn OK into REJECTED (would free after real REG). */
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_UNKNOWN,
			teelog_inject_after_register(
				TEELOG_INJECT_REG_UNKNOWN_POST, TEELOG_COMM_OK));
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_OK,
			teelog_inject_after_register(TEELOG_INJECT_NONE,
						     TEELOG_COMM_OK));
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_REJECTED,
			teelog_inject_after_register(TEELOG_INJECT_NONE,
						     TEELOG_COMM_REJECTED));
	/* Reject inject value must not rewrite post-call classify. */
	KUNIT_EXPECT_EQ(test, TEELOG_COMM_OK,
			teelog_inject_after_register(
				TEELOG_INJECT_REG_REJECT_PRE, TEELOG_COMM_OK));

	KUNIT_EXPECT_EQ(test, TEELOG_COMM_UNKNOWN,
			teelog_inject_after_unregister(
				TEELOG_INJECT_UNREG_UNKNOWN_POST,
				TEELOG_COMM_OK));
	KUNIT_EXPECT_EQ(test, TEELOG_PAGE_ISOLATE,
			teelog_pages_after_unregister(
				teelog_inject_after_unregister(
					TEELOG_INJECT_UNREG_UNKNOWN_POST,
					TEELOG_COMM_OK)));
}

static void test_reject_path_may_release_unknown_must_not(struct kunit *test)
{
	/*
	 * Lifecycle ordering contract used by register_new / stop:
	 * REJECTED → RELEASE; UNKNOWN → ISOLATE (no free_pages).
	 */
	KUNIT_EXPECT_EQ(test, TEELOG_PAGE_RELEASE,
			teelog_pages_after_register(TEELOG_COMM_REJECTED));
	KUNIT_EXPECT_NE(test, TEELOG_PAGE_RELEASE,
			teelog_pages_after_register(TEELOG_COMM_UNKNOWN));
	KUNIT_EXPECT_NE(test, TEELOG_PAGE_RELEASE,
			teelog_pages_after_unregister(TEELOG_COMM_UNKNOWN));
}

static void test_reader_catchup_unsaturated_lap(struct kunit *test)
{
	struct teelog_reader_pos pos = {
		.r_off = 100,
		.r_loops = 1,
		.r_seq = 10,
		.r_serial_valid = true,
		.r_serial = 5,
	};
	struct teelog_hdr_snap snap = {
		.last_pos = 200,
		.write_loops = 3,
		.max_len = 1000,
		.seq = 20,
	};

	KUNIT_EXPECT_EQ(test, 1, teelog_reader_catchup(&pos, &snap, NULL, 0));
	KUNIT_EXPECT_EQ(test, 200U, pos.r_off);
	KUNIT_EXPECT_EQ(test, 2U, pos.r_loops);
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);
}

static void test_reader_catchup_saturated_wrap(struct kunit *test)
{
	struct teelog_reader_pos pos = {
		.r_off = 800,
		.r_loops = U32_MAX,
		.r_seq = 100,
		.r_serial_valid = true,
		.r_serial = 1,
	};
	struct teelog_hdr_snap snap = {
		.last_pos = 100,
		.write_loops = U32_MAX,
		.max_len = 1000,
		.seq = 110,
	};

	/* CR-004: equal saturated loops + last_pos < r_off ⇒ lapped. */
	KUNIT_EXPECT_EQ(test, 1, teelog_reader_catchup(&pos, &snap, NULL, 0));
	KUNIT_EXPECT_EQ(test, 100U, pos.r_off);
	KUNIT_EXPECT_EQ(test, U32_MAX - 1U, pos.r_loops);
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);
}

static void test_reader_catchup_saturated_same_off(struct kunit *test)
{
	struct teelog_reader_pos pos = {
		.r_off = 256,
		.r_loops = U32_MAX,
		.r_seq = 40,
		.r_serial_valid = true,
		.r_serial = 1,
	};
	struct teelog_hdr_snap snap = {
		.last_pos = 256,
		.write_loops = U32_MAX,
		.max_len = 1024,
		.seq = 80,
	};

	/*
	 * Full ring back to the same offset under saturation: loops equal and
	 * last_pos == r_off must not look like "no data".
	 */
	KUNIT_EXPECT_TRUE(test, teelog_reader_has_data(&pos, &snap));
	KUNIT_EXPECT_EQ(test, 1, teelog_reader_catchup(&pos, &snap, NULL, 0));
	KUNIT_EXPECT_EQ(test, 256U, pos.r_off);
	KUNIT_EXPECT_EQ(test, U32_MAX - 1U, pos.r_loops);
	KUNIT_EXPECT_EQ(test, 40ULL, pos.r_seq);
	KUNIT_EXPECT_EQ(test, 80ULL, pos.r_obs_seq);
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);
	KUNIT_EXPECT_TRUE(test, teelog_reader_has_data(&pos, &snap));
}

static void test_reader_catchup_seq_threshold(struct kunit *test)
{
	struct teelog_reader_pos pos = {
		.r_off = 10,
		.r_loops = 0,
		.r_seq = 0,
		.r_serial_valid = true,
	};
	struct teelog_hdr_snap snap = {
		.last_pos = 50,
		.write_loops = 0,
		.max_len = 1000,
		.seq = 1ULL << 32,
	};

	KUNIT_EXPECT_EQ(test, 1, teelog_reader_catchup(&pos, &snap, NULL, 0));
	KUNIT_EXPECT_EQ(test, 0U, pos.r_off);
	KUNIT_EXPECT_FALSE(test, pos.r_serial_valid);
}

static void test_reader_serial_seq_floor(struct kunit *test)
{
	struct teelog_hdr_snap snap = {
		.last_pos = 128, .write_loops = U32_MAX,
		.max_len = TEELOG_DEFAULT_MAX_LEN, .seq = 5000,
	};
	struct teelog_reader_pos pos = {
		.r_off = 128, .r_loops = U32_MAX, .r_serial = 1000,
		.r_serial_valid = true, .r_obs_valid = true,
		.r_obs_seq = 5000, .r_obs_last = 128,
	};

	pos.r_seq = teelog_reader_serial_seq_floor(&snap);
	/* seq floor is older than producer tip, but fully observed empty is empty. */
	KUNIT_EXPECT_FALSE(test, teelog_reader_has_data(&pos, &snap));
	snap.seq += 2;
	KUNIT_EXPECT_TRUE(test, teelog_reader_has_data(&pos, &snap));
	/* Delivered record age can cross half-range before tip-relative delta does. */
	snap.seq = pos.r_seq + (1ULL << 32);
	KUNIT_EXPECT_EQ(test, 1, teelog_reader_catchup(&pos, &snap, NULL, 0));
	KUNIT_EXPECT_FALSE(test, pos.r_serial_valid);
}

/*
 * Saturated wrap that lands last_pos > r_off: observed last_pos regression
 * (261248 → 1152) proves the wrap — not a commit-count threshold.
 */
static void test_reader_catchup_saturated_obs_regression(struct kunit *test)
{
	struct teelog_reader_pos pos = {
		.r_off = 128,
		.r_loops = U32_MAX,
		.r_seq = 1000,
		.r_obs_seq = 1000,
		.r_obs_last = 261248,
		.r_obs_valid = true,
		.r_serial_valid = true,
		.r_serial = 10,
	};
	struct teelog_hdr_snap snap = {
		.last_pos = 1152,
		.write_loops = U32_MAX,
		.max_len = TEELOG_DEFAULT_MAX_LEN,
		.seq = 1004,
	};

	KUNIT_EXPECT_EQ(test, 1, teelog_reader_catchup(&pos, &snap, NULL, 0));
	KUNIT_EXPECT_EQ(test, 1152U, pos.r_off);
	KUNIT_EXPECT_EQ(test, U32_MAX - 1U, pos.r_loops);
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);
}

static void reader_test_record(struct teelog_buffer *ring, u32 off, u32 len)
{
	struct teelog_item *item = (void *)(ring->buffer_start + off);

	item->magic = TEELOG_ITEM_MAGIC;
	item->buffer_len = len - TEELOG_ITEM_HDR_SIZE;
	item->real_len = item->buffer_len;
}

/* No wrap: many short commits must not clear the serial baseline. */
static void test_reader_catchup_saturated_no_wrap_short(struct kunit *test)
{
	struct teelog_reader_pos pos = {
		.r_off = 128, .r_loops = U32_MAX, .r_seq = 2000,
		.r_obs_seq = 2000, .r_obs_last = 128, .r_obs_valid = true,
		.r_serial_valid = true, .r_serial = 1000,
	};
	struct teelog_hdr_snap snap = {
		.last_pos = 32896, .write_loops = U32_MAX,
		.max_len = TEELOG_DEFAULT_MAX_LEN, .seq = 2000 + 512,
	};
	struct teelog_buffer *ring;
	u32 i;

	ring = kunit_kzalloc(test, TEELOG_DEFAULT_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, ring);
	teelog_seq_store(&ring->flag, snap.seq);
	reader_test_record(ring, 0, 128);
	for (i = 0; i < 256; i++)
		reader_test_record(ring, 128 + i * 128, 128);
	KUNIT_EXPECT_EQ(test, 0, teelog_reader_catchup(&pos, &snap, ring,
						     TEELOG_DEFAULT_SIZE));
	KUNIT_EXPECT_EQ(test, 128U, pos.r_off);
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);
	KUNIT_EXPECT_EQ(test, 1000U, pos.r_serial);
	KUNIT_EXPECT_EQ(test, 32896U, pos.r_obs_last);
	KUNIT_EXPECT_EQ(test, snap.seq, pos.r_obs_seq);
}

/* Original F4: one full lap can finish at a numerically larger producer tip. */
static void test_reader_catchup_saturated_full_lap(struct kunit *test)
{
	struct teelog_reader_pos pos = {
		.r_off = 128, .r_loops = U32_MAX, .r_seq = 1000,
		.r_obs_seq = 1000, .r_obs_last = 128, .r_obs_valid = true,
		.r_serial_valid = true, .r_serial = 10,
	};
	struct teelog_hdr_snap snap = {
		.last_pos = 1152, .write_loops = U32_MAX,
		.max_len = TEELOG_DEFAULT_MAX_LEN, .seq = 1514,
	};

	KUNIT_EXPECT_EQ(test, 1, teelog_reader_catchup(&pos, &snap, NULL, 0));
	KUNIT_EXPECT_EQ(test, 1152U, pos.r_off);
	KUNIT_EXPECT_EQ(test, U32_MAX - 1U, pos.r_loops);
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);
}

/* The length lower bound is inconclusive; the exact commit chain decides. */
static void test_reader_catchup_saturated_span(struct kunit *test)
{
	struct teelog_reader_pos pos = {
		.r_off = 128, .r_loops = U32_MAX, .r_seq = 3000,
		.r_obs_seq = 3000, .r_obs_last = 128, .r_obs_valid = true,
		.r_serial_valid = true, .r_serial = 1000,
	};
	struct teelog_reader_pos before = pos;
	struct teelog_hdr_snap snap = {
		.last_pos = 204928, .write_loops = U32_MAX,
		.max_len = TEELOG_DEFAULT_MAX_LEN, .seq = 3800,
	};
	struct teelog_buffer *ring;
	u32 i;

	ring = kunit_kzalloc(test, TEELOG_DEFAULT_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, ring);
	reader_test_record(ring, 0, 128);
	for (i = 0; i < 400; i++)
		reader_test_record(ring, 128 + i * 512, 512);
	teelog_seq_store(&ring->flag, snap.seq + 2);
	KUNIT_EXPECT_EQ(test, -EAGAIN,
		teelog_reader_catchup(&pos, &snap, ring, TEELOG_DEFAULT_SIZE));
	KUNIT_EXPECT_EQ(test, 0, memcmp(&pos, &before, sizeof(pos)));
	teelog_seq_store(&ring->flag, snap.seq);
	KUNIT_EXPECT_EQ(test, 0,
		teelog_reader_catchup(&pos, &snap, ring, TEELOG_DEFAULT_SIZE));
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);

	/* Same span but only 200 complete records for 500 new commits: lapped. */
	memset(ring->buffer_start, 0, TEELOG_DEFAULT_MAX_LEN);
	reader_test_record(ring, 0, 128);
	snap.seq = 4000;
	teelog_seq_store(&ring->flag, snap.seq);
	for (i = 0; i < 200; i++)
		reader_test_record(ring, 128 + i * 1024, 1024);
	pos = before;
	KUNIT_EXPECT_EQ(test, 1,
		teelog_reader_catchup(&pos, &snap, ring, TEELOG_DEFAULT_SIZE));
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);
}

static void test_reader_seek_current(struct kunit *test)
{
	struct teelog_reader_pos pos = {
		.r_obs_seq = 2, .r_obs_last = 261888,
		.r_serial_valid = true, .r_serial = 900,
	};
	struct teelog_hdr_snap snap = {
		.last_pos = 128, .write_loops = U32_MAX, .seq = 2002,
		.max_len = TEELOG_DEFAULT_MAX_LEN,
	};
	struct teelog_reader_pos before = pos;
	struct teelog_buffer *ring;

	ring = kunit_kzalloc(test, TEELOG_DEFAULT_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, ring);
	reader_test_record(ring, 0, 128);
	((struct teelog_item *)ring->buffer_start)->serial_no = 1001;
	teelog_seq_store(&ring->flag, snap.seq + 2);
	KUNIT_EXPECT_EQ(test, -EAGAIN,
		teelog_reader_seek_current(&pos, &snap, ring, TEELOG_DEFAULT_SIZE));
	KUNIT_EXPECT_EQ(test, 0, memcmp(&pos, &before, sizeof(pos)));
	teelog_seq_store(&ring->flag, snap.seq);
	KUNIT_EXPECT_EQ(test, 0,
		teelog_reader_seek_current(&pos, &snap, ring, TEELOG_DEFAULT_SIZE));
	KUNIT_EXPECT_EQ(test, snap.last_pos, pos.r_off);
	KUNIT_EXPECT_EQ(test, snap.write_loops, pos.r_loops);
	KUNIT_EXPECT_EQ(test, snap.seq, pos.r_seq);
	KUNIT_EXPECT_EQ(test, snap.seq, pos.r_obs_seq);
	KUNIT_EXPECT_EQ(test, snap.last_pos, pos.r_obs_last);
	KUNIT_EXPECT_TRUE(test, pos.r_obs_valid);
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);
	KUNIT_EXPECT_EQ(test, 1001U, pos.r_serial);
}

static void test_serial_accept_baseline(struct kunit *test)
{
	struct teelog_reader_pos pos = { .r_serial_valid = false };

	/* accept does not mutate; commit publishes the baseline. */
	KUNIT_EXPECT_TRUE(test, teelog_serial_accept(&pos, 10));
	KUNIT_EXPECT_FALSE(test, pos.r_serial_valid);
	teelog_serial_commit(&pos, 10);
	KUNIT_EXPECT_TRUE(test, pos.r_serial_valid);
	KUNIT_EXPECT_EQ(test, 10U, pos.r_serial);
	KUNIT_EXPECT_FALSE(test, teelog_serial_accept(&pos, 10));
	KUNIT_EXPECT_TRUE(test, teelog_serial_accept(&pos, 11));
	teelog_serial_commit(&pos, 20);
	KUNIT_EXPECT_FALSE(test, teelog_serial_accept(&pos, 15));
	KUNIT_EXPECT_TRUE(test, teelog_serial_accept(&pos, 21));
}

static void test_next_record_reports_found_off(struct kunit *test)
{
	/* Minimal synthetic ring: flag + body with a gap then one item. */
	u8 raw[TEELOG_BUFFER_FLAG_SIZE + 512];
	struct teelog_buffer *ring = (struct teelog_buffer *)raw;
	struct teelog_item *item;
	u8 stage[TEELOG_ITEM_MAX_LEN];
	u32 found = 0, stage_len = 0;
	u32 body = 64;
	int rc;

	memset(raw, 0, sizeof(raw));
	teelog_ctrl_store_u32(&ring->flag.max_len, 512);
	teelog_ctrl_store_u32(&ring->flag.last_pos, 384);
	teelog_ctrl_store_u32(&ring->flag.reserved0, TEELOG_RING_MAGIC);
	teelog_ctrl_store_u32(&ring->flag.reserved[0], TEELOG_RING_PROTO_VERSION);
	teelog_seq_store(&ring->flag, 2);

	/* Invalid gap at 0..255; valid 128 B record at 256. */
	item = (struct teelog_item *)(ring->buffer_start + 256);
	item->magic = TEELOG_ITEM_MAGIC;
	item->buffer_len = body;
	item->real_len = (s16)body;
	item->serial_no = 7;

	rc = teelog_next_record_stable(ring, sizeof(raw), 0, 384, stage,
				       &found, &stage_len, 2);
	KUNIT_EXPECT_EQ(test, 0, rc);
	KUNIT_EXPECT_EQ(test, 256U, found);
	KUNIT_EXPECT_EQ(test, TEELOG_ITEM_HDR_SIZE + body, stage_len);

	/* Residual past last_pos must stay outside the window. */
	item = (struct teelog_item *)(ring->buffer_start + 384);
	item->magic = TEELOG_ITEM_MAGIC;
	item->buffer_len = body;
	item->real_len = (s16)body;
	item->serial_no = 8;
	rc = teelog_next_record_stable(ring, sizeof(raw), 384, 384, stage,
				       &found, &stage_len, 2);
	KUNIT_EXPECT_EQ(test, -ENOENT, rc);
	rc = teelog_next_record_stable(ring, sizeof(raw), 0, 384, stage,
				       &found, &stage_len, 2);
	KUNIT_EXPECT_EQ(test, 0, rc);
	KUNIT_EXPECT_EQ(test, 256U, found);
}

static struct kunit_case teelog_lifecycle_cases[] = {
	KUNIT_CASE(test_classify_ok),
	KUNIT_CASE(test_classify_linux_error_unknown),
	KUNIT_CASE(test_classify_communication_unknown),
	KUNIT_CASE(test_classify_explicit_reject),
	KUNIT_CASE(test_classify_busy_unknown),
	KUNIT_CASE(test_recover_owner_gate),
	KUNIT_CASE(test_recover_inactive_releases),
	KUNIT_CASE(test_recover_active_match_unregisters),
	KUNIT_CASE(test_recover_active_mismatch_aborts),
	KUNIT_CASE(test_recover_query_fail_aborts),
	KUNIT_CASE(test_reg_fields_match),
	KUNIT_CASE(test_pages_after_register_fate),
	KUNIT_CASE(test_pages_after_unregister_fate),
	KUNIT_CASE(test_inject_reject_is_pre_call),
	KUNIT_CASE(test_inject_corrupt_reg_reply_breaks_match),
	KUNIT_CASE(test_inject_post_only_unknown),
	KUNIT_CASE(test_reject_path_may_release_unknown_must_not),
	KUNIT_CASE(test_reader_catchup_unsaturated_lap),
	KUNIT_CASE(test_reader_catchup_saturated_wrap),
	KUNIT_CASE(test_reader_catchup_saturated_same_off),
	KUNIT_CASE(test_reader_catchup_seq_threshold),
	KUNIT_CASE(test_reader_serial_seq_floor),
	KUNIT_CASE(test_reader_catchup_saturated_obs_regression),
	KUNIT_CASE(test_reader_catchup_saturated_no_wrap_short),
	KUNIT_CASE(test_reader_catchup_saturated_full_lap),
	KUNIT_CASE(test_reader_catchup_saturated_span),
	KUNIT_CASE(test_reader_seek_current),
	KUNIT_CASE(test_serial_accept_baseline),
	KUNIT_CASE(test_next_record_reports_found_off),
	{}
};

static struct kunit_suite teelog_lifecycle_suite = {
	.name = "teelog_lifecycle",
	.test_cases = teelog_lifecycle_cases,
};

kunit_test_suite(teelog_lifecycle_suite);

MODULE_DESCRIPTION("KUnit tests for teelog lifecycle helpers");
MODULE_LICENSE("GPL");
