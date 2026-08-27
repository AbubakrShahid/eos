// SPDX-License-Identifier: MIT
// Copyright (c) 2026 EoS Project

/**
 * @file test_ipc_waiters.c
 * @brief Regression test for queue waiter-list bookkeeping in ipc.c
 *
 * The blocking send/receive paths cannot be exercised by test_kernel.c, whose
 * eos_port_yield() mock is empty: a task that blocks on a full queue simply
 * times out with nothing ever making space. This test provides a *scripted*
 * eos_port_yield() so we can drive the exact interleaving that used to leave a
 * satisfied waiter stranded in send_waiters[], which later caused a spurious
 * eos_task_unblock() of a task that was no longer waiting.
 *
 * It compiles kernel/src/ipc.c directly against these mocks rather than linking
 * the whole kernel, so eos_task_get_current()/eos_task_unblock() can be observed
 * and controlled.
 */

#include "eos/kernel.h"
#include "eos/kernel_internal.h"
#include "eos/arch.h"
#include <stdio.h>
#include <assert.h>

/* --- mocks required by ipc.c --- */
volatile uint32_t g_tick = 0;
eos_task_t g_tasks[EOS_MAX_TASKS];
uint8_t eos_task_get_priority_internal(eos_task_handle_t h) { (void)h; return 10; }
void eos_task_set_priority_internal(eos_task_handle_t h, uint8_t p) { (void)h; (void)p; }
void eos_task_block_with_timeout(eos_task_handle_t h, uint32_t t) { (void)h; (void)t; }

static uint8_t g_cur = 0;
eos_task_handle_t eos_task_get_current(void) { return g_cur; }

static int g_unblocked[64];
static int g_unblocked_n = 0;
void eos_task_unblock(eos_task_handle_t h) { g_unblocked[g_unblocked_n++] = h; }

uint32_t eos_port_enter_critical(void) { return 0; }
void eos_port_exit_critical(uint32_t s) { (void)s; }

/* Scripted yield: reproduce two waiters where the *second* one is satisfied
 * out of the waker's FIFO order (the classic stale-entry trigger). */
static eos_queue_handle_t g_q;
static int g_mode = 0;
void eos_port_yield(void)
{
    if (g_mode == 1) {                 /* running inside A(5)'s blocking send */
        g_mode = 2;                    /* B's nested send will hit mode 2 below */
        uint8_t save = g_cur;
        g_cur = 7; int v = 70;         /* B(7) also blocks, then drains a slot */
        eos_queue_send(g_q, &v, 100);
        g_cur = save;
    } else if (g_mode == 2) {          /* running inside B(7)'s blocking send */
        g_mode = 0;
        uint8_t save = g_cur;
        g_cur = 99; int t;
        eos_queue_receive(g_q, &t, EOS_NO_WAIT); /* frees a slot, wakes waiters[0]=A(5) */
        g_cur = save;
    }
}

int main(void)
{
    eos_queue_handle_t q; int v;
    assert(eos_queue_create(&q, sizeof(int), 2) == EOS_KERN_OK);
    g_q = q;

    /* Fill the queue (capacity 2). */
    v = 10; assert(eos_queue_send(q, &v, EOS_NO_WAIT) == EOS_KERN_OK);
    v = 11; assert(eos_queue_send(q, &v, EOS_NO_WAIT) == EOS_KERN_OK);

    /* A(5) blocks; its yield spins up B(7); B's yield drains one item, which
     * wakes A (waiters[0]) and frees a slot; B then succeeds on its retry. */
    g_cur = 5; v = 50; g_mode = 1; g_unblocked_n = 0;
    (void)eos_queue_send(q, &v, 100);
    printf("[info] tasks unblocked during interleaving: %d\n", g_unblocked_n);

    /* Probe: no task is waiting now. A correct queue must not unblock anyone
     * on a plain receive. Before the fix, B(7) lingered in send_waiters and was
     * spuriously force-woken here. */
    g_cur = 1; g_unblocked_n = 0;
    (void)eos_queue_receive(q, &v, EOS_NO_WAIT);
    assert(g_unblocked_n == 0 && "stale waiter caused a spurious eos_task_unblock()");
    printf("[PASS] no spurious wakeup after out-of-order queue satisfaction\n");

    /* Sanity: basic non-blocking behaviour still works. */
    eos_queue_handle_t q2; int out;
    assert(eos_queue_create(&q2, sizeof(int), 1) == EOS_KERN_OK);
    v = 42; assert(eos_queue_send(q2, &v, EOS_NO_WAIT) == EOS_KERN_OK);
    assert(eos_queue_is_full(q2));
    assert(eos_queue_receive(q2, &out, EOS_NO_WAIT) == EOS_KERN_OK && out == 42);
    assert(eos_queue_is_empty(q2));
    printf("[PASS] basic queue send/receive\n");

    printf("=== ALL IPC WAITER TESTS PASSED ===\n");
    return 0;
}
