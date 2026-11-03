/* handoff-test: the bus hand-off pattern in gpio/ps_protocol.c, without
 * the bus. A "CPU" thread marks itself busy with a plain store, then
 * checks for a request with a plain load (no barrier: the emulator's hot
 * path); a "requester" raises the request, orders it, waits for the CPU
 * thread to be idle and enters. Both check the other is not inside.
 *
 *   ./handoff_test 0   no barrier on either side's store->load   (broken)
 *   ./handoff_test 2   requester waits 500 ns instead (the old design)
 *   ./handoff_test 1   requester calls membarrier (the fix)       (0 violations)
 *
 * Run each for 3 s; violations = both inside at once. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/membarrier.h>
#include <stdatomic.h>
#include <time.h>
static volatile unsigned seq, req; static volatile int cpu_in, oth_in;
static volatile int stop; static long viol, cpu_iters, oth_iters; static int mode;
static long mb(int c){return syscall(__NR_membarrier,c,0,0);}
#define CB() __asm__ volatile("":::"memory")
#define MB() __atomic_thread_fence(__ATOMIC_SEQ_CST)
static void *cpu(void*a){ unsigned s=0;
 while(!stop){ seq=++s; CB();
  while(req){ MB(); seq=++s; while(req); MB(); seq=++s; MB(); }
  cpu_in=1; CB(); if(oth_in) viol++; for(volatile int i=0;i<20;i++); cpu_in=0; CB();
  seq=++s; cpu_iters++; for(volatile int i=0;i<30;i++); }
 return 0;}
static void spin_ns(long n){struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);do clock_gettime(CLOCK_MONOTONIC,&b);while((b.tv_sec-a.tv_sec)*1000000000L+(b.tv_nsec-a.tv_nsec)<n);}
static void *oth(void*a){
 while(!stop){ req=1; MB();
  if(mode==1) mb(MEMBARRIER_CMD_PRIVATE_EXPEDITED); else spin_ns(mode==2?500:0);
  while(seq&1); MB();
  oth_in=1; MB(); if(cpu_in) viol++; for(volatile int i=0;i<50;i++); oth_in=0; MB();
  req=0; oth_iters++; for(volatile int i=0;i<200;i++); }
 return 0;}
int main(int c,char**v){
 if(c<2){fprintf(stderr,"usage: %s 0|1|2\n",v[0]);return 2;}
 mode=atoi(v[1]);
 if(mode==1 && mb(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED)){perror("reg");return 1;}
 pthread_t a,b; pthread_create(&a,0,cpu,0); pthread_create(&b,0,oth,0);
 sleep(3); stop=1; pthread_join(a,0); pthread_join(b,0);
 printf("mode=%s violations=%ld cpu=%ld oth=%ld\n", mode==1?"membarrier":mode==2?"timed 500ns":"no-barrier", viol, cpu_iters, oth_iters); }
