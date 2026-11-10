#ifndef SCRLOG_GPU_TABLES_H
#define SCRLOG_GPU_TABLES_H
#include <wafel/types.h>

typedef struct {
    u16 addr;
    u32 clear_bits;
    u32 set_bits;
    u32 usec_delay;
} gpu_init_entry_t;

typedef struct {
    u8 addr;
    u8 reg_idx;
    u8 value;
    u32 usec_delay;
} ave_init_entry_t;

#define NUM_GPU_ENTRIES_C (0x65)
#define NUM_AVE_ENTRIES_C (0x25)

extern gpu_init_entry_t gpu_init_entries_C[];
extern ave_init_entry_t ave_init_entries_C[];
#endif
