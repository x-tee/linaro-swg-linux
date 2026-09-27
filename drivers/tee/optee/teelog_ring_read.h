/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Pure shared-ring reader helpers for /dev/teelog (U6).
 * No sleep, no TEE calls — safe for KUnit with a synthetic buffer.
 */
#ifndef OPTEE_TEELOG_RING_READ_H
#define OPTEE_TEELOG_RING_READ_H

#include <linux/types.h>

#include "teelog_protocol.h"

/* Design §3.2: per-record seq copy retries, then EAGAIN. */
#define TEELOG_SEQ_RETRY_MAX			3

struct teelog_hdr_snap {
	u64 seq;
	u32 last_pos;
	u32 write_loops;
	u32 max_len;
	u32 dropped;
	u32 magic;
	u32 proto;
};

struct teelog_reader_pos {
	u32 r_off;
	u32 r_loops;
	u64 r_seq;
	u32 r_serial;
	bool r_serial_valid;
};

/**
 * teelog_ring_capacity_max_len() - Driver-held body capacity from alloc size
 * @capacity: Total allocated bytes including the 220 B flag
 */
static inline u32 teelog_ring_capacity_max_len(u32 capacity)
{
	if (capacity <= TEELOG_BUFFER_FLAG_SIZE)
		return 0;
	return capacity - TEELOG_BUFFER_FLAG_SIZE;
}

bool teelog_item_hdr_valid(const struct teelog_item *item, u32 max_item_size);

static inline u32 teelog_item_total_len(const struct teelog_item *item)
{
	return TEELOG_ITEM_HDR_SIZE + item->buffer_len;
}

/**
 * teelog_snap_flag() - Take a stable even-seq snapshot of the producer header
 *
 * Returns 0, -EAGAIN (odd/unstable seq), or -EIO (magic/proto/capacity).
 */
int teelog_snap_flag(const struct teelog_buffer *ring, u32 capacity,
		     struct teelog_hdr_snap *out);

void teelog_reader_init_window(struct teelog_reader_pos *pos,
			       const struct teelog_hdr_snap *snap);

/**
 * teelog_reader_catchup() - Reposition if the reader was lapped (CR-004)
 *
 * Returns true if the cursor jumped (serial must be rebuilt).
 */
bool teelog_reader_catchup(struct teelog_reader_pos *pos,
			   const struct teelog_hdr_snap *snap);

bool teelog_reader_has_data(const struct teelog_reader_pos *pos,
			    const struct teelog_hdr_snap *snap);

/**
 * teelog_unread_end() - Exclusive end offset of the current unread segment
 *
 * Same-loop: [r_off, last_pos). Wrapped segment: [r_off, max_len).
 */
static inline u32 teelog_unread_end(const struct teelog_reader_pos *pos,
				    const struct teelog_hdr_snap *snap)
{
	if (pos->r_loops == snap->write_loops)
		return snap->last_pos;
	return snap->max_len;
}

/**
 * teelog_next_record_stable() - Scan [off, end) once; copy first valid record
 * @off: Scan start (64-aligned), relative to buffer_start
 * @end: Exclusive window end (must be <= max_len from snap / capacity)
 * @found_off: Out: offset of the record that was copied
 * @stage_len: Out: total record bytes
 * @snap_seq: Expected even seq for the entire copy
 *
 * Single scan owner: steps by 64 B inside [off, end) only. Does not look past
 * @end (so last_pos residual is never returned as a live record).
 *
 * Returns 0, -EAGAIN (seq changed/odd), -ENOENT (no valid item in window),
 * or -EIO (bad args / capacity).
 */
int teelog_next_record_stable(const struct teelog_buffer *ring, u32 capacity,
			      u32 off, u32 end, u8 *stage, u32 *found_off,
			      u32 *stage_len, u64 snap_seq);

/**
 * teelog_serial_accept() - Whether @serial may be delivered (no mutation)
 *
 * Invalid baseline: any valid record is acceptable (baseline set on commit).
 * Valid baseline: accept only when (int32_t)(serial - r_serial) > 0.
 */
bool teelog_serial_accept(const struct teelog_reader_pos *pos, u32 serial);

/** teelog_serial_commit() - Publish serial after a successful copy_to_user */
void teelog_serial_commit(struct teelog_reader_pos *pos, u32 serial);

#endif /* OPTEE_TEELOG_RING_READ_H */
