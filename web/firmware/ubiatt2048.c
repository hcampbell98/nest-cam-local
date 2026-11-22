/*
 * Tiny zero-dependency ARMv6 Linux EABI helper that calls:
 *   ioctl(open("/dev/ubi_ctrl", O_RDWR), UBI_IOCATT, { ubi_num=0, mtd_num=9, vid_hdr_offset=2048 })
 * Needed because BusyBox 1.23 ubiattach omits the -O vid_hdr_offset option required by
 * Gen 1 Nest Cam Outdoor mtd9 UBIFS (VID header offset 2048 on 512B-subpage NAND).
 */
#include <stdint.h>

#define SYS_exit  1
#define SYS_open  5
#define SYS_close 6
#define SYS_ioctl 54
#define O_RDWR    2
#define UBI_IOCATT 0x40186f40u /* _IOW('o', 64, struct ubi_attach_req [24 bytes]) */

struct ubi_attach_req {
    int32_t ubi_num;
    int32_t mtd_num;
    int32_t vid_hdr_offset;
    int8_t  padding[12];
};

static inline int32_t sys3(int32_t nr, int32_t a0, int32_t a1, int32_t a2) {
    register int32_t r7 __asm__("r7") = nr;
    register int32_t r0 __asm__("r0") = a0;
    register int32_t r1 __asm__("r1") = a1;
    register int32_t r2 __asm__("r2") = a2;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
    return r0;
}

void _start(void) {
    static const char dev[] = "/dev/ubi_ctrl";
    int32_t fd = sys3(SYS_open, (int32_t)dev, O_RDWR, 0);
    if (fd < 0) {
        sys3(SYS_exit, 2, 0, 0);
    }
    struct ubi_attach_req req = { 0, 9, 2048, { 0 } };
    int32_t rc = sys3(SYS_ioctl, fd, (int32_t)UBI_IOCATT, (int32_t)&req);
    sys3(SYS_close, fd, 0, 0);
    sys3(SYS_exit, rc < 0 ? 1 : 0, 0, 0);
}
