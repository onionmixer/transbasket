/* V3 of PLAN_OUT_OF_SCOPE.md: the FIFO slot gate (src/slot_gate.c).
 *
 *   make test-slot-gate          (also built with TSan by that target)
 *
 * Waiters are started one at a time and the test waits until each has joined the
 * queue (slot_gate_waiters), so "join order" is known exactly. It checks:
 *   1. grants follow join order (1 slot and 3 slots);
 *   2. a waiter leaving at its deadline - head, middle or tail - leaves on time,
 *      keeps the order of the rest and loses no slot; a slot freed as its own
 *      deadline passes is still taken (tried before the deadline is looked at);
 *   3. no overtaking: a newcomer arriving while others wait queues behind them;
 *   4. cancellation ends every wait within 50 ms and returns no slot;
 *   4a. a slot freed after the total deadline has passed during a wait is not
 *       taken (the order of the loop the gate replaced), and a wait ends at the
 *       total deadline, not up to 20 ms after it;
 *   5. init refuses 0 slots;
 *   5a. hand-off is prompt: freeing slots wakes the head at once (broadcast on
 *       release and after a grant), not only at its 20 ms re-check - also with six
 *       waiters, where a single signal could wake a waiter that is not the head;
 *   6. stress: 300 rounds of random holders / waiters / deadlines end with all
 *      slots free, no waiter left and every grant in join order among those that
 *      did not time out.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "slot_gate.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    SlotGate *g;
    int id;
    int64_t deadline;
    const TranslationControl *ctl;
    int hold_ms;              /* how long to keep a granted slot */
    int result;
    int64_t done_at;
    int *order;               /* order[k] = id of the k-th grant after the fill */
    atomic_int *order_n;
    const uint64_t *grant_base;   /* grants made before the waiters (the fill) */
} Waiter;

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *run(void *arg) {
    Waiter *w = arg;
    uint64_t grant = 0;
    uint64_t base = w->grant_base ? *w->grant_base : 0;
    w->result = slot_gate_acquire_numbered(w->g, w->deadline, w->ctl, &grant);
    w->done_at = translation_now_ms();
    if (w->result == 0) {
        /* the grant number is assigned under the gate's mutex: no logging race */
        w->order[grant - base] = w->id;
        atomic_fetch_add(w->order_n, 1);
        if (w->hold_ms) sleep_ms(w->hold_ms);
        slot_gate_release(w->g);
    }
    return NULL;
}

/* Start waiters one by one, each after the previous one has joined the queue.
 * The total join count is used, not the number waiting: a waiter leaving at its
 * deadline must not pass for the next one joining. */
static void start_in_order(SlotGate *g, Waiter *ws, pthread_t *th, int n) {
    for (int i = 0; i < n; i++) {
        uint64_t before = slot_gate_joins(g);
        pthread_create(&th[i], NULL, run, &ws[i]);
        int k = 0;
        while (k++ < 2000 && slot_gate_joins(g) == before) sleep_ms(1);
        CHECK(slot_gate_joins(g) == before + 1, "waiter %d joined the queue", i);
    }
}

static void fifo(int slots, int n) {
    SlotGate g;
    CHECK(slot_gate_init(&g, slots) == 0, "init %d", slots);
    for (int i = 0; i < slots; i++) CHECK(slot_gate_acquire(&g, translation_now_ms() + 1000, NULL) == 0, "fill");
    int order[64];
    atomic_int on = 0;
    Waiter ws[64];
    pthread_t th[64];
    const uint64_t base = (uint64_t)slots;           /* grants used by the fill above */
    for (int i = 0; i < n; i++) ws[i] = (Waiter){ &g, i, translation_now_ms() + 20000, NULL, 2, -9, 0, order, &on, &base };
    start_in_order(&g, ws, th, n);
    CHECK(slot_gate_waiters(&g) == n, "all %d queued (%d)", n, slot_gate_waiters(&g));
    for (int i = 0; i < slots; i++) slot_gate_release(&g);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    int ok = atomic_load(&on) == n;
    for (int i = 0; i < n && ok; i++) ok = order[i] == i;
    CHECK(ok, "%d slot(s): grants in join order", slots);
    CHECK(slot_gate_free(&g) == slots && slot_gate_waiters(&g) == 0, "%d slot(s): all free afterwards", slots);
    slot_gate_destroy(&g);
    printf("  %s %d slot(s), %d waiters: grants in join order, nothing lost\n", ok ? "ok  " : "FAIL", slots, n);
}

static void leave(int victim, const char *where) {
    SlotGate g;
    slot_gate_init(&g, 1);
    slot_gate_acquire(&g, translation_now_ms() + 1000, NULL);
    int order[8];
    atomic_int on = 0;
    Waiter ws[5];
    pthread_t th[5];
    static const uint64_t one = 1;                     /* one grant used by the fill */
    for (int i = 0; i < 5; i++) {
        int64_t dl = translation_now_ms() + (i == victim ? 150 : 20000);
        ws[i] = (Waiter){ &g, i, dl, NULL, 1, -9, 0, order, &on, &one };
    }
    start_in_order(&g, ws, th, 5);
    sleep_ms(300);                                 /* the victim's deadline passes */
    CHECK(slot_gate_waiters(&g) == 4, "%s: victim left the queue (%d waiting)", where, slot_gate_waiters(&g));
    slot_gate_release(&g);
    for (int i = 0; i < 5; i++) pthread_join(th[i], NULL);
    int exp[4], k = 0;
    for (int i = 0; i < 5; i++) if (i != victim) exp[k++] = i;
    int ok = ws[victim].result == -1 && atomic_load(&on) == 4;
    for (int i = 0; i < 4 && ok; i++) ok = order[i] == exp[i];
    CHECK(ok, "%s: the others keep their order", where);
    int64_t late = ws[victim].done_at - ws[victim].deadline;
    CHECK(late <= 5, "%s: the victim left %lld ms after its deadline", where, (long long)late);
    ok = ok && late <= 5;
    CHECK(slot_gate_free(&g) == 1, "%s: slot not lost", where);
    slot_gate_destroy(&g);
    printf("  %s waiter leaving at its deadline (%s): order and slot kept\n", ok ? "ok  " : "FAIL", where);
}

static void no_overtaking(void) {
    SlotGate g;
    slot_gate_init(&g, 1);
    slot_gate_acquire(&g, translation_now_ms() + 1000, NULL);
    int order[4];
    atomic_int on = 0;
    Waiter ws[2];
    pthread_t th[2];
    static const uint64_t one = 1;
    ws[0] = (Waiter){ &g, 0, translation_now_ms() + 20000, NULL, 50, -9, 0, order, &on, &one };
    start_in_order(&g, ws, th, 1);
    /* release and immediately try as a newcomer: the queued waiter must win */
    slot_gate_release(&g);
    int newcomer = slot_gate_acquire(&g, translation_now_ms() + 5000, NULL);
    int newcomer_rank = atomic_load(&on);          /* grants before ours */
    if (newcomer == 0) slot_gate_release(&g);
    pthread_join(th[0], NULL);
    CHECK(newcomer == 0 && newcomer_rank == 1 && order[0] == 0, "newcomer queued behind the waiter (rank %d)", newcomer_rank);
    CHECK(slot_gate_free(&g) == 1, "no overtaking: slot back");
    slot_gate_destroy(&g);
    printf("  %s no overtaking: a newcomer waits behind an existing waiter\n", newcomer_rank == 1 ? "ok  " : "FAIL");
}

static void cancellation(void) {
    SlotGate g;
    slot_gate_init(&g, 1);
    slot_gate_acquire(&g, translation_now_ms() + 1000, NULL);
    atomic_bool cancelled = false;
    TranslationControl ctl;
    clock_gettime(CLOCK_MONOTONIC, &ctl.deadline);
    ctl.deadline.tv_sec += 60;
    ctl.cancelled = &cancelled;
    int order[8];
    atomic_int on = 0;
    Waiter ws[4];
    pthread_t th[4];
    static const uint64_t one = 1;
    for (int i = 0; i < 4; i++) ws[i] = (Waiter){ &g, i, translation_now_ms() + 20000, &ctl, 0, -9, 0, order, &on, &one };
    start_in_order(&g, ws, th, 4);
    int64_t t0 = translation_now_ms();
    atomic_store(&cancelled, true);
    int64_t worst = 0;
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        if (ws[i].done_at - t0 > worst) worst = ws[i].done_at - t0;
    }
    int ok = atomic_load(&on) == 0 && worst <= 50 && slot_gate_waiters(&g) == 0;
    for (int i = 0; i < 4; i++) ok = ok && ws[i].result == -1;
    CHECK(ok, "cancellation: all left within 50 ms (worst %lld ms)", (long long)worst);
    slot_gate_release(&g);
    CHECK(slot_gate_free(&g) == 1, "cancellation: slot back");
    slot_gate_destroy(&g);
    printf("  %s cancellation ends every wait (worst %lld ms), no slot granted\n", ok ? "ok  " : "FAIL", (long long)worst);
}

/* The total deadline passes while a waiter is waiting; a slot freed after that
 * must not be taken (the old loop's translation_pause() returned false and it
 * did not try again). The test holds the gate's mutex across the deadline so the
 * waiter wakes, blocks on the mutex, and only then sees the freed slot. */
static void expired_no_grant(void) {
    SlotGate g;
    slot_gate_init(&g, 1);
    slot_gate_acquire(&g, translation_now_ms() + 1000, NULL);
    atomic_bool cancelled = false;
    TranslationControl ctl;
    clock_gettime(CLOCK_MONOTONIC, &ctl.deadline);
    ctl.deadline.tv_nsec += 60 * 1000000L;
    if (ctl.deadline.tv_nsec >= 1000000000L) { ctl.deadline.tv_sec++; ctl.deadline.tv_nsec -= 1000000000L; }
    ctl.cancelled = &cancelled;
    int order[2];
    atomic_int on = 0;
    Waiter w;
    pthread_t th;
    static const uint64_t one = 1;
    w = (Waiter){ &g, 0, translation_now_ms() + 20000, &ctl, 0, -9, 0, order, &on, &one };
    start_in_order(&g, &w, &th, 1);
    sleep_ms(20);
    pthread_mutex_lock(&g.mutex);
    sleep_ms(80);                       /* the total deadline (60 ms) passes in here */
    g.free_slots++;                     /* what slot_gate_release() does */
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.mutex);
    pthread_join(th, NULL);
    int ok = w.result == -1 && atomic_load(&on) == 0 && slot_gate_free(&g) == 1 && slot_gate_waiters(&g) == 0;
    CHECK(ok, "expired: result %d, grants %d, free %d", w.result, atomic_load(&on), slot_gate_free(&g));
    slot_gate_destroy(&g);
    printf("  %s a slot freed after the total deadline is not taken\n", ok ? "ok  " : "FAIL");
}

/* The slot is tried before the waiter's own deadline is looked at, as the old
 * loop did (sem_trywait, then the deadline): a slot freed while the waiter is
 * waking up past its deadline is still taken. Mutex held across the deadline as
 * in expired_no_grant(). */
static void own_deadline_grant(void) {
    SlotGate g;
    slot_gate_init(&g, 1);
    slot_gate_acquire(&g, translation_now_ms() + 1000, NULL);
    int order[2];
    atomic_int on = 0;
    Waiter w;
    pthread_t th;
    static const uint64_t one = 1;
    w = (Waiter){ &g, 0, translation_now_ms() + 60, NULL, 0, -9, 0, order, &on, &one };
    start_in_order(&g, &w, &th, 1);
    sleep_ms(20);
    pthread_mutex_lock(&g.mutex);
    sleep_ms(80);                       /* the waiter's own deadline (60 ms) passes in here */
    g.free_slots++;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.mutex);
    pthread_join(th, NULL);
    int ok = w.result == 0 && atomic_load(&on) == 1 && slot_gate_free(&g) == 1 && slot_gate_waiters(&g) == 0;
    CHECK(ok, "own deadline: result %d, grants %d, free %d", w.result, atomic_load(&on), slot_gate_free(&g));
    slot_gate_destroy(&g);
    printf("  %s a slot freed as the waiter's own deadline passes is still taken (tried first)\n", ok ? "ok  " : "FAIL");
}

/* A wait ends at the total deadline, not at the next 20 ms re-check after it
 * (translation_pause() cut its last slice to the time left). 20 deadlines,
 * 25..44 ms ahead, so they fall at every point of a 20 ms slice. */
static void total_deadline_prompt(void) {
    int64_t worst = 0;
    for (int d = 25; d < 45; d++) {
        SlotGate g;
        slot_gate_init(&g, 1);
        slot_gate_acquire(&g, translation_now_ms() + 1000, NULL);
        atomic_bool cancelled = false;
        TranslationControl ctl;
        clock_gettime(CLOCK_MONOTONIC, &ctl.deadline);
        int64_t end = translation_now_ms() + d;
        ctl.deadline.tv_sec = (time_t)(end / 1000);
        ctl.deadline.tv_nsec = (long)(end % 1000) * 1000000L;
        ctl.cancelled = &cancelled;
        int order[2];
        atomic_int on = 0;
        Waiter w;
        pthread_t th;
        static const uint64_t one = 1;
        w = (Waiter){ &g, 0, translation_now_ms() + 20000, &ctl, 0, -9, 0, order, &on, &one };
        start_in_order(&g, &w, &th, 1);
        pthread_join(th, NULL);
        if (w.result != -1) worst = 1000;
        if (w.done_at - end > worst) worst = w.done_at - end;
        slot_gate_release(&g);
        slot_gate_destroy(&g);
    }
    CHECK(worst <= 5, "total deadline: a wait ended %lld ms after it", (long long)worst);
    printf("  %s a wait ends at the total deadline (worst %lld ms late, 20 deadlines)\n", worst <= 5 ? "ok  " : "FAIL", (long long)worst);
}

/* Release `slots` held slots at once to `n` queued waiters; return the time
 * until the first `slots` of them have their slot. */
static int64_t handoff_ms(int slots, int n) {
    SlotGate g;
    slot_gate_init(&g, slots);
    for (int i = 0; i < slots; i++) slot_gate_acquire(&g, translation_now_ms() + 1000, NULL);
    int order[8];
    atomic_int on = 0;
    Waiter ws[8];
    pthread_t th[8];
    const uint64_t base = (uint64_t)slots;
    /* granted waiters hold their slot (30 ms) so a freed slot can only go to a
     * waiter; with more waiters than slots, the first `slots` hold long enough
     * (200 ms) that the rest are still waiting when they get theirs */
    for (int i = 0; i < n; i++)
        ws[i] = (Waiter){ &g, i, translation_now_ms() + 20000, NULL, n > slots ? (i < slots ? 200 : 0) : 30,
                          -9, 0, order, &on, &base };
    start_in_order(&g, ws, th, n);
    sleep_ms(5);
    int64_t t0 = translation_now_ms(), last = t0;
    for (int i = 0; i < slots; i++) slot_gate_release(&g);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < slots; i++) if (ws[i].done_at > last) last = ws[i].done_at;
    slot_gate_destroy(&g);
    return last - t0;
}

static void handoff(void) {
    /* 1 slot / 1 waiter, 3 slots freed at once / 3 waiters, and 1 slot / 6 waiters:
     * with several waiters a single signal may wake one that is not the head */
    /* rounds: {3, 3} is where a signal mutant shows (about 1 round in 30, measured
     * 2026-11-07), so it runs 200 times */
    static const int cases[][3] = { {1, 1, 20}, {3, 3, 200}, {1, 6, 20} };
    int64_t worst[3] = {0, 0, 0};
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < cases[c][2]; r++) {
            int64_t t = handoff_ms(cases[c][0], cases[c][1]);
            if (t > worst[c]) worst[c] = t;
        }
    int ok = worst[0] <= 10 && worst[1] <= 10 && worst[2] <= 10;
    CHECK(ok, "hand-off: worst %lld / %lld / %lld ms", (long long)worst[0], (long long)worst[1], (long long)worst[2]);
    printf("  %s hand-off is prompt: worst %lld ms (1 slot), %lld ms (3 slots freed at once, 200 rounds), "
           "%lld ms (1 slot, 6 waiting)\n", ok ? "ok  " : "FAIL", (long long)worst[0], (long long)worst[1], (long long)worst[2]);
}

static void stress(void) {
    unsigned seed = 20261107;
    int bad = 0;
    for (int round = 0; round < 300; round++) {
        int slots = 1 + rand_r(&seed) % 3, n = 2 + rand_r(&seed) % 6;
        SlotGate g;
        slot_gate_init(&g, slots);
        int order[16];
        atomic_int on = 0;
        Waiter ws[16];
        pthread_t th[16];
        for (int i = 0; i < slots; i++) slot_gate_acquire(&g, translation_now_ms() + 1000, NULL);
        const uint64_t base = (uint64_t)slots;
        for (int i = 0; i < n; i++) {
            int short_dl = rand_r(&seed) % 4 == 0;
            /* a short deadline still outlives the join check in start_in_order() */
            ws[i] = (Waiter){ &g, i, translation_now_ms() + (short_dl ? 30 + rand_r(&seed) % 30 : 20000), NULL,
                              rand_r(&seed) % 2, -9, 0, order, &on, &base };
        }
        start_in_order(&g, ws, th, n);
        for (int i = 0; i < slots; i++) slot_gate_release(&g);
        for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
        /* grants among long-deadline waiters must be increasing in id */
        int last = -1, granted = 0, longs = 0;
        for (int i = 0; i < n; i++) longs += ws[i].deadline - ws[i].done_at > 10000 || ws[i].result == 0;
        for (int k = 0; k < atomic_load(&on); k++) {
            int id = order[k];
            if (ws[id].deadline > translation_now_ms() + 5000) {   /* long deadline */
                if (id < last) bad++;
                last = id;
            }
            granted++;
        }
        for (int i = 0; i < n; i++) if (ws[i].deadline > translation_now_ms() + 5000 && ws[i].result != 0) bad++;
        if (slot_gate_free(&g) != slots || slot_gate_waiters(&g) != 0) bad++;
        (void)granted; (void)longs;
        slot_gate_destroy(&g);
    }
    CHECK(bad == 0, "stress: %d violations", bad);
    printf("  %s stress: 300 rounds, every long-deadline waiter served in join order, all slots back\n", bad ? "FAIL" : "ok  ");
}

int main(void) {
    fifo(1, 6);
    fifo(3, 10);
    leave(0, "head");
    leave(2, "middle");
    leave(4, "tail");
    no_overtaking();
    cancellation();
    expired_no_grant();
    total_deadline_prompt();
    own_deadline_grant();
    SlotGate g;
    CHECK(slot_gate_init(&g, 0) == -1, "init with 0 slots must fail");
    printf("  %s init refuses 0 slots\n", failures ? "...." : "ok  ");
    handoff();
    stress();
    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
