/*
 * smi_reset.c
 *
 * Forcibly resets the BCM2711 SMI peripheral and its clock.
 * Run this if ps_smi_init left the hardware in a stuck state.
 *
 * Build:  gcc -O2 -o smi_reset smi_reset.c
 * Run:    sudo ./smi_reset
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define PERIPH_BASE     0xfe000000UL
#define SMI_OFFSET      0x00600000UL
#define CM_OFFSET       0x00101000UL
#define MAP_SIZE        4096

/* Clock Manager */
#define CM_PASSWORD     0x5A000000UL
#define CM_SMICTL       (0xB0 / 4)
#define CM_SMIDIV       (0xB4 / 4)
#define CM_CTL_ENAB     (1u << 4)
#define CM_CTL_KILL     (1u << 5)
#define CM_CTL_BUSY     (1u << 7)
#define CM_CTL_SRC_OSC  1u          /* 19.2 MHz oscillator — safe fallback */

/* SMI direct-mode registers */
#define SMIDCS          (0x34 / 4)
#define SMIDA           (0x38 / 4)
#define SMIDD           (0x3C / 4)

/* SMI DMA-path CS (just zero it out too) */
#define SMICS           (0x00 / 4)

int main ( void )
{
  int fd = open ( "/dev/mem", O_RDWR | O_SYNC );
  if ( fd < 0 ) { perror ( "open /dev/mem" ); return 1; }

  volatile uint32_t *smi_regs = mmap ( NULL, MAP_SIZE,
      PROT_READ | PROT_WRITE, MAP_SHARED, fd,
      PERIPH_BASE + SMI_OFFSET );

  volatile uint32_t *cm_regs  = mmap ( NULL, MAP_SIZE,
      PROT_READ | PROT_WRITE, MAP_SHARED, fd,
      PERIPH_BASE + CM_OFFSET );

  close ( fd );

  if ( smi_regs == MAP_FAILED || cm_regs == MAP_FAILED )
  {
    perror ( "mmap" );
    return 1;
  }

  printf ( "CM_SMICTL before: 0x%08X\n", cm_regs[CM_SMICTL] );
  printf ( "SMIDCS    before: 0x%08X\n", smi_regs[SMIDCS] );

  /* ------------------------------------------------------------------
   * Kill the SMI clock.
   *
   * Correct BCM sequence:
   *   1. Write KILL bit (with password) — this overrides any stuck state
   *   2. Write source only (no ENAB, no KILL) — clock manager needs a
   *      source selected before BUSY will de-assert
   *   3. Poll BUSY — should clear within a few microseconds
   *
   * Do NOT poll BUSY immediately after writing KILL alone; the clock
   * manager requires a valid source to be set first.
   * ------------------------------------------------------------------ */
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_KILL;
  usleep ( 10 );
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_SRC_OSC;   /* source, no enable */

  int timeout = 10000;
  while ( (cm_regs[CM_SMICTL] & CM_CTL_BUSY) && --timeout > 0 )
    usleep ( 1 );

  if ( timeout <= 0 )
    printf ( "WARNING: CM_CTL_BUSY did not clear — hardware may need power cycle\n" );
  else
    printf ( "Clock stopped OK  (CM_SMICTL=0x%08X)\n", cm_regs[CM_SMICTL] );

  /* Zero the divisor */
  cm_regs[CM_SMIDIV] = CM_PASSWORD;

  /* Enable the SMI clock temporarily so the FIFO CLEAR bit actually works.
   * The CLEAR logic is clocked — writing CLEAR with the clock stopped has
   * no effect, which is why SMICS stayed at 0x54000000 across resets.
   * Use the oscillator (19.2 MHz) as a safe low-speed source. */
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_KILL;
  usleep ( 10 );
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_SRC_OSC;
  int t = 10000;
  while ( (cm_regs[CM_SMICTL] & CM_CTL_BUSY) && --t > 0 ) usleep(1);
  cm_regs[CM_SMIDIV] = CM_PASSWORD | (2u << 12);  /* /2 = 9.6 MHz */
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_SRC_OSC;
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_SRC_OSC | CM_CTL_ENAB;
  usleep ( 100 );

  printf ( "CM_SMICTL (clock running): 0x%08X\n", cm_regs[CM_SMICTL] );

  /* Now CLEAR will actually flush the FIFOs */
  smi_regs[SMICS] = (1u << 0);              /* ENABLE            */
  smi_regs[SMICS] = (1u << 0) | (1u << 4); /* ENABLE | CLEAR    */
  usleep ( 100 );
  printf ( "SMICS after CLEAR:  0x%08X  (bits 28,30 should be gone)\n", smi_regs[SMICS] );
  smi_regs[SMICS]  = 0;

  /* Reset direct-mode registers */
  smi_regs[SMIDCS] = 0;
  smi_regs[SMIDA]  = 0;
  smi_regs[SMIDD]  = 0;

  /* Stop the clock again — leave hardware clean and stopped */
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_KILL;
  usleep ( 10 );
  cm_regs[CM_SMICTL] = CM_PASSWORD | CM_CTL_SRC_OSC;
  t = 10000;
  while ( (cm_regs[CM_SMICTL] & CM_CTL_BUSY) && --t > 0 ) usleep(1);
  cm_regs[CM_SMIDIV] = CM_PASSWORD;
  cm_regs[CM_SMICTL] = CM_PASSWORD;

  printf ( "CM_SMICTL after:  0x%08X\n", cm_regs[CM_SMICTL] );
  printf ( "SMICS     after:  0x%08X  (expect 0x00000000, RXD/RXF cleared)\n", smi_regs[SMICS] );
  printf ( "SMIDCS    after:  0x%08X\n", smi_regs[SMIDCS] );
  printf ( "SMI reset complete.\n" );

  return 0;
}
