/* Host-side unit test for platforms/atari/mfp_hub.c - see run.sh.
 * pst_dbg_mfp_hub and kbd_usb_note_iack are stubbed so the hub links alone. */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "platforms/atari/mfp_hub.h"
volatile int pst_dbg_mfp_hub = 1;
void kbd_usb_note_iack(void) {}
static int level; static uint32_t seq;
static int poll(void){ return level; }
static uint32_t sq(void){ return seq; }
static int fails;
#define CHECK(c,msg) do{ if(!(c)){printf("FAIL: %s\n",msg);fails++;} else printf("ok:   %s\n",msg);}while(0)
static void wb(uint32_t a,uint8_t v){ mfp_hub_write_snoop(a,v,0); }
int main(void){
  mfp_hub_register_edge(6, poll, sq);
  wb(0xFFFA17,0x48); wb(0xFFFA09,0x40); wb(0xFFFA15,0x40); wb(0xFFFA07,0); wb(0xFFFA13,0);
  CHECK(!mfp_hub_irq_wanted(),"idle: no irq");
  level=1; seq=100;                         /* byte 100 arrives */
  CHECK(mfp_hub_irq_wanted(),"byte arrives: irq");
  CHECK(mfp_hub_iack()==0x46,"IACK -> vector $46");
  CHECK(!mfp_hub_irq_wanted(),"in service: no irq");
  wb(0xFFFA11,0xBF);                        /* Risky Woods: EOI before reading */
  for (int i=0;i<1000;i++) if (mfp_hub_irq_wanted()) { CHECK(0,"EOI before read: re-delivered same byte"); break; }
  CHECK(!mfp_hub_irq_wanted(),"EOI before read: same byte NOT re-delivered");
  seq=101; level=0;                         /* guest reads; serial pacing gap */
  CHECK(!mfp_hub_irq_wanted(),"read, pacing gap: no irq");
  level=1;                                  /* byte 101 visible */
  CHECK(mfp_hub_irq_wanted(),"next byte: irq again");
  CHECK(mfp_hub_iack()==0x46,"IACK next byte");
  wb(0xFFFA11,0xBF);
  seq=102;                                  /* read, but next byte already queued (no gap) */
  CHECK(mfp_hub_irq_wanted(),"back-to-back byte (no gap): new seq latches");
  CHECK(mfp_hub_iack()==0x46,"IACK it");
  wb(0xFFFA11,0xBF);
  /* lost edge: pending cleared by the guest before IACK */
  seq=103;
  CHECK(mfp_hub_irq_wanted(),"byte 103 latched");
  wb(0xFFFA0D,0xBF);                        /* IPRB bit 6 cleared by guest */
  CHECK(!mfp_hub_irq_wanted(),"IPRB cleared: no irq");
  struct timespec ts={0,60000000}; nanosleep(&ts,0);
  CHECK(mfp_hub_irq_wanted(),"after 50 ms, same unread byte re-latched (safety net)");
  CHECK(mfp_hub_iack()==0x46,"IACK it"); wb(0xFFFA11,0xBF);
  nanosleep(&ts,0);
  CHECK(mfp_hub_irq_wanted(),"unread 50 ms after its IACK+EOI: re-latched (safety net)");
  mfp_hub_iack(); wb(0xFFFA11,0xBF);
  /* disabled channel */
  seq=104; wb(0xFFFA09,0x00);
  CHECK(!mfp_hub_irq_wanted(),"IER off: no irq");
  wb(0xFFFA09,0x40);
  CHECK(mfp_hub_irq_wanted(),"IER back on with byte waiting: irq");
  mfp_hub_iack();
  /* higher channel still preempts; ch6 in service blocks lower */
  wb(0xFFFA07,0x20); wb(0xFFFA13,0x20); mfp_hub_raise(13);
  CHECK(mfp_hub_top_wanted()==13,"Timer A (ch13) preempts ch6 in service");
  printf("%s (%d failures)\n", fails?"FAILED":"ALL PASS", fails);
  return fails!=0;
}
