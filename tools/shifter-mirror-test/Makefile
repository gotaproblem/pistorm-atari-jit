# Host test for platforms/atari/video/shifter_mirror.c - see test.c
ROOT = ../..
CFLAGS = -O2 -g -Wall -Wextra -DSHM_TEST -I$(ROOT)

shm-test: test.c $(ROOT)/platforms/atari/video/shifter_mirror.c $(ROOT)/platforms/atari/video/shifter_mirror.h
	$(CC) $(CFLAGS) -o $@ test.c $(ROOT)/platforms/atari/video/shifter_mirror.c -lm

clean:
	rm -f shm-test
