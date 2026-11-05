# Host test for platforms/atari/video/shifter_mirror.c - see test.c
ROOT = ../..
CFLAGS = -O2 -g -Wall -Wextra -DSHM_TEST -I$(ROOT)

shm-test: test.c $(ROOT)/platforms/atari/video/shifter_mirror.c $(ROOT)/platforms/atari/video/shifter_mirror.h
	$(CC) $(CFLAGS) -o $@ test.c $(ROOT)/platforms/atari/video/shifter_mirror.c -lm

# both Shifters: the ST checks, then the STE double-buffer case
check: shm-test
	./shm-test
	SHM_TEST_STE=1 ./shm-test

clean:
	rm -f shm-test
