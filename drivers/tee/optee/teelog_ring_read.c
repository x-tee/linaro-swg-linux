// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared-ring reader helpers (U6). Seq barriers match TEE dsb_ish / design §3.2.
 */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "teelog_ring_read.h"

bool teelog_item_hdr_valid(const struct teelog_item *item, u32 max_item_size)
{
	u32 total;

	if (!item || item->magic != TEELOG_ITEM_MAGIC)
		return false;
	if (item->buffer_len == 0 || item->real_len <= 0)
		return false;
	if (item->buffer_len % TEELOG_ITEM_LEN_ALIGN)
		return false;
	if ((u32)item->real_len > item->buffer_len)
		return false;
	if ((item->buffer_len - (u32)item->real_len) >= TEELOG_ITEM_LEN_ALIGN)
		return false;
	total = TEELOG_ITEM_HDR_SIZE + item->buffer_len;
	if (total > TEELOG_ITEM_MAX_LEN || total > max_item_size)
		return false;
	return true;
}
EXPORT_SYMBOL_GPL(teelog_item_hdr_valid);

int teelog_snap_flag(const struct teelog_buffer *ring, u32 capacity,
		     struct teelog_hdr_snap *out)
{
	const struct teelog_buffer_flag *f;
	u32 body_cap;
	u64 seq1, seq2;
	u32 last_pos, write_loops, max_len, dropped, magic, proto;

	if (!ring || !out || capacity < TEELOG_MIN_SIZE ||
	    capacity > TEELOG_MAX_SIZE)
		return -EIO;

	body_cap = teelog_ring_capacity_max_len(capacity);
	f = &ring->flag;

	seq1 = teelog_seq_load(f);
	if (seq1 & 1ULL)
		return -EAGAIN;
	smp_rmb();

	magic = teelog_ctrl_load_u32(&f->reserved0);
	proto = teelog_ctrl_load_u32(&f->reserved[0]);
	last_pos = teelog_ctrl_load_u32(&f->last_pos);
	write_loops = teelog_ctrl_load_u32(&f->write_loops);
	max_len = teelog_ctrl_load_u32(&f->max_len);
	dropped = teelog_ctrl_load_u32(&f->reserved[TEELOG_DROPPED_INDEX]);

	smp_rmb();
	seq2 = teelog_seq_load(f);
	if (seq2 != seq1 || (seq2 & 1ULL))
		return -EAGAIN;

	if (magic != TEELOG_RING_MAGIC || proto != TEELOG_RING_PROTO_VERSION)
		return -EIO;
	if (max_len == 0 || max_len > body_cap)
		return -EIO;
	if (last_pos > max_len)
		return -EIO;

	out->seq = seq2;
	out->last_pos = last_pos;
	out->write_loops = write_loops;
	out->max_len = max_len;
	out->dropped = dropped;
	out->magic = magic;
	out->proto = proto;
	return 0;
}
EXPORT_SYMBOL_GPL(teelog_snap_flag);

static void teelog_reader_init_current(struct teelog_reader_pos *pos,
				      const struct teelog_hdr_snap *snap)
{
	if (!pos || !snap)
		return;

	*pos = (struct teelog_reader_pos) {
		.r_off = snap->last_pos,
		.r_loops = snap->write_loops,
		.r_seq = snap->seq,
		.r_obs_seq = snap->seq,
		.r_obs_last = snap->last_pos,
		.r_obs_valid = true,
	};
}

void teelog_reader_init_window(struct teelog_reader_pos *pos,
			       const struct teelog_hdr_snap *snap)
{
	if (!pos || !snap)
		return;

	teelog_reader_init_current(pos, snap);
	if (snap->write_loops)
		pos->r_loops--;
	else
		pos->r_off = 0;
}
EXPORT_SYMBOL_GPL(teelog_reader_init_window);

static void teelog_reader_jump_retain(struct teelog_reader_pos *pos,
				      const struct teelog_hdr_snap *snap,
				      bool rebase)
{
	u32 serial = pos->r_serial;
	u64 seq = pos->r_seq;
	bool valid = pos->r_serial_valid;

	teelog_reader_init_window(pos, snap);
	if (!rebase && valid) {
		/* A wrap need not have destroyed records already read in the tail. */
		pos->r_serial = serial;
		pos->r_serial_valid = true;
		pos->r_seq = seq;
	}
}

/*
 * Follow real record boundaries from zero through the current prefix. Starting
 * at an old observed offset is unsafe after wrap: it may now be inside payload
 * containing valid-looking headers. One ring of >=128-byte records bounds work.
 */
static int teelog_prefix_snapshot(const struct teelog_buffer *ring, u32 capacity,
				  const struct teelog_hdr_snap *snap, u32 observed,
				  u32 *suffix, u32 *last_serial, bool *boundary)
{
	struct teelog_item item;
	u32 off = 0, count = 0, serial = 0;
	bool found = false;
	int rc = 0;

	if (!ring || capacity < TEELOG_MIN_SIZE || capacity > TEELOG_MAX_SIZE ||
	    snap->max_len > teelog_ring_capacity_max_len(capacity) ||
	    snap->last_pos > snap->max_len)
		return -EIO;
	if ((snap->seq & 1ULL) || teelog_seq_load(&ring->flag) != snap->seq)
		return -EAGAIN;
	smp_rmb();
	while (off < snap->last_pos) {
		if (snap->last_pos - off < TEELOG_ITEM_HDR_SIZE) {
			rc = -EIO;
			break;
		}
		memcpy(&item, ring->buffer_start + off, sizeof(item));
		if (!teelog_item_hdr_valid(&item, snap->last_pos - off)) {
			rc = -EIO;
			break;
		}
		if (off == observed)
			found = true;
		if (found)
			count++;
		serial = item.serial_no;
		off += teelog_item_total_len(&item);
	}
	smp_rmb();
	if (teelog_seq_load(&ring->flag) != snap->seq)
		return -EAGAIN;
	if (rc)
		return rc;
	*suffix = count;
	*last_serial = serial;
	*boundary = found || off == observed;
	return 0;
}

int teelog_reader_seek_current(struct teelog_reader_pos *pos,
			       const struct teelog_hdr_snap *snap,
			       const struct teelog_buffer *ring, u32 capacity)
{
	struct teelog_reader_pos next;
	u32 count, serial;
	bool boundary;
	int rc;

	if (!pos || !snap)
		return -EIO;
	rc = teelog_prefix_snapshot(ring, capacity, snap, 0,
				    &count, &serial, &boundary);
	if (rc)
		return rc;
	teelog_reader_init_current(&next, snap);
	if (snap->last_pos) {
		/* Skip pre-seek records even if a short wrap leaves them retained. */
		next.r_serial = serial;
		next.r_serial_valid = true;
	}
	*pos = next;
	return 0;
}
EXPORT_SYMBOL_GPL(teelog_reader_seek_current);

/* A rising tip is no-wrap only if its new suffix contains every new commit. */
static int teelog_observed_span_wrapped(const struct teelog_reader_pos *pos,
				      const struct teelog_hdr_snap *snap,
				      const struct teelog_buffer *ring,
				      u32 capacity)
{
	u64 commits = (snap->seq - pos->r_obs_seq) / 2;
	u32 count, serial;
	bool boundary;
	int rc;

	if (commits > (snap->last_pos - pos->r_obs_last) /
			(TEELOG_ITEM_HDR_SIZE + TEELOG_ITEM_LEN_ALIGN))
		return 1;
	if (!commits)
		return pos->r_obs_last != snap->last_pos;
	rc = teelog_prefix_snapshot(ring, capacity, snap, pos->r_obs_last,
				    &count, &serial, &boundary);
	if (rc)
		return rc;
	return !boundary || count != commits;
}

int teelog_reader_catchup(struct teelog_reader_pos *pos,
			  const struct teelog_hdr_snap *snap,
			  const struct teelog_buffer *ring, u32 capacity)
{
	u64 seq_delta;
	u32 loop_delta;
	int lapped = 0;

	if (!pos || !snap || snap->last_pos > snap->max_len)
		return -EIO;

	/* seq is even and advances by 2 per commit -> 2^31 commits = 2^32. */
	seq_delta = snap->seq - pos->r_seq;
	if (seq_delta >= (1ULL << 32) || pos->r_off > snap->max_len) {
		teelog_reader_jump_retain(pos, snap, true);
		return 1;
	}

	if (pos->r_loops != snap->write_loops) {
		loop_delta = snap->write_loops - pos->r_loops;
		if (loop_delta > 1U ||
		    (loop_delta == 1U && pos->r_off < snap->last_pos))
			lapped = 1;
	}
	if (!lapped && snap->write_loops == U32_MAX) {
		/* Also check readers still consuming the retained tail (MAX-1). */
		if (!pos->r_obs_valid || pos->r_obs_last > snap->max_len) {
			lapped = 1;
		} else if (snap->seq != pos->r_obs_seq) {
			if (snap->last_pos <= pos->r_obs_last ||
			    (pos->r_loops == U32_MAX &&
			     snap->last_pos <= pos->r_off))
				lapped = 1;
			else
				lapped = teelog_observed_span_wrapped(pos, snap,
							     ring, capacity);
		}
	}
	if (lapped < 0)
		return lapped;
	if (lapped) {
		teelog_reader_jump_retain(pos, snap, false);
		return 1;
	}

	pos->r_obs_last = snap->last_pos;
	pos->r_obs_seq = snap->seq;
	pos->r_obs_valid = true;
	return 0;
}
EXPORT_SYMBOL_GPL(teelog_reader_catchup);

bool teelog_reader_has_data(const struct teelog_reader_pos *pos,
			    const struct teelog_hdr_snap *snap)
{
	if (!pos || !snap)
		return false;

	if (pos->r_off != snap->last_pos || pos->r_loops != snap->write_loops)
		return true;

	/*
	 * Same cursor as producer: under saturation a full-ring overwrite
	 * returns to the same offset with a newer seq — still unread work
	 * after catchup repositions to the retain window.
	 */
	if (snap->write_loops == U32_MAX && pos->r_loops == U32_MAX &&
	    snap->seq != (pos->r_obs_valid ? pos->r_obs_seq : pos->r_seq))
		return true;

	return false;
}
EXPORT_SYMBOL_GPL(teelog_reader_has_data);

int teelog_next_record_stable(const struct teelog_buffer *ring, u32 capacity,
			      u32 off, u32 end, u8 *stage, u32 *found_off,
			      u32 *stage_len, u64 snap_seq)
{
	const u8 *base;
	u32 body_cap, max_len, i;
	u64 seq;

	if (!ring || !stage || !found_off || !stage_len)
		return -EIO;
	if (off % TEELOG_ITEM_LEN_ALIGN)
		return -EIO;

	body_cap = teelog_ring_capacity_max_len(capacity);
	max_len = teelog_ctrl_load_u32(&ring->flag.max_len);
	if (max_len == 0 || max_len > body_cap)
		return -EIO;
	if (end > max_len || off > end)
		return -EIO;
	if (off == end)
		return -ENOENT;

	base = ring->buffer_start;

	seq = teelog_seq_load(&ring->flag);
	if (seq != snap_seq || (seq & 1ULL))
		return -EAGAIN;
	smp_rmb();

	for (i = 0; off + i + TEELOG_ITEM_HDR_SIZE + TEELOG_ITEM_LEN_ALIGN <=
		    end; i += TEELOG_ITEM_LEN_ALIGN) {
		const struct teelog_item *src;
		u32 at = off + i;
		u32 room = end - at;
		u32 max_item = min_t(u32, room, TEELOG_ITEM_MAX_LEN);
		struct teelog_item *dst = (struct teelog_item *)stage;
		u32 total;

		if (max_item < TEELOG_ITEM_HDR_SIZE + TEELOG_ITEM_LEN_ALIGN)
			break;

		src = (const struct teelog_item *)(base + at);
		memcpy(stage, src, TEELOG_ITEM_HDR_SIZE);
		smp_rmb();
		seq = teelog_seq_load(&ring->flag);
		if (seq != snap_seq || (seq & 1ULL))
			return -EAGAIN;

		if (!teelog_item_hdr_valid(dst, max_item))
			continue;

		total = teelog_item_total_len(dst);
		if (at + total > end)
			continue;

		memcpy(stage + TEELOG_ITEM_HDR_SIZE, src->log_buffer,
		       dst->buffer_len);
		smp_rmb();
		seq = teelog_seq_load(&ring->flag);
		if (seq != snap_seq || (seq & 1ULL))
			return -EAGAIN;

		if (!teelog_item_hdr_valid(dst, max_item))
			return -EAGAIN;

		*found_off = at;
		*stage_len = total;
		return 0;
	}

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(teelog_next_record_stable);

bool teelog_serial_accept(const struct teelog_reader_pos *pos, u32 serial)
{
	if (!pos)
		return false;
	if (!pos->r_serial_valid)
		return true;
	return (s32)(serial - pos->r_serial) > 0;
}
EXPORT_SYMBOL_GPL(teelog_serial_accept);

void teelog_serial_commit(struct teelog_reader_pos *pos, u32 serial)
{
	if (!pos)
		return;
	pos->r_serial = serial;
	pos->r_serial_valid = true;
}
EXPORT_SYMBOL_GPL(teelog_serial_commit);
