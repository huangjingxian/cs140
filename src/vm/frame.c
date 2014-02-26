#include "vm/frame.h"
#include "userprog/pagedir.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <debug.h>
#include <stdio.h>
#include "threads/pte.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/page.h"

/* Globals */
static struct frame* frame_table;
static size_t total_frames;
static void* first_frame;
struct lock frame_evict_lock;
static uint32_t clock_hand = 0;


/*
 --------------------------------------------------------------------
 IMPLIMENTATION NOTES:
 --------------------------------------------------------------------
 */
void init_frame_table(size_t num_frames, uint8_t* frame_base) {
    total_frames = num_frames;
    first_frame = frame_base;
    lock_init(&frame_evict_lock);
    frame_table = malloc(sizeof(struct frame) * num_frames);
    if(frame_table == NULL)
    {
        return false;
    }
    struct frame basic_frame;
    basic_frame.spte = NULL;
    basic_frame.physical_mem_frame_base = NULL;
    uint32_t i;
    for(i = 0; i < num_frames; i++)
    {
        memcpy((frame_table + i), &basic_frame, sizeof(struct frame));
        lock_init(&(frame_table[i].frame_lock));
        frame_table[i].physical_mem_frame_base = (void*)((uint32_t)first_frame + (i << 12));
    }
}

/*
 --------------------------------------------------------------------
 DESCRIPTION: simply advances the clock_hand one frame forward.
 NOTE: the frame_evict_lock must be held by the current thread.
 --------------------------------------------------------------------
 */
void advance_clock_hand() {
    ASSERT(lock_held_by_current_thread(&frame_evict_lock));
    clock_hand++;
    if (clock_hand >= total_frames) {
        clock_hand = 0;
    }
}

/*
 --------------------------------------------------------------------
 DESCRIPTION: evict frame. This is a private function of the frame 
    file. In this function, we check the table of frames, and when
    we find one suitable for eviction, we write the contents of the
    frame to associated memory location, and then return the frame.
 NOTE: when checking the access bits, we need to make sure that if
    multiple virtual addresses refer to same frame, that they all
    see the update. Curently do so by only checking the kernel address.
 --------------------------------------------------------------------
 */
static struct frame* evict_frame(void) {
    ASSERT(lock_held_by_current_thread(&frame_evict_lock));
    struct frame* frame;
    while (true) {
        frame = &(frame_table[clock_hand]);
        bool aquired = lock_try_acquire(&frame->frame_lock);
        if (aquired) {
            lock_release(&frame_evict_lock);
            uint32_t* pagedir = frame->spte->owner_thread->pagedir;
            bool accessed = pagedir_is_accessed(pagedir, frame->physical_memory_addr);
            bool pinned = frame->spte->is_pinned;
            if (accessed || pinned) {
                pagedir_set_accessed(pagedir, frame->physical_memory_addr, false);
                lock_release(&frame->frame_lock);
            } else {
                break;
            }
            lock_aquire(&frame_evict_lock);
        }
        advance_clock_hand();
    }
    evict_page_from_physical_memory(frame->spte, frame->physical_memory_addr);
    return frame;
}

/*
 --------------------------------------------------------------------
 DESCRIPTION: given a physical memory address that is returned
    by palloc, returns the index into the frame table for the 
    corresponding frame struct.
 --------------------------------------------------------------------
 */
static inline uint32_t get_frame_index(void* physical_memory_addr) {
    ASSERT (first_frame != NULL);
    ASSERT ((uint32_t)first_frame <= (uint32_t)kaddr);
    uint32_t index = ((uint32_t)kaddr - (uint32_t)first_frame) >> 12;
    ASSERT (index < total_frames);
    return index;
}

/*
 --------------------------------------------------------------------
 IMPLIMENTATION NOTES: 
 NOTE: Update so that this takes a boolean and releases the lock
    only if boolean is true
 --------------------------------------------------------------------
 */
bool frame_handler_palloc(bool zeros, struct spte* spte, bool should_pin) {
    lock_aquire(&frame_evict_lock);
    void* physical_memory_addr = palloc_get_page (PAL_USER | (zeros ? PAL_ZERO : 0));
    
    struct frame* frame;
    if (physical_memory_addr != NULL) {
        frame = frame_table + get_frame_index(physical_memory_addr);
        lock_aquire(&frame->frame_lock);
        lock_release(&frame_evict_lock);
        ASSERT(frame->resident_page == NULL)
    } else {
        frame = evict_frame();
    }
    
    if (zeros) memset(frame->physical_memory_addr, 0, PGSIZE);
    bool success = load_page_into_physical_memory(spte, physical_memory_addr);
    
    if (!success) {
        barrier();
        palloc_free_page(physical_memory_addr);
    } else {
        frame->resident_page = spte;
        spte->is_loaded = true;
    }
    if (should_pin == false) {
        lock_release(&frame->frame_lock);
    }
    return success;
}

/*
 --------------------------------------------------------------------
 IMPLIMENTATION NOTES:
 --------------------------------------------------------------------
 */
bool frame_handler_palloc_free(void* physical_memory_address, struct spte* spte) {
    struct frame* frame = frame_table + get_frame_index(physical_memory_address);
    lock_acquire(&frame->frame_lock);
    
}


