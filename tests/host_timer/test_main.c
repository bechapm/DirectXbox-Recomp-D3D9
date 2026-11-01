/*
 * host_timer_test -- see CMakeLists.txt.
 */
#include "platform/xbox_winnt.h"
#include "platform/host_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
            g_failures++;                                             \
        }                                                             \
    } while (0)

/* How late a wake may be. Locally 20 ms, which a working timer never comes
 * near (the worst measured on this project's Macs is under 0.2 ms): a
 * regression to scheduler-tick sleeps fails here. On a CI runner -- a shared,
 * often overloaded VM, where GitHub's macOS one has been seen to wake 46 ms
 * late for an 8 ms wait -- only gross errors fail: 100 ms late, a wait that
 * returns early (checked everywhere, at the same bound), or an event that
 * never wakes the wait. The numbers are printed either way. */
static int64_t SLACK_US = 20000;

static int on_ci(void)
{
    const char *ci = getenv("CI"), *gha = getenv("GITHUB_ACTIONS");
    return (ci && *ci && strcmp(ci, "0") != 0 && strcmp(ci, "false") != 0) ||
           (gha && *gha && strcmp(gha, "false") != 0);
}

static HANDLE g_event;

static DWORD WINAPI set_later(LPVOID param)
{
    Sleep((DWORD)(uintptr_t)param);
    SetEvent(g_event);
    return 0;
}

static void test_wait_us(host_timer *t)
{
    static const int64_t asks[] = { 100, 500, 1000, 2500, 8000 };
    size_t i;

    for (i = 0; i < sizeof asks / sizeof asks[0]; i++) {
        int64_t worst = 0;
        int k;
        for (k = 0; k < 10; k++) {
            int64_t t0 = host_time_ns(), took_us;
            int r = host_timer_wait_us(t, asks[i], 1000);
            took_us = (host_time_ns() - t0) / 1000;
            CHECK(r == HOST_WAIT_ELAPSED, "wait_us(%lld) returned %d",
                  (long long)asks[i], r);
            CHECK(took_us >= asks[i] - 50, "wait_us(%lld) woke after %lld us",
                  (long long)asks[i], (long long)took_us);
            if (took_us - asks[i] > worst)
                worst = took_us - asks[i];
        }
        printf("wait_us(%5lld): worst overshoot %lld us\n",
               (long long)asks[i], (long long)worst);
        CHECK(worst < SLACK_US, "wait_us(%lld) overshot by %lld us",
              (long long)asks[i], (long long)worst);
    }
}

/* The coarse wait: never early, and not wildly late. It may overshoot by the
 * host's leeway (it does not spin), so it is held to the same loose bound. */
static void test_wait_us_coarse(host_timer *t)
{
    static const int64_t asks[] = { 50, 500, 2000 };
    size_t i;

    for (i = 0; i < sizeof asks / sizeof asks[0]; i++) {
        int64_t worst = 0;
        int k;
        for (k = 0; k < 10; k++) {
            int64_t t0 = host_time_ns(), took_us;
            int r = host_timer_wait_us_coarse(t, asks[i], 1000);
            took_us = (host_time_ns() - t0) / 1000;
            CHECK(r == HOST_WAIT_ELAPSED, "wait_us_coarse(%lld) returned %d",
                  (long long)asks[i], r);
            CHECK(took_us >= asks[i] - 50, "wait_us_coarse(%lld) woke after %lld us",
                  (long long)asks[i], (long long)took_us);
            if (took_us - asks[i] > worst)
                worst = took_us - asks[i];
        }
        printf("wait_us_coarse(%5lld): worst overshoot %lld us\n",
               (long long)asks[i], (long long)worst);
        CHECK(worst < SLACK_US, "wait_us_coarse(%lld) overshot by %lld us",
              (long long)asks[i], (long long)worst);
    }
    CHECK(host_timer_wait_us_coarse(t, 500000, 20) == HOST_WAIT_FAILED,
          "wait_us_coarse past its cap did not say so");
}

static void test_wait_until(host_timer *t)
{
    /* A periodic wait: each deadline is the last plus the period, so a late
     * wake does not push the next one. 60 Hz for 20 periods. */
    const int64_t period = 16666667;
    int64_t start = host_time_ns(), deadline = start, end;
    int k;

    for (k = 0; k < 20; k++) {
        deadline += period;
        CHECK(host_timer_wait_until(t, deadline, 1000) == HOST_WAIT_ELAPSED,
              "wait_until failed at period %d", k);
    }
    end = host_time_ns();
    printf("20 periods of 16.667 ms: %.3f ms (want 333.333)\n",
           (double)(end - start) / 1e6);
    CHECK(end >= deadline, "periodic wait finished early");
    CHECK(end - deadline < (int64_t)SLACK_US * 1000, "periodic wait drifted %lld us",
          (long long)((end - deadline) / 1000));
    CHECK(host_timer_wait_until(t, start, 1000) == HOST_WAIT_ELAPSED,
          "a past deadline did not return at once");
}

static void test_wait_or_event(host_timer *t)
{
    HANDLE th;
    int64_t t0, took_us;
    int r;

    g_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(g_event != NULL, "CreateEvent failed");

    /* No event: the time passes. */
    t0 = host_time_ns();
    r = host_timer_wait_us_or_event(t, 2000, g_event, 250);
    took_us = (host_time_ns() - t0) / 1000;
    CHECK(r == HOST_WAIT_ELAPSED, "or_event with no event returned %d", r);
    CHECK(took_us >= 1950 && took_us < 2000 + SLACK_US,
          "or_event(2000 us) took %lld us", (long long)took_us);

    /* Already signalled: at once, and the auto-reset signal is consumed. */
    SetEvent(g_event);
    r = host_timer_wait_us_or_event(t, 100000, g_event, 250);
    CHECK(r == HOST_WAIT_EVENT, "or_event with a set event returned %d", r);
    CHECK(WaitForSingleObject(g_event, 0) == WAIT_TIMEOUT,
          "auto-reset event still signalled after the wait");

    /* Signalled during the wait: wakes on it, long before the time. */
    th = CreateThread(NULL, 0, set_later, (LPVOID)(uintptr_t)5, 0, NULL);
    t0 = host_time_ns();
    r = host_timer_wait_us_or_event(t, 200000, g_event, 250);
    took_us = (host_time_ns() - t0) / 1000;
    printf("or_event woken by SetEvent after %lld us (set at ~5000)\n",
           (long long)took_us);
    CHECK(r == HOST_WAIT_EVENT, "or_event did not wake on the event (%d)", r);
    CHECK(took_us < 100000, "or_event took %lld us to see the event",
          (long long)took_us);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);

    /* The cap is shorter than the time: it says so. */
    r = host_timer_wait_us_or_event(t, 500000, g_event, 20);
    CHECK(r == HOST_WAIT_FAILED, "or_event past its cap returned %d", r);
    CloseHandle(g_event);
}

/* A real-time thread (host_thread_realtime): its waits are one sleep, and
 * still on time. Where the policy does not exist the test says so. */
static DWORD WINAPI realtime_thread(LPVOID param)
{
    host_timer *t = (host_timer *)param;
    int64_t worst = 0;
    int k;

    if (!host_thread_realtime(16667, 500, 2000)) {
        printf("real-time policy: not on this host\n");
        return 0;
    }
    for (k = 0; k < 20; k++) {
        int64_t t0 = host_time_ns(), took_us;
        host_timer_wait_us(t, 4000, 1000);
        took_us = (host_time_ns() - t0) / 1000;
        CHECK(took_us >= 3950, "real-time wait_us(4000) woke after %lld us",
              (long long)took_us);
        if (took_us - 4000 > worst)
            worst = took_us - 4000;
    }
    printf("real-time wait_us( 4000): worst overshoot %lld us\n", (long long)worst);
    CHECK(worst < SLACK_US, "real-time wait overshot by %lld us", (long long)worst);
    return 0;
}

static host_waker *g_waker;
static volatile LONG g_wake_rounds;

static DWORD WINAPI wake_later(LPVOID param)
{
    Sleep((DWORD)(uintptr_t)param);
    host_waker_wake(g_waker);
    return 0;
}

/* Wakes as fast as it can until told to stop. */
static DWORD WINAPI wake_storm(LPVOID param)
{
    (void)param;
    while (InterlockedCompareExchange(&g_wake_rounds, 0, 0) >= 0)
        host_waker_wake(g_waker);
    return 0;
}

static void test_waker(host_timer *t)
{
    int64_t t0, us;
    HANDLE th;
    int r, i, woken = 0;

    g_waker = host_waker_create();
    CHECK(g_waker != NULL, "host_waker_create failed");
    if (!g_waker)
        return;

    /* Nobody wakes it: the time passes. */
    t0 = host_time_ns();
    r = host_waker_wait_us(g_waker, t, 2000, 250);
    us = (host_time_ns() - t0) / 1000;
    CHECK(r == HOST_WAIT_ELAPSED, "an unwoken sleep gave %d", r);
    CHECK(us >= 1900 - 100, "an unwoken sleep of 2 ms ended after %lld us", (long long)us);

    /* A wake during the wait ends it long before its time. */
    th = CreateThread(NULL, 0, wake_later, (LPVOID)(uintptr_t)20, 0, NULL);
    t0 = host_time_ns();
    r = host_waker_wait_us(g_waker, t, 2000000, 5000);
    us = (host_time_ns() - t0) / 1000;
    CHECK(r == HOST_WAIT_EVENT, "a woken sleep gave %d", r);
    CHECK(us < 1000000, "a sleep woken at 20 ms ended after %lld us", (long long)us);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);

    /* A wake before the wait is not lost: the next one ends at once. */
    host_waker_wake(g_waker);
    t0 = host_time_ns();
    r = host_waker_wait_us(g_waker, t, 2000000, 5000);
    us = (host_time_ns() - t0) / 1000;
    CHECK(r == HOST_WAIT_EVENT, "a sleep after a wake gave %d", r);
    CHECK(us < 500000, "a sleep after a wake took %lld us", (long long)us);

    /* And it is spent: the one after that sleeps. */
    r = host_waker_wait_us(g_waker, t, 2000, 250);
    CHECK(r == HOST_WAIT_ELAPSED, "a wake was seen twice: %d", r);

    /* Wakes racing the sleeper leave nothing behind, and never hang it. */
    g_wake_rounds = 0;
    th = CreateThread(NULL, 0, wake_storm, NULL, 0, NULL);
    for (i = 0; i < 20000; i++) {
        r = host_waker_wait_us(g_waker, t, 200, 250);
        CHECK(r == HOST_WAIT_EVENT || r == HOST_WAIT_ELAPSED, "round %d gave %d", i, r);
        woken += r == HOST_WAIT_EVENT;
    }
    InterlockedExchange(&g_wake_rounds, -1);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    printf("waker: %d of 20000 sleeps woken under a storm of wakes\n", woken);
    CHECK(woken > 0, "no wake reached the sleeper");
    host_waker_wake(g_waker);       /* whatever the storm left, spent */
    (void)host_waker_wait_us(g_waker, t, 200, 250);
    r = host_waker_wait_us(g_waker, t, 2000, 250);
    CHECK(r == HOST_WAIT_ELAPSED, "a stale signal outlived the storm: %d", r);

    host_waker_destroy(g_waker);
}

int main(void)
{
    host_timer *t = host_timer_create(HOST_TIMER_ANY);

    if (on_ci()) {
        SLACK_US = 100000;
        printf("CI runner: only gross errors fail (100 ms late, an early wake, "
               "an event that does not wake)\n");
    }
    CHECK(t != NULL, "host_timer_create(ANY) returned NULL");
    if (!t)
        return 1;
    printf("high resolution: %s\n", host_timer_high_res(t) ? "yes" : "no");
    test_wait_us(t);
    test_wait_us_coarse(t);
    test_wait_until(t);
    test_wait_or_event(t);
    test_waker(t);
    {
        HANDLE th = CreateThread(NULL, 0, realtime_thread, t, 0, NULL);
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
    }
    host_timer_destroy(t);

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("host_timer: all passed\n");
    return 0;
}
