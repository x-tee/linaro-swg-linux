/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OP-TEE TEE log (teelog) lifecycle — register, stop, recover (U5).
 * /dev/teelog read/poll is U6; this header only exposes init/stop and
 * the refcounted instance U6 will hold from open/release.
 */
#ifndef OPTEE_TEELOG_H
#define OPTEE_TEELOG_H

#include <linux/types.h>

struct optee;
struct optee_teelog;

#if IS_ENABLED(CONFIG_OPTEE_TEELOG)

void optee_teelog_init(struct optee *optee);
void optee_teelog_stop(struct optee *optee);

/**
 * optee_teelog_get() - Take a reference on the published teelog instance
 * @optee: Host OP-TEE device; caller must keep @optee alive for the call
 *
 * May sleep (takes teelog_slot_mu). A non-NULL return only guarantees the
 * teelog object remains allocated until optee_teelog_put(). It does **not**
 * guarantee ACTIVE state, usable log pages, or a live TEE context/session —
 * stop may already be in progress or finished. U6 readers must synchronise
 * separately with stop/free (e.g. state_lock + state checks) before touching
 * the ring or TEE objects.
 */
struct optee_teelog *optee_teelog_get(struct optee *optee);

/**
 * optee_teelog_put() - Drop a reference from optee_teelog_get() / host owner
 * @tl: Instance previously returned by get, or the host-published object
 *
 * May free @tl when the last reference drops. Do not use @tl afterward.
 */
void optee_teelog_put(struct optee_teelog *tl);

#else /* !CONFIG_OPTEE_TEELOG */

static inline void optee_teelog_init(struct optee *optee)
{
}

static inline void optee_teelog_stop(struct optee *optee)
{
}

static inline struct optee_teelog *optee_teelog_get(struct optee *optee)
{
	return NULL;
}

static inline void optee_teelog_put(struct optee_teelog *tl)
{
}

#endif /* CONFIG_OPTEE_TEELOG */

#endif /* OPTEE_TEELOG_H */
