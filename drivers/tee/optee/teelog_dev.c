// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/teelog read, poll, ioctl, async notif wake-up, and 100 ms worker (U6).
 */
#define pr_fmt(fmt) "optee teelog: " fmt

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "optee_private.h"
#include "teelog.h"
#include "teelog_priv.h"
#include "teelog_protocol.h"
#include "teelog_ring_read.h"

#define TEELOG_POLL_MS			100
#define TEELOG_READ_STAGE_LEN		TEELOG_ITEM_MAX_LEN

struct teelog_reader {
	struct mutex mu;
	struct optee_teelog *tl;
	struct teelog_reader_pos pos;
	u8 *stage;
	bool claim_owner;
};

static struct teelog_buffer *teelog_ring_or_null(struct optee_teelog *tl)
{
	if (!tl || tl->state != OPTEE_TEELOG_ACTIVE || !tl->va)
		return NULL;
	return (struct teelog_buffer *)tl->va;
}

static int teelog_snap_active(struct optee_teelog *tl,
			      struct teelog_hdr_snap *snap)
{
	struct teelog_buffer *ring;
	int rc;

	mutex_lock(&tl->state_lock);
	ring = teelog_ring_or_null(tl);
	if (!ring) {
		mutex_unlock(&tl->state_lock);
		return -ENODEV;
	}
	rc = teelog_snap_flag(ring, tl->size, snap);
	mutex_unlock(&tl->state_lock);
	return rc;
}

static void teelog_wake_readers(struct optee_teelog *tl)
{
	wake_up_interruptible(&tl->wq);
}

void teelog_dev_kick(struct optee_teelog *tl)
{
	if (tl)
		teelog_wake_readers(tl);
}

static void teelog_poll_work_fn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct optee_teelog *tl =
		container_of(dwork, struct optee_teelog, poll_work);
	struct teelog_hdr_snap snap;
	int rc;

	rc = teelog_snap_active(tl, &snap);
	if (rc == -ENODEV)
		return;

	/*
	 * Unconditional 100 ms period (design §5.1). Wake only when the
	 * producer seq advanced since the last wake to limit poll noise.
	 */
	if (rc == 0) {
		u64 prev = READ_ONCE(tl->worker_seen_seq);

		if (snap.seq != prev) {
			WRITE_ONCE(tl->worker_seen_seq, snap.seq);
			teelog_wake_readers(tl);
		}
	}

	mutex_lock(&tl->state_lock);
	if (tl->state == OPTEE_TEELOG_ACTIVE)
		schedule_delayed_work(&tl->poll_work,
				      msecs_to_jiffies(TEELOG_POLL_MS));
	mutex_unlock(&tl->state_lock);
}

bool optee_teelog_notif_handle(struct optee *optee, u32 value)
{
	struct optee_teelog *tl;
	bool handled = false;

	if (!optee || value == PTA_TEELOG_NOTIF_ID_NONE)
		return false;

	rcu_read_lock();
	tl = rcu_dereference(optee->teelog_notif);
	if (tl && tl->notif_id == value &&
	    tl->notif_id != PTA_TEELOG_NOTIF_ID_NONE) {
		teelog_wake_readers(tl);
		handled = true;
	}
	rcu_read_unlock();
	return handled;
}
EXPORT_SYMBOL_GPL(optee_teelog_notif_handle);

static int teelog_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct optee_teelog *tl = container_of(misc, struct optee_teelog, misc);
	struct teelog_reader *reader;
	struct teelog_hdr_snap snap;
	struct teelog_buffer *ring;
	int rc;

	(void)inode;

	if (!kref_get_unless_zero(&tl->kref))
		return -ENODEV;

	mutex_lock(&tl->state_lock);
	if (tl->state != OPTEE_TEELOG_ACTIVE || !tl->va) {
		mutex_unlock(&tl->state_lock);
		optee_teelog_put(tl);
		return -ENODEV;
	}
	ring = (struct teelog_buffer *)tl->va;
	rc = teelog_snap_flag(ring, tl->size, &snap);
	mutex_unlock(&tl->state_lock);
	if (rc) {
		optee_teelog_put(tl);
		return rc == -EAGAIN ? -EAGAIN : -ENODEV;
	}

	reader = kzalloc(sizeof(*reader), GFP_KERNEL);
	if (!reader) {
		optee_teelog_put(tl);
		return -ENOMEM;
	}
	reader->stage = kmalloc(TEELOG_READ_STAGE_LEN, GFP_KERNEL);
	if (!reader->stage) {
		kfree(reader);
		optee_teelog_put(tl);
		return -ENOMEM;
	}

	mutex_init(&reader->mu);
	reader->tl = tl;
	teelog_reader_init_window(&reader->pos, &snap);
	file->private_data = reader;
	return 0;
}

static int teelog_release(struct inode *inode, struct file *file)
{
	struct teelog_reader *reader = file->private_data;
	struct optee_teelog *tl;

	if (!reader)
		return 0;

	tl = reader->tl;
	if (reader->claim_owner) {
		mutex_lock(&tl->state_lock);
		if (tl->tlogcat_owner == file)
			tl->tlogcat_owner = NULL;
		mutex_unlock(&tl->state_lock);
	}

	mutex_destroy(&reader->mu);
	kfree(reader->stage);
	kfree(reader);
	file->private_data = NULL;
	optee_teelog_put(tl);
	return 0;
}

/*
 * Fetch one deliverable record under a stable snap into reader->stage.
 * On success: *found_off / *stage_len set; tmp advanced past skipped gaps
 * but not past the found record (caller commits after copy_to_user).
 *
 * Seq instability retries are bounded by @seq_tries (design §3.2).
 */
static int teelog_fetch_one(struct teelog_reader *reader,
			    struct teelog_reader_pos *tmp,
			    struct teelog_hdr_snap *snap,
			    u32 *found_off, u32 *stage_len,
			    unsigned int *seq_tries)
{
	struct optee_teelog *tl = reader->tl;
	struct teelog_buffer *ring;
	u32 scan_budget;
	int rc;

retry_snap:
	if (*seq_tries >= TEELOG_SEQ_RETRY_MAX)
		return -EAGAIN;

	if (tl->state != OPTEE_TEELOG_ACTIVE || !tl->va)
		return -ENODEV;

	ring = (struct teelog_buffer *)tl->va;
	rc = teelog_snap_flag(ring, tl->size, snap);
	if (rc == -EAGAIN) {
		(*seq_tries)++;
		goto retry_snap;
	}
	if (rc)
		return rc;

	teelog_reader_catchup(tmp, snap);
	if (!teelog_reader_has_data(tmp, snap))
		return -ENODATA;

	/*
	 * One ring of 64 B steps as the scan budget for this snap. The helper
	 * owns the scan inside [off, end); we only wrap the unread window.
	 */
	scan_budget = snap->max_len / TEELOG_ITEM_LEN_ALIGN + 1U;

	while (scan_budget--) {
		u32 off = tmp->r_off;
		u32 end = teelog_unread_end(tmp, snap);
		struct teelog_item *item;

		if (off >= end) {
			if (tmp->r_loops != snap->write_loops) {
				tmp->r_off = 0;
				tmp->r_loops = snap->write_loops;
				continue;
			}
			return -ENODATA;
		}

		rc = teelog_next_record_stable(ring, tl->size, off, end,
					       reader->stage, found_off,
					       stage_len, snap->seq);
		if (rc == -EAGAIN) {
			(*seq_tries)++;
			goto retry_snap;
		}
		if (rc == -ENOENT) {
			/* Whole [off, end) scanned; advance past this segment. */
			tmp->r_off = end;
			if (tmp->r_loops != snap->write_loops) {
				tmp->r_off = 0;
				tmp->r_loops = snap->write_loops;
				continue;
			}
			return -ENODATA;
		}
		if (rc)
			return rc;

		item = (struct teelog_item *)reader->stage;
		if (!teelog_serial_accept(tmp, item->serial_no)) {
			tmp->r_off = *found_off + *stage_len;
			if (tmp->r_off >= snap->max_len &&
			    tmp->r_loops != snap->write_loops) {
				tmp->r_off = 0;
				tmp->r_loops = snap->write_loops;
			}
			continue;
		}
		return 0;
	}

	return -EIO;
}

static ssize_t teelog_read_records(struct teelog_reader *reader,
				   char __user *buf, size_t count)
{
	struct optee_teelog *tl = reader->tl;
	struct teelog_hdr_snap snap;
	struct teelog_reader_pos tmp;
	size_t written = 0;
	unsigned int seq_tries = 0;
	int rc;

	if (count < TEELOG_ITEM_MAX_LEN)
		return -EINVAL;

	tmp = reader->pos;

	while (written + TEELOG_ITEM_MAX_LEN <= count) {
		u32 found_off = 0, stage_len = 0;
		struct teelog_item *item;

		mutex_lock(&tl->state_lock);
		rc = teelog_fetch_one(reader, &tmp, &snap, &found_off,
				      &stage_len, &seq_tries);
		mutex_unlock(&tl->state_lock);

		if (rc == -ENODATA) {
			reader->pos = tmp;
			return written;
		}
		if (rc == -EAGAIN)
			return written ? written : -EAGAIN;
		if (rc == -ENODEV)
			return written ? written : -ENODEV;
		if (rc)
			return written ? written : rc;

		item = (struct teelog_item *)reader->stage;
		if (copy_to_user(buf + written, reader->stage, stage_len)) {
			/* Do not commit cursor or serial for this record. */
			return written ? written : -EFAULT;
		}

		written += stage_len;
		tmp.r_off = found_off + stage_len;
		tmp.r_seq = snap.seq;
		teelog_serial_commit(&tmp, item->serial_no);
		if (tmp.r_off >= snap.max_len &&
		    tmp.r_loops != snap.write_loops) {
			tmp.r_off = 0;
			tmp.r_loops = snap.write_loops;
		}
		reader->pos = tmp;
		seq_tries = 0; /* fresh budget for the next record */
	}

	return written;
}

static ssize_t teelog_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos)
{
	struct teelog_reader *reader = file->private_data;
	ssize_t ret;

	(void)ppos;
	if (!reader)
		return -EINVAL;

	mutex_lock(&reader->mu);
	ret = teelog_read_records(reader, buf, count);
	mutex_unlock(&reader->mu);
	return ret;
}

static __poll_t teelog_poll(struct file *file, poll_table *wait)
{
	struct teelog_reader *reader = file->private_data;
	struct optee_teelog *tl;
	struct teelog_hdr_snap snap;
	struct teelog_reader_pos tmp;
	__poll_t mask = 0;
	int rc;

	if (!reader)
		return EPOLLERR;

	tl = reader->tl;
	poll_wait(file, &tl->wq, wait);

	mutex_lock(&tl->state_lock);
	if (tl->state != OPTEE_TEELOG_ACTIVE || !tl->va) {
		mutex_unlock(&tl->state_lock);
		return EPOLLHUP | EPOLLERR;
	}
	rc = teelog_snap_flag((struct teelog_buffer *)tl->va, tl->size, &snap);
	mutex_unlock(&tl->state_lock);

	if (rc == -ENODEV || rc == -EIO)
		return EPOLLHUP | EPOLLERR;
	if (rc)
		return 0;

	mutex_lock(&reader->mu);
	tmp = reader->pos;
	teelog_reader_catchup(&tmp, &snap);
	if (teelog_reader_has_data(&tmp, &snap))
		mask |= EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&reader->mu);
	return mask;
}

static int teelog_ioctl_get_version(struct optee_teelog *tl,
				    char __user *argp)
{
	char ver[TEELOGGER_MAX_VERSION_LEN] = {};
	struct teelog_buffer *ring;

	mutex_lock(&tl->state_lock);
	ring = teelog_ring_or_null(tl);
	if (ring)
		memcpy(ver, ring->flag.version_info,
		       min_t(size_t, TEELOG_VERSION_INFO_LEN,
			     TEELOGGER_MAX_VERSION_LEN - 1));
	mutex_unlock(&tl->state_lock);
	ver[TEELOGGER_MAX_VERSION_LEN - 1] = '\0';

	if (copy_to_user(argp, ver, sizeof(ver)))
		return -EFAULT;
	return 0;
}

static long teelog_unlocked_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	struct teelog_reader *reader = file->private_data;
	struct optee_teelog *tl;
	void __user *argp = (void __user *)arg;
	struct teelog_hdr_snap snap;
	struct teelog_caps caps = {};
	struct teelog_stats stats = {};
	int rc;

	if (!reader)
		return -EINVAL;
	tl = reader->tl;

	switch (cmd) {
	case TEELOGGER_GET_VERSION:
		return teelog_ioctl_get_version(tl, argp);

	case TEELOGGER_SET_READERPOS_CUR:
		mutex_lock(&reader->mu);
		rc = teelog_snap_active(tl, &snap);
		if (!rc) {
			reader->pos.r_off = snap.last_pos;
			reader->pos.r_loops = snap.write_loops;
			reader->pos.r_seq = snap.seq;
			reader->pos.r_serial_valid = false;
		}
		mutex_unlock(&reader->mu);
		return rc;

	case TEELOGGER_SET_TLOGCAT_STAT:
		/*
		 * Atomic claim (design: EBUSY if another owner). OpenTrustee
		 * SET is not atomic; GET remains global occupied (0/1).
		 */
		mutex_lock(&tl->state_lock);
		if (tl->tlogcat_owner && tl->tlogcat_owner != file) {
			mutex_unlock(&tl->state_lock);
			return -EBUSY;
		}
		tl->tlogcat_owner = file;
		reader->claim_owner = true;
		mutex_unlock(&tl->state_lock);
		return 0;

	case TEELOGGER_GET_TLOGCAT_STAT:
		/* Global claim occupied — not "does this fd own it". */
		mutex_lock(&tl->state_lock);
		rc = tl->tlogcat_owner ? 1 : 0;
		mutex_unlock(&tl->state_lock);
		return rc;

	case TEELOGGER_GET_CAPS:
		mutex_lock(&tl->state_lock);
		caps.protocol_version = PTA_TEELOG_PROTOCOL_VERSION;
		caps.ring_size = (tl->state == OPTEE_TEELOG_ACTIVE) ? tl->size : 0;
		caps.capabilities = TEELOG_USER_CAP_SEQ64;
		if (tl->tee_caps & PTA_TEELOG_CAPS_ASYNC_NOTIF)
			caps.capabilities |= TEELOG_USER_CAP_ASYNC_NOTIF;
		mutex_unlock(&tl->state_lock);
		if (copy_to_user(argp, &caps, sizeof(caps)))
			return -EFAULT;
		return 0;

	case TEELOGGER_GET_STATS:
		rc = teelog_snap_active(tl, &snap);
		if (rc)
			return rc;
		stats.seq = snap.seq;
		stats.last_pos = snap.last_pos;
		stats.write_loops = snap.write_loops;
		stats.dropped = snap.dropped;
		stats.reserved = 0;
		if (copy_to_user(argp, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations teelog_fops = {
	.owner		= THIS_MODULE,
	.open		= teelog_open,
	.release	= teelog_release,
	.read		= teelog_read,
	.poll		= teelog_poll,
	.unlocked_ioctl	= teelog_unlocked_ioctl,
	.compat_ioctl	= teelog_unlocked_ioctl,
	.llseek		= noop_llseek,
};

int teelog_dev_start(struct optee_teelog *tl)
{
	int rc;

	if (!tl || !tl->va || tl->state != OPTEE_TEELOG_ACTIVE)
		return -ENODEV;

	init_waitqueue_head(&tl->wq);
	INIT_DELAYED_WORK(&tl->poll_work, teelog_poll_work_fn);
	tl->dwork_inited = true;
	tl->tlogcat_owner = NULL;

	tl->misc.minor = MISC_DYNAMIC_MINOR;
	tl->misc.name = "teelog";
	tl->misc.fops = &teelog_fops;
	tl->misc.parent = NULL;
	tl->misc.mode = 0640;

	rc = misc_register(&tl->misc);
	if (rc) {
		pr_err("misc_register(/dev/teelog) failed (%d)\n", rc);
		return rc;
	}
	tl->misc_registered = true;

	if (tl->optee)
		rcu_assign_pointer(tl->optee->teelog_notif, tl);

	schedule_delayed_work(&tl->poll_work, 0);
	/* Registration completed: check backlog immediately. */
	teelog_wake_readers(tl);
	pr_info("TEELOG_DEV_READY size=%u notif=0x%x\n", tl->size, tl->notif_id);
	return 0;
}

void teelog_dev_shutdown(struct optee_teelog *tl)
{
	bool was_started;

	if (!tl)
		return;

	/*
	 * teelog_dev_start() inits wq + delayed_work together. Only touch
	 * them when start ran — release/orphan may call us on a never-started
	 * instance.
	 */
	was_started = tl->dwork_inited || tl->misc_registered;

	if (tl->optee) {
		rcu_assign_pointer(tl->optee->teelog_notif, NULL);
		synchronize_rcu();
	}

	if (tl->dwork_inited) {
		cancel_delayed_work_sync(&tl->poll_work);
		tl->dwork_inited = false;
	}

	if (tl->misc_registered) {
		misc_deregister(&tl->misc);
		tl->misc_registered = false;
	}

	if (was_started)
		teelog_wake_readers(tl);
}
