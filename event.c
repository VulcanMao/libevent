/*
 * Copyright (c) 2000-2007 Niels Provos <provos@citi.umich.edu>
 * Copyright (c) 2007-2012 Niels Provos and Nick Mathewson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "event2/event-config.h"
#include "evconfig-private.h"

#ifdef _WIN32
#include <winsock2.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#if !defined(_WIN32) && defined(EVENT__HAVE_SYS_TIME_H)
#include <sys/time.h>
#endif
#include <sys/queue.h>
#ifdef EVENT__HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#ifdef EVENT__HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#ifdef EVENT__HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "event2/event.h"
#include "event2/event_struct.h"
#include "event2/event_compat.h"
#include "event-internal.h"
#include "defer-internal.h"
#include "evthread-internal.h"
#include "event2/thread.h"
#include "event2/util.h"
#include "log-internal.h"
#include "evmap-internal.h"
#include "iocp-internal.h"
#include "changelist-internal.h"
#define HT_NO_CACHE_HASH_VALUES
#include "ht-internal.h"
#include "util-internal.h"


#ifdef EVENT__HAVE_WORKING_KQUEUE
#include "kqueue-internal.h"
#endif

#ifdef EVENT__HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef EVENT__HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef EVENT__HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef EVENT__HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef EVENT__HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef EVENT__HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef _WIN32
extern const struct eventop win32ops;
#endif

/* Array of backends in order of preference. */
static const struct eventop *eventops[] = {
#ifdef EVENT__HAVE_EVENT_PORTS
	&evportops,
#endif
#ifdef EVENT__HAVE_WORKING_KQUEUE
	&kqops,
#endif
#ifdef EVENT__HAVE_EPOLL
	&epollops,
#endif
#ifdef EVENT__HAVE_DEVPOLL
	&devpollops,
#endif
#ifdef EVENT__HAVE_POLL
	&pollops,
#endif
#ifdef EVENT__HAVE_SELECT
	&selectops,
#endif
#ifdef _WIN32
	&win32ops,
#endif
	NULL
};

/* Global state; deprecated */
EVENT2_EXPORT_SYMBOL
struct event_base *event_global_current_base_ = NULL;
#define current_base event_global_current_base_

/* Global state */

static void *event_self_cbarg_ptr_ = NULL;

/* Prototypes */
static void	event_queue_insert_active(struct event_base *, struct event_callback *);
static void	event_queue_insert_active_later(struct event_base *, struct event_callback *);
static void	event_queue_insert_timeout(struct event_base *, struct event *);
static void	event_queue_insert_inserted(struct event_base *, struct event *);
static void	event_queue_remove_active(struct event_base *, struct event_callback *);
static void	event_queue_remove_active_later(struct event_base *, struct event_callback *);
static void	event_queue_remove_timeout(struct event_base *, struct event *);
static void	event_queue_remove_inserted(struct event_base *, struct event *);
static void event_queue_make_later_events_active(struct event_base *base);

static int evthread_make_base_notifiable_nolock_(struct event_base *base);
static int event_del_(struct event *ev, int blocking);

#ifdef USE_REINSERT_TIMEOUT
/* This code seems buggy; only turn it on if we find out what the trouble is. */
static void	event_queue_reinsert_timeout(struct event_base *,struct event *, int was_common, int is_common, int old_timeout_idx);
#endif

static int	event_haveevents(struct event_base *);

static int	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);

static inline void	event_signal_closure(struct event_base *, struct event *ev);
static inline void	event_persist_closure(struct event_base *, struct event *ev);

static int	evthread_notify_base(struct event_base *base);

static void insert_common_timeout_inorder(struct common_timeout_list *ctl,
    struct event *ev);

#ifndef EVENT__DISABLE_DEBUG_MODE
/* These functions implement a hashtable of which 'struct event *' structures
 * have been setup or added.  We don't want to trust the content of the struct
 * event itself, since we're trying to work through cases where an event gets
 * clobbered or freed.  Instead, we keep a hashtable indexed by the pointer.
 */

struct event_debug_entry {
	HT_ENTRY(event_debug_entry) node;
	const struct event *ptr;
	unsigned added : 1;
};

static inline unsigned
hash_debug_entry(const struct event_debug_entry *e)
{
	/* We need to do this silliness to convince compilers that we
	 * honestly mean to cast e->ptr to an integer, and discard any
	 * part of it that doesn't fit in an unsigned.
	 */
	unsigned u = (unsigned) ((ev_uintptr_t) e->ptr);
	/* Our hashtable implementation is pretty sensitive to low bits,
	 * and every struct event is over 64 bytes in size, so we can
	 * just say >>6. */
	return (u >> 6);
}

static inline int
eq_debug_entry(const struct event_debug_entry *a,
    const struct event_debug_entry *b)
{
	return a->ptr == b->ptr;
}

int event_debug_mode_on_ = 0;


#if !defined(EVENT__DISABLE_THREAD_SUPPORT) && !defined(EVENT__DISABLE_DEBUG_MODE)
/**
 * @brief debug mode variable which is set for any function/structure that needs
 *        to be shared across threads (if thread support is enabled).
 *
 *        When and if evthreads are initialized, this variable will be evaluated,
 *        and if set to something other than zero, this means the evthread setup 
 *        functions were called out of order.
 *
 *        See: "Locks and threading" in the documentation.
 */
int event_debug_created_threadable_ctx_ = 0;
#endif

/* Set if it's too late to enable event_debug_mode. */
static int event_debug_mode_too_late = 0;
#ifndef EVENT__DISABLE_THREAD_SUPPORT
static void *event_debug_map_lock_ = NULL;
#endif
static HT_HEAD(event_debug_map, event_debug_entry) global_debug_map =
	HT_INITIALIZER();

HT_PROTOTYPE(event_debug_map, event_debug_entry, node, hash_debug_entry,
    eq_debug_entry)
HT_GENERATE(event_debug_map, event_debug_entry, node, hash_debug_entry,
    eq_debug_entry, 0.5, mm_malloc, mm_realloc, mm_free)

/* record that ev is now setup (that is, ready for an add) */
static void event_debug_note_setup_(const struct event *ev)
{
	struct event_debug_entry *dent, find;

	if (!event_debug_mode_on_)
		goto out;

	find.ptr = ev;
	EVLOCK_LOCK(event_debug_map_lock_, 0);
	dent = HT_FIND(event_debug_map, &global_debug_map, &find);
	if (dent) {
		dent->added = 0;
	} else {
		dent = mm_malloc(sizeof(*dent));
		if (!dent)
			event_err(1,
			    "Out of memory in debugging code");
		dent->ptr = ev;
		dent->added = 0;
		HT_INSERT(event_debug_map, &global_debug_map, dent);
	}
	EVLOCK_UNLOCK(event_debug_map_lock_, 0);

out:
	event_debug_mode_too_late = 1;
}
/* record that ev is no longer setup */
static void event_debug_note_teardown_(const struct event *ev)
{
	struct event_debug_entry *dent, find;

	if (!event_debug_mode_on_)
		goto out;

	find.ptr = ev;
	EVLOCK_LOCK(event_debug_map_lock_, 0);
	dent = HT_REMOVE(event_debug_map, &global_debug_map, &find);
	if (dent)
		mm_free(dent);
	EVLOCK_UNLOCK(event_debug_map_lock_, 0);

out:
	event_debug_mode_too_late = 1;
}
/* Macro: record that ev is now added */
static void event_debug_note_add_(const struct event *ev)
{
	struct event_debug_entry *dent,find;

	if (!event_debug_mode_on_)
		goto out;

	find.ptr = ev;
	EVLOCK_LOCK(event_debug_map_lock_, 0);
	dent = HT_FIND(event_debug_map, &global_debug_map, &find);
	if (dent) {
		dent->added = 1;
	} else {
		event_errx(EVENT_ERR_ABORT_,
		    "%s: noting an add on a non-setup event %p"
		    " (events: 0x%x, fd: "EV_SOCK_FMT
		    ", flags: 0x%x)",
		    __func__, ev, ev->ev_events,
		    EV_SOCK_ARG(ev->ev_fd), ev->ev_flags);
	}
	EVLOCK_UNLOCK(event_debug_map_lock_, 0);

out:
	event_debug_mode_too_late = 1;
}
/* record that ev is no longer added */
static void event_debug_note_del_(const struct event *ev)
{
	struct event_debug_entry *dent, find;

	if (!event_debug_mode_on_)
		goto out;

	find.ptr = ev;
	EVLOCK_LOCK(event_debug_map_lock_, 0);
	dent = HT_FIND(event_debug_map, &global_debug_map, &find);
	if (dent) {
		dent->added = 0;
	} else {
		event_errx(EVENT_ERR_ABORT_,
		    "%s: noting a del on a non-setup event %p"
		    " (events: 0x%x, fd: "EV_SOCK_FMT
		    ", flags: 0x%x)",
		    __func__, ev, ev->ev_events,
		    EV_SOCK_ARG(ev->ev_fd), ev->ev_flags);
	}
	EVLOCK_UNLOCK(event_debug_map_lock_, 0);

out:
	event_debug_mode_too_late = 1;
}
/* assert that ev is setup (i.e., okay to add or inspect) */
static void event_debug_assert_is_setup_(const struct event *ev)
{
	struct event_debug_entry *dent, find;

	if (!event_debug_mode_on_)
		return;

	find.ptr = ev;
	EVLOCK_LOCK(event_debug_map_lock_, 0);
	dent = HT_FIND(event_debug_map, &global_debug_map, &find);
	if (!dent) {
		event_errx(EVENT_ERR_ABORT_,
		    "%s called on a non-initialized event %p"
		    " (events: 0x%x, fd: "EV_SOCK_FMT
		    ", flags: 0x%x)",
		    __func__, ev, ev->ev_events,
		    EV_SOCK_ARG(ev->ev_fd), ev->ev_flags);
	}
	EVLOCK_UNLOCK(event_debug_map_lock_, 0);
}
/* assert that ev is not added (i.e., okay to tear down or set up again) */
static void event_debug_assert_not_added_(const struct event *ev)
{
	struct event_debug_entry *dent, find;

	if (!event_debug_mode_on_)
		return;

	find.ptr = ev;
	EVLOCK_LOCK(event_debug_map_lock_, 0);
	dent = HT_FIND(event_debug_map, &global_debug_map, &find);
	if (dent && dent->added) {
		event_errx(EVENT_ERR_ABORT_,
		    "%s called on an already added event %p"
		    " (events: 0x%x, fd: "EV_SOCK_FMT", "
		    "flags: 0x%x)",
		    __func__, ev, ev->ev_events,
		    EV_SOCK_ARG(ev->ev_fd), ev->ev_flags);
	}
	EVLOCK_UNLOCK(event_debug_map_lock_, 0);
}
static void event_debug_assert_socket_nonblocking_(evutil_socket_t fd)
{
	if (!event_debug_mode_on_)
		return;
	if (fd < 0)
		return;

#ifndef _WIN32
	{
		int flags;
		if ((flags = fcntl(fd, F_GETFL, NULL)) >= 0) {
			EVUTIL_ASSERT(flags & O_NONBLOCK);
		}
	}
#endif
}
#else
static void event_debug_note_setup_(const struct event *ev) { (void)ev; }
static void event_debug_note_teardown_(const struct event *ev) { (void)ev; }
static void event_debug_note_add_(const struct event *ev) { (void)ev; }
static void event_debug_note_del_(const struct event *ev) { (void)ev; }
static void event_debug_assert_is_setup_(const struct event *ev) { (void)ev; }
static void event_debug_assert_not_added_(const struct event *ev) { (void)ev; }
static void event_debug_assert_socket_nonblocking_(evutil_socket_t fd) { (void)fd; }
#endif

#define EVENT_BASE_ASSERT_LOCKED(base)		\
	EVLOCK_ASSERT_LOCKED((base)->th_base_lock)

/* How often (in seconds) do we check for changes in wall clock time relative
 * to monotonic time?  Set this to -1 for 'never.' */
#define CLOCK_SYNC_INTERVAL 5

/** Set 'tp' to the current time according to 'base'.  We must hold the lock
 * on 'base'.  If there is a cached time, return it.  Otherwise, use
 * clock_gettime or gettimeofday as appropriate to find out the right time.
 * Return 0 on success, -1 on failure.
 */
static int
gettime(struct event_base *base, struct timeval *tp)
{
	EVENT_BASE_ASSERT_LOCKED(base);

	//cache可用时
	if (base->tv_cache.tv_sec) {
		*tp = base->tv_cache;
		return (0);
	}

	if (evutil_gettime_monotonic_(&base->monotonic_timer, tp) == -1) {
		return -1;
	}

	if (base->last_updated_clock_diff + CLOCK_SYNC_INTERVAL
	    < tp->tv_sec) {
		struct timeval tv;
		evutil_gettimeofday(&tv,NULL);
		evutil_timersub(&tv, tp, &base->tv_clock_diff);
		base->last_updated_clock_diff = tp->tv_sec;
	}

	return 0;
}

int
event_base_gettimeofday_cached(struct event_base *base, struct timeval *tv)
{
	int r;
	if (!base) {
		base = current_base;
		if (!current_base)
			return evutil_gettimeofday(tv, NULL);
	}

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	if (base->tv_cache.tv_sec == 0) {
		r = evutil_gettimeofday(tv, NULL);
	} else {
		evutil_timeradd(&base->tv_cache, &base->tv_clock_diff, tv);
		r = 0;
	}
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return r;
}

/** Make 'base' have no current cached time. */
static inline void
clear_time_cache(struct event_base *base)
{
	base->tv_cache.tv_sec = 0;
}

/** Replace the cached time in 'base' with the current time. */
static inline void
update_time_cache(struct event_base *base)
{
	base->tv_cache.tv_sec = 0;
	if (!(base->flags & EVENT_BASE_FLAG_NO_CACHE_TIME))
	    gettime(base, &base->tv_cache);
}

int
event_base_update_cache_time(struct event_base *base)
{

	if (!base) {
		base = current_base;
		if (!current_base)
			return -1;
	}

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	if (base->running_loop)
		update_time_cache(base);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return 0;
}

static inline struct event *
event_callback_to_event(struct event_callback *evcb)
{
	EVUTIL_ASSERT((evcb->evcb_flags & EVLIST_INIT));
	return EVUTIL_UPCAST(evcb, struct event, ev_evcallback);
}

static inline struct event_callback *
event_to_event_callback(struct event *ev)
{
	return &ev->ev_evcallback;
}

struct event_base *
event_init(void)
{
	struct event_base *base = event_base_new_with_config(NULL);

	if (base == NULL) {
		event_errx(1, "%s: Unable to construct event_base", __func__);
		return NULL;
	}

	current_base = base;

	return (base);
}

struct event_base *
event_base_new(void)
{
	struct event_base *base = NULL;
	struct event_config *cfg = event_config_new();
	if (cfg) {
		base = event_base_new_with_config(cfg);
		event_config_free(cfg);
	}
	return base;
}

/** Return true iff 'method' is the name of a method that 'cfg' tells us to
 * avoid. */
static int
event_config_is_avoided_method(const struct event_config *cfg,
    const char *method)
{
	struct event_config_entry *entry;

	TAILQ_FOREACH(entry, &cfg->entries, next) {
		if (entry->avoid_method != NULL &&
		    strcmp(entry->avoid_method, method) == 0)
			return (1);
	}

	return (0);
}

/** Return true iff 'method' is disabled according to the environment. */
static int
event_is_method_disabled(const char *name)
{
	char environment[64];
	int i;

	evutil_snprintf(environment, sizeof(environment), "EVENT_NO%s", name);
	for (i = 8; environment[i] != '\0'; ++i)
		environment[i] = EVUTIL_TOUPPER_(environment[i]);
	/* Note that evutil_getenv_() ignores the environment entirely if
	 * we're setuid */
	return (evutil_getenv_(environment) != NULL);
}

int
event_base_get_features(const struct event_base *base)
{
	return base->evsel->features;
}

void
event_enable_debug_mode(void)
{
#ifndef EVENT__DISABLE_DEBUG_MODE
	if (event_debug_mode_on_)
		event_errx(1, "%s was called twice!", __func__);
	if (event_debug_mode_too_late)
		event_errx(1, "%s must be called *before* creating any events "
		    "or event_bases",__func__);

	event_debug_mode_on_ = 1;

	HT_INIT(event_debug_map, &global_debug_map);
#endif
}

void
event_disable_debug_mode(void)
{
#ifndef EVENT__DISABLE_DEBUG_MODE
	struct event_debug_entry **ent, *victim;

	EVLOCK_LOCK(event_debug_map_lock_, 0);
	for (ent = HT_START(event_debug_map, &global_debug_map); ent; ) {
		victim = *ent;
		ent = HT_NEXT_RMV(event_debug_map, &global_debug_map, ent);
		mm_free(victim);
	}
	HT_CLEAR(event_debug_map, &global_debug_map);
	EVLOCK_UNLOCK(event_debug_map_lock_ , 0);

	event_debug_mode_on_  = 0;
#endif
}

struct event_base *
event_base_new_with_config(const struct event_config *cfg)
{
	int i;
	struct event_base *base;
	int should_check_environment;

#ifndef EVENT__DISABLE_DEBUG_MODE
	event_debug_mode_too_late = 1;
#endif

	//之所以不用mm_mallco是因为mm_malloc并不会清零该内存区域,
	//而这个函数是会清零申请到的内存区域,相当于给base初始化
	if ((base = mm_calloc(1, sizeof(struct event_base))) == NULL) {
		event_warn("%s: calloc", __func__);
		return NULL;
	}

	if (cfg)
		base->flags = cfg->flags;

	should_check_environment =
	    !(cfg && (cfg->flags & EVENT_BASE_FLAG_IGNORE_ENV));

	{
		struct timeval tmp;
		int precise_time =
		    cfg && (cfg->flags & EVENT_BASE_FLAG_PRECISE_TIMER);
		int flags;
		if (should_check_environment && !precise_time) {
			precise_time = evutil_getenv_("EVENT_PRECISE_TIMER") != NULL;
			if (precise_time) {
				base->flags |= EVENT_BASE_FLAG_PRECISE_TIMER;
			}
		}
		flags = precise_time ? EV_MONOT_PRECISE : 0;
		evutil_configure_monotonic_time_(&base->monotonic_timer, flags);

		gettime(base, &tmp);
	}

	min_heap_ctor_(&base->timeheap);

	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;
	base->th_notify_fd[0] = -1;
	base->th_notify_fd[1] = -1;

	TAILQ_INIT(&base->active_later_queue);

	evmap_io_initmap_(&base->io);
	evmap_signal_initmap_(&base->sigmap);
	event_changelist_init_(&base->changelist);

	base->evbase = NULL;

	if (cfg) {
		memcpy(&base->max_dispatch_time,
		    &cfg->max_dispatch_interval, sizeof(struct timeval));
		base->limit_callbacks_after_prio =
		    cfg->limit_callbacks_after_prio;
	} else {
		base->max_dispatch_time.tv_sec = -1;
		base->limit_callbacks_after_prio = 1;
	}
	if (cfg && cfg->max_dispatch_callbacks >= 0) {
		base->max_dispatch_callbacks = cfg->max_dispatch_callbacks;
	} else {
		base->max_dispatch_callbacks = INT_MAX;
	}
	if (base->max_dispatch_callbacks == INT_MAX &&
	    base->max_dispatch_time.tv_sec == -1)
		base->limit_callbacks_after_prio = INT_MAX;

	for (i = 0; eventops[i] && !base->evbase; i++) {
		if (cfg != NULL) {
			/* determine if this backend should be avoided */
			if (event_config_is_avoided_method(cfg,
				eventops[i]->name))
				continue;
			if ((eventops[i]->features & cfg->require_features)
			    != cfg->require_features)
				continue;
		}

		/* also obey the environment variables */
		if (should_check_environment &&
		    event_is_method_disabled(eventops[i]->name))
			continue;

		base->evsel = eventops[i];

		base->evbase = base->evsel->init(base);
	}

	if (base->evbase == NULL) {
		event_warnx("%s: no event mechanism available",
		    __func__);
		base->evsel = NULL;
		event_base_free(base);
		return NULL;
	}

	if (evutil_getenv_("EVENT_SHOW_METHOD"))
		event_msgx("libevent using: %s", base->evsel->name);

	/* allocate a single active event queue */
	if (event_base_priority_init(base, 1) < 0) {
		event_base_free(base);
		return NULL;
	}

	/* prepare for threading */

#if !defined(EVENT__DISABLE_THREAD_SUPPORT) && !defined(EVENT__DISABLE_DEBUG_MODE)
	event_debug_created_threadable_ctx_ = 1;
#endif

#ifndef EVENT__DISABLE_THREAD_SUPPORT

	//对于th_base_lock变量,目前的值为NULL, EVTHREAD_LOCKING_ENABLED宏是
	//测试ev_thread_lock_fns.lock是否不为NULL
	if (EVTHREAD_LOCKING_ENABLED() &&
	    (!cfg || !(cfg->flags & EVENT_BASE_FLAG_NOLOCK))) {
		int r;
		//申请锁变量
		EVTHREAD_ALLOC_LOCK(base->th_base_lock, 0);
		EVTHREAD_ALLOC_COND(base->current_event_cond);
		r = evthread_make_base_notifiable(base);
		if (r<0) {
			event_warnx("%s: Unable to make base notifiable.", __func__);
			event_base_free(base);
			return NULL;
		}
	}
#endif

#ifdef _WIN32
	if (cfg && (cfg->flags & EVENT_BASE_FLAG_STARTUP_IOCP))
		event_base_start_iocp_(base, cfg->n_cpus_hint);
#endif

	return (base);
}

int
event_base_start_iocp_(struct event_base *base, int n_cpus)
{
#ifdef _WIN32
	if (base->iocp)
		return 0;
	base->iocp = event_iocp_port_launch_(n_cpus);
	if (!base->iocp) {
		event_warnx("%s: Couldn't launch IOCP", __func__);
		return -1;
	}
	return 0;
#else
	return -1;
#endif
}

void
event_base_stop_iocp_(struct event_base *base)
{
#ifdef _WIN32
	int rv;

	if (!base->iocp)
		return;
	rv = event_iocp_shutdown_(base->iocp, -1);
	EVUTIL_ASSERT(rv >= 0);
	base->iocp = NULL;
#endif
}

static int
event_base_cancel_single_callback_(struct event_base *base,
    struct event_callback *evcb,
    int run_finalizers)
{
	int result = 0;

	if (evcb->evcb_flags & EVLIST_INIT) {
		struct event *ev = event_callback_to_event(evcb);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {
			event_del_(ev, EVENT_DEL_EVEN_IF_FINALIZING);
			result = 1;
		}
	} else {
		EVBASE_ACQUIRE_LOCK(base, th_base_lock);
		event_callback_cancel_nolock_(base, evcb, 1);
		EVBASE_RELEASE_LOCK(base, th_base_lock);
		result = 1;
	}

	if (run_finalizers && (evcb->evcb_flags & EVLIST_FINALIZING)) {
		switch (evcb->evcb_closure) {
		case EV_CLOSURE_EVENT_FINALIZE:
		case EV_CLOSURE_EVENT_FINALIZE_FREE: {
			struct event *ev = event_callback_to_event(evcb);
			ev->ev_evcallback.evcb_cb_union.evcb_evfinalize(ev, ev->ev_arg);
			if (evcb->evcb_closure == EV_CLOSURE_EVENT_FINALIZE_FREE)
				mm_free(ev);
			break;
		}
		case EV_CLOSURE_CB_FINALIZE:
			evcb->evcb_cb_union.evcb_cbfinalize(evcb, evcb->evcb_arg);
			break;
		default:
			break;
		}
	}
	return result;
}

static int event_base_free_queues_(struct event_base *base, int run_finalizers)
{
	int deleted = 0, i;

	for (i = 0; i < base->nactivequeues; ++i) {
		struct event_callback *evcb, *next;
		for (evcb = TAILQ_FIRST(&base->activequeues[i]); evcb; ) {
			next = TAILQ_NEXT(evcb, evcb_active_next);
			deleted += event_base_cancel_single_callback_(base, evcb, run_finalizers);
			evcb = next;
		}
	}

	{
		struct event_callback *evcb;
		while ((evcb = TAILQ_FIRST(&base->active_later_queue))) {
			deleted += event_base_cancel_single_callback_(base, evcb, run_finalizers);
		}
	}

	return deleted;
}

static void
event_base_free_(struct event_base *base, int run_finalizers)
{
	int i;
	size_t n_deleted=0;
	struct event *ev;
	/* XXXX grab the lock? If there is contention when one thread frees
	 * the base, then the contending thread will be very sad soon. */

	/* event_base_free(NULL) is how to free the current_base if we
	 * made it with event_init and forgot to hold a reference to it. */
	if (base == NULL && current_base)
		base = current_base;
	/* Don't actually free NULL. */
	if (base == NULL) {
		event_warnx("%s: no base to free", __func__);
		return;
	}
	/* XXX(niels) - check for internal events first */

#ifdef _WIN32
	event_base_stop_iocp_(base);
#endif

	/* threading fds if we have them */
	if (base->th_notify_fd[0] != -1) {
		event_del(&base->th_notify);
		EVUTIL_CLOSESOCKET(base->th_notify_fd[0]);
		if (base->th_notify_fd[1] != -1)
			EVUTIL_CLOSESOCKET(base->th_notify_fd[1]);
		base->th_notify_fd[0] = -1;
		base->th_notify_fd[1] = -1;
		event_debug_unassign(&base->th_notify);
	}

	/* Delete all non-internal events. */
	evmap_delete_all_(base);

	while ((ev = min_heap_top_(&base->timeheap)) != NULL) {
		event_del(ev);
		++n_deleted;
	}
	for (i = 0; i < base->n_common_timeouts; ++i) {
		struct common_timeout_list *ctl =
		    base->common_timeout_queues[i];
		event_del(&ctl->timeout_event); /* Internal; doesn't count */
		event_debug_unassign(&ctl->timeout_event);
		for (ev = TAILQ_FIRST(&ctl->events); ev; ) {
			struct event *next = TAILQ_NEXT(ev,
			    ev_timeout_pos.ev_next_with_common_timeout);
			if (!(ev->ev_flags & EVLIST_INTERNAL)) {
				event_del(ev);
				++n_deleted;
			}
			ev = next;
		}
		mm_free(ctl);
	}
	if (base->common_timeout_queues)
		mm_free(base->common_timeout_queues);

	for (;;) {
		/* For finalizers we can register yet another finalizer out from
		 * finalizer, and iff finalizer will be in active_later_queue we can
		 * add finalizer to activequeues, and we will have events in
		 * activequeues after this function returns, which is not what we want
		 * (we even have an assertion for this).
		 *
		 * A simple case is bufferevent with underlying (i.e. filters).
		 */
		int i = event_base_free_queues_(base, run_finalizers);
		event_debug(("%s: %d events freed", __func__, i));
		if (!i) {
			break;
		}
		n_deleted += i;
	}

	if (n_deleted)
		event_debug(("%s: "EV_SIZE_FMT" events were still set in base",
			__func__, n_deleted));

	while (LIST_FIRST(&base->once_events)) {
		struct event_once *eonce = LIST_FIRST(&base->once_events);
		LIST_REMOVE(eonce, next_once);
		mm_free(eonce);
	}

	if (base->evsel != NULL && base->evsel->dealloc != NULL)
		base->evsel->dealloc(base);

	for (i = 0; i < base->nactivequeues; ++i)
		EVUTIL_ASSERT(TAILQ_EMPTY(&base->activequeues[i]));

	EVUTIL_ASSERT(min_heap_empty_(&base->timeheap));
	min_heap_dtor_(&base->timeheap);

	mm_free(base->activequeues);

	evmap_io_clear_(&base->io);
	evmap_signal_clear_(&base->sigmap);
	event_changelist_freemem_(&base->changelist);

	EVTHREAD_FREE_LOCK(base->th_base_lock, 0);
	EVTHREAD_FREE_COND(base->current_event_cond);

	/* If we're freeing current_base, there won't be a current_base. */
	if (base == current_base)
		current_base = NULL;
	mm_free(base);
}

void
event_base_free_nofinalize(struct event_base *base)
{
	event_base_free_(base, 0);
}

void
event_base_free(struct event_base *base)
{
	event_base_free_(base, 1);
}

/* Fake eventop; used to disable the backend temporarily inside event_reinit
 * so that we can call event_del() on an event without telling the backend.
 */
static int
nil_backend_del(struct event_base *b, evutil_socket_t fd, short old,
    short events, void *fdinfo)
{
	return 0;
}
const struct eventop nil_eventop = {
	"nil",
	NULL, /* init: unused. */
	NULL, /* add: unused. */
	nil_backend_del, /* del: used, so needs to be killed. */
	NULL, /* dispatch: unused. */
	NULL, /* dealloc: unused. */
	0, 0, 0
};

/* reinitialize the event base after a fork */
int
event_reinit(struct event_base *base)
{
	const struct eventop *evsel;
	int res = 0;
	int was_notifiable = 0;
	int had_signal_added = 0;

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	evsel = base->evsel;

	/* check if this event mechanism requires reinit on the backend */
	if (evsel->need_reinit) {
		/* We're going to call event_del() on our notify events (the
		 * ones that tell about signals and wakeup events).  But we
		 * don't actually want to tell the backend to change its
		 * state, since it might still share some resource (a kqueue,
		 * an epoll fd) with the parent process, and we don't want to
		 * delete the fds from _that_ backend, we temporarily stub out
		 * the evsel with a replacement.
		 */
		base->evsel = &nil_eventop;
	}

	/* We need to re-create a new signal-notification fd and a new
	 * thread-notification fd.  Otherwise, we'll still share those with
	 * the parent process, which would make any notification sent to them
	 * get received by one or both of the event loops, more or less at
	 * random.
	 */
	if (base->sig.ev_signal_added) {
		event_del_nolock_(&base->sig.ev_signal, EVENT_DEL_AUTOBLOCK);
		event_debug_unassign(&base->sig.ev_signal);
		memset(&base->sig.ev_signal, 0, sizeof(base->sig.ev_signal));
		had_signal_added = 1;
		base->sig.ev_signal_added = 0;
	}
	if (base->sig.ev_signal_pair[0] != -1)
		EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[0]);
	if (base->sig.ev_signal_pair[1] != -1)
		EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[1]);
	if (base->th_notify_fn != NULL) {
		was_notifiable = 1;
		base->th_notify_fn = NULL;
	}
	if (base->th_notify_fd[0] != -1) {
		event_del_nolock_(&base->th_notify, EVENT_DEL_AUTOBLOCK);
		EVUTIL_CLOSESOCKET(base->th_notify_fd[0]);
		if (base->th_notify_fd[1] != -1)
			EVUTIL_CLOSESOCKET(base->th_notify_fd[1]);
		base->th_notify_fd[0] = -1;
		base->th_notify_fd[1] = -1;
		event_debug_unassign(&base->th_notify);
	}

	/* Replace the original evsel. */
        base->evsel = evsel;

	if (evsel->need_reinit) {
		/* Reconstruct the backend through brute-force, so that we do
		 * not share any structures with the parent process. For some
		 * backends, this is necessary: epoll and kqueue, for
		 * instance, have events associated with a kernel
		 * structure. If didn't reinitialize, we'd share that
		 * structure with the parent process, and any changes made by
		 * the parent would affect our backend's behavior (and vice
		 * versa).
		 */
		if (base->evsel->dealloc != NULL)
			base->evsel->dealloc(base);
		base->evbase = evsel->init(base);
		if (base->evbase == NULL) {
			event_errx(1,
			   "%s: could not reinitialize event mechanism",
			   __func__);
			res = -1;
			goto done;
		}

		/* Empty out the changelist (if any): we are starting from a
		 * blank slate. */
		event_changelist_freemem_(&base->changelist);

		/* Tell the event maps to re-inform the backend about all
		 * pending events. This will make the signal notification
		 * event get re-created if necessary. */
		if (evmap_reinit_(base) < 0)
			res = -1;
	} else {
		res = evsig_init_(base);
		if (res == 0 && had_signal_added) {
			res = event_add_nolock_(&base->sig.ev_signal, NULL, 0);
			if (res == 0)
				base->sig.ev_signal_added = 1;
		}
	}

	/* If we were notifiable before, and nothing just exploded, become
	 * notifiable again. */
	if (was_notifiable && res == 0)
		res = evthread_make_base_notifiable_nolock_(base);

done:
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return (res);
}

/* Get the monotonic time for this event_base' timer */
int
event_gettime_monotonic(struct event_base *base, struct timeval *tv)
{
  int rv = -1;

  if (base && tv) {
    EVBASE_ACQUIRE_LOCK(base, th_base_lock);
    rv = evutil_gettime_monotonic_(&(base->monotonic_timer), tv);
    EVBASE_RELEASE_LOCK(base, th_base_lock);
  }

  return rv;
}

const char **
event_get_supported_methods(void)
{
	static const char **methods = NULL;
	const struct eventop **method;
	const char **tmp;
	int i = 0, k;

	/* count all methods */
	for (method = &eventops[0]; *method != NULL; ++method) {
		++i;
	}

	/* allocate one more than we need for the NULL pointer */
	tmp = mm_calloc((i + 1), sizeof(char *));
	if (tmp == NULL)
		return (NULL);

	/* populate the array with the supported methods */
	for (k = 0, i = 0; eventops[k] != NULL; ++k) {
		tmp[i++] = eventops[k]->name;
	}
	tmp[i] = NULL;

	if (methods != NULL)
		mm_free((char**)methods);

	methods = tmp;

	return (methods);
}

struct event_config *
event_config_new(void)
{
	struct event_config *cfg = mm_calloc(1, sizeof(*cfg));

	if (cfg == NULL)
		return (NULL);

	TAILQ_INIT(&cfg->entries);
	cfg->max_dispatch_interval.tv_sec = -1;
	cfg->max_dispatch_callbacks = INT_MAX;
	cfg->limit_callbacks_after_prio = 1;

	return (cfg);
}

static void
event_config_entry_free(struct event_config_entry *entry)
{
	if (entry->avoid_method != NULL)
		mm_free((char *)entry->avoid_method);
	mm_free(entry);
}

void
event_config_free(struct event_config *cfg)
{
	struct event_config_entry *entry;

	while ((entry = TAILQ_FIRST(&cfg->entries)) != NULL) {
		TAILQ_REMOVE(&cfg->entries, entry, next);
		event_config_entry_free(entry);
	}
	mm_free(cfg);
}

int
event_config_set_flag(struct event_config *cfg, int flag)
{
	if (!cfg)
		return -1;
	cfg->flags |= flag;
	return 0;
}

int
event_config_avoid_method(struct event_config *cfg, const char *method)
{
	struct event_config_entry *entry = mm_malloc(sizeof(*entry));
	if (entry == NULL)
		return (-1);

	if ((entry->avoid_method = mm_strdup(method)) == NULL) {
		mm_free(entry);
		return (-1);
	}

	TAILQ_INSERT_TAIL(&cfg->entries, entry, next);

	return (0);
}

int
event_config_require_features(struct event_config *cfg,
    int features)
{
	if (!cfg)
		return (-1);
	cfg->require_features = features;
	return (0);
}

int
event_config_set_num_cpus_hint(struct event_config *cfg, int cpus)
{
	if (!cfg)
		return (-1);
	cfg->n_cpus_hint = cpus;
	return (0);
}

int
event_config_set_max_dispatch_interval(struct event_config *cfg,
    const struct timeval *max_interval, int max_callbacks, int min_priority)
{
	if (max_interval)
		memcpy(&cfg->max_dispatch_interval, max_interval,
		    sizeof(struct timeval));
	else
		cfg->max_dispatch_interval.tv_sec = -1;
	cfg->max_dispatch_callbacks =
	    max_callbacks >= 0 ? max_callbacks : INT_MAX;
	if (min_priority < 0)
		min_priority = 0;
	cfg->limit_callbacks_after_prio = min_priority;
	return (0);
}

int
event_priority_init(int npriorities)
{
	return event_base_priority_init(current_base, npriorities);
}

int
event_base_priority_init(struct event_base *base, int npriorities)
{
	int i, r;
	r = -1;

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	//由N_ACTIVE_CALLBACKS宏可知,本函数应该在event_base_dispatch之前调用,否则将无法设置
	if (N_ACTIVE_CALLBACKS(base) || npriorities < 1
	    || npriorities >= EVENT_MAX_PRIORITIES)
		goto err;

	//之前和现在设置的优先级是一样的
	if (npriorities == base->nactivequeues)
		goto ok;

	//释放之前的,因为N_ACTIVE_CALLBACKS,所以没有active的event
	//可以随便mm_free
	if (base->nactivequeues) {
		mm_free(base->activequeues);
		base->nactivequeues = 0;
	}

	/* Allocate our priority queues */
	//分配一个优先级数组
	base->activequeues = (struct evcallback_list *)
	  mm_calloc(npriorities, sizeof(struct evcallback_list));
	if (base->activequeues == NULL) {
		event_warn("%s: calloc", __func__);
		goto err;
	}
	base->nactivequeues = npriorities;

	for (i = 0; i < base->nactivequeues; ++i) {
		TAILQ_INIT(&base->activequeues[i]);
	}

ok:
	r = 0;
err:
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return (r);
}

int
event_base_get_npriorities(struct event_base *base)
{

	int n;
	if (base == NULL)
		base = current_base;

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	n = base->nactivequeues;
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return (n);
}

int
event_base_get_num_events(struct event_base *base, unsigned int type)
{
	int r = 0;

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	if (type & EVENT_BASE_COUNT_ACTIVE)
		r += base->event_count_active;

	if (type & EVENT_BASE_COUNT_VIRTUAL)
		r += base->virtual_event_count;

	if (type & EVENT_BASE_COUNT_ADDED)
		r += base->event_count;

	EVBASE_RELEASE_LOCK(base, th_base_lock);

	return r;
}

int
event_base_get_max_events(struct event_base *base, unsigned int type, int clear)
{
	int r = 0;

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	if (type & EVENT_BASE_COUNT_ACTIVE) {
		r += base->event_count_active_max;
		if (clear)
			base->event_count_active_max = 0;
	}

	if (type & EVENT_BASE_COUNT_VIRTUAL) {
		r += base->virtual_event_count_max;
		if (clear)
			base->virtual_event_count_max = 0;
	}

	if (type & EVENT_BASE_COUNT_ADDED) {
		r += base->event_count_max;
		if (clear)
			base->event_count_max = 0;
	}

	EVBASE_RELEASE_LOCK(base, th_base_lock);

	return r;
}

/* Returns true iff we're currently watching any events. */
static int
event_haveevents(struct event_base *base)
{
	/* Caller must hold th_base_lock */
	return (base->virtual_event_count > 0 || base->event_count > 0);
}

/* "closure" function called when processing active signal events */
static inline void
event_signal_closure(struct event_base *base, struct event *ev)
{
	short ncalls;
	int should_break;

	/* Allows deletes to work */
	ncalls = ev->ev_ncalls;
	if (ncalls != 0)
		ev->ev_pncalls = &ncalls;
	//while循环里面会调用用户设置的回调函数,该回调函数可能会执行很久
	//所以先解锁
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	//如果信号发生了多次,那么就需要多次执行回调函数
	while (ncalls) {
		ncalls--;
		ev->ev_ncalls = ncalls;
		if (ncalls == 0)
			ev->ev_pncalls = NULL;
		(*ev->ev_callback)(ev->ev_fd, ev->ev_res, ev->ev_arg);

		//其他线程可能调用event_base_lppobreak函数,中断事件循环
		//故检测一下
		EVBASE_ACQUIRE_LOCK(base, th_base_lock);
		should_break = base->event_break;
		EVBASE_RELEASE_LOCK(base, th_base_lock);

		if (should_break) {
			if (ncalls != 0)
				ev->ev_pncalls = NULL;
			return;
		}
	}
}

/* Common timeouts are special timeouts that are handled as queues rather than
 * in the minheap.  This is more efficient than the minheap if we happen to
 * know that we're going to get several thousands of timeout events all with
 * the same timeout value.
 *
 * Since all our timeout handling code assumes timevals can be copied,
 * assigned, etc, we can't use "magic pointer" to encode these common
 * timeouts.  Searching through a list to see if every timeout is common could
 * also get inefficient.  Instead, we take advantage of the fact that tv_usec
 * is 32 bits long, but only uses 20 of those bits (since it can never be over
 * 999999.)  We use the top bits to encode 4 bites of magic number, and 8 bits
 * of index into the event_base's aray of common timeouts.
 */

#define MICROSECONDS_MASK       COMMON_TIMEOUT_MICROSECONDS_MASK
#define COMMON_TIMEOUT_IDX_MASK 0x0ff00000
#define COMMON_TIMEOUT_IDX_SHIFT 20
#define COMMON_TIMEOUT_MASK     0xf0000000
#define COMMON_TIMEOUT_MAGIC    0x50000000

#define COMMON_TIMEOUT_IDX(tv) \
	(((tv)->tv_usec & COMMON_TIMEOUT_IDX_MASK)>>COMMON_TIMEOUT_IDX_SHIFT)

/** Return true iff if 'tv' is a common timeout in 'base' */
static inline int
is_common_timeout(const struct timeval *tv,
    const struct event_base *base)
{
	int idx;
	//不具有common-timeout标志位,那么就肯定不是commont-timeout时间了
	if ((tv->tv_usec & COMMON_TIMEOUT_MASK) != COMMON_TIMEOUT_MAGIC)
		return 0;
	//获取数组下标
	idx = COMMON_TIMEOUT_IDX(tv);
	return idx < base->n_common_timeouts;
}

/* True iff tv1 and tv2 have the same common-timeout index, or if neither
 * one is a common timeout. */
static inline int
is_same_common_timeout(const struct timeval *tv1, const struct timeval *tv2)
{
	return (tv1->tv_usec & ~MICROSECONDS_MASK) ==
	    (tv2->tv_usec & ~MICROSECONDS_MASK);
}

/** Requires that 'tv' is a common timeout.  Return the corresponding
 * common_timeout_list. */
static inline struct common_timeout_list *
get_common_timeout_list(struct event_base *base, const struct timeval *tv)
{
	return base->common_timeout_queues[COMMON_TIMEOUT_IDX(tv)];
}

#if 0
static inline int
common_timeout_ok(const struct timeval *tv,
    struct event_base *base)
{
	const struct timeval *expect =
	    &get_common_timeout_list(base, tv)->duration;
	return tv->tv_sec == expect->tv_sec &&
	    tv->tv_usec == expect->tv_usec;
}
#endif

/* Add the timeout for the first event in given common timeout list to the
 * event_base's minheap. */
static void
common_timeout_schedule(struct common_timeout_list *ctl,
    const struct timeval *now, struct event *head)
{
	struct timeval timeout = head->ev_timeout;
	timeout.tv_usec &= MICROSECONDS_MASK;		//清除common-timeout标志
	//用common_timeout_list结构体的一个event成员作为超时event调用event_add_nolock_
	//由于已经清除了common-timeout标志,所以这次将插入到小根堆中
	event_add_nolock_(&ctl->timeout_event, &timeout, 1);
}

/* Callback: invoked when the timeout for a common timeout queue triggers.
 * This means that (at least) the first event in that queue should be run,
 * and the timeout should be rescheduled if there are more events. */
static void
common_timeout_callback(evutil_socket_t fd, short what, void *arg)
{
	struct timeval now;
	struct common_timeout_list *ctl = arg;
	struct event_base *base = ctl->base;
	struct event *ev = NULL;
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	gettime(base, &now);
	while (1) {
		ev = TAILQ_FIRST(&ctl->events);

		//该超时event还没到超时时间,不用检查其他了,因为是升序的
		if (!ev || ev->ev_timeout.tv_sec > now.tv_sec ||
		    (ev->ev_timeout.tv_sec == now.tv_sec &&
			(ev->ev_timeout.tv_usec&MICROSECONDS_MASK) > now.tv_usec))
			break;

		//一系列的删除操作,包括从这个超时event队列中删除
		event_del_nolock_(ev, EVENT_DEL_NOBLOCK);
		//手动激活超时event,注意:这个ev是用户的超时event
		event_active_nolock_(ev, EV_TIMEOUT, 1);
	}

	//不是NULL,说明该队列还有超时event,需要再次common_timeout_schedule,进行监听
	if (ev)
		common_timeout_schedule(ctl, &now, ev);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

#define MAX_COMMON_TIMEOUTS 256

//申请一个时长为duration的common_timeout_list
const struct timeval *
event_base_init_common_timeout(struct event_base *base,
    const struct timeval *duration)
{
	int i;
	struct timeval tv;
	const struct timeval *result=NULL;
	struct common_timeout_list *new_ctl;

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	
	//这个时间的微秒位应该进位,用户没有将之进位.
	//比如二进制的103,各位的3需进位
	if (duration->tv_usec > 1000000) {
		//将之进位,因为下面会用到高位
		memcpy(&tv, duration, sizeof(struct timeval));
		if (is_common_timeout(duration, base))
			tv.tv_usec &= MICROSECONDS_MASK;	//去除common-timeout标志
		tv.tv_sec += tv.tv_usec / 1000000;		//进位
		tv.tv_usec %= 1000000;
		duration = &tv;
	}
	for (i = 0; i < base->n_common_timeouts; ++i) {
		const struct common_timeout_list *ctl =
		    base->common_timeout_queues[i];
		//具有相同的duration,即之前有申请过这个超时时长,那么就不用分配空间
		if (duration->tv_sec == ctl->duration.tv_sec &&
		    duration->tv_usec ==
		    (ctl->duration.tv_usec & MICROSECONDS_MASK)) { //要&这个宏才是正确的时间
			EVUTIL_ASSERT(is_common_timeout(&ctl->duration, base));
			result = &ctl->duration;
			goto done;
		}
	}

	//达到了最大申请个数,不能再分配了
	if (base->n_common_timeouts == MAX_COMMON_TIMEOUTS) {
		event_warnx("%s: Too many common timeouts already in use; "
		    "we only support %d per event_base", __func__,
		    MAX_COMMON_TIMEOUTS);
		goto done;
	}

	//新的超时时长,需要分配一个common_timeout_list结构体
	//之前分配的空间已经用完了,要重新申请空间
	if (base->n_common_timeouts_allocated == base->n_common_timeouts) {
		int n = base->n_common_timeouts < 16 ? 16 :
		    base->n_common_timeouts*2;
		struct common_timeout_list **newqueues =
		    mm_realloc(base->common_timeout_queues,
			n*sizeof(struct common_timeout_queue *));
		if (!newqueues) {
			event_warn("%s: realloc",__func__);
			goto done;
		}
		base->n_common_timeouts_allocated = n;
		base->common_timeout_queues = newqueues;
	}

	//为该超时时长分配一个common_timeout_list结构体
	new_ctl = mm_calloc(1, sizeof(struct common_timeout_list));
	if (!new_ctl) {
		event_warn("%s: calloc",__func__);
		goto done;
	}

	//为这个结构体进行一些设置
	TAILQ_INIT(&new_ctl->events);
	new_ctl->duration.tv_sec = duration->tv_sec;
	new_ctl->duration.tv_usec =
	    duration->tv_usec | COMMON_TIMEOUT_MAGIC |				//为这个时间加上common-timeout标志
	    (base->n_common_timeouts << COMMON_TIMEOUT_IDX_SHIFT);	//加入下标值

	//对timeout_event这个内部event进行赋值,设置回调函数和回调参数
	evtimer_assign(&new_ctl->timeout_event, base,
	    common_timeout_callback, new_ctl);
	new_ctl->timeout_event.ev_flags |= EVLIST_INTERNAL;			//标志成内部event
	event_priority_set(&new_ctl->timeout_event, 0);				//优先级为最高级
	new_ctl->base = base;
	//放到数组对应的位置上
	base->common_timeout_queues[base->n_common_timeouts++] = new_ctl;
	result = &new_ctl->duration;

done:
	if (result)
		EVUTIL_ASSERT(is_common_timeout(result, base));

	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return result;
}

/* Closure function invoked when we're activating a persistent event. */
static inline void
event_persist_closure(struct event_base *base, struct event *ev)
{
	void (*evcb_callback)(evutil_socket_t, short, void *);

        // Other fields of *ev that must be stored before executing
        evutil_socket_t evcb_fd;
        short evcb_res;
        void *evcb_arg;

	/* reschedule the persistent event if we have a timeout. */
	//在event_add_nolock函数中,如果是超时event并且有EV_PERSIST,那么就会把ev_io_timeout
	//设置成用户设置的超时时间(相对时间).否则为0,即不进入判断体中.
	//说明这个if只用于处理具有EV_PERSIST属性的超时event
	if (ev->ev_io_timeout.tv_sec || ev->ev_io_timeout.tv_usec) {
		/* If there was a timeout, we want it to run at an interval of
		 * ev_io_timeout after the last time it was _scheduled_ for,
		 * not ev_io_timeout after _now_.  If it fired for another
		 * reason, though, the timeout ought to start ticking _now_. */
		struct timeval run_at, relative_to, delay, now;
		ev_uint32_t usec_mask = 0;
		EVUTIL_ASSERT(is_same_common_timeout(&ev->ev_timeout,
			&ev->ev_io_timeout));
		gettime(base, &now);
		if (is_common_timeout(&ev->ev_timeout, base)) {
			//delay是用户设置的超时时间,event_add的第二个参数
			delay = ev->ev_io_timeout;
			usec_mask = delay.tv_usec & ~MICROSECONDS_MASK;
			delay.tv_usec &= MICROSECONDS_MASK;
			//是因为超时才执行到这里,event可以同时监听多种事件.
			//如果是因为可读而执行到这里,那么就说明还没超时
			if (ev->ev_res & EV_TIMEOUT) {
				//如果是因为超时而激活,那么下次超时的时间就是本次超时的
				relative_to = ev->ev_timeout;	//加上delay
				relative_to.tv_usec &= MICROSECONDS_MASK;
			} else {
				relative_to = now;		//重新计算超时值
			}
		} else {
			delay = ev->ev_io_timeout;
			if (ev->ev_res & EV_TIMEOUT) {
				relative_to = ev->ev_timeout;
			} else {
				relative_to = now;
			}
		}
		evutil_timeradd(&relative_to, &delay, &run_at);

		//无论relative是哪个事件,run_at都不应该小于now.
		//如果小于,则说明是用户手动修改了系统时间,使得gettime()函数获取了一个
		//之前的时间.比如现在是9点,用户调回了7点
		if (evutil_timercmp(&run_at, &now, <)) {
			/* Looks like we missed at least one invocation due to
			 * a clock jump, not running the event loop for a
			 * while, really slow callbacks, or
			 * something. Reschedule relative to now.
			 */
			//那么就以新的系统事件为准
			evutil_timeradd(&now, &delay, &run_at);
		}
		run_at.tv_usec |= usec_mask;
		//把这个event再次添加到event_base中.注意:此时第三个参数是1,说明是一个绝对时间
		event_add_nolock_(ev, &run_at, 1);
	}

	// Save our callback before we release the lock
	evcb_callback = ev->ev_callback;
        evcb_fd = ev->ev_fd;
        evcb_res = ev->ev_res;
        evcb_arg = ev->ev_arg;

	// Release the lock
 	EVBASE_RELEASE_LOCK(base, th_base_lock);

	// Execute the callback
        (evcb_callback)(evcb_fd, evcb_res, evcb_arg);
}

/*
  Helper for event_process_active to process all the events in a single queue,
  releasing the lock as we go.  This function requires that the lock be held
  when it's invoked.  Returns -1 if we get a signal or an event_break that
  means we should stop processing any active events now.  Otherwise returns
  the number of non-internal event_callbacks that we processed.
*/
static int
event_process_active_single_queue(struct event_base *base,
    struct evcallback_list *activeq,
    int max_to_process, const struct timeval *endtime)
{
	struct event_callback *evcb;
	int count = 0;

	EVUTIL_ASSERT(activeq != NULL);

	//遍历同一优先级的所有event
	for (evcb = TAILQ_FIRST(activeq); evcb; evcb = TAILQ_FIRST(activeq)) {
		struct event *ev=NULL;
		if (evcb->evcb_flags & EVLIST_INIT) {
			ev = event_callback_to_event(evcb);

			//这个if-else是用于IO event的
			//如果是永久事件,那么只需从active队列中删除
			if (ev->ev_events & EV_PERSIST || ev->ev_flags & EVLIST_FINALIZING)
				event_queue_remove_active(base, evcb);
			else	//不是的话,那么就要把这个event删除掉
				event_del_nolock_(ev, EVENT_DEL_NOBLOCK);
			event_debug((
			    "event_process_active: event: %p, %s%s%scall %p",
			    ev,
			    ev->ev_res & EV_READ ? "EV_READ " : " ",
			    ev->ev_res & EV_WRITE ? "EV_WRITE " : " ",
			    ev->ev_res & EV_CLOSED ? "EV_CLOSED " : " ",
			    ev->ev_callback));
		} else {
			event_queue_remove_active(base, evcb);
			event_debug(("event_process_active: event_callback %p, "
				"closure %d, call %p",
				evcb, evcb->evcb_closure, evcb->evcb_cb_union.evcb_callback));
		}

		if (!(evcb->evcb_flags & EVLIST_INTERNAL))
			++count;


		base->current_event = evcb;
#ifndef EVENT__DISABLE_THREAD_SUPPORT
		base->current_event_waiters = 0;
#endif

		switch (evcb->evcb_closure) {
		case EV_CLOSURE_EVENT_SIGNAL:
			EVUTIL_ASSERT(ev != NULL);
			event_signal_closure(base, ev);
			break;

			//这个case只对超时event的EV_PERSIST有用,IO的没用
		case EV_CLOSURE_EVENT_PERSIST:
			EVUTIL_ASSERT(ev != NULL);
			event_persist_closure(base, ev);
			break;
		case EV_CLOSURE_EVENT: {
			void (*evcb_callback)(evutil_socket_t, short, void *);
			short res;
			EVUTIL_ASSERT(ev != NULL);
			evcb_callback = *ev->ev_callback;
			res = ev->ev_res;
			EVBASE_RELEASE_LOCK(base, th_base_lock);
			evcb_callback(ev->ev_fd, res, ev->ev_arg);
		}
		break;
		case EV_CLOSURE_CB_SELF: {
			void (*evcb_selfcb)(struct event_callback *, void *) = evcb->evcb_cb_union.evcb_selfcb;
			EVBASE_RELEASE_LOCK(base, th_base_lock);
			evcb_selfcb(evcb, evcb->evcb_arg);
		}
		break;
		case EV_CLOSURE_EVENT_FINALIZE:
		case EV_CLOSURE_EVENT_FINALIZE_FREE: {
			void (*evcb_evfinalize)(struct event *, void *);
			int evcb_closure = evcb->evcb_closure;
			EVUTIL_ASSERT(ev != NULL);
			base->current_event = NULL;
			evcb_evfinalize = ev->ev_evcallback.evcb_cb_union.evcb_evfinalize;
			EVUTIL_ASSERT((evcb->evcb_flags & EVLIST_FINALIZING));
			EVBASE_RELEASE_LOCK(base, th_base_lock);
			evcb_evfinalize(ev, ev->ev_arg);
			event_debug_note_teardown_(ev);
			if (evcb_closure == EV_CLOSURE_EVENT_FINALIZE_FREE)
				mm_free(ev);
		}
		break;
		case EV_CLOSURE_CB_FINALIZE: {
			void (*evcb_cbfinalize)(struct event_callback *, void *) = evcb->evcb_cb_union.evcb_cbfinalize;
			base->current_event = NULL;
			EVUTIL_ASSERT((evcb->evcb_flags & EVLIST_FINALIZING));
			EVBASE_RELEASE_LOCK(base, th_base_lock);
			evcb_cbfinalize(evcb, evcb->evcb_arg);
		}
		break;
		default:
			EVUTIL_ASSERT(0);
		}

		EVBASE_ACQUIRE_LOCK(base, th_base_lock);
		base->current_event = NULL;
#ifndef EVENT__DISABLE_THREAD_SUPPORT
		if (base->current_event_waiters) {
			base->current_event_waiters = 0;
			EVTHREAD_COND_BROADCAST(base->current_event_cond);
		}
#endif

		if (base->event_break)
			return -1;
		if (count >= max_to_process)
			return count;
		if (count && endtime) {
			struct timeval now;
			update_time_cache(base);
			gettime(base, &now);
			if (evutil_timercmp(&now, endtime, >=))
				return count;
		}
		if (base->event_continue)
			break;
	}
	return count;
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */

static int
event_process_active(struct event_base *base)
{
	/* Caller must hold th_base_lock */
	struct evcallback_list *activeq = NULL;
	int i, c = 0;
	const struct timeval *endtime;
	struct timeval tv;
	const int maxcb = base->max_dispatch_callbacks;
	const int limit_after_prio = base->limit_callbacks_after_prio;
	if (base->max_dispatch_time.tv_sec >= 0) {
		update_time_cache(base);
		gettime(base, &tv);
		evutil_timeradd(&base->max_dispatch_time, &tv, &tv);
		endtime = &tv;
	} else {
		endtime = NULL;
	}

	//从高优先级到低优先级遍历优先级数组
	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(&base->activequeues[i]) != NULL) {
			base->event_running_priority = i;
			activeq = &base->activequeues[i];
			if (i < limit_after_prio)
				c = event_process_active_single_queue(base, activeq,
				    INT_MAX, NULL);
			else
				c = event_process_active_single_queue(base, activeq,
				    maxcb, endtime);
			if (c < 0) {
				goto done;
			} else if (c > 0)
				break; /* Processed a real event; do not
					* consider lower-priority events */
			/* If we get here, all of the events we processed
			 * were internal.  Continue. */
		}
	}

done:
	base->event_running_priority = -1;

	return c;
}

/*
 * Wait continuously for events.  We exit only if no events are left.
 */

int
event_dispatch(void)
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)
{
	return (event_base_loop(event_base, 0));
}

const char *
event_base_get_method(const struct event_base *base)
{
	EVUTIL_ASSERT(base);
	return (base->evsel->name);
}

/** Callback: used to implement event_base_loopexit by telling the event_base
 * that it's time to exit its loop. */
static void
event_loopexit_cb(evutil_socket_t fd, short what, void *arg)
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

int
event_loopexit(const struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    current_base, tv));
}

int
event_base_loopexit(struct event_base *event_base, const struct timeval *tv)
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,
		    event_base, tv));
}

int
event_loopbreak(void)
{
	return (event_base_loopbreak(current_base));
}

int
event_base_loopbreak(struct event_base *event_base)
{
	int r = 0;
	if (event_base == NULL)
		return (-1);

	EVBASE_ACQUIRE_LOCK(event_base, th_base_lock);
	event_base->event_break = 1;

	if (EVBASE_NEED_NOTIFY(event_base)) {
		r = evthread_notify_base(event_base);
	} else {
		r = (0);
	}
	EVBASE_RELEASE_LOCK(event_base, th_base_lock);
	return r;
}

int
event_base_loopcontinue(struct event_base *event_base)
{
	int r = 0;
	if (event_base == NULL)
		return (-1);

	EVBASE_ACQUIRE_LOCK(event_base, th_base_lock);
	event_base->event_continue = 1;

	if (EVBASE_NEED_NOTIFY(event_base)) {
		r = evthread_notify_base(event_base);
	} else {
		r = (0);
	}
	EVBASE_RELEASE_LOCK(event_base, th_base_lock);
	return r;
}

int
event_base_got_break(struct event_base *event_base)
{
	int res;
	EVBASE_ACQUIRE_LOCK(event_base, th_base_lock);
	res = event_base->event_break;
	EVBASE_RELEASE_LOCK(event_base, th_base_lock);
	return res;
}

int
event_base_got_exit(struct event_base *event_base)
{
	int res;
	EVBASE_ACQUIRE_LOCK(event_base, th_base_lock);
	res = event_base->event_gotterm;
	EVBASE_RELEASE_LOCK(event_base, th_base_lock);
	return res;
}

/* not thread safe */

int
event_loop(int flags)
{
	return event_base_loop(current_base, flags);
}

int
event_base_loop(struct event_base *base, int flags)
{
	const struct eventop *evsel = base->evsel;
	struct timeval tv;
	struct timeval *tv_p;
	int res, done, retval = 0;

	/* Grab the lock.  We will release it inside evsel.dispatch, and again
	 * as we invoke user callbacks. */
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	if (base->running_loop) {
		event_warnx("%s: reentrant invocation.  Only one event_base_loop"
		    " can run on each event_base at once.", __func__);
		EVBASE_RELEASE_LOCK(base, th_base_lock);
		return -1;
	}

	base->running_loop = 1;

	//要使用cache时间,在配置event_base时,不能加入EVENT_BASE_FLAG_NO_CACHE_TIME选项
	clear_time_cache(base);

	if (base->sig.ev_signal_added && base->sig.ev_n_signals_added)
		evsig_set_base_(base);

	done = 0;

#ifndef EVENT__DISABLE_THREAD_SUPPORT
	base->th_owner_id = EVTHREAD_GET_ID();
#endif

	base->event_gotterm = base->event_break = 0;

	while (!done) {
		base->event_continue = 0;
		base->n_deferreds_queued = 0;

		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) {
			break;
		}

		if (base->event_break) {
			break;
		}

		tv_p = &tv;
		if (!N_ACTIVE_CALLBACKS(base) && !(flags & EVLOOP_NONBLOCK)) {
			//获取超时值
			timeout_next(base, &tv_p);
		} else {
			/*
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			//把等待时间设置为0,即可不进行等待,马上触发事件
			evutil_timerclear(&tv);
		}

		/* If we have no events, we just exit */
		if (0==(flags&EVLOOP_NO_EXIT_ON_EMPTY) &&
		    !event_haveevents(base) && !N_ACTIVE_CALLBACKS(base)) {
			event_debug(("%s: no events registered.", __func__));
			retval = 1;
			goto done;
		}

		event_queue_make_later_events_active(base);

		//之所以要在进入dispatch之前清零,是因为进入dispatch后,可能会等待一段时间.
		//cache的时间就很不准确了. 如果其他线程此时想add一个event到这个event_base里,
		//在event_add_internal函数里会调用gettime. 如果chche不清零,那么会使用这个cache
		//时间,这将是一个很不准确的时间
		clear_time_cache(base);

		//调用多路复用函数,对event进行监听,并且把满足条件的event放到event_base的激活队列中
		res = evsel->dispatch(base, tv_p);

		if (res == -1) {
			event_debug(("%s: dispatch returned unsuccessfully.",
				__func__));
			retval = -1;
			goto done;
		}

		//缓存时间(将系统时间赋值到cache中)
		update_time_cache(base);

		//处理超时事件,将超时事件插入到激活链表中
		timeout_process(base);

		if (N_ACTIVE_CALLBACKS(base)) {
			//遍历这个激活队列中的所有event,逐个调用对应的回调函数
			int n = event_process_active(base);
			if ((flags & EVLOOP_ONCE)
			    && N_ACTIVE_CALLBACKS(base) == 0
			    && n != 0)
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)
			done = 1;
	}	
	event_debug(("%s: asked to terminate loop.", __func__));

done:
	clear_time_cache(base);
	base->running_loop = 0;

	EVBASE_RELEASE_LOCK(base, th_base_lock);

	return (retval);
}

/* One-time callback to implement event_base_once: invokes the user callback,
 * then deletes the allocated storage */
static void
event_once_cb(evutil_socket_t fd, short events, void *arg)
{
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	EVBASE_ACQUIRE_LOCK(eonce->ev.ev_base, th_base_lock);
	LIST_REMOVE(eonce, next_once);
	EVBASE_RELEASE_LOCK(eonce->ev.ev_base, th_base_lock);
	event_debug_unassign(&eonce->ev);
	mm_free(eonce);
}

/* not threadsafe, event scheduled once. */
int
event_once(evutil_socket_t fd, short events,
    void (*callback)(evutil_socket_t, short, void *),
    void *arg, const struct timeval *tv)
{
	return event_base_once(current_base, fd, events, callback, arg, tv);
}

/* Schedules an event once */
int
event_base_once(struct event_base *base, evutil_socket_t fd, short events,
    void (*callback)(evutil_socket_t, short, void *),
    void *arg, const struct timeval *tv)
{
	struct event_once *eonce;
	int res = 0;
	int activate = 0;

	/* We cannot support signals that just fire once, or persistent
	 * events. */
	if (events & (EV_SIGNAL|EV_PERSIST))
		return (-1);

	if ((eonce = mm_calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;

	if ((events & (EV_TIMEOUT|EV_SIGNAL|EV_READ|EV_WRITE|EV_CLOSED)) == EV_TIMEOUT) {
		evtimer_assign(&eonce->ev, base, event_once_cb, eonce);

		if (tv == NULL || ! evutil_timerisset(tv)) {
			/* If the event is going to become active immediately,
			 * don't put it on the timeout queue.  This is one
			 * idiom for scheduling a callback, so let's make
			 * it fast (and order-preserving). */
			activate = 1;
		}
	} else if (events & (EV_READ|EV_WRITE|EV_CLOSED)) {
		events &= EV_READ|EV_WRITE|EV_CLOSED;

		event_assign(&eonce->ev, base, fd, events, event_once_cb, eonce);
	} else {
		/* Bad event combination */
		mm_free(eonce);
		return (-1);
	}

	if (res == 0) {
		EVBASE_ACQUIRE_LOCK(base, th_base_lock);
		if (activate)
			event_active_nolock_(&eonce->ev, EV_TIMEOUT, 1);
		else
			res = event_add_nolock_(&eonce->ev, tv, 0);

		if (res != 0) {
			mm_free(eonce);
			return (res);
		} else {
			LIST_INSERT_HEAD(&base->once_events, eonce, next_once);
		}
		EVBASE_RELEASE_LOCK(base, th_base_lock);
	}

	return (0);
}

int
event_assign(struct event *ev, struct event_base *base, evutil_socket_t fd, short events, void (*callback)(evutil_socket_t, short, void *), void *arg)
{
	if (!base)
		base = current_base;
	if (arg == &event_self_cbarg_ptr_)
		arg = ev;

	if (!(events & EV_SIGNAL))
		event_debug_assert_socket_nonblocking_(fd);
	event_debug_assert_not_added_(ev);

	ev->ev_base = base;

	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;
	ev->ev_res = 0;
	ev->ev_flags = EVLIST_INIT;
	ev->ev_ncalls = 0;
	ev->ev_pncalls = NULL;

	if (events & EV_SIGNAL) {
		if ((events & (EV_READ|EV_WRITE|EV_CLOSED)) != 0) {
			event_warnx("%s: EV_SIGNAL is not compatible with "
			    "EV_READ, EV_WRITE or EV_CLOSED", __func__);
			return -1;
		}
		ev->ev_closure = EV_CLOSURE_EVENT_SIGNAL;
	} else {
		if (events & EV_PERSIST) {
			evutil_timerclear(&ev->ev_io_timeout);
			ev->ev_closure = EV_CLOSURE_EVENT_PERSIST;
		} else {
			ev->ev_closure = EV_CLOSURE_EVENT;
		}
	}

	min_heap_elem_init_(ev);

	if (base != NULL) {
		/* by default, we put new events into the middle priority */
		ev->ev_pri = base->nactivequeues / 2;
	}

	event_debug_note_setup_(ev);

	return 0;
}

int
event_base_set(struct event_base *base, struct event *ev)
{
	/* Only innocent events may be assigned to a different base */
	if (ev->ev_flags != EVLIST_INIT)
		return (-1);

	event_debug_assert_is_setup_(ev);

	ev->ev_base = base;
	ev->ev_pri = base->nactivequeues/2;

	return (0);
}

void
event_set(struct event *ev, evutil_socket_t fd, short events,
	  void (*callback)(evutil_socket_t, short, void *), void *arg)
{
	int r;
	r = event_assign(ev, current_base, fd, events, callback, arg);
	EVUTIL_ASSERT(r == 0);
}

void *
event_self_cbarg(void)
{
	return &event_self_cbarg_ptr_;
}

struct event *
event_base_get_running_event(struct event_base *base)
{
	struct event *ev = NULL;
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	if (EVBASE_IN_THREAD(base)) {
		struct event_callback *evcb = base->current_event;
		if (evcb->evcb_flags & EVLIST_INIT)
			ev = event_callback_to_event(evcb);
	}
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return ev;
}

struct event *
event_new(struct event_base *base, evutil_socket_t fd, short events, void (*cb)(evutil_socket_t, short, void *), void *arg)
{
	struct event *ev;
	ev = mm_malloc(sizeof(struct event));
	if (ev == NULL)
		return (NULL);
	if (event_assign(ev, base, fd, events, cb, arg) < 0) {
		mm_free(ev);
		return (NULL);
	}

	return (ev);
}

void
event_free(struct event *ev)
{
	/* This is disabled, so that events which have been finalized be a
	 * valid target for event_free(). That's */
	// event_debug_assert_is_setup_(ev);

	/* make sure that this event won't be coming back to haunt us. */
	event_del(ev);
	event_debug_note_teardown_(ev);
	mm_free(ev);

}

void
event_debug_unassign(struct event *ev)
{
	event_debug_assert_not_added_(ev);
	event_debug_note_teardown_(ev);

	ev->ev_flags &= ~EVLIST_INIT;
}

#define EVENT_FINALIZE_FREE_ 0x10000
static int
event_finalize_nolock_(struct event_base *base, unsigned flags, struct event *ev, event_finalize_callback_fn cb)
{
	ev_uint8_t closure = (flags & EVENT_FINALIZE_FREE_) ?
	    EV_CLOSURE_EVENT_FINALIZE_FREE : EV_CLOSURE_EVENT_FINALIZE;

	event_del_nolock_(ev, EVENT_DEL_NOBLOCK);
	ev->ev_closure = closure;
	ev->ev_evcallback.evcb_cb_union.evcb_evfinalize = cb;
	event_active_nolock_(ev, EV_FINALIZE, 1);
	ev->ev_flags |= EVLIST_FINALIZING;
	return 0;
}

static int
event_finalize_impl_(unsigned flags, struct event *ev, event_finalize_callback_fn cb)
{
	int r;
	struct event_base *base = ev->ev_base;
	if (EVUTIL_FAILURE_CHECK(!base)) {
		event_warnx("%s: event has no event_base set.", __func__);
		return -1;
	}

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	r = event_finalize_nolock_(base, flags, ev, cb);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return r;
}

int
event_finalize(unsigned flags, struct event *ev, event_finalize_callback_fn cb)
{
	return event_finalize_impl_(flags, ev, cb);
}

int
event_free_finalize(unsigned flags, struct event *ev, event_finalize_callback_fn cb)
{
	return event_finalize_impl_(flags|EVENT_FINALIZE_FREE_, ev, cb);
}

void
event_callback_finalize_nolock_(struct event_base *base, unsigned flags, struct event_callback *evcb, void (*cb)(struct event_callback *, void *))
{
	struct event *ev = NULL;
	if (evcb->evcb_flags & EVLIST_INIT) {
		ev = event_callback_to_event(evcb);
		event_del_nolock_(ev, EVENT_DEL_NOBLOCK);
	} else {
		event_callback_cancel_nolock_(base, evcb, 0); /*XXX can this fail?*/
	}

	evcb->evcb_closure = EV_CLOSURE_CB_FINALIZE;
	evcb->evcb_cb_union.evcb_cbfinalize = cb;
	event_callback_activate_nolock_(base, evcb); /* XXX can this really fail?*/
	evcb->evcb_flags |= EVLIST_FINALIZING;
}

void
event_callback_finalize_(struct event_base *base, unsigned flags, struct event_callback *evcb, void (*cb)(struct event_callback *, void *))
{
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	event_callback_finalize_nolock_(base, flags, evcb, cb);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

/** Internal: Finalize all of the n_cbs callbacks in evcbs.  The provided
 * callback will be invoked on *one of them*, after they have *all* been
 * finalized. */
int
event_callback_finalize_many_(struct event_base *base, int n_cbs, struct event_callback **evcbs, void (*cb)(struct event_callback *, void *))
{
	int n_pending = 0, i;

	if (base == NULL)
		base = current_base;

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	event_debug(("%s: %d events finalizing", __func__, n_cbs));

	/* At most one can be currently executing; the rest we just
	 * cancel... But we always make sure that the finalize callback
	 * runs. */
	for (i = 0; i < n_cbs; ++i) {
		struct event_callback *evcb = evcbs[i];
		if (evcb == base->current_event) {
			event_callback_finalize_nolock_(base, 0, evcb, cb);
			++n_pending;
		} else {
			event_callback_cancel_nolock_(base, evcb, 0);
		}
	}

	if (n_pending == 0) {
		/* Just do the first one. */
		event_callback_finalize_nolock_(base, 0, evcbs[0], cb);
	}

	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return 0;
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */

int
event_priority_set(struct event *ev, int pri)
{
	event_debug_assert_is_setup_(ev);

	//由此可知,需在调用event_base_dispatch前设置优先级,因为一旦
	//调用event_base_dispatch,那么event就随时可能变成EVLIST_ACTIVE状态
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);

	//优先级不能越界
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);

	//pri值越小,其优先级就越高
	ev->ev_pri = pri;

	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */

int
event_pending(const struct event *ev, short event, struct timeval *tv)
{
	int flags = 0;

	if (EVUTIL_FAILURE_CHECK(ev->ev_base == NULL)) {
		event_warnx("%s: event has no event_base set.", __func__);
		return 0;
	}

	EVBASE_ACQUIRE_LOCK(ev->ev_base, th_base_lock);
	event_debug_assert_is_setup_(ev);

	//flags记录用户监听了哪些事件
	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_CLOSED|EV_SIGNAL));

	//falsgs记录event被什么事件激活了,用户可以调用event_active
	//手动激活event,并且可以使用之前用户没有监听的事件作为激活原因
	if (ev->ev_flags & (EVLIST_ACTIVE|EVLIST_ACTIVE_LATER))
		flags |= ev->ev_res;

	//记录该event是否有超时属性
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;

	//event可以被用户乱设置值,然后作为参数.这里为了保证其值只能是下面的事件
	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_CLOSED|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) {
		struct timeval tmp = ev->ev_timeout;
		tmp.tv_usec &= MICROSECONDS_MASK;
		/* correctly remamp to real time */
		evutil_timeradd(&ev->ev_base->tv_clock_diff, &tmp, tv);
	}

	EVBASE_RELEASE_LOCK(ev->ev_base, th_base_lock);

	return (flags & event);
}

//检测是否已初始化
int
event_initialized(const struct event *ev)
{
	if (!(ev->ev_flags & EVLIST_INIT))
		return 0;

	return 1;
}

//一个函数获取所有,如果不想获取某个参数,传入NULL即可
void
event_get_assignment(const struct event *event, struct event_base **base_out, evutil_socket_t *fd_out, short *events_out, event_callback_fn *callback_out, void **arg_out)
{
	event_debug_assert_is_setup_(event);

	if (base_out)
		*base_out = event->ev_base;
	if (fd_out)
		*fd_out = event->ev_fd;
	if (events_out)
		*events_out = event->ev_events;
	if (callback_out)
		*callback_out = event->ev_callback;
	if (arg_out)
		*arg_out = event->ev_arg;
}

size_t
event_get_struct_event_size(void)
{
	return sizeof(struct event);
}

//返回监听的文件描述符fd
evutil_socket_t
event_get_fd(const struct event *ev)
{
	event_debug_assert_is_setup_(ev);
	return ev->ev_fd;
}

//获取event_base
struct event_base *
event_get_base(const struct event *ev)
{
	event_debug_assert_is_setup_(ev);
	return ev->ev_base;
}

//获取该event监听的事件
short
event_get_events(const struct event *ev)
{
	event_debug_assert_is_setup_(ev);
	return ev->ev_events;
}

//获取回调函数
event_callback_fn
event_get_callback(const struct event *ev)
{
	event_debug_assert_is_setup_(ev);
	return ev->ev_callback;
}

//获取回调函数参数
void *
event_get_callback_arg(const struct event *ev)
{
	event_debug_assert_is_setup_(ev);
	return ev->ev_arg;
}

int
event_get_priority(const struct event *ev)
{
	event_debug_assert_is_setup_(ev);
	return ev->ev_pri;
}

int
event_add(struct event *ev, const struct timeval *tv)
{
	int res;

	if (EVUTIL_FAILURE_CHECK(!ev->ev_base)) {
		event_warnx("%s: event has no event_base set.", __func__);
		return -1;
	}

	//加锁
	EVBASE_ACQUIRE_LOCK(ev->ev_base, th_base_lock);

	res = event_add_nolock_(ev, tv, 0);

	//解锁
	EVBASE_RELEASE_LOCK(ev->ev_base, th_base_lock);

	return (res);
}

/* Helper callback: wake an event_base from another thread.  This version
 * works by writing a byte to one end of a socketpair, so that the event_base
 * listening on the other end will wake up as the corresponding event
 * triggers */
static int
evthread_notify_base_default(struct event_base *base)
{
	char buf[1];
	int r;
	buf[0] = (char) 0;
	//用来唤醒,写一个字节足矣
#ifdef _WIN32
	r = send(base->th_notify_fd[1], buf, 1, 0);
#else
	r = write(base->th_notify_fd[1], buf, 1);
#endif
	//即使errno等于EAGAIN也无所谓,因为这是由于通信通道已经塞满了
	//这已经能唤醒主线程了,没必要在写入数据了
	return (r < 0 && ! EVUTIL_ERR_IS_EAGAIN(errno)) ? -1 : 0;
}

#ifdef EVENT__HAVE_EVENTFD
/* Helper callback: wake an event_base from another thread.  This version
 * assumes that you have a working eventfd() implementation. */
static int
evthread_notify_base_eventfd(struct event_base *base)
{
	ev_uint64_t msg = 1;
	int r;
	do {
		r = write(base->th_notify_fd[0], (void*) &msg, sizeof(msg));
	} while (r < 0 && errno == EAGAIN);

	return (r < 0) ? -1 : 0;
}
#endif


/** Tell the thread currently running the event_loop for base (if any) that it
 * needs to stop waiting in its dispatch function (if it is) and process all
 * active callbacks. */
static int
evthread_notify_base(struct event_base *base)
{
	EVENT_BASE_ASSERT_LOCKED(base);
	if (!base->th_notify_fn)
		return -1;

	//写入一个字节就能是event_base被唤醒
	//如果处于未决状态,就没必要多写一个字节
	if (base->is_notify_pending)
		return 0;

	//通知处于未决状态,当event_base醒过来就变成已决的了
	base->is_notify_pending = 1;
	return base->th_notify_fn(base);
}

/* Implementation function to remove a timeout on a currently pending event.
 */
int
event_remove_timer_nolock_(struct event *ev)
{
	struct event_base *base = ev->ev_base;

	EVENT_BASE_ASSERT_LOCKED(base);
	event_debug_assert_is_setup_(ev);

	event_debug(("event_remove_timer_nolock: event: %p", ev));

	/* If it's not pending on a timeout, we don't need to do anything. */
	if (ev->ev_flags & EVLIST_TIMEOUT) {
		event_queue_remove_timeout(base, ev);
		evutil_timerclear(&ev->ev_.ev_io.ev_timeout);
	}

	return (0);
}

int
event_remove_timer(struct event *ev)
{
	int res;

	if (EVUTIL_FAILURE_CHECK(!ev->ev_base)) {
		event_warnx("%s: event has no event_base set.", __func__);
		return -1;
	}

	EVBASE_ACQUIRE_LOCK(ev->ev_base, th_base_lock);

	res = event_remove_timer_nolock_(ev);

	EVBASE_RELEASE_LOCK(ev->ev_base, th_base_lock);

	return (res);
}

/* Implementation function to add an event.  Works just like event_add,
 * except: 1) it requires that we have the lock.  2) if tv_is_absolute is set,
 * we treat tv as an absolute time, not as an interval to add to the current
 * time */
int
event_add_nolock_(struct event *ev, const struct timeval *tv,
    int tv_is_absolute)
{
	struct event_base *base = ev->ev_base;
	int res = 0;
	int notify = 0;

	EVENT_BASE_ASSERT_LOCKED(base);
	event_debug_assert_is_setup_(ev);

	event_debug((
		 "event_add: event: %p (fd "EV_SOCK_FMT"), %s%s%s%scall %p",
		 ev,
		 EV_SOCK_ARG(ev->ev_fd),
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 ev->ev_events & EV_CLOSED ? "EV_CLOSED " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	EVUTIL_ASSERT(!(ev->ev_flags & ~EVLIST_ALL));

	if (ev->ev_flags & EVLIST_FINALIZING) {
		/* XXXX debug */
		return (-1);
	}

	/*
	 * prepare for timeout insertion further below, if we get a
	 * failure on any step, we should not change any state.
	 */
	//tv不为NULL,就说明是一个超时event,在小根堆中为其留一个位置
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
		if (min_heap_reserve_(&base->timeheap,
			1 + min_heap_size_(&base->timeheap)) == -1)
			return (-1);  /* ENOMEM == errno */
	}

	/* If the main thread is currently executing a signal event's
	 * callback, and we are not the main thread, then we want to wait
	 * until the callback is done before we mess with the event, or else
	 * we can race on ev_ncalls and ev_pncalls below. */
#ifndef EVENT__DISABLE_THREAD_SUPPORT
	if (base->current_event == event_to_event_callback(ev) &&
	    (ev->ev_events & EV_SIGNAL)
	    && !EVBASE_IN_THREAD(base)) {
		++base->current_event_waiters;
		EVTHREAD_COND_WAIT(base->current_event_cond, base->th_base_lock);
	}
#endif

	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_CLOSED|EV_SIGNAL)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE|EVLIST_ACTIVE_LATER))) {
		if (ev->ev_events & (EV_READ|EV_WRITE|EV_CLOSED))
			res = evmap_io_add_(base, ev->ev_fd, ev);
		else if (ev->ev_events & EV_SIGNAL)
			res = evmap_signal_add_(base, (int)ev->ev_fd, ev);
		if (res != -1)
			event_queue_insert_inserted(base, ev);
		if (res == 1) {
			/* evmap says we need to notify the main thread. */
			notify = 1;
			res = 0;
		}
	}

	/*
	 * we should change the timeout state only if the previous event
	 * addition succeeded.
	 */
	if (res != -1 && tv != NULL) {
		struct timeval now;
		int common_timeout;
#ifdef USE_REINSERT_TIMEOUT
		int was_common;
		int old_timeout_idx;
#endif

		/*
		 * for persistent timeout events, we remember the
		 * timeout value and re-add the event.
		 *
		 * If tv_is_absolute, this was already set.
		 */
		//用户把这个event设置成EV_PERSIST,即永久event.
		//如果没有这样设置的话,那么只会超时一次.设置了,那么就可以吃
		//超时多次,需要记录用户设置的超时值
		if (ev->ev_closure == EV_CLOSURE_EVENT_PERSIST && !tv_is_absolute)
			ev->ev_io_timeout = *tv;

		//该event之前被加入到超时队列,用户可以对同一个event调用多次event_add
		//并且可以每次都用不同的超时值
#ifndef USE_REINSERT_TIMEOUT
		if (ev->ev_flags & EVLIST_TIMEOUT) {
			event_queue_remove_timeout(base, ev);
		}
#endif

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {
			if (ev->ev_events & EV_SIGNAL) {
				/* See if we are just active executing
				 * this event in a loop
				 */
				if (ev->ev_ncalls && ev->ev_pncalls) {
					/* Abort loop */
					*ev->ev_pncalls = 0;
				}
			}

			event_queue_remove_active(base, event_to_event_callback(ev));
		}

		gettime(base, &now);

		//判断这个时间是否为common-timeout标志
		common_timeout = is_common_timeout(tv, base);
#ifdef USE_REINSERT_TIMEOUT
		was_common = is_common_timeout(&ev->ev_timeout, base);
		old_timeout_idx = COMMON_TIMEOUT_IDX(&ev->ev_timeout);
#endif

		//虽然用户在event_add时只需要用一个相对时间,但实际上
		//在libevent内部还是要把这个时间转换成绝对时间.
		//从存储角度来说,存绝对时间只需一个变量,而相对时间则需要两个,
		//一个存相对值,另一个存参考值
		if (tv_is_absolute) {
			ev->ev_timeout = *tv;
		} else if (common_timeout) {
			struct timeval tmp = *tv;
			//只取真正的时间部分,common-timeout标志位和下标为不要
			tmp.tv_usec &= MICROSECONDS_MASK;
			//转换为绝对时间
			evutil_timeradd(&now, &tmp, &ev->ev_timeout);
			ev->ev_timeout.tv_usec |=
			    (tv->tv_usec & ~MICROSECONDS_MASK);	//加入标志位
		} else {
			evutil_timeradd(&now, tv, &ev->ev_timeout);
		}

		event_debug((
			 "event_add: event %p, timeout in %d seconds %d useconds, call %p",
			 ev, (int)tv->tv_sec, (int)tv->tv_usec, ev->ev_callback));

#ifdef USE_REINSERT_TIMEOUT
		event_queue_reinsert_timeout(base, ev, was_common, common_timeout, old_timeout_idx);
#else
		event_queue_insert_timeout(base, ev);
#endif

		if (common_timeout) {
			struct common_timeout_list *ctl =
			    get_common_timeout_list(base, &ev->ev_timeout);
			if (ev == TAILQ_FIRST(&ctl->events)) {
				common_timeout_schedule(ctl, &now, ev);
			}
		} else {
			struct event* top = NULL;
			/* See if the earliest timeout is now earlier than it
			 * was before: if so, we will need to tell the main
			 * thread to wake up earlier than it would otherwise.
			 * We double check the timeout of the top element to
			 * handle time distortions due to system suspension.
			 */
			if (min_heap_elt_is_top_(ev))
				notify = 1;
			else if ((top = min_heap_top_(&base->timeheap)) != NULL &&
					 evutil_timercmp(&top->ev_timeout, &now, <))
				notify = 1;
		}
	}

	/* if we are not in the right thread, we need to wake up the loop */
	if (res != -1 && notify && EVBASE_NEED_NOTIFY(base))
		evthread_notify_base(base);

	event_debug_note_add_(ev);

	return (res);
}

static int
event_del_(struct event *ev, int blocking)
{
	int res;
	struct event_base *base = ev->ev_base;

	if (EVUTIL_FAILURE_CHECK(!base)) {
		event_warnx("%s: event has no event_base set.", __func__);
		return -1;
	}

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	res = event_del_nolock_(ev, blocking);
	EVBASE_RELEASE_LOCK(base, th_base_lock);

	return (res);
}

int
event_del(struct event *ev)
{
	return event_del_(ev, EVENT_DEL_AUTOBLOCK);
}

int
event_del_block(struct event *ev)
{
	return event_del_(ev, EVENT_DEL_BLOCK);
}

int
event_del_noblock(struct event *ev)
{
	return event_del_(ev, EVENT_DEL_NOBLOCK);
}

/** Helper for event_del: always called with th_base_lock held.
 *
 * "blocking" must be one of the EVENT_DEL_{BLOCK, NOBLOCK, AUTOBLOCK,
 * EVEN_IF_FINALIZING} values. See those for more information.
 */
int
event_del_nolock_(struct event *ev, int blocking)
{
	struct event_base *base;
	int res = 0, notify = 0;

	event_debug(("event_del: %p (fd "EV_SOCK_FMT"), callback %p",
		ev, EV_SOCK_ARG(ev->ev_fd), ev->ev_callback));

	/* An event without a base has not been added */
	if (ev->ev_base == NULL)
		return (-1);

	EVENT_BASE_ASSERT_LOCKED(ev->ev_base);

	if (blocking != EVENT_DEL_EVEN_IF_FINALIZING) {
		if (ev->ev_flags & EVLIST_FINALIZING) {
			/* XXXX Debug */
			return 0;
		}
	}

	base = ev->ev_base;

	EVUTIL_ASSERT(!(ev->ev_flags & ~EVLIST_ALL));

	/* See if we are just active executing this event in a loop */
	if (ev->ev_events & EV_SIGNAL) {
		if (ev->ev_ncalls && ev->ev_pncalls) {
			/* Abort loop */
			*ev->ev_pncalls = 0;
		}
	}

	//从超时集合中删除,超时集合可能是小根堆也可能是common-timeout
	if (ev->ev_flags & EVLIST_TIMEOUT) {
		/* NOTE: We never need to notify the main thread because of a
		 * deleted timeout event: all that could happen if we don't is
		 * that the dispatch loop might wake up too early.  But the
		 * point of notifying the main thread _is_ to wake up the
		 * dispatch loop early anyway, so we wouldn't gain anything by
		 * doing it.
		 */
		//删除超时event并不需要通知主线程,如果该event不是最早超时的,那肯定不用通知了.
		//如果是的话,那么主线程会醒来,醒来后,主线程还是会再次检查超时集合中有哪些
		//超时event超时了,这个被删除的超时event自然也检查不出来,主线程只会"空手而回"
		event_queue_remove_timeout(base, ev);
	}

	//该event已经在active队列中了,那么需要再active队列中删除
	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove_active(base, event_to_event_callback(ev));
	else if (ev->ev_flags & EVLIST_ACTIVE_LATER)
		event_queue_remove_active_later(base, event_to_event_callback(ev));

	//该event已经在注册队列(eventqueue)中了,那么需要再注册队列中删除
	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove_inserted(base, ev);

		//此外还要在该fd或者sig队列中删除,同一个fd可以有多个event.所以这里还有一个队列
		if (ev->ev_events & (EV_READ|EV_WRITE|EV_CLOSED))
			res = evmap_io_del_(base, ev->ev_fd, ev);
		else
			res = evmap_signal_del_(base, (int)ev->ev_fd, ev);
		if (res == 1) {
			/* evmap says we need to notify the main thread. */
			notify = 1;
			res = 0;
		}
		/* If we do not have events, let's notify event base so it can
		 * exit without waiting */
		if (!event_haveevents(base) && !N_ACTIVE_CALLBACKS(base))
			notify = 1;
	}

	/* if we are not in the right thread, we need to wake up the loop */
	//可能需要通知主线程
	if (res != -1 && notify && EVBASE_NEED_NOTIFY(base))
		evthread_notify_base(base);

	event_debug_note_del_(ev);

	/* If the main thread is currently executing this event's callback,
	 * and we are not the main thread, then we want to wait until the
	 * callback is done before returning. That way, when this function
	 * returns, it will be safe to free the user-supplied argument.
	 */
#ifndef EVENT__DISABLE_THREAD_SUPPORT
	if (blocking != EVENT_DEL_NOBLOCK &&
	    base->current_event == event_to_event_callback(ev) &&
	    !EVBASE_IN_THREAD(base) &&
	    (blocking == EVENT_DEL_BLOCK || !(ev->ev_events & EV_FINALIZE))) {
		++base->current_event_waiters;
		EVTHREAD_COND_WAIT(base->current_event_cond, base->th_base_lock);
	}
#endif

	return (res);
}

//res是激活原因,例如:EV_READ,EV_TIMEOUT之类的宏
//ncalls只对EV_SIGNAL信号有用,表示信号的次数
//因为IO事件的次数没用,信号才有用
void
event_active(struct event *ev, int res, short ncalls)
{
	if (EVUTIL_FAILURE_CHECK(!ev->ev_base)) {
		event_warnx("%s: event has no event_base set.", __func__);
		return;
	}

	EVBASE_ACQUIRE_LOCK(ev->ev_base, th_base_lock);

	event_debug_assert_is_setup_(ev);

	event_active_nolock_(ev, res, ncalls);

	EVBASE_RELEASE_LOCK(ev->ev_base, th_base_lock);
}


void
event_active_nolock_(struct event *ev, int res, short ncalls)
{
	struct event_base *base;

	event_debug(("event_active: %p (fd "EV_SOCK_FMT"), res %d, callback %p",
		ev, EV_SOCK_ARG(ev->ev_fd), (int)res, ev->ev_callback));

	base = ev->ev_base;
	EVENT_BASE_ASSERT_LOCKED(base);

	if (ev->ev_flags & EVLIST_FINALIZING) {
		/* XXXX debug */
		return;
	}

	switch ((ev->ev_flags & (EVLIST_ACTIVE|EVLIST_ACTIVE_LATER))) {
	default:
	case EVLIST_ACTIVE|EVLIST_ACTIVE_LATER:
		EVUTIL_ASSERT(0);
		break;
	case EVLIST_ACTIVE:
		/* We get different kinds of events, add them together */
		ev->ev_res |= res;
		return;
	case EVLIST_ACTIVE_LATER:
		ev->ev_res |= res;
		break;
	case 0:
		ev->ev_res = res;
		break;
	}

	//这将停止处理低优先级的event,一路回退到event_base_loop中
	if (ev->ev_pri < base->event_running_priority)
		base->event_continue = 1;

	if (ev->ev_events & EV_SIGNAL) {
#ifndef EVENT__DISABLE_THREAD_SUPPORT
		if (base->current_event == event_to_event_callback(ev) &&
		    !EVBASE_IN_THREAD(base)) {
			++base->current_event_waiters;
			EVTHREAD_COND_WAIT(base->current_event_cond, base->th_base_lock);
		}
#endif
		ev->ev_ncalls = ncalls;
		ev->ev_pncalls = NULL;
	}

	//插入到激活队列的队尾
	event_callback_activate_nolock_(base, event_to_event_callback(ev));
}

void
event_active_later_(struct event *ev, int res)
{
	EVBASE_ACQUIRE_LOCK(ev->ev_base, th_base_lock);
	event_active_later_nolock_(ev, res);
	EVBASE_RELEASE_LOCK(ev->ev_base, th_base_lock);
}

void
event_active_later_nolock_(struct event *ev, int res)
{
	struct event_base *base = ev->ev_base;
	EVENT_BASE_ASSERT_LOCKED(base);

	if (ev->ev_flags & (EVLIST_ACTIVE|EVLIST_ACTIVE_LATER)) {
		/* We get different kinds of events, add them together */
		ev->ev_res |= res;
		return;
	}

	ev->ev_res = res;

	event_callback_activate_later_nolock_(base, event_to_event_callback(ev));
}

int
event_callback_activate_(struct event_base *base,
    struct event_callback *evcb)
{
	int r;
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	r = event_callback_activate_nolock_(base, evcb);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return r;
}

int
event_callback_activate_nolock_(struct event_base *base,
    struct event_callback *evcb)
{
	int r = 1;

	if (evcb->evcb_flags & EVLIST_FINALIZING)
		return 0;

	switch (evcb->evcb_flags & (EVLIST_ACTIVE|EVLIST_ACTIVE_LATER)) {
	default:
		EVUTIL_ASSERT(0);
		EVUTIL_FALLTHROUGH;
	case EVLIST_ACTIVE_LATER:
		event_queue_remove_active_later(base, evcb);
		r = 0;
		break;
	case EVLIST_ACTIVE:
		return 0;
	case 0:
		break;
	}

	event_queue_insert_active(base, evcb);

	if (EVBASE_NEED_NOTIFY(base))
		evthread_notify_base(base);

	return r;
}

int
event_callback_activate_later_nolock_(struct event_base *base,
    struct event_callback *evcb)
{
	if (evcb->evcb_flags & (EVLIST_ACTIVE|EVLIST_ACTIVE_LATER))
		return 0;

	event_queue_insert_active_later(base, evcb);
	if (EVBASE_NEED_NOTIFY(base))
		evthread_notify_base(base);
	return 1;
}

void
event_callback_init_(struct event_base *base,
    struct event_callback *cb)
{
	memset(cb, 0, sizeof(*cb));
	cb->evcb_pri = base->nactivequeues - 1;
}

int
event_callback_cancel_(struct event_base *base,
    struct event_callback *evcb)
{
	int r;
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	r = event_callback_cancel_nolock_(base, evcb, 0);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return r;
}

int
event_callback_cancel_nolock_(struct event_base *base,
    struct event_callback *evcb, int even_if_finalizing)
{
	if ((evcb->evcb_flags & EVLIST_FINALIZING) && !even_if_finalizing)
		return 0;

	if (evcb->evcb_flags & EVLIST_INIT)
		return event_del_nolock_(event_callback_to_event(evcb),
		    even_if_finalizing ? EVENT_DEL_EVEN_IF_FINALIZING : EVENT_DEL_AUTOBLOCK);

	switch ((evcb->evcb_flags & (EVLIST_ACTIVE|EVLIST_ACTIVE_LATER))) {
	default:
	case EVLIST_ACTIVE|EVLIST_ACTIVE_LATER:
		EVUTIL_ASSERT(0);
		break;
	case EVLIST_ACTIVE:
		/* We get different kinds of events, add them together */
		event_queue_remove_active(base, evcb);
		return 0;
	case EVLIST_ACTIVE_LATER:
		event_queue_remove_active_later(base, evcb);
		break;
	case 0:
		break;
	}

	return 0;
}

void
event_deferred_cb_init_(struct event_callback *cb, ev_uint8_t priority, deferred_cb_fn fn, void *arg)
{
	memset(cb, 0, sizeof(*cb));
	cb->evcb_cb_union.evcb_selfcb = fn;
	cb->evcb_arg = arg;
	cb->evcb_pri = priority;
	cb->evcb_closure = EV_CLOSURE_CB_SELF;
}

void
event_deferred_cb_set_priority_(struct event_callback *cb, ev_uint8_t priority)
{
	cb->evcb_pri = priority;
}

void
event_deferred_cb_cancel_(struct event_base *base, struct event_callback *cb)
{
	if (!base)
		base = current_base;
	event_callback_cancel_(base, cb);
}

#define MAX_DEFERREDS_QUEUED 32
int
event_deferred_cb_schedule_(struct event_base *base, struct event_callback *cb)
{
	int r = 1;
	if (!base)
		base = current_base;
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	if (base->n_deferreds_queued > MAX_DEFERREDS_QUEUED) {
		r = event_callback_activate_later_nolock_(base, cb);
	} else {
		r = event_callback_activate_nolock_(base, cb);
		if (r) {
			++base->n_deferreds_queued;
		}
	}
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return r;
}

static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	/* Caller must hold th_base_lock */
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;
	int res = 0;

	ev = min_heap_top_(&base->timeheap);

	//堆中没有元素
	if (ev == NULL) {
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		goto out;
	}

	//获取当前时间
	if (gettime(base, &now) == -1) {
		res = -1;
		goto out;
	}

	//如果超时时间<=当前时间,不能等待,需要立即返回
	//因为ev_timeout这个时间是event_add调用时的绝对时间+相对时间,
	//所以ev_timeout是绝对时间,可能在调用event_add之后,过了一段时间
	//才调用event_base_dispatch,所以现在可能已经超时
	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {
		evutil_timerclear(tv);	//清零,这样可以让dispatch不再等待,马上返回
		goto out;
	}

	//计算等待的时间 = 当前时间-最小超时时间
	evutil_timersub(&ev->ev_timeout, &now, tv);

	EVUTIL_ASSERT(tv->tv_sec >= 0);
	EVUTIL_ASSERT(tv->tv_usec >= 0);
	event_debug(("timeout_next: event: %p, in %d seconds, %d useconds", ev, (int)tv->tv_sec, (int)tv->tv_usec));

out:
	return (res);
}

/* Activate every event whose timeout has elapsed. */
//把超时了的event, 放到激活队列中,并且,其激活原因设置为EV_TIMEOUT
static void
timeout_process(struct event_base *base)
{
	/* Caller must hold lock. */
	struct timeval now;
	struct event *ev;

	if (min_heap_empty_(&base->timeheap)) {
		return;
	}

	gettime(base, &now);

	//遍历小根堆的元素.之所以不是只取堆顶元素,是因为当主线程调用多路IO复用函数
	//进入等待时,其他线程可能添加了多个超时值更小的event
	while ((ev = min_heap_top_(&base->timeheap))) {
		//ev->ev_timeout存的是超时时间
		//超时时间比此刻时间大,说明该event还没超时,那么之后的元素就不用检查了
		if (evutil_timercmp(&ev->ev_timeout, &now, >))
			break;

		/* delete this event from the I/O queues */
		//把event从这个event_base中删除,event_base不再监听
		//注意:这里(timeout_process函数)只处理有超时值的event(因为前边判断了)
		//对于有EV_PERSIST选项的event,在处理激活event的时候,会再次添加进event_base,
		//这样做的一个好处是:再次添加时,会重新计算该event的超时时间(绝对时间)
		event_del_nolock_(ev, EVENT_DEL_NOBLOCK);

		event_debug(("timeout_process: event: %p, call %p",
			 ev, ev->ev_callback));

		//把这个event加入到event_base的激活队列中.
		//event_base的激活队列中又有该event了,所以如果该event是
		//EV_PERSIST的,是可以再次添加进该event_base的
		event_active_nolock_(ev, EV_TIMEOUT, 1);
	}
}

#ifndef MAX
#define MAX(a,b) (((a)>(b))?(a):(b))
#endif

#define MAX_EVENT_COUNT(var, v) var = MAX(var, v)

/* These are a fancy way to spell
     if (~flags & EVLIST_INTERNAL)
         base->event_count--/++;
*/
#define DECR_EVENT_COUNT(base,flags) \
	((base)->event_count -= !((flags) & EVLIST_INTERNAL))
#define INCR_EVENT_COUNT(base,flags) do {					\
	((base)->event_count += !((flags) & EVLIST_INTERNAL));			\
	MAX_EVENT_COUNT((base)->event_count_max, (base)->event_count);		\
} while (0)

static void
event_queue_remove_inserted(struct event_base *base, struct event *ev)
{
	EVENT_BASE_ASSERT_LOCKED(base);
	if (EVUTIL_FAILURE_CHECK(!(ev->ev_flags & EVLIST_INSERTED))) {
		event_errx(1, "%s: %p(fd "EV_SOCK_FMT") not on queue %x", __func__,
		    ev, EV_SOCK_ARG(ev->ev_fd), EVLIST_INSERTED);
		return;
	}
	DECR_EVENT_COUNT(base, ev->ev_flags);
	ev->ev_flags &= ~EVLIST_INSERTED;
}
static void
event_queue_remove_active(struct event_base *base, struct event_callback *evcb)
{
	EVENT_BASE_ASSERT_LOCKED(base);
	if (EVUTIL_FAILURE_CHECK(!(evcb->evcb_flags & EVLIST_ACTIVE))) {
		event_errx(1, "%s: %p not on queue %x", __func__,
			   evcb, EVLIST_ACTIVE);
		return;
	}
	DECR_EVENT_COUNT(base, evcb->evcb_flags);
	evcb->evcb_flags &= ~EVLIST_ACTIVE;
	base->event_count_active--;

	TAILQ_REMOVE(&base->activequeues[evcb->evcb_pri],
	    evcb, evcb_active_next);
}
static void
event_queue_remove_active_later(struct event_base *base, struct event_callback *evcb)
{
	EVENT_BASE_ASSERT_LOCKED(base);
	if (EVUTIL_FAILURE_CHECK(!(evcb->evcb_flags & EVLIST_ACTIVE_LATER))) {
		event_errx(1, "%s: %p not on queue %x", __func__,
			   evcb, EVLIST_ACTIVE_LATER);
		return;
	}
	DECR_EVENT_COUNT(base, evcb->evcb_flags);
	evcb->evcb_flags &= ~EVLIST_ACTIVE_LATER;
	base->event_count_active--;

	TAILQ_REMOVE(&base->active_later_queue, evcb, evcb_active_next);
}
static void
event_queue_remove_timeout(struct event_base *base, struct event *ev)
{
	EVENT_BASE_ASSERT_LOCKED(base);
	if (EVUTIL_FAILURE_CHECK(!(ev->ev_flags & EVLIST_TIMEOUT))) {
		event_errx(1, "%s: %p(fd "EV_SOCK_FMT") not on queue %x", __func__,
		    ev, EV_SOCK_ARG(ev->ev_fd), EVLIST_TIMEOUT);
		return;
	}
	DECR_EVENT_COUNT(base, ev->ev_flags);
	ev->ev_flags &= ~EVLIST_TIMEOUT;

	if (is_common_timeout(&ev->ev_timeout, base)) {
		struct common_timeout_list *ctl =
		    get_common_timeout_list(base, &ev->ev_timeout);
		TAILQ_REMOVE(&ctl->events, ev,
		    ev_timeout_pos.ev_next_with_common_timeout);
	} else {
		min_heap_erase_(&base->timeheap, ev);
	}
}

#ifdef USE_REINSERT_TIMEOUT
/* Remove and reinsert 'ev' into the timeout queue. */
static void
event_queue_reinsert_timeout(struct event_base *base, struct event *ev,
    int was_common, int is_common, int old_timeout_idx)
{
	struct common_timeout_list *ctl;
	if (!(ev->ev_flags & EVLIST_TIMEOUT)) {
		event_queue_insert_timeout(base, ev);
		return;
	}

	switch ((was_common<<1) | is_common) {
	case 3: /* Changing from one common timeout to another */
		ctl = base->common_timeout_queues[old_timeout_idx];
		TAILQ_REMOVE(&ctl->events, ev,
		    ev_timeout_pos.ev_next_with_common_timeout);
		ctl = get_common_timeout_list(base, &ev->ev_timeout);
		insert_common_timeout_inorder(ctl, ev);
		break;
	case 2: /* Was common; is no longer common */
		ctl = base->common_timeout_queues[old_timeout_idx];
		TAILQ_REMOVE(&ctl->events, ev,
		    ev_timeout_pos.ev_next_with_common_timeout);
		min_heap_push_(&base->timeheap, ev);
		break;
	case 1: /* Wasn't common; has become common. */
		min_heap_erase_(&base->timeheap, ev);
		ctl = get_common_timeout_list(base, &ev->ev_timeout);
		insert_common_timeout_inorder(ctl, ev);
		break;
	case 0: /* was in heap; is still on heap. */
		min_heap_adjust_(&base->timeheap, ev);
		break;
	default:
		EVUTIL_ASSERT(0); /* unreachable */
		break;
	}
}
#endif

/* Add 'ev' to the common timeout list in 'ev'. */
static void
insert_common_timeout_inorder(struct common_timeout_list *ctl,
    struct event *ev)
{
	struct event *e;
	/* By all logic, we should just be able to append 'ev' to the end of
	 * ctl->events, since the timeout on each 'ev' is set to {the common
	 * timeout} + {the time when we add the event}, and so the events
	 * should arrive in order of their timeeouts.  But just in case
	 * there's some wacky threading issue going on, we do a search from
	 * the end of 'ev' to find the right insertion point.
	 */
	//虽然有相同超时时长,但超时时间却是 超时时长+ 调用event_add的时间.
	//所以是在不同的时间触发超时的.他们根据绝对超时时间,升序放在队列中.
	//一般来说,直接插入队尾即可,因为后插入的绝对时间肯定大.
	//但由于线程抢占的原因,可能有一个线程在evutil_timeradd(&now,&tmp,&ev->ev_timeout);
	//执行完,还没来得及插入,就被另外一个线程抢占了.而这个线程也是要插入一个
	//common-timeour的超时event.这样就会发生:超时时间小的反而后插入.
	//所以从后面开始遍历队列,寻找一个合适的地方
	TAILQ_FOREACH_REVERSE(e, &ctl->events,
	    event_list, ev_timeout_pos.ev_next_with_common_timeout) {
		/* This timercmp is a little sneaky, since both ev and e have
		 * magic values in tv_usec.  Fortunately, they ought to have
		 * the _same_ magic values in tv_usec.  Let's assert for that.
		 */
		EVUTIL_ASSERT(
			is_same_common_timeout(&e->ev_timeout, &ev->ev_timeout));
		if (evutil_timercmp(&ev->ev_timeout, &e->ev_timeout, >=)) {
			TAILQ_INSERT_AFTER(&ctl->events, e, ev,			//从队列后面插入
			    ev_timeout_pos.ev_next_with_common_timeout);
			return;
		}
	}
	//在队列头插入,只会发生在前面的寻找都没有找到的情况下
	TAILQ_INSERT_HEAD(&ctl->events, ev,
	    ev_timeout_pos.ev_next_with_common_timeout);
}

static void
event_queue_insert_inserted(struct event_base *base, struct event *ev)
{
	EVENT_BASE_ASSERT_LOCKED(base);

	if (EVUTIL_FAILURE_CHECK(ev->ev_flags & EVLIST_INSERTED)) {
		event_errx(1, "%s: %p(fd "EV_SOCK_FMT") already inserted", __func__,
		    ev, EV_SOCK_ARG(ev->ev_fd));
		return;
	}

	INCR_EVENT_COUNT(base, ev->ev_flags);

	ev->ev_flags |= EVLIST_INSERTED;
}

static void
event_queue_insert_active(struct event_base *base, struct event_callback *evcb)
{
	EVENT_BASE_ASSERT_LOCKED(base);

	if (evcb->evcb_flags & EVLIST_ACTIVE) {
		/* Double insertion is possible for active events */
		return;
	}

	INCR_EVENT_COUNT(base, evcb->evcb_flags);

	evcb->evcb_flags |= EVLIST_ACTIVE;

	base->event_count_active++;
	MAX_EVENT_COUNT(base->event_count_active_max, base->event_count_active);
	EVUTIL_ASSERT(evcb->evcb_pri < base->nactivequeues);
	//将event插入到对应的优先级的激活队列
	TAILQ_INSERT_TAIL(&base->activequeues[evcb->evcb_pri],
	    evcb, evcb_active_next);
}

static void
event_queue_insert_active_later(struct event_base *base, struct event_callback *evcb)
{
	EVENT_BASE_ASSERT_LOCKED(base);
	if (evcb->evcb_flags & (EVLIST_ACTIVE_LATER|EVLIST_ACTIVE)) {
		/* Double insertion is possible */
		return;
	}

	INCR_EVENT_COUNT(base, evcb->evcb_flags);
	evcb->evcb_flags |= EVLIST_ACTIVE_LATER;
	base->event_count_active++;
	MAX_EVENT_COUNT(base->event_count_active_max, base->event_count_active);
	EVUTIL_ASSERT(evcb->evcb_pri < base->nactivequeues);
	TAILQ_INSERT_TAIL(&base->active_later_queue, evcb, evcb_active_next);
}

static void
event_queue_insert_timeout(struct event_base *base, struct event *ev)
{
	EVENT_BASE_ASSERT_LOCKED(base);

	if (EVUTIL_FAILURE_CHECK(ev->ev_flags & EVLIST_TIMEOUT)) {
		event_errx(1, "%s: %p(fd "EV_SOCK_FMT") already on timeout", __func__,
		    ev, EV_SOCK_ARG(ev->ev_fd));
		return;
	}

	INCR_EVENT_COUNT(base, ev->ev_flags);

	ev->ev_flags |= EVLIST_TIMEOUT;

	if (is_common_timeout(&ev->ev_timeout, base)) {
		//根据时间向event_base获取对应的common_timeout_list
		struct common_timeout_list *ctl =
		    get_common_timeout_list(base, &ev->ev_timeout);
		insert_common_timeout_inorder(ctl, ev);
	} else {
		min_heap_push_(&base->timeheap, ev);
	}
}

static void
event_queue_make_later_events_active(struct event_base *base)
{
	struct event_callback *evcb;
	EVENT_BASE_ASSERT_LOCKED(base);

	while ((evcb = TAILQ_FIRST(&base->active_later_queue))) {
		TAILQ_REMOVE(&base->active_later_queue, evcb, evcb_active_next);
		evcb->evcb_flags = (evcb->evcb_flags & ~EVLIST_ACTIVE_LATER) | EVLIST_ACTIVE;
		EVUTIL_ASSERT(evcb->evcb_pri < base->nactivequeues);
		TAILQ_INSERT_TAIL(&base->activequeues[evcb->evcb_pri], evcb, evcb_active_next);
		base->n_deferreds_queued += (evcb->evcb_closure == EV_CLOSURE_CB_SELF);
	}
}

/* Functions for debugging */

const char *
event_get_version(void)
{
	return (EVENT__VERSION);
}

ev_uint32_t
event_get_version_number(void)
{
	return (EVENT__NUMERIC_VERSION);
}

/*
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */

const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}

#ifndef EVENT__DISABLE_MM_REPLACEMENT
static void *(*mm_malloc_fn_)(size_t sz) = NULL;
static void *(*mm_realloc_fn_)(void *p, size_t sz) = NULL;
static void (*mm_free_fn_)(void *p) = NULL;

void *
event_mm_malloc_(size_t sz)
{
	if (sz == 0)
		return NULL;

	if (mm_malloc_fn_)
		return mm_malloc_fn_(sz);
	else
		return malloc(sz);
}

void *
event_mm_calloc_(size_t count, size_t size)
{
	if (count == 0 || size == 0)
		return NULL;

	if (mm_malloc_fn_) {
		size_t sz = count * size;
		void *p = NULL;
		if (count > EV_SIZE_MAX / size)
			goto error;
		p = mm_malloc_fn_(sz);
		if (p)
			return memset(p, 0, sz);
	} else {
		void *p = calloc(count, size);
#ifdef _WIN32
		/* Windows calloc doesn't reliably set ENOMEM */
		if (p == NULL)
			goto error;
#endif
		return p;
	}

error:
	errno = ENOMEM;
	return NULL;
}

char *
event_mm_strdup_(const char *str)
{
	if (!str) {
		errno = EINVAL;
		return NULL;
	}

	if (mm_malloc_fn_) {
		size_t ln = strlen(str);
		void *p = NULL;
		if (ln == EV_SIZE_MAX)
			goto error;
		p = mm_malloc_fn_(ln+1);
		if (p)
			return memcpy(p, str, ln+1);
	} else
#ifdef _WIN32
		return _strdup(str);
#else
		return strdup(str);
#endif

error:
	errno = ENOMEM;
	return NULL;
}

void *
event_mm_realloc_(void *ptr, size_t sz)
{
	if (mm_realloc_fn_)
		return mm_realloc_fn_(ptr, sz);
	else
		return realloc(ptr, sz);
}

void
event_mm_free_(void *ptr)
{
	if (mm_free_fn_)
		mm_free_fn_(ptr);
	else
		free(ptr);
}

void
event_set_mem_functions(void *(*malloc_fn)(size_t sz),
			void *(*realloc_fn)(void *ptr, size_t sz),
			void (*free_fn)(void *ptr))
{
	mm_malloc_fn_ = malloc_fn;
	mm_realloc_fn_ = realloc_fn;
	mm_free_fn_ = free_fn;
}
#endif

#ifdef EVENT__HAVE_EVENTFD
static void
evthread_notify_drain_eventfd(evutil_socket_t fd, short what, void *arg)
{
	ev_uint64_t msg;
	ev_ssize_t r;
	struct event_base *base = arg;

	r = read(fd, (void*) &msg, sizeof(msg));
	if (r<0 && errno != EAGAIN) {
		event_sock_warn(fd, "Error reading from eventfd");
	}
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	base->is_notify_pending = 0;
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}
#endif

static void
evthread_notify_drain_default(evutil_socket_t fd, short what, void *arg)
{
	unsigned char buf[1024];
	struct event_base *base = arg;

	//读完fd的所有数据,免得再次被唤醒
#ifdef _WIN32
	while (recv(fd, (char*)buf, sizeof(buf), 0) > 0)
		;
#else
	while (read(fd, (char*)buf, sizeof(buf)) > 0)
		;
#endif

	//修改之,使得其不再是未决的,当然这也能让其他线程可以再次唤醒事件循环
	//参看evthread_notify_base函数
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	base->is_notify_pending = 0;
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

int
evthread_make_base_notifiable(struct event_base *base)
{
	int r;
	if (!base)
		return -1;

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	r = evthread_make_base_notifiable_nolock_(base);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return r;
}

static int
evthread_make_base_notifiable_nolock_(struct event_base *base)
{
	void (*cb)(evutil_socket_t, short, void *);
	int (*notify)(struct event_base *);

	if (base->th_notify_fn != NULL) {
		/* The base is already notifiable: we're doing fine. */
		return 0;
	}

#if defined(EVENT__HAVE_WORKING_KQUEUE)
	if (base->evsel == &kqops && event_kq_add_notify_event_(base) == 0) {
		base->th_notify_fn = event_kq_notify_base_;
		/* No need to add an event here; the backend can wake
		 * itself up just fine. */
		return 0;
	}
#endif

#ifdef EVENT__HAVE_EVENTFD
	//libevent优先使用eventfd,但eventfd的通信机制和其他的不一样
	//所以要专门为eventfd创建通知函数和event回调函数
	base->th_notify_fd[0] = evutil_eventfd_(0,
	    EVUTIL_EFD_CLOEXEC|EVUTIL_EFD_NONBLOCK);
	if (base->th_notify_fd[0] >= 0) {
		base->th_notify_fd[1] = -1;
		notify = evthread_notify_base_eventfd;
		cb = evthread_notify_drain_eventfd;
	} else
#endif
	if (evutil_make_internal_pipe_(base->th_notify_fd) == 0) {
		notify = evthread_notify_base_default;
		cb = evthread_notify_drain_default;
	} else {
		return -1;
	}

	base->th_notify_fn = notify;

	/* prepare an event that we can use for wakeup */
	event_assign(&base->th_notify, base, base->th_notify_fd[0],
				 EV_READ|EV_PERSIST, cb, base);

	/* we need to mark this as internal event */
	base->th_notify.ev_flags |= EVLIST_INTERNAL;
	event_priority_set(&base->th_notify, 0);

	return event_add_nolock_(&base->th_notify, NULL, 0);
}

int
event_base_foreach_event_nolock_(struct event_base *base,
    event_base_foreach_event_cb fn, void *arg)
{
	int r, i;
	size_t u;
	struct event *ev;

	/* Start out with all the EVLIST_INSERTED events. */
	if ((r = evmap_foreach_event_(base, fn, arg)))
		return r;

	/* Okay, now we deal with those events that have timeouts and are in
	 * the min-heap. */
	for (u = 0; u < base->timeheap.n; ++u) {
		ev = base->timeheap.p[u];
		if (ev->ev_flags & EVLIST_INSERTED) {
			/* we already processed this one */
			continue;
		}
		if ((r = fn(base, ev, arg)))
			return r;
	}

	/* Now for the events in one of the timeout queues.
	 * the min-heap. */
	for (i = 0; i < base->n_common_timeouts; ++i) {
		struct common_timeout_list *ctl =
		    base->common_timeout_queues[i];
		TAILQ_FOREACH(ev, &ctl->events,
		    ev_timeout_pos.ev_next_with_common_timeout) {
			if (ev->ev_flags & EVLIST_INSERTED) {
				/* we already processed this one */
				continue;
			}
			if ((r = fn(base, ev, arg)))
				return r;
		}
	}

	/* Finally, we deal wit all the active events that we haven't touched
	 * yet. */
	for (i = 0; i < base->nactivequeues; ++i) {
		struct event_callback *evcb;
		TAILQ_FOREACH(evcb, &base->activequeues[i], evcb_active_next) {
			if ((evcb->evcb_flags & (EVLIST_INIT|EVLIST_INSERTED|EVLIST_TIMEOUT)) != EVLIST_INIT) {
				/* This isn't an event (evlist_init clear), or
				 * we already processed it. (inserted or
				 * timeout set */
				continue;
			}
			ev = event_callback_to_event(evcb);
			if ((r = fn(base, ev, arg)))
				return r;
		}
	}

	return 0;
}

/* Helper for event_base_dump_events: called on each event in the event base;
 * dumps only the inserted events. */
static int
dump_inserted_event_fn(const struct event_base *base, const struct event *e, void *arg)
{
	FILE *output = arg;
	const char *gloss = (e->ev_events & EV_SIGNAL) ?
	    "sig" : "fd ";

	if (! (e->ev_flags & (EVLIST_INSERTED|EVLIST_TIMEOUT)))
		return 0;

	fprintf(output, "  %p [%s "EV_SOCK_FMT"]%s%s%s%s%s%s%s",
	    (void*)e, gloss, EV_SOCK_ARG(e->ev_fd),
	    (e->ev_events&EV_READ)?" Read":"",
	    (e->ev_events&EV_WRITE)?" Write":"",
	    (e->ev_events&EV_CLOSED)?" EOF":"",
	    (e->ev_events&EV_SIGNAL)?" Signal":"",
	    (e->ev_events&EV_PERSIST)?" Persist":"",
	    (e->ev_events&EV_ET)?" ET":"",
	    (e->ev_flags&EVLIST_INTERNAL)?" Internal":"");
	if (e->ev_flags & EVLIST_TIMEOUT) {
		struct timeval tv;
		tv.tv_sec = e->ev_timeout.tv_sec;
		tv.tv_usec = e->ev_timeout.tv_usec & MICROSECONDS_MASK;
		evutil_timeradd(&tv, &base->tv_clock_diff, &tv);
		fprintf(output, " Timeout=%ld.%06d",
		    (long)tv.tv_sec, (int)(tv.tv_usec & MICROSECONDS_MASK));
	}
	fputc('\n', output);

	return 0;
}

/* Helper for event_base_dump_events: called on each event in the event base;
 * dumps only the active events. */
static int
dump_active_event_fn(const struct event_base *base, const struct event *e, void *arg)
{
	FILE *output = arg;
	const char *gloss = (e->ev_events & EV_SIGNAL) ?
	    "sig" : "fd ";

	if (! (e->ev_flags & (EVLIST_ACTIVE|EVLIST_ACTIVE_LATER)))
		return 0;

	fprintf(output, "  %p [%s "EV_SOCK_FMT", priority=%d]%s%s%s%s%s active%s%s\n",
	    (void*)e, gloss, EV_SOCK_ARG(e->ev_fd), e->ev_pri,
	    (e->ev_res&EV_READ)?" Read":"",
	    (e->ev_res&EV_WRITE)?" Write":"",
	    (e->ev_res&EV_CLOSED)?" EOF":"",
	    (e->ev_res&EV_SIGNAL)?" Signal":"",
	    (e->ev_res&EV_TIMEOUT)?" Timeout":"",
	    (e->ev_flags&EVLIST_INTERNAL)?" [Internal]":"",
	    (e->ev_flags&EVLIST_ACTIVE_LATER)?" [NextTime]":"");

	return 0;
}

int
event_base_foreach_event(struct event_base *base,
    event_base_foreach_event_cb fn, void *arg)
{
	int r;
	if ((!fn) || (!base)) {
		return -1;
	}
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	r = event_base_foreach_event_nolock_(base, fn, arg);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
	return r;
}


void
event_base_dump_events(struct event_base *base, FILE *output)
{
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	fprintf(output, "Inserted events:\n");
	event_base_foreach_event_nolock_(base, dump_inserted_event_fn, output);

	fprintf(output, "Active events:\n");
	event_base_foreach_event_nolock_(base, dump_active_event_fn, output);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

void
event_base_active_by_fd(struct event_base *base, evutil_socket_t fd, short events)
{
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	/* Activate any non timer events */
	if (!(events & EV_TIMEOUT)) {
		evmap_io_active_(base, fd, events & (EV_READ|EV_WRITE|EV_CLOSED));
	} else {
		/* If we want to activate timer events, loop and activate each event with
		 * the same fd in both the timeheap and common timeouts list */
		int i;
		size_t u;
		struct event *ev;

		for (u = 0; u < base->timeheap.n; ++u) {
			ev = base->timeheap.p[u];
			if (ev->ev_fd == fd) {
				event_active_nolock_(ev, EV_TIMEOUT, 1);
			}
		}

		for (i = 0; i < base->n_common_timeouts; ++i) {
			struct common_timeout_list *ctl = base->common_timeout_queues[i];
			TAILQ_FOREACH(ev, &ctl->events,
				ev_timeout_pos.ev_next_with_common_timeout) {
				if (ev->ev_fd == fd) {
					event_active_nolock_(ev, EV_TIMEOUT, 1);
				}
			}
		}
	}

	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

void
event_base_active_by_signal(struct event_base *base, int sig)
{
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	evmap_signal_active_(base, sig, 1);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}


void
event_base_add_virtual_(struct event_base *base)
{
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	base->virtual_event_count++;
	MAX_EVENT_COUNT(base->virtual_event_count_max, base->virtual_event_count);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

void
event_base_del_virtual_(struct event_base *base)
{
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	EVUTIL_ASSERT(base->virtual_event_count > 0);
	base->virtual_event_count--;
	if (base->virtual_event_count == 0 && EVBASE_NEED_NOTIFY(base))
		evthread_notify_base(base);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

static void
event_free_debug_globals_locks(void)
{
#ifndef EVENT__DISABLE_THREAD_SUPPORT
#ifndef EVENT__DISABLE_DEBUG_MODE
	if (event_debug_map_lock_ != NULL) {
		EVTHREAD_FREE_LOCK(event_debug_map_lock_, 0);
		event_debug_map_lock_ = NULL;
		evthreadimpl_disable_lock_debugging_();
	}
#endif /* EVENT__DISABLE_DEBUG_MODE */
#endif /* EVENT__DISABLE_THREAD_SUPPORT */
	return;
}

static void
event_free_debug_globals(void)
{
	event_free_debug_globals_locks();
}

static void
event_free_evsig_globals(void)
{
	evsig_free_globals_();
}

static void
event_free_evutil_globals(void)
{
	evutil_free_globals_();
}

static void
event_free_globals(void)
{
	event_free_debug_globals();
	event_free_evsig_globals();
	event_free_evutil_globals();
}

void
libevent_global_shutdown(void)
{
	event_disable_debug_mode();
	event_free_globals();
}

#ifndef EVENT__DISABLE_THREAD_SUPPORT
int
event_global_setup_locks_(const int enable_locks)
{
#ifndef EVENT__DISABLE_DEBUG_MODE
	EVTHREAD_SETUP_GLOBAL_LOCK(event_debug_map_lock_, 0);
#endif
	if (evsig_global_setup_locks_(enable_locks) < 0)
		return -1;
	if (evutil_global_setup_locks_(enable_locks) < 0)
		return -1;
	if (evutil_secure_rng_global_setup_locks_(enable_locks) < 0)
		return -1;
	return 0;
}
#endif

void
event_base_assert_ok_(struct event_base *base)
{
	EVBASE_ACQUIRE_LOCK(base, th_base_lock);
	event_base_assert_ok_nolock_(base);
	EVBASE_RELEASE_LOCK(base, th_base_lock);
}

void
event_base_assert_ok_nolock_(struct event_base *base)
{
	int i;
	size_t u;
	int count;

	/* First do checks on the per-fd and per-signal lists */
	evmap_check_integrity_(base);

	/* Check the heap property */
	for (u = 1; u < base->timeheap.n; ++u) {
		size_t parent = (u - 1) / 2;
		struct event *ev, *p_ev;
		ev = base->timeheap.p[u];
		p_ev = base->timeheap.p[parent];
		EVUTIL_ASSERT(ev->ev_flags & EVLIST_TIMEOUT);
		EVUTIL_ASSERT(evutil_timercmp(&p_ev->ev_timeout, &ev->ev_timeout, <=));
		EVUTIL_ASSERT(ev->ev_timeout_pos.min_heap_idx == u);
	}

	/* Check that the common timeouts are fine */
	for (i = 0; i < base->n_common_timeouts; ++i) {
		struct common_timeout_list *ctl = base->common_timeout_queues[i];
		struct event *last=NULL, *ev;

		EVUTIL_ASSERT_TAILQ_OK(&ctl->events, event, ev_timeout_pos.ev_next_with_common_timeout);

		TAILQ_FOREACH(ev, &ctl->events, ev_timeout_pos.ev_next_with_common_timeout) {
			if (last)
				EVUTIL_ASSERT(evutil_timercmp(&last->ev_timeout, &ev->ev_timeout, <=));
			EVUTIL_ASSERT(ev->ev_flags & EVLIST_TIMEOUT);
			EVUTIL_ASSERT(is_common_timeout(&ev->ev_timeout,base));
			EVUTIL_ASSERT(COMMON_TIMEOUT_IDX(&ev->ev_timeout) == i);
			last = ev;
		}
	}

	/* Check the active queues. */
	count = 0;
	for (i = 0; i < base->nactivequeues; ++i) {
		struct event_callback *evcb;
		EVUTIL_ASSERT_TAILQ_OK(&base->activequeues[i], event_callback, evcb_active_next);
		TAILQ_FOREACH(evcb, &base->activequeues[i], evcb_active_next) {
			EVUTIL_ASSERT((evcb->evcb_flags & (EVLIST_ACTIVE|EVLIST_ACTIVE_LATER)) == EVLIST_ACTIVE);
			EVUTIL_ASSERT(evcb->evcb_pri == i);
			++count;
		}
	}

	{
		struct event_callback *evcb;
		TAILQ_FOREACH(evcb, &base->active_later_queue, evcb_active_next) {
			EVUTIL_ASSERT((evcb->evcb_flags & (EVLIST_ACTIVE|EVLIST_ACTIVE_LATER)) == EVLIST_ACTIVE_LATER);
			++count;
		}
	}
	EVUTIL_ASSERT(count == base->event_count_active);
}
