#ifndef SLOT_GATE_H
#define SLOT_GATE_H

/* First-come first-served concurrency slots for the codex backend
 * (PLAN_OUT_OF_SCOPE.md C). Replaces a semaphore polled with sem_trywait, which
 * let a later waiter take a freed slot before an earlier one.
 *
 * Rules (all under one mutex): a waiter takes a slot only when it is the head of
 * the queue and a slot is free; a newcomer joins the queue whenever it is not
 * empty, so nobody overtakes; a waiter that leaves (deadline or cancellation) is
 * unlinked wherever it is; every change that may let the head proceed broadcasts.
 * FIFO means the order of joining the queue. */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "translation_control.h"

typedef struct SlotWaiter {
    struct SlotWaiter *prev, *next;
} SlotWaiter;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;        /* CLOCK_MONOTONIC */
    int             free_slots;
    int             waiters;
    uint64_t        grants;      /* slots handed out so far (grant numbers) */
    uint64_t        joins;       /* waiters that have joined the queue so far */
    SlotWaiter     *head, *tail;
} SlotGate;

/* 0, or -1 when slots < 1 or the monotonic condition variable cannot be made. */
int  slot_gate_init(SlotGate *g, int slots);
/* Only when no thread holds or waits for a slot. */
void slot_gate_destroy(SlotGate *g);

/* 0 = got a slot; -1 = deadline_ms (translation_now_ms() clock) reached, or the
 * control's total deadline / cancellation ended the wait. Like the code it
 * replaces (sem_trywait + translation_pause(control, 20)): a free slot is taken
 * before deadline_ms is looked at, but not once the total deadline or
 * cancellation has hit during a wait; cancellation is noticed within 20 ms and
 * no wait runs past either deadline. A NULL control has no total deadline. */
int  slot_gate_acquire(SlotGate *g, int64_t deadline_ms, const TranslationControl *control);
/* The same, also storing the grant number (0, 1, 2, ... in grant order, assigned
 * under the gate's mutex) - lets tests check the order without racing. */
int  slot_gate_acquire_numbered(SlotGate *g, int64_t deadline_ms, const TranslationControl *control,
                                uint64_t *grant_no);
void slot_gate_release(SlotGate *g);

/* For tests and logs. */
int  slot_gate_free(SlotGate *g);
int  slot_gate_waiters(SlotGate *g);
uint64_t slot_gate_joins(SlotGate *g);   /* total joins: a waiter that left does not lower it */

#endif /* SLOT_GATE_H */
