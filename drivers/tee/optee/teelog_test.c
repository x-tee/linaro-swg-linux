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
#include <linux/types.h>

#include "teelog_internal.h"

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

static struct kunit_case teelog_lifecycle_cases[] = {
	KUNIT_CASE(test_classify_ok),
	KUNIT_CASE(test_classify_linux_error_unknown),
	KUNIT_CASE(test_classify_communication_unknown),
	KUNIT_CASE(test_classify_explicit_reject),
	KUNIT_CASE(test_classify_busy_unknown),
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
	{}
};

static struct kunit_suite teelog_lifecycle_suite = {
	.name = "teelog_lifecycle",
	.test_cases = teelog_lifecycle_cases,
};

kunit_test_suite(teelog_lifecycle_suite);

MODULE_DESCRIPTION("KUnit tests for teelog lifecycle helpers");
MODULE_LICENSE("GPL");
