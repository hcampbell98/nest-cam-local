CC      := arm-linux-gnueabi-gcc
STRIP   := arm-linux-gnueabi-strip
ROOTFS  ?= ../workspace/bak_extracted/86976456/rootfs
CRTDIR  := /usr/arm-linux-gnueabi/lib

CFLAGS  := -std=gnu99 -march=armv6 -mfloat-abi=soft \
           -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 \
           -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
           -Os -ffunction-sections -fdata-sections -Wall -Wextra

all: nest_onvif

nest_onvif.o: nest_onvif.c nest_events.h ../nest_rtsp/nest_audio.h
	$(CC) $(CFLAGS) -c nest_onvif.c -o nest_onvif.o

nest_events.o: nest_events.c nest_events.h ../nest_rtsp/nest_motion.h
	$(CC) $(CFLAGS) -c nest_events.c -o nest_events.o

nest_onvif: nest_onvif.o nest_events.o
	$(CC) -march=armv6 -mfloat-abi=soft -nostdlib -Wl,--gc-sections -Wl,--build-id=none \
	  -Wl,-dynamic-linker,/lib/ld-linux.so.3 \
	  $(CRTDIR)/crt1.o $(CRTDIR)/crti.o \
	  nest_onvif.o nest_events.o \
	  $(ROOTFS)/lib/libc-2.13.so $(ROOTFS)/lib/ld-2.13.so -lgcc \
	  $(CRTDIR)/crtn.o \
	  -o nest_onvif
	$(STRIP) --strip-all -R .comment -R .note.ABI-tag nest_onvif

clean:
	rm -f nest_onvif nest_onvif.o nest_events.o
