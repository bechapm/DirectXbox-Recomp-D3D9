/*
 * host_timer.h - a timer that wakes when asked, on every host.
 *
 * Three runtime threads pace themselves to well under a millisecond: the
 * vblank clock (kernel_bridge.c), the flip gate's lent wait and the NV2A ack
 * thread's idle wait (xbox_memory_layout.c). A plain Sleep cannot do that --
 * the Windows scheduler tick is 15.6 ms -- so each used a high-resolution
 * waitable timer. This is that timer with the Win32 calls behind it:
 *
 *   Windows  CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION),
 *            Windows 10 1803+, about 0.5 ms; optionally falling back to a
 *            plain waitable timer, exactly as the three callers did.
 *   macOS    mach_wait_until on an absolute mach_absolute_time deadline.
 *   Linux    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME).
 *
 * A host_timer belongs to one thread at a time: it is not a shared object.
 * Periodic waits are a deadline the caller advances by its period
 * (host_timer_wait_until), so a late wake does not push every later one.
 */
#ifndef HOST_TIMER_H
#define HOST_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct host_timer host_timer;

/* Kinds of timer host_timer_create may hand back. */
#define HOST_TIMER_HIGH_RES_ONLY   0   /* NULL where no high-resolution timer exists */
#define HOST_TIMER_ANY             1   /* else a plain, scheduler-tick timer */

/* NULL when no timer of the asked kind can be made (only possible on
 * Windows; POSIX hosts always have a precise one). */
host_timer *host_timer_create(int kind);
void        host_timer_destroy(host_timer *t);

/* 1 when the timer waits to sub-millisecond precision. */
int host_timer_high_res(const host_timer *t);

/* A monotonic clock in nanoseconds, the time base of host_timer_wait_until. */
int64_t host_time_ns(void);

/* Results of the waits below. */
#define HOST_WAIT_EVENT       1    /* the event was signalled */
#define HOST_WAIT_ELAPSED     0    /* the time passed */
#define HOST_WAIT_NOT_ARMED (-1)   /* the timer could not be armed: no wait happened */
#define HOST_WAIT_FAILED    (-2)   /* the cap passed first, or the wait failed */

/* Wait `us` microseconds from now. `cap_ms` bounds the wait in case the
 * timer never fires (the Windows waits used one; 0xFFFFFFFF for none).
 * HOST_WAIT_ELAPSED, _NOT_ARMED or _FAILED. */
int host_timer_wait_us(host_timer *t, int64_t us, uint32_t cap_ms);

/* Wait about `us` microseconds, never less, as cheaply as the host allows:
 * one plain sleep, no stepping and no spin, so it may wake late by whatever
 * leeway the host adds (a quarter of the wait on macOS, ~50 us on Linux, the
 * timer's granularity on Windows, where it is host_timer_wait_us). For a
 * thread that polls and gains nothing from precision (the NV2A ack thread),
 * where host_timer_wait_us would spin the last 200 us of every wait on macOS.
 * Same results as host_timer_wait_us. */
int host_timer_wait_us_coarse(host_timer *t, int64_t us, uint32_t cap_ms);

/* Wait until `deadline_ns` on host_time_ns()'s clock; a deadline already
 * past returns HOST_WAIT_ELAPSED at once. Same results as host_timer_wait_us. */
int host_timer_wait_until(host_timer *t, int64_t deadline_ns, uint32_t cap_ms);

/* Wait `us` microseconds or until `event` (a HANDLE from CreateEvent, passed
 * as void * so this header needs no Win32 types) is signalled, whichever is
 * first, never longer than `cap_ms`. Consumes an auto-reset event's signal,
 * as WaitForMultipleObjects does. HOST_WAIT_EVENT (the timer is then
 * cancelled), _ELAPSED, _NOT_ARMED or _FAILED. */
int host_timer_wait_us_or_event(host_timer *t, int64_t us, void *event, uint32_t cap_ms);

/* One thread's sleep that another can cut short. The sleeper waits with
 * host_waker_wait_us; any thread may call host_waker_wake, which costs no
 * system call unless the sleeper is really asleep, so it can be called often.
 * A wake that comes while the sleeper is awake is kept, and ends its next wait
 * at once, so none is lost between the sleeper's last look and its sleep. */
typedef struct host_waker host_waker;

host_waker *host_waker_create(void);
void        host_waker_destroy(host_waker *w);
void        host_waker_wake(host_waker *w);
/* Results as host_timer_wait_us_or_event's: HOST_WAIT_EVENT when woken. */
int         host_waker_wait_us(host_waker *w, host_timer *t, int64_t us, uint32_t cap_ms);

/* Make the calling thread a real-time one, for a host thread that wakes on
 * a cadence and does little each time (the vblank clock): on macOS
 * THREAD_TIME_CONSTRAINT_POLICY with these figures (0 period: aperiodic),
 * which removes the leeway described in host_timer.c, so this thread's
 * waits are one plain sleep instead of stepped ones and a spin. If the
 * scheduler demotes the thread later (a real-time thread that computes too
 * much is), its wakes come late again; the waits notice and go back to
 * stepping. 1 if the policy took, 0 where there is none (Windows, Linux
 * without privileges: nothing changes there). */
int host_thread_realtime(uint32_t period_us, uint32_t computation_us,
                         uint32_t constraint_us);

/* Make Sleep() good to about a millisecond where it is not already: on
 * Windows timeBeginPeriod(1), at the cost of a system-wide 1 kHz tick (the
 * vblank clock's fallback when there is no high-resolution timer). Nothing
 * on POSIX, where nanosleep already is. */
void host_sleep_precision_1ms(void);

#ifdef __cplusplus
}
#endif

#endif /* HOST_TIMER_H */
