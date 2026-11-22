ROOTFS ?= ../workspace/bak_extracted/86976456/rootfs
CC     := arm-linux-gnueabi-gcc
CFLAGS := -std=gnu99 -march=armv6 -mfloat-abi=soft -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -O2 -Wall -Wextra

all: nest_rtsp

nest_rtsp.o: nest_rtsp.c nest_audio.h nest_motion.h nv12_jpeg.h
	$(CC) $(CFLAGS) -c nest_rtsp.c -o nest_rtsp.o

nest_audio.o: nest_audio.c nest_audio.h
	$(CC) $(CFLAGS) -c nest_audio.c -o nest_audio.o

nest_motion.o: nest_motion.c nest_motion.h
	$(CC) $(CFLAGS) -c nest_motion.c -o nest_motion.o

nest_rtsp: nest_rtsp.o nest_audio.o nest_motion.o
	$(CC) -march=armv6 -mfloat-abi=soft -nostdlib \
	  -Wl,-dynamic-linker,/lib/ld-linux.so.3 \
	  /usr/arm-linux-gnueabi/lib/crt1.o /usr/arm-linux-gnueabi/lib/crti.o \
	  nest_rtsp.o nest_audio.o nest_motion.o \
	  -Wl,-rpath-link,$(ROOTFS)/lib:$(ROOTFS)/usr/lib \
	  $(ROOTFS)/usr/lib/libasound.so.2.0.0 \
	  $(ROOTFS)/lib/libc-2.13.so $(ROOTFS)/lib/ld-2.13.so -lgcc \
	  /usr/arm-linux-gnueabi/lib/crtn.o \
	  -o nest_rtsp
	arm-linux-gnueabi-strip nest_rtsp

clean:
	rm -f nest_rtsp nest_rtsp.o nest_audio.o nest_motion.o
