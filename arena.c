#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <stdbool.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// the initial offset of the size of a mem_arena 16 bytes 
#define ARENA_OFFSET (sizeof(mem_arena))
// this gives us the size of a void pointer 8 bytes
#define ARENA_ALIGN (sizeof(void*))

// helper macro to align the size to what size we want ie 8 bytes 
#define ALIGN_POW2(size, align) (((size) + (align) - 1) & ~((align) - 1))

typedef struct{
    u64  total_capacity;
    u64  pos;
}mem_arena;

mem_arena* create_arena(u64 total_capacity);
void* arena_push(mem_arena* arena, u64 size);
void arena_pop(mem_arena* arena, u64 size);
void arena_pop_to(mem_arena* arena, u64 size);
void arena_clear(mem_arena* arena);
void destroy_arena(mem_arena* arena);

mem_arena* create_arena(u64 capacity){
    mem_arena *arena = (mem_arena*)malloc(sizeof(mem_arena) + capacity);
    if (arena == NULL) {return NULL;} 
    
    arena->total_capacity = capacity;
    arena->pos = ARENA_OFFSET; 

    return arena;
}

void* arena_push(mem_arena* arena, u64 size){
    // this gives us the new pos to align with for the push
    u64 pos_to_align = ALIGN_POW2(size, ARENA_ALIGN);
    // set the new pos to the pos to align + the current arena pos 
    u64 new_pos = pos_to_align + arena->pos;

    // ERROR check to see if the new pos exceeds the arena size 
    if(new_pos > arena->total_capacity){return NULL;}

    // this will give us the output pointer of the current pos 
    // this takes the arena size which is the pointer to the start of the arena directly and cast it to a u8
    // this ensures that we can perform the correct pointer arithmetic + the pos to align 
    u8* cur_pos = (u8*)arena + arena->pos;
    arena->pos = new_pos;
    
    return cur_pos;
}

void arena_pop(mem_arena* arena, u64 size){
    // check the to see if the current pos is lower then the base offset 
    if (arena->pos < ARENA_OFFSET){ return; }
    // set the pos to pop to the aligned size of the other data 
    u64 pos_to_pop = ALIGN_POW2(size, ARENA_ALIGN);
    // check to see if the pos to pop is greater than the size of the position 
    if (pos_to_pop > (arena->pos - ARENA_OFFSET)) { return; }
    // set the pos to the new pos 
    arena->pos -= pos_to_pop;
}

void arena_pop_to(mem_arena* arena, u64 pos){
    // set the cur pos to arena pos minus the pos we want to go to 
    // Check to see if pos we want to move to is less then the curr arena size if so set the new pos
    // set to 0 as there is nothing to pop
    u64 size = pos < arena->pos ? arena->pos - pos : 0;
    arena_pop(arena, size);
}

void arena_clear(mem_arena* arena){
    // clear all the data to the offset 
    arena_pop(arena, ARENA_OFFSET);
}

void destroy_arena(mem_arena* arena){
    free(arena);
}
