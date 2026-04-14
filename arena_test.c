/*
 * arena_test.c  –  Comprehensive test suite for the custom memory arena.
 *
 * Compile & run:
 *   gcc arena_test.c -o arena_test && ./arena_test
 *
 * ── Layout reminder ──────────────────────────────────────────────────────────
 *   [mem_arena header  ARENA_OFFSET=16 bytes] [user data ...]
 *    ^                                         ^── first allocation lands here
 *    arena pointer (malloc'd base)
 *
 * ── Usable capacity ──────────────────────────────────────────────────────────
 *   create_arena(N) stores total_capacity = N.
 *   Usable bytes = N − ARENA_OFFSET.
 *   arena_push checks  new_pos > total_capacity  where new_pos accumulates
 *   from ARENA_OFFSET, so the guard fires when pos would reach N, not N+16.
 *
 * ── Known bug (arena_clear) ──────────────────────────────────────────────────
 *   arena_clear calls arena_pop(arena, ARENA_OFFSET) which only pops 16 bytes.
 *   It does NOT reset pos to ARENA_OFFSET when more than 16 bytes are live.
 *   Fix: replace the body with  arena_pop_to(arena, ARENA_OFFSET);
 *   test_arena_clear deliberately exposes this with a [FAIL] assertion so the
 *   bug is visible in the test output.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <stdbool.h>
#include <time.h>

#include "arena.c"

/* ── minimal test framework ──────────────────────────────────────────────────
 * ASSERT is non-aborting: records pass/fail and always continues, so the
 * full suite runs even when the arena_clear bug triggers a [FAIL].          */
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
 * TEST_CAP  : total_capacity passed to create_arena; gives 512 usable bytes.
 * SMALL_CAP : gives exactly 32 usable bytes (good for boundary tests).       */
#define TEST_CAP  (sizeof(mem_arena) + 512)   /* usable: 512 bytes */
#define SMALL_CAP (sizeof(mem_arena) + 32)    /* usable:  32 bytes */

/* ══════════════════════════════════════════════════════════════════════════════
 * 1. CREATE / DESTROY
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_create_arena(void)
{
    TEST("create_arena / destroy_arena");

    /* Normal creation. */
    mem_arena *a = create_arena(TEST_CAP);
    ASSERT(a != NULL,
           "create_arena returns non-NULL for a valid capacity");
    ASSERT(a->total_capacity == TEST_CAP,
           "total_capacity stored matches the requested value");
    ASSERT(a->pos == sizeof(mem_arena),
           "pos initialised to ARENA_OFFSET (sizeof(mem_arena) = 16)");
    destroy_arena(a);

    /* Zero capacity: arena header is still allocated; push must fail. */
    mem_arena *z = create_arena(0);
    ASSERT(z != NULL,
           "create_arena(0) returns non-NULL (header still allocated)");
    ASSERT(arena_push(z, 1) == NULL,
           "push into a zero-capacity arena returns NULL");
    destroy_arena(z);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 2. SINGLE PUSH
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_push_single(void)
{
    TEST("arena_push – single allocation");

    mem_arena *a = create_arena(TEST_CAP);

    void *ptr = arena_push(a, 8);
    ASSERT(ptr != NULL,
           "8-byte push returns non-NULL");
    /* First allocation must land exactly at base + ARENA_OFFSET. */
    ASSERT(ptr == (u8 *)a + ARENA_OFFSET,
           "first allocation sits at (base + ARENA_OFFSET)");
    ASSERT(a->pos == ARENA_OFFSET + 8,
           "pos advances by 8 after an 8-byte push");

    /* Written value must be readable. */
    *(u64 *)ptr = 0xCAFEBABEDEADBEEFULL;
    ASSERT(*(u64 *)ptr == 0xCAFEBABEDEADBEEFULL,
           "value written to allocation is readable back");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 3. MULTIPLE SEQUENTIAL PUSHES
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_push_multiple(void)
{
    TEST("arena_push – multiple sequential allocations");

    mem_arena *a = create_arena(TEST_CAP);

    void *p1 = arena_push(a, 8);   /* pos: 16 → 24 */
    void *p2 = arena_push(a, 8);   /* pos: 24 → 32 */
    void *p3 = arena_push(a, 16);  /* pos: 32 → 48 */

    ASSERT(p1 != NULL && p2 != NULL && p3 != NULL,
           "all three allocations return non-NULL");
    /* Blocks must be contiguous — no gap, no overlap. */
    ASSERT((u8 *)p2 == (u8 *)p1 + 8,
           "p2 starts immediately after p1 (no gap)");
    ASSERT((u8 *)p3 == (u8 *)p2 + 8,
           "p3 starts immediately after p2 (no gap)");

    /* Over-capacity push must return NULL without changing pos. */
    u64 pos_before_fail = a->pos;
    ASSERT(arena_push(a, TEST_CAP * 2) == NULL,
           "push larger than remaining capacity returns NULL");
    ASSERT(a->pos == pos_before_fail,
           "pos is unchanged after a failed push");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 4. POINTER ALIGNMENT
 * Every returned pointer must be aligned to ARENA_ALIGN (= sizeof(void*) = 8)
 * regardless of the requested size.  The allocator achieves this by rounding
 * the requested size up via ALIGN_POW2 before advancing pos.
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_alignment(void)
{
    TEST("alignment – non-multiples-of-8 sizes must yield aligned pointers");

    mem_arena *a = create_arena(512);

    const u64 sizes[]  = { 1, 2, 3, 5, 7, 9, 11, 13, 15 };
    const int  n_sizes = (int)(sizeof sizes / sizeof sizes[0]);

    for (int i = 0; i < n_sizes; i++) {
        u64 pos_before = a->pos;
        void *ptr = arena_push(a, sizes[i]);
        ASSERT(ptr != NULL, "allocation succeeds");

        bool ptr_aligned = ((uintptr_t)ptr % ARENA_ALIGN) == 0;
        bool pos_aligned = (a->pos % ARENA_ALIGN) == 0;
        /* pos must advance by at least sizes[i] and be a multiple of 8. */
        bool pos_advanced = (a->pos >= pos_before + sizes[i]) &&
                            (a->pos == pos_before + ALIGN_POW2(sizes[i], ARENA_ALIGN));

        printf("  size %-3llu  ptr %-18p  ptr_aligned: %-3s  pos_advanced_correctly: %s\n",
               (unsigned long long)sizes[i], ptr,
               ptr_aligned ? "YES" : "NO",
               pos_advanced ? "YES" : "NO");

        ASSERT(ptr_aligned, "returned pointer is aligned to sizeof(void*)");
        ASSERT(pos_aligned, "pos remains a multiple of ARENA_ALIGN after push");
        ASSERT(pos_advanced, "pos advances by ALIGN_POW2(requested_size, 8)");
    }

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 5. CAPACITY – EXACT BOUNDARY
 * Pushing exactly (total_capacity − ARENA_OFFSET) bytes must succeed.
 * One more byte must return NULL.
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_capacity_boundary(void)
{
    TEST("capacity – exact boundary and over-capacity");

    /* SMALL_CAP = sizeof(mem_arena)+32 → usable = 32 bytes. */
    mem_arena *a = create_arena(SMALL_CAP);
    u64 usable = a->total_capacity - ARENA_OFFSET;

    void *ptr = arena_push(a, usable);
    ASSERT(ptr != NULL,
           "push of exactly usable capacity succeeds");
    ASSERT(a->pos == a->total_capacity,
           "pos equals total_capacity after filling arena exactly");

    /* Next push (even 1 byte) must fail. */
    ASSERT(arena_push(a, 1) == NULL,
           "push of 1 byte beyond capacity returns NULL");
    ASSERT(a->pos == a->total_capacity,
           "pos unchanged after failed over-capacity push");

    destroy_arena(a);

    /* Also verify an initially over-sized push fails cleanly. */
    mem_arena *b = create_arena(SMALL_CAP);
    u64 pos_clean = b->pos;
    ASSERT(arena_push(b, 1000) == NULL,
           "vastly oversized push returns NULL");
    ASSERT(b->pos == pos_clean,
           "pos unchanged after vastly oversized push");
    destroy_arena(b);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 6. ARENA_POP – BASIC
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_arena_pop(void)
{
    TEST("arena_pop");

    mem_arena *a = create_arena(TEST_CAP);

    arena_push(a, 8);   /* pos = ARENA_OFFSET + 8  */
    arena_push(a, 8);   /* pos = ARENA_OFFSET + 16 */
    u64 pos_after_two = a->pos;

    /* Pop one slot back. */
    arena_pop(a, 8);
    ASSERT(a->pos == pos_after_two - 8,
           "pop(8) decrements pos by 8");

    /* ALIGN_POW2(3, 8) = 8; remaining used above base = 8; 8 > 8 is FALSE,
     * so the pop executes and fully empties the arena.                      */
    arena_pop(a, 3);
    ASSERT(a->pos == sizeof(mem_arena),
           "pop(3) rounds to 8, consuming last 8 bytes; pos returns to ARENA_OFFSET");

    /* Pop on an empty arena must be a safe no-op. */
    arena_pop(a, 8);
    ASSERT(a->pos == sizeof(mem_arena),
           "pop on empty arena leaves pos at ARENA_OFFSET (no underflow)");

    /* Popping more than currently used must be a safe no-op. */
    arena_push(a, 8);
    u64 before_over = a->pos;
    arena_pop(a, 200);
    ASSERT(a->pos == before_over,
           "pop of size > current used bytes is a safe no-op");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 7. ARENA_POP – LIFO SEQUENCE
 * Push three blocks of different sizes, then pop them in reverse order.
 * Each pop must restore the exact position saved after the matching push.
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_lifo(void)
{
    TEST("LIFO – push three blocks, pop in reverse order");

    mem_arena *a = create_arena(TEST_CAP);
    u64 p0 = a->pos;   /* 16 */

    arena_push(a,  8);  u64 p1 = a->pos;  /* 24 */
    arena_push(a, 16);  u64 p2 = a->pos;  /* 40 */
    arena_push(a, 24);  u64 p3 = a->pos;  /* 64 */

    ASSERT(p1 == ARENA_OFFSET +  8, "after push(8)  pos == ARENA_OFFSET + 8");
    ASSERT(p2 == ARENA_OFFSET + 24, "after push(16) pos == ARENA_OFFSET + 24");
    ASSERT(p3 == ARENA_OFFSET + 48, "after push(24) pos == ARENA_OFFSET + 48");

    arena_pop(a, 24); ASSERT(a->pos == p2, "pop(24) restores pos to p2");
    arena_pop(a, 16); ASSERT(a->pos == p1, "pop(16) restores pos to p1");
    arena_pop(a,  8); ASSERT(a->pos == p0, "pop(8)  restores pos to p0 (base)");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 8. ARENA_POP_TO
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_arena_pop_to(void)
{
    TEST("arena_pop_to");

    mem_arena *a = create_arena(TEST_CAP);

    /* Checkpoint at base, advance, then restore all the way back. */
    u64 base_checkpoint = a->pos;   /* ARENA_OFFSET */
    arena_push(a, 8);
    arena_push(a, 16);
    ASSERT(a->pos > base_checkpoint,
           "pos advanced past base checkpoint after two pushes");
    arena_pop_to(a, base_checkpoint);
    ASSERT(a->pos == base_checkpoint,
           "pop_to base checkpoint resets pos all the way to ARENA_OFFSET");

    /* Mid-sequence checkpoint: save after two pushes, push one more, restore. */
    arena_push(a, 8);
    arena_push(a, 8);
    u64 mid = a->pos;   /* ARENA_OFFSET + 16 */
    arena_push(a, 8);   /* advance one more slot */
    arena_pop_to(a, mid);
    ASSERT(a->pos == mid,
           "pop_to mid-checkpoint lands at the saved position");

    /* pop_to a position beyond current pos must be a no-op. */
    u64 pos_before_future = a->pos;
    arena_pop_to(a, a->pos + 32);
    ASSERT(a->pos == pos_before_future,
           "pop_to a future position is a no-op");

    /* pop_to 0 (below ARENA_OFFSET): the arena_pop safety guard prevents
     * underflowing the header.  pos must stay >= ARENA_OFFSET.            */
    arena_push(a, 8);
    arena_pop_to(a, 0);
    ASSERT(a->pos >= ARENA_OFFSET,
           "pop_to 0 must never underflow below ARENA_OFFSET");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 9. ARENA_CLEAR
 *
 * !! BUG IN CURRENT IMPLEMENTATION !!
 *
 * arena_clear calls:
 *   arena_pop(arena, ARENA_OFFSET);     ← pops only 16 bytes
 *
 * Correct implementation should be:
 *   arena_pop_to(arena, ARENA_OFFSET);  ← resets to empty
 *
 * Case A passes with the current code because exactly ARENA_OFFSET bytes
 * are pushed (8+8=16), so pop(16) coincidentally empties the arena.
 *
 * Case B deliberately asserts the CORRECT expected behaviour.  It will
 * print [FAIL] with the current implementation – that is intentional and
 * is meant to signal the bug to the developer.
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_arena_clear(void)
{
    TEST("arena_clear (see BUG note in source and output below)");

    /* Case A – push exactly ARENA_OFFSET (16) bytes then clear.
     * arena_pop(arena,16): pos_to_pop=16, guard 16>(32-16=16) is FALSE → pops.
     * pos goes from 32 → 16 = ARENA_OFFSET.  Passes by coincidence.        */
    {
        mem_arena *a = create_arena(TEST_CAP);
        arena_push(a, 8);   /* pos = 24 */
        arena_push(a, 8);   /* pos = 32; used above base = 16 == ARENA_OFFSET */
        arena_clear(a);
        ASSERT(a->pos == ARENA_OFFSET,
               "[A] clear after exactly ARENA_OFFSET bytes pushed resets pos");
        ASSERT(arena_push(a, 8) != NULL,
               "[A] arena is reusable after clear");
        destroy_arena(a);
    }

    /* Case B – push more than ARENA_OFFSET bytes then clear.
     * BUG: arena_pop(arena,16) only pops 16 bytes; pos is NOT reset.
     * The ASSERT below documents the CORRECT expectation and will FAIL
     * until arena_clear is fixed.                                           */
    {
        mem_arena *a = create_arena(TEST_CAP);
        arena_push(a, 64);   /* pos = 80  */
        arena_push(a, 32);   /* pos = 112 */
        printf("  [INFO] pos before clear : %llu\n", (unsigned long long)a->pos);
        arena_clear(a);
        printf("  [INFO] pos after  clear : %llu  (correct expected: %zu)\n",
               (unsigned long long)a->pos, ARENA_OFFSET);
        if (a->pos != ARENA_OFFSET)
            printf("  [BUG ] arena_clear only popped %llu bytes – arena NOT reset.\n"
                   "         Fix: replace arena_pop(arena, ARENA_OFFSET) with\n"
                   "              arena_pop_to(arena, ARENA_OFFSET);\n",
                   (unsigned long long)(ARENA_OFFSET));
        ASSERT(a->pos == ARENA_OFFSET,
               "[B] BUG: clear after >ARENA_OFFSET bytes MUST reset pos to ARENA_OFFSET");
        destroy_arena(a);
    }
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 10. WRITE / READ SAFETY
 * Write sentinel values into adjacent allocations; verify that no value is
 * corrupted by writes to neighbouring slots.
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_write_read_safety(void)
{
    TEST("write/read safety – no overlap or cross-slot corruption");

    mem_arena *a = create_arena(4096);

    u64 *pa = (u64 *)arena_push(a, sizeof(u64));
    u64 *pb = (u64 *)arena_push(a, sizeof(u64));
    u32 *pc = (u32 *)arena_push(a, sizeof(u32));

    ASSERT(pa && pb && pc, "all three allocations succeed");
    /* Guard: bail before a NULL dereference if alloc fails. */
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

    /* Write a byte array and verify each byte. */
    u8 *buf = (u8 *)arena_push(a, 16);
    ASSERT(buf != NULL, "16-byte buffer allocation succeeds");
    if (buf) {
        for (int i = 0; i < 16; i++) buf[i] = (u8)i;
        bool ok = true;
        for (int i = 0; i < 16; i++) ok &= (buf[i] == (u8)i);
        ASSERT(ok, "all 16 bytes of buffer read back correctly");
    }

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 11. DESTROY
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_destroy_arena(void)
{
    TEST("destroy_arena");

    /* Destroying a used arena must not crash. */
    mem_arena *a = create_arena(TEST_CAP);
    arena_push(a, 32);
    destroy_arena(a);
    ASSERT(true, "destroy_arena on a used arena does not crash");

    /* Destroying an empty arena must not crash. */
    mem_arena *empty = create_arena(TEST_CAP);
    destroy_arena(empty);
    ASSERT(true, "destroy_arena on an empty arena does not crash");
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 12. STRESS TEST
 * Fill a 64 KiB arena with back-to-back 8-byte chunks.  The allocation count
 * must exactly equal usable / chunk.  Drain via pop_to and confirm reuse.
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
           "arena filled with exactly (usable / chunk_size) allocations");

    /* Drain via pop_to base and confirm arena is fully reusable. */
    arena_pop_to(a, ARENA_OFFSET);
    ASSERT(a->pos == ARENA_OFFSET,
           "pop_to ARENA_OFFSET after full fill resets arena to empty");
    ASSERT(arena_push(a, STRESS_CHUNK) != NULL,
           "arena accepts allocations again after full drain");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 13. RANDOMISED ALLOCATION SIZES
 * Push random 1–64-byte chunks until the arena is full.  Verify that every
 * returned pointer is aligned and that pos is always a multiple of ARENA_ALIGN.
 * ══════════════════════════════════════════════════════════════════════════════ */
static void test_random_allocs(void)
{
    TEST("randomised allocation sizes (1–64 bytes)");

    srand((unsigned)time(NULL));

    mem_arena *a           = create_arena(8192);
    int        ok          = 0;
    bool       all_aligned = true;

    for (int i = 0; i < 2000; i++) {
        u64   sz  = (u64)(rand() % 64) + 1;   /* 1 – 64 bytes */
        void *ptr = arena_push(a, sz);
        if (!ptr) break;                        /* arena full, stop */

        if (((uintptr_t)ptr % ARENA_ALIGN) != 0 || (a->pos % ARENA_ALIGN) != 0) {
            all_aligned = false;
            printf("  misaligned at iteration %d (size %llu ptr %p pos %llu)\n",
                   i, (unsigned long long)sz, ptr, (unsigned long long)a->pos);
            break;
        }
        ok++;
    }

    printf("  completed %d random allocations before arena full\n", ok);
    ASSERT(ok > 0,
           "at least one random allocation succeeds");
    ASSERT(all_aligned,
           "all random-sized allocations return 8-byte-aligned pointers");

    destroy_arena(a);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("══════════════════════════════════════════\n");
    printf("  Arena Allocator – Test Suite\n");
    printf("══════════════════════════════════════════\n");

    test_create_arena();
    test_push_single();
    test_push_multiple();
    test_alignment();
    test_capacity_boundary();
    test_arena_pop();
    test_lifo();
    test_arena_pop_to();
    test_arena_clear();         /* NOTE: one [FAIL] expected (arena_clear bug) */
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
