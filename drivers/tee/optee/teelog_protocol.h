/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026
 *
 * Shared teelog ring layout, ioctl ABI, and PTA constants for the OP-TEE
 * Linux driver. Layout must match optee_os core/include/kernel/teelog.h and
 * optee_client/teelogcat/include/teelog_protocol.h. PTA UUID/commands must
 * match libutee/include/pta_teelog.h.
 */
#ifndef __OPTEE_TEELOG_PROTOCOL_H
#define __OPTEE_TEELOG_PROTOCOL_H

#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/ioctl.h>
#include <linux/types.h>

/* ---- PTA constants (mirror of pta_teelog.h) ---- */

#define PTA_TEELOG_UUID \
	{ 0xb5a1c0de, 0x7e31, 0x4a9c, \
		{ 0x9f, 0x2e, 0x41, 0x58, 0x6d, 0x73, 0xa0, 0x11 } }

#define PTA_TEELOG_PROTOCOL_VERSION		1U

#define PTA_TEELOG_CMD_REGISTER			0U
#define PTA_TEELOG_CMD_UNREGISTER		1U
#define PTA_TEELOG_CMD_GET_VERSION		2U
#define PTA_TEELOG_CMD_GET_REGISTRATION		3U

#define PTA_TEELOG_REG_F_NONE			0U
#define PTA_TEELOG_CAPS_ASYNC_NOTIF		(1U << 0)
#define PTA_TEELOG_NOTIF_ID_NONE		0xffffffffU
#define PTA_TEELOG_GENERATION_INVALID		0U
#define PTA_TEELOG_GENERATION_FIRST		1U

/* ---- Shared ring / record layout ---- */

#define TEELOG_ITEM_MAGIC			0x5a5aU
#define TEELOG_ITEM_LEN_ALIGN			64U
#define TEELOG_ITEM_MAX_LEN			1024U
#define TEELOG_ITEM_MAX_BODY			960U
#define TEELOG_NEVER_USED_LEN			32U
#define TEELOG_UUID_LEN				16U
#define TEELOG_ITEM_RESERVED_LEN		1U
#define TEELOG_ITEM_HDR_SIZE			64U

#define TEELOG_BUFFER_RESERVED_LEN		11U
#define TEELOG_VERSION_INFO_LEN			156U
#define TEELOG_BUFFER_FLAG_SIZE			220U

#define TEELOG_DEFAULT_SIZE			(256U * 1024U)
#define TEELOG_MIN_SIZE				TEELOG_DEFAULT_SIZE
#define TEELOG_MAX_SIZE				(1024U * 1024U)
#define TEELOG_DEFAULT_MAX_LEN \
	(TEELOG_DEFAULT_SIZE - TEELOG_BUFFER_FLAG_SIZE) /* 261924 */

/*
 * reserved0 = TEELOG_RING_MAGIC; reserved[0] = TEELOG_RING_PROTO_VERSION;
 * reserved[1] = 0 (LOG_POOL would use reserved[0]/[1]; not enabled).
 * First ring access: magic/proto mismatch → -EIO / -ENODEV, no record scan.
 *
 * TEELOG_RING_MAGIC spells 'TLG1' as a number; little-endian memory bytes are
 * 31 47 4c 54, so a hexdump shows "1GLT". Only the numeric value is compared.
 */
#define TEELOG_RING_MAGIC			0x544c4731U
#define TEELOG_RING_PROTO_VERSION		1U
#define TEELOG_SEQ_OFFSET			24U
#define TEELOG_SEQ_SIZE				8U
#define TEELOG_DROPPED_INDEX			4U
#define TEELOG_DROPPED_OFFSET			32U
/* Producer losses: boot-cumulative, saturate at UINT32_MAX, published under seq. */

/*
 * real_len is s16 (OpenTrustee TEE writer). Reference Linux tlogger.c used
 * unsigned short — intentional correction; do not revert to u16.
 */
struct teelog_item {
	u8 never_used[TEELOG_NEVER_USED_LEN];
	u16 magic;
	u16 reserved0;
	u32 serial_no;
	s16 real_len;
	u16 buffer_len;
	u8 uuid[TEELOG_UUID_LEN];
	u8 log_source_type;
	u8 reserved[TEELOG_ITEM_RESERVED_LEN];
	u8 log_level;
	u8 new_line;
	u8 log_buffer[];
};

struct teelog_buffer_flag {
	u32 reserved0;
	u32 last_pos;
	u32 write_loops;
	u32 log_level;
	u32 reserved[TEELOG_BUFFER_RESERVED_LEN];
	u32 max_len;
	u8 version_info[TEELOG_VERSION_INFO_LEN];
};

struct teelog_buffer {
	struct teelog_buffer_flag flag;
	u8 buffer_start[];
};

static_assert(sizeof(struct teelog_item) == TEELOG_ITEM_HDR_SIZE);
static_assert(sizeof(struct teelog_buffer_flag) == TEELOG_BUFFER_FLAG_SIZE);
static_assert(offsetof(struct teelog_buffer_flag, reserved[2]) ==
	      TEELOG_SEQ_OFFSET);
static_assert(TEELOG_SEQ_OFFSET % 8 == 0);
static_assert(TEELOG_DEFAULT_MAX_LEN == 261924U);

static_assert(offsetof(struct teelog_buffer_flag, reserved[TEELOG_DROPPED_INDEX]) ==
	      TEELOG_DROPPED_OFFSET);

typedef u64 __attribute__((__may_alias__)) teelog_seq_word_t;

/*
 * Seq / control accessors. Barriers match TEE dsb_ishst / dsb_ish:
 *   writer: store odd → smp_wmb → payload/meta → smp_wmb → store even
 *   reader: load even → smp_rmb → copy → smp_rmb → reload
 */
static inline u64 teelog_seq_load(const struct teelog_buffer_flag *f)
{
	const teelog_seq_word_t *p =
		(const teelog_seq_word_t *)(const void *)&f->reserved[2];

	return READ_ONCE(*p);
}

static inline void teelog_seq_store(struct teelog_buffer_flag *f, u64 seq)
{
	teelog_seq_word_t *p =
		(teelog_seq_word_t *)(void *)&f->reserved[2];

	WRITE_ONCE(*p, seq);
}

static inline u32 teelog_ctrl_load_u32(const u32 *p)
{
	return READ_ONCE(*p);
}

static inline void teelog_ctrl_store_u32(u32 *p, u32 v)
{
	WRITE_ONCE(*p, v);
}

/* ---- /dev/teelog ioctl (OpenTrustee-compatible numbers) ---- */

#define TEELOGGER_IOC_MAGIC			0xBE

#define TEELOGGER_GET_VERSION_BASE		5
#define TEELOGGER_SET_READERPOS_CUR_BASE	6
#define TEELOGGER_SET_TLOGCAT_STAT_BASE		7
#define TEELOGGER_GET_TLOGCAT_STAT_BASE		8
#define TEELOGGER_GET_CAPS_BASE			11
#define TEELOGGER_GET_STATS_BASE		12

#define TEELOGGER_MAX_VERSION_LEN		256

/*
 * Legacy GET_VERSION: returns up to 156 bytes of version_info text into a
 * 256-byte user buffer (NUL-terminated). Encoded value 0x8100be05 on
 * asm-generic ioctl (READ, type 0xBE, nr 5, size 256).
 * No active registration / device not ready: empty NUL string.
 */
#define TEELOGGER_GET_VERSION \
	_IOR(TEELOGGER_IOC_MAGIC, TEELOGGER_GET_VERSION_BASE, \
	     char[TEELOGGER_MAX_VERSION_LEN])

#define TEELOGGER_SET_READERPOS_CUR \
	_IO(TEELOGGER_IOC_MAGIC, TEELOGGER_SET_READERPOS_CUR_BASE)

/*
 * Atomic single-instance claim for tlogcat -f.
 * Success: this fd owns the claim.
 * -EBUSY: another fd already holds the claim (explicit repair vs reference
 * OpenTrustee non-atomic GET-then-SET).
 * Close of the owning fd releases the claim.
 */
#define TEELOGGER_SET_TLOGCAT_STAT \
	_IO(TEELOGGER_IOC_MAGIC, TEELOGGER_SET_TLOGCAT_STAT_BASE)

#define TEELOGGER_GET_TLOGCAT_STAT \
	_IO(TEELOGGER_IOC_MAGIC, TEELOGGER_GET_TLOGCAT_STAT_BASE)

struct teelog_caps {
	__u32 protocol_version;	/* PTA_TEELOG_PROTOCOL_VERSION */
	__u32 ring_size;	/* allocated bytes, 0 if not ready */
	__u32 capabilities;	/* TEELOG_USER_CAP_* */
	__u32 reserved;
};

#define TEELOG_USER_CAP_ASYNC_NOTIF		(1U << 0)
#define TEELOG_USER_CAP_SEQ64			(1U << 1)

#define TEELOGGER_GET_CAPS \
	_IOR(TEELOGGER_IOC_MAGIC, TEELOGGER_GET_CAPS_BASE, struct teelog_caps)

/*
 * Global producer snapshot (not per-fd reader state).
 * dropped comes from flag.reserved[TEELOG_DROPPED_INDEX] in the same seq
 * snapshot. TEE keeps the authoritative boot-cumulative counter in secure
 * memory, carries it across registration changes, and saturates at U32_MAX.
 *   seq / last_pos / write_loops — shared ring producer header
 *   dropped — losses the producer can observe by itself: records discarded by
 *             migration truncation and by early-ring overflow before
 *             registration. Overwrites that only the reader can notice (its
 *             cursor was lapped) are NOT counted here; the U8 harness derives
 *             those from write_loops / serial gaps.
 *   reserved — must be 0. Do not append fields: the ioctl encoding includes
 *              sizeof(struct teelog_stats), so growing it changes
 *              TEELOGGER_GET_STATS (0x8018be0c). Add a new ioctl number.
 */
struct teelog_stats {
	__u64 seq;
	__u32 last_pos;
	__u32 write_loops;
	__u32 dropped;
	__u32 reserved;
};

#define TEELOGGER_GET_STATS \
	_IOR(TEELOGGER_IOC_MAGIC, TEELOGGER_GET_STATS_BASE, struct teelog_stats)

/*
 * compat_ioctl: same command numbers and structure layouts as unlocked_ioctl
 * (no pointer-containing args). Handlers are shared.
 */

/*
 * Resident ledger API (implemented in tee_core.c, U2). Signatures frozen here
 * for cross-unit reference; declarations live in linux/tee_drv.h.
 *
 *   int teelog_ledger_begin(void);
 *       Returns 0 on success. Nested begin is forbidden: return -EBUSY.
 *   void teelog_ledger_end(void);
 *       Releases the lifecycle lock taken by a successful begin().
 *
 *   int teelog_ledger_record(u64 pa, u32 size, u32 order, u64 owner_key,
 *			      u32 *generation);
 *       Under ledger mutex: allocate next monotonic generation, then register
 *       the slot with live=false and the given non-zero owner_key. On success
 *       writes *generation. Non-empty slot → -EBUSY; generation exhausted →
 *       -ENOSPC. Caller must hold begin(). Generation lives in tee_core
 *       resident state (not optee.ko), so reload does not reset the counter.
 *
 *   int teelog_ledger_claim_live(u32 generation, u64 owner_key);
 *   int teelog_ledger_drop_live(u32 generation, u64 owner_key);
 *       Toggle the live mapper flag; recovery must not free while live.
 *
 *   int teelog_ledger_mark_isolated(u32 generation);
 *       Under ledger mutex: set isolated on matching slot. Preserves
 *       pa/size/order/generation. Lost-contact path only.
 *
 *   int teelog_ledger_snapshot(struct teelog_ledger_entry *entry);
 *       Discovers the resident slot without a generation; caller holds begin().
 *       Empty snapshot is all zero. Nonempty includes generation, owner_key,
 *       live, and isolation.
 *
 *   int teelog_ledger_lookup(u32 generation, u64 *pa, u32 *size, u32 *order,
 *			      bool *isolated);
 *   int teelog_ledger_release_once(u32 generation);
 *
 * Lock order (outer → inner):
 *   teelog_ledger_begin/end (lifecycle mutex)
 *     → ledger mutex (taken inside release_once / record / lookup / mark_isolated)
 * Notif callback removal uses RCU (rcu_read_lock on dispatch,
 * synchronize_rcu after pointer clear). Trace/IRQ/worker must not take the
 * lifecycle mutex.
 *
 * No teelog_ledger_clear release API. No separate new_generation() — allocate
 * and record are one atomic step inside record().
 */

#endif /* __OPTEE_TEELOG_PROTOCOL_H */
