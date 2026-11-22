#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* Ambarella A5s Synopsys UDC20 registers */
#define EP0_IN_CTRL    REG32(0x60006000u)
#define EP0_IN_DESC    REG32(0x60006014u)
#define EP1_IN_CTRL    REG32(0x60006020u)
#define EP1_IN_STS     REG32(0x60006024u)
#define EP1_IN_BUFSZ   REG32(0x60006028u)
#define EP1_IN_MAXPKT  REG32(0x6000602cu)
#define EP1_IN_DESC    REG32(0x60006034u)

#define EP0_OUT_CTRL   REG32(0x60006200u)
#define EP0_OUT_SETUP  REG32(0x60006210u)
#define EP0_OUT_DESC   REG32(0x60006214u)
#define EP1_OUT_CTRL   REG32(0x60006220u)
#define EP1_OUT_STS    REG32(0x60006224u)
#define EP1_OUT_MAXPKT REG32(0x6000622cu)
#define EP1_OUT_DESC   REG32(0x60006234u)

#define UDC_DEV_CFG    REG32(0x60006400u)
#define UDC_DEV_CTRL   REG32(0x60006404u)
#define UDC_DEV_INTR   REG32(0x6000640cu)
#define UDC_DEV_IMSK   REG32(0x60006410u)
#define UDC_EP_INTR    REG32(0x60006414u)
#define UDC_EP_IMSK    REG32(0x60006418u)

/* Ambarella A5s UDC20 endpoint CSR slots (IN=0..4, OUT=id-11 -> 5..9) */
#define UDC_CSR_EP0_IN  REG32(0x60006504u)
#define UDC_CSR_EP1_IN  REG32(0x60006508u)
#define UDC_CSR_EP2_IN  REG32(0x6000650cu)
#define UDC_CSR_EP3_IN  REG32(0x60006510u)
#define UDC_CSR_EP4_IN  REG32(0x60006514u)
#define UDC_CSR_EP0_OUT REG32(0x60006518u)
#define UDC_CSR_EP1_OUT REG32(0x6000651cu)

/* Fixed uncached DDR scratch area (stub at 0xc0090000, stack at 0xc009fc00) */
#define DESC_OUT   ((volatile uint32_t *)0xc0098000u)
#define DESC_IN    ((volatile uint32_t *)0xc0098040u)
#define DESC_DUMMY ((volatile uint32_t *)0xc0098080u)
#define CMD_BUF    ((volatile uint32_t *)0xc0098100u)
#define RSP_BUF    ((volatile uint32_t *)0xc0098200u)

#define MAGIC_AMBA 0x414D4241u
#define MAGIC_OKAY 0x4F4B4159u
#define MAGIC_ERR  0x45525221u

#define CMD_PING       1u
#define CMD_WRITE_MEM  2u
#define CMD_WRITE_WORD 3u
#define CMD_EXEC       4u
#define CMD_READ_WORD  5u

static const uint32_t crc32_nib[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

static void delay_loops(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) {
        __asm__ volatile("" ::: "memory");
    }
}

static uint32_t ep1_recv_pkt(uint32_t dst_addr) {
    DESC_OUT[0] = 0x08000000u; /* BS=00 (HOST_READY), L=1 (LAST) */
    DESC_OUT[1] = 0xFFFFFFFFu;
    DESC_OUT[2] = dst_addr;
    DESC_OUT[3] = (uint32_t)DESC_OUT;

    EP1_OUT_DESC = (uint32_t)DESC_OUT;
    EP1_OUT_STS  = EP1_OUT_STS;
    UDC_DEV_CTRL |= 0x00000004u; /* RDE */
    delay_loops(16u);
    EP1_OUT_CTRL |= 0x00000100u; /* CNAK */

    uint32_t st;
    do {
        st = DESC_OUT[0];
    } while ((st & 0xC0000000u) != 0x80000000u);

    EP1_OUT_CTRL |= 0x00000080u; /* SNAK */
    EP1_OUT_STS  = EP1_OUT_STS;
    UDC_EP_INTR  = 0x00020000u;  /* clear EP1 OUT intr */
    return st & 0xFFFFu;
}

static void ep1_send_pkt(uint32_t src_addr, uint32_t len) {
    DESC_IN[0] = 0x08000000u | (len & 0xFFFFu);
    DESC_IN[1] = 0xFFFFFFFFu;
    DESC_IN[2] = src_addr;
    DESC_IN[3] = (uint32_t)DESC_IN;

    EP1_IN_DESC = (uint32_t)DESC_IN;
    EP1_IN_STS  = EP1_IN_STS;
    UDC_DEV_CTRL |= 0x00000008u; /* TDE */
    EP1_IN_CTRL |= 0x00000100u;  /* CNAK */

    /* Wait for host IN token (STS bit 6 = 0x40), clear it, then assert POLL_DEMAND */
    while ((EP1_IN_STS & 0x40u) == 0u) {
    }
    EP1_IN_STS = 0x40u;
    EP1_IN_CTRL |= 0x00000008u;  /* POLL_DEMAND */

    while ((DESC_IN[0] & 0xC0000000u) != 0x80000000u) {
    }
    for (volatile uint32_t w = 0; w < 100000u; w++) {
        if (EP1_IN_STS & 0x400u) break;
    }
    EP1_IN_CTRL |= 0x00000080u;  /* SNAK */
    EP1_IN_STS  = EP1_IN_STS;
    UDC_EP_INTR = 0x00000002u;   /* clear EP1 IN intr */
}

static uint32_t crc32_mem(uint32_t addr, uint32_t len) {
    const uint8_t *p = (const uint8_t *)addr;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t b = p[i];
        crc = (crc >> 4) ^ crc32_nib[(crc ^ b) & 0x0Fu];
        crc = (crc >> 4) ^ crc32_nib[(crc ^ (b >> 4)) & 0x0Fu];
    }
    return ~crc;
}

static void send_rsp(uint32_t magic, uint32_t r1, uint32_t r2, uint32_t r3) {
    RSP_BUF[0] = magic;
    RSP_BUF[1] = r1;
    RSP_BUF[2] = r2;
    RSP_BUF[3] = r3;
    ep1_send_pkt((uint32_t)RSP_BUF, 16u);
}

void loader_main(void) {
    /* Configure UDC20 EP1 IN/OUT while keeping existing USB connection alive (SD=0) */
    UDC_DEV_IMSK = 0x0000007Fu;
    UDC_EP_IMSK  = ~0x00020002u; /* unmask EP1 IN (bit 1) and EP1 OUT (bit 17); CPU IRQ is off */
    UDC_DEV_INTR = 0x0000007Fu;
    UDC_EP_INTR  = 0xFFFFFFFFu;

    /* Dummy DMA_DONE descriptors so no EP ever points at 0x00000000 */
    DESC_DUMMY[0] = 0x88000000u;
    DESC_DUMMY[1] = 0xFFFFFFFFu;
    DESC_DUMMY[2] = (uint32_t)CMD_BUF;
    DESC_DUMMY[3] = (uint32_t)DESC_DUMMY;
    EP0_IN_CTRL   = 0x00000080u;
    EP0_IN_DESC   = (uint32_t)DESC_DUMMY;
    EP0_OUT_CTRL  = 0x00000080u;
    EP0_OUT_SETUP = (uint32_t)DESC_DUMMY;
    EP0_OUT_DESC  = (uint32_t)DESC_DUMMY;

    /* Enable CSR programming + HS/SP/PI/DIR/STATUS/SET_DESC */
    UDC_DEV_CFG = 0x000a006cu;

    /* Program A5s UDC20 CSR slots: EP0 IN (0x504), EP1 IN (0x508), EP0 OUT (0x518), EP1 OUT (0x51c) */
    UDC_CSR_EP0_IN  = 0x02000090u;
    UDC_CSR_EP1_IN  = 0x100000d1u;
    UDC_CSR_EP2_IN  = 0x00000000u;
    UDC_CSR_EP3_IN  = 0x00000000u;
    UDC_CSR_EP4_IN  = 0x00000000u;
    UDC_CSR_EP0_OUT = 0x02000080u;
    UDC_CSR_EP1_OUT = 0x100000c1u;

    EP1_IN_CTRL   = 0x000000a0u; /* BULK + SNAK */
    EP1_IN_STS    = 0xFFFFFFFFu;
    EP1_IN_BUFSZ  = 0x00000080u; /* 128 words = 512 B */
    EP1_IN_MAXPKT = 0x00000200u; /* 512 bytes */
    EP1_IN_DESC   = (uint32_t)DESC_DUMMY;

    EP1_OUT_CTRL   = 0x000000a0u; /* BULK + SNAK */
    EP1_OUT_STS    = 0xFFFFFFFFu;
    EP1_OUT_MAXPKT = 0x00000200u; /* 512 bytes */
    EP1_OUT_DESC   = (uint32_t)DESC_DUMMY;

    /* DMA mode, BURST_LEN=7, BRST_EN=1, TDE=1, CSR_DONE=1, SD=0 */
    UDC_DEV_CTRL = 0x00072308u;

    for (;;) {
        uint32_t n = ep1_recv_pkt((uint32_t)CMD_BUF);
        if (n < 16u || CMD_BUF[0] != MAGIC_AMBA) {
            continue;
        }
        uint32_t cmd  = CMD_BUF[1];
        uint32_t arg1 = CMD_BUF[2];
        uint32_t arg2 = CMD_BUF[3];

        if (cmd == CMD_PING) {
            send_rsp(MAGIC_OKAY, 0xA55A0002u, UDC_DEV_CFG, UDC_DEV_CTRL);
        } else if (cmd == CMD_WRITE_MEM) {
            uint32_t dst = arg1;
            uint32_t rem = arg2;
            uint32_t total = 0;
            while (rem > 0u) {
                uint32_t got = ep1_recv_pkt(dst);
                if (got == 0u) continue;
                dst   += got;
                total += got;
                if (got >= rem) break;
                rem -= got;
            }
            uint32_t crc = crc32_mem(arg1, arg2);
            send_rsp(MAGIC_OKAY, crc, total, arg1);
        } else if (cmd == CMD_WRITE_WORD) {
            REG32(arg1) = arg2;
            send_rsp(MAGIC_OKAY, REG32(arg1), arg1, 0u);
        } else if (cmd == CMD_READ_WORD) {
            send_rsp(MAGIC_OKAY, REG32(arg1), arg1, 0u);
        } else if (cmd == CMD_EXEC) {
            send_rsp(MAGIC_OKAY, arg1, 0u, 0u);
            delay_loops(500000u);
            /* Soft-disconnect UDC cleanly before jumping to v312-bld.bin */
            UDC_DEV_CTRL = 0x00000400u;
            UDC_DEV_CFG  = 0x00000004u;
            delay_loops(100000u);
            /* Invalidate and disable I-cache before jumping to freshly loaded code */
            __asm__ volatile(
                "mov r0, #0\n"
                "mcr p15, 0, r0, c7, c5, 0\n"
                "mrc p15, 0, r0, c1, c0, 0\n"
                "bic r0, r0, #0x1000\n"
                "mcr p15, 0, r0, c1, c0, 0\n"
                ::: "r0", "memory"
            );
            ((void (*)(void))arg1)();
            for (;;) {}
        } else {
            send_rsp(MAGIC_ERR, cmd, arg1, arg2);
        }
    }
}

__attribute__((naked, section(".text.entry")))
void _start(void) {
    __asm__ volatile(
        "mrs r0, cpsr\n"
        "orr r0, r0, #0xc0\n"          /* Disable IRQ & FIQ */
        "msr cpsr_c, r0\n"
        "mrc p15, 0, r0, c1, c0, 0\n"
        "bic r0, r0, #0x05\n"          /* Disable MMU (bit 0) and D-cache (bit 2) */
        "orr r0, r0, #0x1000\n"        /* Enable I-cache (bit 12) */
        "mcr p15, 0, r0, c1, c0, 0\n"
        "mov r0, #0x60000000\n"
        "orr r0, r0, #0x14\n"          /* ARM11 Peripheral Port 0x60000000, 256MB */
        "mcr p15, 0, r0, c15, c2, 4\n"
        "ldr sp, =0xc009fc00\n"
        "bl loader_main\n"
        "1: b 1b\n"
    );
}
