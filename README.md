# Arenas

## What is an Arena / Bump Allocator?

An arena (also called a bump allocator) is a memory management strategy where you allocate one large contiguous block of memory upfront, then hand out pieces of it by simply advancing a pointer — "bumping" it forward. It is a stack of memory. 

Instead of calling `malloc`/`free` for every individual object, you:
1. Allocate a big slab of memory once (`create_arena`)
2. Hand out sub-regions by bumping an offset forward (`arena_push`)
3. Free everything at once by resetting the offset to the start (`arena_clear` / `destroy_arena`)

This is different from general-purpose allocators because there is no per-object free. You trade the ability to free individual allocations for simplicity and speed.

## How The Implementation Works

The arena lives in a single contiguous allocation:

```
[ mem_arena header (16 bytes) | ... user data ... ]
^                              ^
arena pointer                  pos starts here (ARENA_OFFSET)
```

The `mem_arena` struct is stored at the very beginning of the allocation itself — no separate metadata pointer needed. `pos` tracks how far into the buffer we've bumped.

| Function | What it does |
|---|---|
| `create_arena(capacity)` | Calls `malloc` once for `sizeof(mem_arena) + capacity` bytes |
| `arena_push(arena, size)` | Aligns `size` to 8 bytes, bumps `pos` forward, returns pointer to old `pos` |
| `arena_pop(arena, size)` | Walks `pos` backward by `size` (aligned) — cheap "undo" of the last push |
| `arena_pop_to(arena, pos)` | Resets `pos` back to a previously saved position |
| `arena_clear(arena)` | Resets `pos` to `ARENA_OFFSET`, logically freeing all user data |
| `destroy_arena(arena)` | Calls `free` once on the whole slab |

All sizes are rounded up to the next multiple of 8 (`ALIGN_POW2`) so every allocation is naturally aligned for any primitive type on 64-bit platforms.

Reserve and Commit
With an implementation using mmap and unmmap, I have added more flexibility in the way memory is allocated, with virtual memory being used upfront, and only allocating what memory we need currently to the arena 

  - Reserve  →  claim virtual address space only (no RAM used)
  - Commit   →  back that virtual space with actual physical RAM/swap
  - Decommit →  give back the physical RAM, keep the virtual address
  - Release  →  give back everything (virtual + physical)

## Why Use an Arena?

- **Speed** — `arena_push` is essentially two additions and a comparison, no lock contention, no free-list traversal.
- **No fragmentation** — allocations are packed tightly; the bump pointer never leaves gaps.
- **Simple lifetime management** — if all the data you allocate belongs to the same logical lifetime (e.g., a single frame, a request, a parse tree), you free it all at once instead of tracking individual pointers.
- **Cache friendly** — related objects end up next to each other in memory.

## Removing `malloc` / `free`

 //Completed The current implementation bootstraps the arena with a single `malloc` call. The goal is to replace this with a lower-level memory source that doesn't rely on the C runtime heap at all. 

- **`mmap` — ask the OS directly for a page-aligned region. No heap overhead; the OS returns physical pages as they are touched. On Linux/macOS: `mmap(NULL, capacity, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`. Freeing becomes `munmap`.

## Test File 
- To test the code I had Claude Code create a arena_test file to ensure that the arena is properly pushing and popping data. 
- The prompt to generate the test file can be found in the prompt.txt file.

References:

https://www.youtube.com/watch?v=jgiMagdjA1s&t=543s 

https://www.dgtlgrove.com/p/untangling-lifetimes-the-arena-allocator 

https://www.bytesbeneath.com/p/the-arena-custom-memory-allocators 



