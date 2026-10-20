/* Regression for MotorStorm's profile startup: real guest register order and
 * big-endian outputs, including a partially used container. */
#include "sys_memory.h"
#include <assert.h>
#include <stdlib.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
static uint32_t be32(unsigned p) {
    return ((uint32_t)vm_base[p]<<24) | ((uint32_t)vm_base[p+1]<<16) |
           ((uint32_t)vm_base[p+2]<<8) | vm_base[p+3];
}
int main(void) {
    vm_base = mmap(NULL, VM_TOTAL_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
    assert(mprotect(vm_base, 4096, PROT_READ | PROT_WRITE) == 0);
    lv2_syscall_table table = {0};
    sys_memory_init(&table);
    ppu_context ctx = {0};
    ctx.gpr[3]=0x100; ctx.gpr[4]=0x800000;
    assert(table.handlers[341](&ctx)==CELL_OK);
    unsigned id=be32(0x100);
    assert(id>0);
    g_sys_mem_containers[id-1].used_size=0x200000;
    ctx.gpr[3]=0x200; ctx.gpr[4]=id;
    assert(table.handlers[343](&ctx)==CELL_OK);
    assert(be32(0x200)==0x800000 && be32(0x204)==0x600000);
    ctx.gpr[3]=0x100000; ctx.gpr[4]=id;
    ctx.gpr[5]=SYS_MEMORY_PAGE_SIZE_1M; ctx.gpr[6]=0x300;
    assert(table.handlers[350](&ctx)==CELL_OK);
    unsigned allocated=be32(0x300);
    assert(allocated && !(allocated & 0xFFFFF));
    vm_base[allocated]=0x5A;
    assert(g_sys_mem_containers[id-1].used_size==0x300000);
    ctx.gpr[3]=0x200; ctx.gpr[4]=id;
    assert(table.handlers[343](&ctx)==CELL_OK);
    assert(be32(0x204)==0x500000);
    ctx.gpr[3]=0x600000; ctx.gpr[4]=id;
    ctx.gpr[5]=SYS_MEMORY_PAGE_SIZE_1M; ctx.gpr[6]=0x300;
    assert(table.handlers[350](&ctx)==(int64_t)(int32_t)CELL_ENOMEM);
    assert(be32(0x300)==allocated);
    ctx.gpr[3]=id;
    assert(table.handlers[342](&ctx)==(int64_t)(int32_t)CELL_EBUSY);
    ctx.gpr[3]=allocated;
    assert(table.handlers[349](&ctx)==CELL_OK);
    assert(g_sys_mem_containers[id-1].used_size==0x200000);
    ctx.gpr[3]=id;
    g_sys_mem_containers[id-1].used_size=0;
    assert(table.handlers[342](&ctx)==CELL_OK);
    ctx.gpr[3]=0x200; ctx.gpr[4]=id;
    assert(sys_memory_container_get_size(&ctx)==(int64_t)(int32_t)CELL_ESRCH);
    assert(be32(0x200)==0x800000 && be32(0x204)==0x500000);
    munmap(vm_base,VM_TOTAL_SIZE);
    puts("memory container ABI regression passed");
}
