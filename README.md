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

## Why Use an Arena?

- **Speed** — `arena_push` is essentially two additions and a comparison, no lock contention, no free-list traversal.
- **No fragmentation** — allocations are packed tightly; the bump pointer never leaves gaps.
- **Simple lifetime management** — if all the data you allocate belongs to the same logical lifetime (e.g., a single frame, a request, a parse tree), you free it all at once instead of tracking individual pointers.
- **Cache friendly** — related objects end up next to each other in memory.

## Planned: Removing `malloc` / `free`

The current implementation bootstraps the arena with a single `malloc` call. The goal is to replace this with a lower-level memory source that doesn't rely on the C runtime heap at all. Options under consideration:

- **`mmap` / `VirtualAlloc`** — ask the OS directly for a page-aligned region. No heap overhead, the OS gives back physical pages lazily as they are touched. On Linux/macOS: `mmap(NULL, capacity, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`. Freeing becomes `munmap`.
- **Static / stack buffer** — embed the backing buffer as a `static` array or a stack-local array and overlay the `mem_arena` header on top of it. Zero allocation cost; useful for small, fixed-size scratch arenas.
- **OS virtual memory with commit-on-demand** — reserve a large virtual address range upfront (cheap) and only commit physical pages as `pos` crosses into new pages, keeping RSS low until the memory is actually needed.

The public API (`arena_push`, `arena_pop`, `arena_clear`, etc.) stays the same regardless of the backing allocator — only `create_arena` and `destroy_arena` need to change.

References:

https://www.youtube.com/watch?v=jgiMagdjA1s&t=543s 

https://www.dgtlgrove.com/p/untangling-lifetimes-the-arena-allocator 

https://www.bytesbeneath.com/p/the-arena-custom-memory-allocators 



