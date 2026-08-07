#include <stdint.h>

#define REG32(addr) (*((volatile uint32_t *)(addr)))

#define RPU_GLBL_CNTL  0xFF9A0000UL
#define RST_LPD_TOP    0xFF5E023CUL
#define TCM_AXI_BASE   0xFFE00000UL

extern char _binary_r5_firmware_bin_start[];
extern char _binary_r5_firmware_bin_end[];

static void wait_ms(uint64_t ms)
{
    for (uint64_t i = 0; i < ms * 100000; i++) {
        __asm__("nop");
    }
}

void main(void)
{
    uint32_t *src = (uint32_t *)_binary_r5_firmware_bin_start;
    uint32_t count = (uint32_t)(uint64_t)(_binary_r5_firmware_bin_end - _binary_r5_firmware_bin_start);
    count = (count + 3) / 4;

    REG32(RST_LPD_TOP) = 0x00188fd7;
    REG32(RPU_GLBL_CNTL) = 0x00000008;
    wait_ms(100);

    volatile uint32_t *tcm = (volatile uint32_t *)TCM_AXI_BASE;
    for (uint32_t i = 0; i < count; i++) {
        tcm[i] = src[i];
    }

    wait_ms(50);
    REG32(RST_LPD_TOP) = 0x00188fd6;

    for (;;) {
        __asm__("wfi");
    }
}
