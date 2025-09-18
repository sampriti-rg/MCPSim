#ifndef __MCP_HOOKS_H__
#define __MCP_HOOKS_H__

#include <stdint.h>
#include <stdio.h>

//Avoid optimizing compilers moving code around this barrier
#define COMPILER_BARRIER() { __asm__ __volatile__("" ::: "memory");}

#define MAGIC_OP_ROI_BEGIN       (1030)
#define MAGIC_OP_REGION_BEGIN    (1031) 
#define MAGIC_OP_REGION_END      (1032)


static inline void magic_op(uint64_t op, int id) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op), "d"(id));
    COMPILER_BARRIER();
}

static inline void magic_op_1(uint64_t op) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%ecx, %%ecx;" : : "c"(op));
    COMPILER_BARRIER();
}

static inline void mcp_roi_begin() {
    printf("ROI Begin in Execution.\n");
    magic_op_1(MAGIC_OP_ROI_BEGIN);
}

static inline void roi_region_begin(int id) {
    magic_op(MAGIC_OP_REGION_BEGIN, id);
}

static inline void roi_region_end(int id) {
    magic_op(MAGIC_OP_REGION_END, id);
}

#endif /*__MCP_HOOKS_H__*/