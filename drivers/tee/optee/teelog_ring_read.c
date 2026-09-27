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

void teelog_reader_init_window(struct teelog_reader_pos *pos,
			       const struct teelog_hdr_snap *snap)
{
	if (!pos || !snap)
		return;

	if (snap->write_loops == 0) {
		pos->r_off = 0;
		pos->r_loops = 0;
	} else {
		/* Oldest retained byte starts at last_pos after a wrap. */
		pos->r_off = snap->last_pos;
		pos->r_loops = snap->write_loops - 1U;
	}
	pos->r_seq = snap->seq;
	pos->r_serial = 0;
	pos->r_serial_valid = false;
}
EXPORT_SYMBOL_GPL(teelog_reader_init_window);

static void teelog_reader_jump_retain(struct teelog_reader_pos *pos,
				      const struct teelog_hdr_snap *snap)
{
	teelog_reader_init_window(pos, snap);
}

/*
 * Approx seq units for one full ring of minimum-size records (hdr+64 body).
 * seq advances by 2 per commit; used only under write_loops saturation.
 */
static u64 teelog_seq_per_ring(u32 max_len)
{
	u32 min_rec = TEELOG_ITEM_HDR_SIZE + TEELOG_ITEM_LEN_ALIGN;
	u32 max_recs;

	if (!max_len || max_len < min_rec)
		return 2;
	max_recs = max_len / min_rec;
	return (u64)max_recs * 2ULL;
}

bool teelog_reader_catchup(struct teelog_reader_pos *pos,
			   const struct teelog_hdr_snap *snap)
{
	u64 seq_delta;
	u32 loop_delta;
	bool lapped = false;

	if (!pos || !snap)
		return false;

	/* seq is even and advances by 2 per commit → 2^31 commits = 2^32. */
	seq_delta = snap->seq - pos->r_seq;
	if (seq_delta >= (1ULL << 32)) {
		teelog_reader_jump_retain(pos, snap);
		return true;
	}

	if (pos->r_off > snap->max_len) {
		teelog_reader_jump_retain(pos, snap);
		return true;
	}

	if (snap->write_loops == U32_MAX && pos->r_loops == U32_MAX) {
		/*
		 * CR-004: loops stuck at saturation. Detect lapping via
		 * position relation and seq progress — not only last_pos <
		 * r_off (a full ring back to the same offset keeps last_pos
		 * == r_off while seq advances).
		 */
		if (snap->seq != pos->r_seq) {
			if (snap->last_pos < pos->r_off)
				lapped = true;
			else if (snap->last_pos == pos->r_off)
				lapped = true;
			else if (seq_delta >= teelog_seq_per_ring(snap->max_len))
				lapped = true;
		}
	} else if (pos->r_loops != snap->write_loops) {
		loop_delta = snap->write_loops - pos->r_loops;
		if (loop_delta > 1U ||
		    (loop_delta == 1U && pos->r_off < snap->last_pos))
			lapped = true;
	}

	if (!lapped)
		return false;

	teelog_reader_jump_retain(pos, snap);
	return true;
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
	    snap->seq != pos->r_seq)
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
