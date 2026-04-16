#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <stdbool.h>
#include <time.h>

#include "arena.c"

/* ── minimal test framework ──────────────────────────────────────────────── */
static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define ASSERT(cond, msg)                                                       \
    do {                                                                        \
        g_tests_run++;                                                          \
        if (cond) {                                                             \
            printf("  [PASS] %s\n", msg);                                      \
            g_tests_passed++;                                                   \
        } else {                                                                \
            printf("  [FAIL] %s  (line %d)\n", msg, __LINE__);                 \
        }                                                                       \
    } while (0)

#define TEST(name) printf("\n=== %s ===\n", name)

/* ── capacity helpers ────────────────────────────────────────────────────────
 * sizeof(mem_arena) is now 32 bytes (4 x u64: total_capacity, pos,
 * committed, page_size).  All ARENA_OFFSET references use sizeof(mem_arena)
 * so they stay correct if the struct changes again.                         */
#define TEST_CAP  (sizeof(mem_arena) + 512)   /* usable: 512 bytes */
#define SMALL_CAP (sizeof(mem_arena) + 32)    /* usable:  32 bytes */

/* ══════════════════════════════════════════════════════════════════════════════
 * 1. CREATE_ARENA
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_create_arena(void)
{
    TEST("create_arena");

    u64 page_size = get_page_size();

    /* Normal creation — all fields must be initialised correctly. */
    mem_arena *a = create_arena(TEST_CAP);
    ASSERT(a != NULL,
           "create_arena returns non-NULL for a valid capacity");
    ASSERT(a->total_capacity == TEST_CAP,
           "total_capacity stored matches the requested value");
    ASSERT(a->pos == sizeof(mem_arena),
           "pos initialised to ARENA_OFFSET (sizeof(mem_arena) = 32)");
    ASSERT(a->page_size == page_size,
           "page_size field stores the system page size");
    ASSERT(a->committed == page_size,
           "committed initialised to one page");
    destroy_arena(a);

    /* capacity exactly one page. */
    mem_arena *b = create_arena(page_size);
    ASSERT(b != NULL,           "create_arena(page_size) returns non-NULL");
    ASSERT(b->total_capacity == page_size, "total_capacity == page_size");
    ASSERT(b->committed == page_size,      "committed == page_size for one-page arena");
    destroy_arena(b);

    /* capacity smaller than a page — mmap still maps a full page internally,
     * but total_capacity limits usable bytes.                               */
    mem_arena *c = create_arena(SMALL_CAP);
    ASSERT(c != NULL,                       "create_arena(SMALL_CAP) returns non-NULL");
    ASSERT(c->total_capacity == SMALL_CAP,  "total_capacity stores the requested small value");
    ASSERT(c->pos == sizeof(mem_arena),     "pos initialised to ARENA_OFFSET for small arena");
    destroy_arena(c);

    /* Zero capacity: mmap cannot reserve 0 bytes; must return NULL. */
    mem_arena *z = create_arena(0);
    ASSERT(z == NULL,
           "create_arena(0) returns NULL (mmap cannot reserve 0 bytes)");
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 2. ARENA_PUSH – SINGLE
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_push_single(void)
{
    TEST("arena_push – single allocation");

    mem_arena *a = create_arena(TEST_CAP);

    void *ptr = arena_push(a, 8);
    ASSERT(ptr != NULL,
           "8-byte push returns non-NULL");
    ASSERT(ptr == (u8 *)a + ARENA_OFFSET,
           "first push lands at (base + ARENA_OFFSET)");
    ASSERT(a->pos == ARENA_OFFSET + 8,
           "pos advances by 8 after an 8-byte push");

    *(u64 *)ptr = 0xCAFEBABEDEADBEEFULL;
    ASSERT(*(u64 *)ptr == 0xCAFEBABEDEADBEEFULL,
           "value written to allocation is readable back");

    /* Push of size 0 — pos and ptr behaviour: pos advances by 0, ptr is
     * valid (points to current pos).                                        */
    u64 pos_before_zero = a->pos;
    void *zptr = arena_push(a, 0);
    ASSERT(zptr != NULL,           "push(0) returns a non-NULL pointer");
    ASSERT(a->pos == pos_before_zero, "push(0) does not advance pos");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 3. ARENA_PUSH – MULTIPLE SEQUENTIAL
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_push_multiple(void)
{
    TEST("arena_push – multiple sequential allocations");

    mem_arena *a = create_arena(TEST_CAP);

    /* pos comments reflect ARENA_OFFSET = 32 (sizeof(mem_arena)). */
    void *p1 = arena_push(a, 8);    /* pos: 32 → 40  */
    void *p2 = arena_push(a, 8);    /* pos: 40 → 48  */
    void *p3 = arena_push(a, 16);   /* pos: 48 → 64  */

    ASSERT(p1 != NULL && p2 != NULL && p3 != NULL,
           "all three allocations return non-NULL");
    ASSERT((u8 *)p2 == (u8 *)p1 + 8,
           "p2 starts immediately after p1 (no gap)");
    ASSERT((u8 *)p3 == (u8 *)p2 + 8,
           "p3 starts immediately after p2 (no gap)");

    /* A failed push must not mutate pos. */
    u64 pos_before_fail = a->pos;
    ASSERT(arena_push(a, TEST_CAP * 2) == NULL,
           "push larger than remaining capacity returns NULL");
    ASSERT(a->pos == pos_before_fail,
           "pos is unchanged after a failed push");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 4. ARENA_PUSH – ALIGNMENT
 * Every returned pointer must be aligned to ARENA_ALIGN (sizeof(void*) = 8)
 * regardless of requested size.
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_push_alignment(void)
{
    TEST("arena_push – pointer alignment for non-multiple-of-8 sizes");

    mem_arena *a = create_arena(1024);

    const u64 sizes[]  = { 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 13, 15, 17, 31, 33 };
    const int  n_sizes = (int)(sizeof sizes / sizeof sizes[0]);

    for (int i = 0; i < n_sizes; i++) {
        u64 pos_before = a->pos;
        void *ptr = arena_push(a, sizes[i]);
        ASSERT(ptr != NULL, "allocation succeeds");

        bool ptr_aligned = ((uintptr_t)ptr % ARENA_ALIGN) == 0;
        bool pos_aligned = (a->pos % ARENA_ALIGN) == 0;
        bool pos_advanced = (a->pos >= pos_before + sizes[i]) &&
                            (a->pos == pos_before + ALIGN_POW2(sizes[i], ARENA_ALIGN));

        printf("  size %-3llu  ptr %-18p  ptr_aligned: %-3s  pos_correct: %s\n",
               (unsigned long long)sizes[i], ptr,
               ptr_aligned ? "YES" : "NO",
               pos_advanced ? "YES" : "NO");

        ASSERT(ptr_aligned,   "returned pointer is aligned to sizeof(void*)");
        ASSERT(pos_aligned,   "pos stays a multiple of ARENA_ALIGN after push");
        ASSERT(pos_advanced,  "pos advances by ALIGN_POW2(requested_size, 8)");
    }

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 5. ARENA_PUSH – CAPACITY BOUNDARY
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_push_capacity_boundary(void)
{
    TEST("arena_push – exact capacity boundary and over-capacity");

    mem_arena *a = create_arena(SMALL_CAP);
    u64 usable = a->total_capacity - ARENA_OFFSET;

    void *ptr = arena_push(a, usable);
    ASSERT(ptr != NULL,
           "push of exactly usable capacity succeeds");
    ASSERT(a->pos == a->total_capacity,
           "pos equals total_capacity after filling arena exactly");

    ASSERT(arena_push(a, 1) == NULL,
           "push of 1 byte beyond capacity returns NULL");
    ASSERT(a->pos == a->total_capacity,
           "pos unchanged after over-capacity push");
    destroy_arena(a);

    /* Initial over-capacity push on a fresh arena. */
    mem_arena *b = create_arena(SMALL_CAP);
    u64 pos_clean = b->pos;
    ASSERT(arena_push(b, 1000) == NULL,
           "vastly oversized single push returns NULL");
    ASSERT(b->pos == pos_clean,
           "pos unchanged after vastly oversized push");
    destroy_arena(b);

    /* Push exactly 1 byte below the capacity limit, then 1 byte over. */
    mem_arena *c    = create_arena(SMALL_CAP);
    u64 one_short   = usable - 8;   /* leave 8 bytes of usable space */
    arena_push(c, one_short);
    ASSERT(arena_push(c, 8) != NULL,
           "push fitting in remaining 8 bytes succeeds");
    ASSERT(arena_push(c, 1) == NULL,
           "push of 1 byte when arena is exactly full returns NULL");
    destroy_arena(c);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 6. ARENA_PUSH – PAGE COMMIT TRACKING
 *
 * When a push causes new_pos to exceed arena->committed, the allocator must
 * extend committed by exactly ALIGN_POW2(new_pos - old_committed, page_size).
 *
 * BUG in current implementation (arena.c line 101):
 *   commit_size = ALIGN_POW2(size, arena->page_size)   ← uses 'size'
 * Correct:
 *   commit_size = ALIGN_POW2(new_pos - arena->committed, arena->page_size)
 *
 * This matters when 'size' alone rounds to a larger page multiple than the
 * actual overflow (new_pos - old_committed) does.  The test below will FAIL
 * until the bug is fixed.
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_push_commit_tracking(void)
{
    TEST("arena_push – committed field tracks pages correctly (BUG expected)");

    u64 page_size = get_page_size();
    u64 capacity  = 4 * page_size;

    mem_arena *a = create_arena(capacity);
    ASSERT(a->committed == page_size,
           "committed starts at one page");

    /* Push exactly page_size bytes.
     * Starting pos = ARENA_OFFSET = 32.
     * new_pos = 32 + page_size.  Only 32 bytes spill into the second page.
     * Correct commit_size = ALIGN_POW2(32, page_size) = page_size.
     * Bug      commit_size = ALIGN_POW2(page_size, page_size) = page_size.
     * Both agree here — this push alone does NOT expose the bug.            */
    u64 old_committed = a->committed;
    void *p1 = arena_push(a, page_size);
    ASSERT(p1 != NULL, "push(page_size) succeeds");
    u64 new_pos_1 = ARENA_OFFSET + ALIGN_POW2(page_size, ARENA_ALIGN);
    u64 expected_committed_1 = old_committed +
        ALIGN_POW2(new_pos_1 - old_committed, page_size);
    ASSERT(a->committed == expected_committed_1,
           "committed correct after push(page_size) from ARENA_OFFSET");

    /* Reset and use a push size that exposes the bug.
     * push_size = 2*page_size - ARENA_OFFSET  (e.g. 8160 on 4 KiB pages).
     * new_pos   = ARENA_OFFSET + push_size = 2 * page_size.
     * Overflow  = 2*page_size - page_size = page_size.
     * Correct commit_size = ALIGN_POW2(page_size,         page_size) = page_size.
     * Bug     commit_size = ALIGN_POW2(2*page_size - 32,  page_size) = 2*page_size.
     * committed should be 2*page_size; bug gives 3*page_size.              */
    mem_arena *b = create_arena(capacity);
    u64 push_size = 2 * page_size - ARENA_OFFSET;
    u64 b_old_committed = b->committed;
    void *p2 = arena_push(b, push_size);
    ASSERT(p2 != NULL, "large push succeeds");
    u64 new_pos_2 = ARENA_OFFSET + ALIGN_POW2(push_size, ARENA_ALIGN);
    u64 expected_committed_2 = b_old_committed +
        ALIGN_POW2(new_pos_2 - b_old_committed, page_size);
    printf("  push_size=%llu  new_pos=%llu  expected committed=%llu  actual=%llu\n",
           (unsigned long long)push_size,
           (unsigned long long)new_pos_2,
           (unsigned long long)expected_committed_2,
           (unsigned long long)b->committed);
    ASSERT(b->committed == expected_committed_2,
           "BUG: committed must equal old_committed + ALIGN_POW2(new_pos - old_committed, page_size)");
    destroy_arena(b);

    /* A push that stays within already-committed memory must NOT change committed. */
    mem_arena *c = create_arena(capacity);
    u64 c_committed_before = c->committed;
    arena_push(c, 8);
    ASSERT(c->committed == c_committed_before,
           "small push within committed range leaves committed unchanged");
    destroy_arena(c);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 7. ARENA_POP – BASIC
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_arena_pop(void)
{
    TEST("arena_pop");

    mem_arena *a = create_arena(TEST_CAP);

    arena_push(a, 8);
    arena_push(a, 8);
    u64 pos_after_two = a->pos;   /* ARENA_OFFSET + 16 */

    arena_pop(a, 8);
    ASSERT(a->pos == pos_after_two - 8,
           "pop(8) decrements pos by 8");

    /* ALIGN_POW2(3, 8) = 8; remaining = 8; guard: 8 > 8 is FALSE → pops. */
    arena_pop(a, 3);
    ASSERT(a->pos == sizeof(mem_arena),
           "pop(3) rounds up to 8, fully emptying the arena back to ARENA_OFFSET");

    /* Pop on empty arena must be a safe no-op. */
    arena_pop(a, 8);
    ASSERT(a->pos == sizeof(mem_arena),
           "pop on empty arena leaves pos at ARENA_OFFSET (no underflow)");

    /* Pop more than currently used must be a safe no-op. */
    arena_push(a, 8);
    u64 before_over = a->pos;
    arena_pop(a, 200);
    ASSERT(a->pos == before_over,
           "pop(200) when only 8 bytes used is a safe no-op");

    /* Pop exactly the amount used empties the arena. */
    arena_pop(a, 8);
    ASSERT(a->pos == sizeof(mem_arena),
           "pop(exactly used bytes) returns pos to ARENA_OFFSET");

    /* Pop of 0 is a no-op. */
    arena_push(a, 8);
    u64 before_zero = a->pos;
    arena_pop(a, 0);
    ASSERT(a->pos == before_zero,
           "pop(0) is a no-op");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 8. ARENA_POP – LIFO SEQUENCE
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_lifo(void)
{
    TEST("LIFO – push three blocks, pop in reverse order");

    mem_arena *a = create_arena(TEST_CAP);
    u64 p0 = a->pos;   /* ARENA_OFFSET = 32 */

    arena_push(a,  8);  u64 p1 = a->pos;   /* 40  */
    arena_push(a, 16);  u64 p2 = a->pos;   /* 56  */
    arena_push(a, 24);  u64 p3 = a->pos;   /* 80  */

    ASSERT(p1 == ARENA_OFFSET +  8, "after push(8)  pos == ARENA_OFFSET + 8");
    ASSERT(p2 == ARENA_OFFSET + 24, "after push(16) pos == ARENA_OFFSET + 24");
    ASSERT(p3 == ARENA_OFFSET + 48, "after push(24) pos == ARENA_OFFSET + 48");

    arena_pop(a, 24); ASSERT(a->pos == p2, "pop(24) restores pos to p2");
    arena_pop(a, 16); ASSERT(a->pos == p1, "pop(16) restores pos to p1");
    arena_pop(a,  8); ASSERT(a->pos == p0, "pop(8)  restores pos to p0 (base)");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 9. ARENA_POP_TO
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_arena_pop_to(void)
{
    TEST("arena_pop_to");

    mem_arena *a = create_arena(TEST_CAP);

    /* Pop to base checkpoint restores pos all the way to ARENA_OFFSET. */
    u64 base = a->pos;
    arena_push(a, 8);
    arena_push(a, 16);
    arena_push(a, 24);
    ASSERT(a->pos > base, "pos advanced past base after three pushes");
    arena_pop_to(a, base);
    ASSERT(a->pos == base,
           "pop_to base resets pos to ARENA_OFFSET");

    /* Mid-sequence checkpoint. */
    arena_push(a, 8);
    arena_push(a, 8);
    u64 mid = a->pos;
    arena_push(a, 8);
    arena_push(a, 16);
    arena_pop_to(a, mid);
    ASSERT(a->pos == mid,
           "pop_to mid-checkpoint lands exactly at the saved position");

    /* pop_to current pos is a no-op. */
    u64 cur = a->pos;
    arena_pop_to(a, cur);
    ASSERT(a->pos == cur,
           "pop_to current pos is a no-op");

    /* pop_to a future position (> current pos) is a no-op. */
    u64 pos_before_future = a->pos;
    arena_pop_to(a, a->pos + 32);
    ASSERT(a->pos == pos_before_future,
           "pop_to a future position is a no-op");

    /* pop_to 0 (below ARENA_OFFSET): guard must prevent underflow. */
    arena_push(a, 8);
    arena_pop_to(a, 0);
    ASSERT(a->pos >= ARENA_OFFSET,
           "pop_to 0 must never underflow below ARENA_OFFSET");

    /* After pop_to, arena is still usable. */
    arena_pop_to(a, base);
    ASSERT(arena_push(a, 8) != NULL,
           "arena is still pushable after pop_to");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 10. ARENA_CLEAR
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_arena_clear(void)
{
    TEST("arena_clear");

    u64 page_size = get_page_size();

    /* Basic clear: pos and committed must both be reset. */
    {
        mem_arena *a = create_arena(TEST_CAP);
        arena_push(a, 64);
        arena_push(a, 32);
        arena_clear(a);
        ASSERT(a->pos == ARENA_OFFSET,
               "clear resets pos to ARENA_OFFSET");
        ASSERT(a->committed == page_size,
               "clear resets committed to page_size");
        destroy_arena(a);
    }

    /* Arena must be fully reusable after clear. */
    {
        mem_arena *a = create_arena(TEST_CAP);
        arena_push(a, 128);
        arena_clear(a);
        void *p = arena_push(a, 8);
        ASSERT(p != NULL,
               "arena accepts allocations after clear");
        ASSERT(p == (u8 *)a + ARENA_OFFSET,
               "first allocation after clear sits at ARENA_OFFSET");
        destroy_arena(a);
    }

    /* Clear with only one page ever committed — no decommit fires,
     * but pos and committed must still be in the correct state.   */
    {
        mem_arena *a = create_arena(TEST_CAP);
        arena_push(a, 8);   /* stays within first page */
        arena_clear(a);
        ASSERT(a->pos == ARENA_OFFSET,
               "clear with single-page usage resets pos");
        ASSERT(a->committed == page_size,
               "clear with single-page usage keeps committed at page_size");
        destroy_arena(a);
    }

    /* Multi-page clear: fill into a second page, clear, verify committed
     * drops back to one page and arena is reusable.                      */
    {
        u64 capacity = 4 * page_size;
        mem_arena *a = create_arena(capacity);

        /* Push enough to spill into the second page. */
        u64 fill = page_size;   /* new_pos = ARENA_OFFSET + page_size > page_size */
        arena_push(a, fill);
        ASSERT(a->committed > page_size,
               "committed grows beyond one page after multi-page push");

        arena_clear(a);
        ASSERT(a->pos == ARENA_OFFSET,
               "multi-page clear resets pos to ARENA_OFFSET");
        ASSERT(a->committed == page_size,
               "multi-page clear resets committed to exactly one page");

        /* Allocate again to confirm memory is accessible. */
        void *p = arena_push(a, 8);
        ASSERT(p != NULL,       "allocation succeeds after multi-page clear");
        *(u64 *)p = 0xDEADBEEFULL;
        ASSERT(*(u64 *)p == 0xDEADBEEFULL,
               "allocation after multi-page clear is writable and readable");
        destroy_arena(a);
    }

    /* Multiple clear cycles must be idempotent. */
    {
        mem_arena *a = create_arena(TEST_CAP);
        for (int i = 0; i < 5; i++) {
            arena_push(a, 64);
            arena_push(a, 32);
            arena_clear(a);
        }
        ASSERT(a->pos == ARENA_OFFSET,
               "pos is ARENA_OFFSET after 5 fill-and-clear cycles");
        ASSERT(a->committed == page_size,
               "committed is page_size after 5 fill-and-clear cycles");
        destroy_arena(a);
    }
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 11. WRITE / READ SAFETY — NO OVERLAP OR CROSS-SLOT CORRUPTION
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_write_read_safety(void)
{
    TEST("write/read safety – no overlap or cross-slot corruption");

    mem_arena *a = create_arena(4096);

    u64 *pa = (u64 *)arena_push(a, sizeof(u64));
    u64 *pb = (u64 *)arena_push(a, sizeof(u64));
    u32 *pc = (u32 *)arena_push(a, sizeof(u32));

    ASSERT(pa && pb && pc, "all three allocations succeed");
    if (!pa || !pb || !pc) { destroy_arena(a); return; }

    *pa = 0xDEADBEEFCAFEBABEULL;
    *pb = 0x0102030405060708ULL;
    *pc = 0xABCD1234U;

    ASSERT(*pa == 0xDEADBEEFCAFEBABEULL, "pa retains its sentinel value");
    ASSERT(*pb == 0x0102030405060708ULL,  "pb retains its sentinel value");
    ASSERT(*pc == 0xABCD1234U,            "pc retains its sentinel value");

    /* Mutate pc; pa and pb must not be affected. */
    *pc = 0x00000000U;
    ASSERT(*pa == 0xDEADBEEFCAFEBABEULL, "pa unaffected by write to pc");
    ASSERT(*pb == 0x0102030405060708ULL,  "pb unaffected by write to pc");

    /* Mutate pa; pb and pc must not be affected. */
    *pa = 0x1111111111111111ULL;
    ASSERT(*pb == 0x0102030405060708ULL, "pb unaffected by write to pa");
    ASSERT(*pc == 0x00000000U,           "pc unaffected by write to pa");

    /* Byte array: write pattern, verify every byte. */
    u8 *buf = (u8 *)arena_push(a, 32);
    ASSERT(buf != NULL, "32-byte buffer allocation succeeds");
    if (buf) {
        for (int i = 0; i < 32; i++) buf[i] = (u8)(i * 3 + 7);
        bool ok = true;
        for (int i = 0; i < 32; i++) ok &= (buf[i] == (u8)(i * 3 + 7));
        ASSERT(ok, "all 32 bytes of buffer read back correctly");
    }

    /* Pointers must not overlap. */
    ASSERT((u8 *)pb >= (u8 *)pa + sizeof(u64), "pb does not overlap pa");
    ASSERT((u8 *)pc >= (u8 *)pb + sizeof(u64), "pc does not overlap pb");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 12. DESTROY_ARENA
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_destroy_arena(void)
{
    TEST("destroy_arena");

    /* Destroying a used arena must not crash. */
    mem_arena *a = create_arena(TEST_CAP);
    arena_push(a, 32);
    arena_push(a, 64);
    destroy_arena(a);
    ASSERT(true, "destroy_arena on a used arena does not crash");

    /* Destroying an empty (just created) arena must not crash. */
    mem_arena *b = create_arena(TEST_CAP);
    destroy_arena(b);
    ASSERT(true, "destroy_arena on an empty arena does not crash");

    /* Destroying a cleared arena must not crash. */
    mem_arena *c = create_arena(TEST_CAP);
    arena_push(c, 128);
    arena_clear(c);
    destroy_arena(c);
    ASSERT(true, "destroy_arena on a cleared arena does not crash");

    /* Destroying a multi-page arena must not crash. */
    u64 page_size = get_page_size();
    mem_arena *d = create_arena(4 * page_size);
    arena_push(d, 2 * page_size);
    destroy_arena(d);
    ASSERT(true, "destroy_arena on a multi-page arena does not crash");
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 13. STRESS – SEQUENTIAL FILL AND DRAIN
 * ══════════════════════════════════════════════════════════════════════════════ */
#define STRESS_CAP   (64u * 1024u)
#define STRESS_CHUNK 8u

static void test_stress(void)
{
    TEST("stress – sequential fill and full drain (64 KiB arena)");

    mem_arena *a           = create_arena(STRESS_CAP);
    u64        usable       = a->total_capacity - ARENA_OFFSET;
    u64        expect_count = usable / STRESS_CHUNK;
    u64        count        = 0;

    while (arena_push(a, STRESS_CHUNK)) count++;

    printf("  capacity %u B  usable %llu B  expected %llu allocs  actual %llu\n",
           STRESS_CAP,
           (unsigned long long)usable,
           (unsigned long long)expect_count,
           (unsigned long long)count);

    ASSERT(count == expect_count,
           "arena fills with exactly (usable / chunk_size) allocations");

    /* Drain via pop_to and verify the arena is fully reusable. */
    arena_pop_to(a, ARENA_OFFSET);
    ASSERT(a->pos == ARENA_OFFSET,
           "pop_to ARENA_OFFSET after full fill resets arena");
    ASSERT(arena_push(a, STRESS_CHUNK) != NULL,
           "arena accepts allocations again after full drain");

    /* Fill again after drain — count must be the same. */
    arena_pop_to(a, ARENA_OFFSET);
    u64 count2 = 0;
    while (arena_push(a, STRESS_CHUNK)) count2++;
    ASSERT(count2 == expect_count,
           "second fill after drain yields the same allocation count");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 14. RANDOMISED ALLOCATION SIZES
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_random_allocs(void)
{
    TEST("randomised allocation sizes (1–128 bytes)");

    srand((unsigned)time(NULL));

    mem_arena *a           = create_arena(16384);
    int        ok          = 0;
    bool       all_aligned = true;

    for (int i = 0; i < 4000; i++) {
        u64   sz  = (u64)(rand() % 128) + 1;
        void *ptr = arena_push(a, sz);
        if (!ptr) break;

        if (((uintptr_t)ptr % ARENA_ALIGN) != 0 || (a->pos % ARENA_ALIGN) != 0) {
            all_aligned = false;
            printf("  misaligned at iteration %d (size %llu  ptr %p  pos %llu)\n",
                   i, (unsigned long long)sz, ptr, (unsigned long long)a->pos);
            break;
        }
        ok++;
    }

    printf("  completed %d random allocations before arena full\n", ok);
    ASSERT(ok > 0,         "at least one random allocation succeeds");
    ASSERT(all_aligned,    "all random-sized allocations return 8-byte-aligned pointers");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("══════════════════════════════════════════\n");
    printf("  Arena Allocator – Test Suite\n");
    printf("  sizeof(mem_arena) = %zu bytes (ARENA_OFFSET)\n", sizeof(mem_arena));
    printf("  page_size         = %llu bytes\n", (unsigned long long)get_page_size());
    printf("══════════════════════════════════════════\n");

    test_create_arena();
    test_push_single();
    test_push_multiple();
    test_push_alignment();
    test_push_capacity_boundary();
    test_push_commit_tracking();   /* BUG: one [FAIL] expected — see test body */
    test_arena_pop();
    test_lifo();
    test_arena_pop_to();
    test_arena_clear();
    test_write_read_safety();
    test_destroy_arena();
    test_stress();
    test_random_allocs();

    printf("\n══════════════════════════════════════════\n");
    printf("  Results: %d / %d passed\n", g_tests_passed, g_tests_run);
    if (g_tests_passed == g_tests_run) {
        printf("  All tests passed.\n");
    } else {
        printf("  %d test(s) FAILED – see [FAIL] lines above.\n",
               g_tests_run - g_tests_passed);
    }
    printf("══════════════════════════════════════════\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
