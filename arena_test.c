#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <stdbool.h>

// ── pull in the implementation ────────────────────────────────────────────────
// Redefine main prevention isn't needed here; arena.c has no main().
#include "arena.c"

// ── minimal test framework ────────────────────────────────────────────────────
static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define ASSERT(cond, msg)                                                      \
    do {                                                                       \
        g_tests_run++;                                                         \
        if (cond) {                                                            \
            printf("  [PASS] %s\n", msg);                                      \
            g_tests_passed++;                                                  \
        } else {                                                               \
            printf("  [FAIL] %s  (line %d)\n", msg, __LINE__);                \
        }                                                                      \
    } while (0)

#define TEST(name) printf("\n=== %s ===\n", name)

// ── helper ────────────────────────────────────────────────────────────────────
// Capacity large enough for all tests: header (16 B) + 512 B of user data.
#define TEST_CAP (sizeof(mem_arena) + 512)

// ── test functions ────────────────────────────────────────────────────────────

// create_arena ----------------------------------------------------------------
static void test_create_arena(void) {
    TEST("create_arena");

    mem_arena *a = create_arena(TEST_CAP);
    ASSERT(a != NULL,
           "returns non-NULL for a valid capacity");
    ASSERT(a->total_capacity == TEST_CAP,
           "total_capacity matches the requested size");
    ASSERT(a->pos == sizeof(mem_arena),
           "pos starts at ARENA_OFFSET (sizeof(mem_arena) = 16)");

    destroy_arena(a);

    // Zero capacity: arena is valid but the first push should fail.
    mem_arena *z = create_arena(0);
    ASSERT(z != NULL,
           "create_arena(0) returns non-NULL");
    ASSERT(arena_push(z, 1) == NULL,
           "push into a zero-capacity arena returns NULL");
    destroy_arena(z);
}

// arena_push ------------------------------------------------------------------
static void test_arena_push(void) {
    TEST("arena_push");

    mem_arena *a = create_arena(TEST_CAP);

    // Basic push.
    void *p1 = arena_push(a, 8);
    ASSERT(p1 != NULL,
           "push 8 bytes returns non-NULL");

    // Pointer must sit inside the allocated block.
    u8 *base = (u8 *)a;
    ASSERT((u8 *)p1 >= base + sizeof(mem_arena) &&
           (u8 *)p1 <  base + TEST_CAP,
           "returned pointer is within arena bounds");

    // pos advances by the aligned size.
    u64 expected_pos = sizeof(mem_arena) + 8; // 8 is already aligned
    ASSERT(a->pos == expected_pos,
           "pos advances correctly after an 8-byte push");

    // Alignment: odd-sized push must be rounded up to 8.
    u64 pos_before = a->pos;
    void *p2 = arena_push(a, 3);
    ASSERT(p2 != NULL,
           "push 3 bytes (unaligned) returns non-NULL");
    ASSERT(a->pos == pos_before + 8,
           "3-byte push aligns up to 8 bytes");

    // Two consecutive pushes must not overlap.
    ASSERT((u8 *)p2 == (u8 *)p1 + 8,
           "consecutive pushes are contiguous (no gap, no overlap)");

    // Data written to a push region is readable.
    int *nums = (int *)arena_push(a, sizeof(int) * 2);
    ASSERT(nums != NULL, "push for int[2] succeeds");
    nums[0] = 42;
    nums[1] = 99;
    ASSERT(nums[0] == 42 && nums[1] == 99,
           "data written to pushed region is readable");

    // Over-capacity push returns NULL and does not change pos.
    u64 pos_before_fail = a->pos;
    void *fail = arena_push(a, TEST_CAP * 2);
    ASSERT(fail == NULL,
           "push larger than remaining capacity returns NULL");
    ASSERT(a->pos == pos_before_fail,
           "pos is unchanged after a failed push");

    destroy_arena(a);
}

// arena_pop -------------------------------------------------------------------
static void test_arena_pop(void) {
    TEST("arena_pop");

    mem_arena *a = create_arena(TEST_CAP);

    arena_push(a, 8);  // pos → ARENA_OFFSET + 8
    arena_push(a, 8);  // pos → ARENA_OFFSET + 16
    u64 pos_after_two = a->pos;

    // Pop one slot back.
    arena_pop(a, 8);
    ASSERT(a->pos == pos_after_two - 8,
           "pop 8 bytes decrements pos by 8");

    // Pop with a size that exactly matches remaining used space should succeed.
    // After two pushes and one pop: pos = ARENA_OFFSET + 8, used = 8.
    // ALIGN_POW2(3,8) = 8. 8 > 8 is FALSE, so the pop proceeds.
    arena_pop(a, 3);  // rounds up to 8, exactly consuming the remaining 8 bytes
    ASSERT(a->pos == sizeof(mem_arena),
           "pop of size equal to remaining used space resets pos to ARENA_OFFSET");

    // Pop on an empty arena must not crash.
    mem_arena *empty = create_arena(TEST_CAP);
    arena_pop(empty, 8);
    ASSERT(empty->pos == sizeof(mem_arena),
           "pop on empty arena leaves pos at ARENA_OFFSET (no underflow)");
    destroy_arena(empty);

    destroy_arena(a);
}

// arena_pop_to ----------------------------------------------------------------
static void test_arena_pop_to(void) {
    TEST("arena_pop_to");

    mem_arena *a = create_arena(TEST_CAP);

    // Save checkpoint, make allocations, restore.
    u64 checkpoint = a->pos;          // == ARENA_OFFSET
    arena_push(a, 8);
    arena_push(a, 16);
    ASSERT(a->pos > checkpoint,
           "pos advanced past checkpoint after two pushes");

    arena_pop_to(a, checkpoint);
    ASSERT(a->pos == checkpoint,
           "pop_to checkpoint (ARENA_OFFSET) resets pos all the way back to base");

    // Partial pop_to should work when size < used space.
    arena_push(a, 8);
    arena_push(a, 8);
    arena_push(a, 8);
    u64 mid = a->pos - 8;   // one slot before current end
    arena_pop_to(a, mid);
    ASSERT(a->pos == mid,
           "partial pop_to lands at the requested position");

    // pop_to with a pos in the future (beyond current pos) must be a no-op.
    u64 pos_before = a->pos;
    arena_pop_to(a, a->pos + 32);
    ASSERT(a->pos == pos_before,
           "pop_to a position beyond current pos is a no-op");

    destroy_arena(a);
}

// arena_clear -----------------------------------------------------------------
static void test_arena_clear(void) {
    TEST("arena_clear");

    mem_arena *a = create_arena(TEST_CAP);
    arena_push(a, 8);
    arena_push(a, 8);

    arena_clear(a);
    ASSERT(a->pos == sizeof(mem_arena),
           "arena_clear resets pos to ARENA_OFFSET");

    // Arena should be fully reusable after a clear.
    void *p = arena_push(a, 8);
    ASSERT(p != NULL,
           "arena is reusable after arena_clear");

    destroy_arena(a);
}

// destroy_arena ---------------------------------------------------------------
static void test_destroy_arena(void) {
    TEST("destroy_arena");

    mem_arena *a = create_arena(TEST_CAP);
    arena_push(a, 32);
    destroy_arena(a);   // must not crash
    ASSERT(true, "destroy_arena does not crash");

    // Destroying an empty arena must also be safe.
    mem_arena *empty = create_arena(TEST_CAP);
    destroy_arena(empty);
    ASSERT(true, "destroy_arena on an empty arena does not crash");
}

// ── entry point ───────────────────────────────────────────────────────────────
int main(void) {
    printf("Arena allocator tests\n");
    printf("=====================\n");

    test_create_arena();
    test_arena_push();
    test_arena_pop();
    test_arena_pop_to();
    test_arena_clear();
    test_destroy_arena();

    printf("\n=====================\n");
    printf("Results: %d / %d passed\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
