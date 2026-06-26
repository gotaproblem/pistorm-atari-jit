/*
 * smi_diag.c
 *
 * Dumps BCM2711 SMI and Clock Manager register state.
 * Run this to see exactly what the hardware is doing.
 *
 * Build:  gcc -O2 -o smi_diag smi_diag.c
 * Run:    sudo ./smi_diag
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
#define GPIO_OFFSET     0x00200000UL
#define MAP_SIZE        4096

/* Clock Manager */
#define CM_PASSWORD     0x5A000000UL
#define CM_SMICTL       (0xB0 / 4)
#define CM_SMIDIV       (0xB4 / 4)
#define CM_CTL_ENAB     (1u << 4)
#define CM_CTL_KILL     (1u << 5)
#define CM_CTL_BUSY     (1u << 7)

/* SMI DMA-path registers */
#define SMICS           (0x00 / 4)
#define SMIL            (0x04 / 4)
#define SMIA            (0x08 / 4)
#define SMID            (0x0C / 4)
#define SMIDSR0         (0x10 / 4)
#define SMIDSW0         (0x14 / 4)
#define SMIDSR1         (0x18 / 4)
#define SMIDSW1         (0x1C / 4)
#define SMIDSR2         (0x20 / 4)
#define SMIDSW2         (0x24 / 4)
#define SMIDSR3         (0x28 / 4)
#define SMIDSW3         (0x2C / 4)
#define SMIDC           (0x30 / 4)

/* SMI direct-mode registers */
#define SMIDCS          (0x34 / 4)
#define SMIDA           (0x38 / 4)
#define SMIDD           (0x3C / 4)
#define SMIFD           (0x40 / 4)

/* SMICS bits — from bcm2835_smi.h kernel header */
#define SMICS_ENABLE    (1u << 0)
#define SMICS_DONE      (1u << 1)
#define SMICS_ACTIVE    (1u << 2)
#define SMICS_START     (1u << 3)
#define SMICS_CLEAR     (1u << 4)
#define SMICS_WRITE     (1u << 5)
#define SMICS_TEEN      (1u << 8)
#define SMICS_INTD      (1u << 9)
#define SMICS_INTT      (1u << 10)
#define SMICS_INTR      (1u << 11)
#define SMICS_PVMODE    (1u << 12)
#define SMICS_SETERR    (1u << 13)
#define SMICS_PXLDAT   (1u << 14)
#define SMICS_EDREQ    (1u << 15)
#define SMICS_AFERR    (1u << 25)
#define SMICS_TXW      (1u << 26)
#define SMICS_RXR      (1u << 27)
#define SMICS_TXD      (1u << 28)
#define SMICS_RXD      (1u << 29)
#define SMICS_TXE      (1u << 30)
#define SMICS_RXF      (1u << 31)

/* SMIDCS bits */
#define SMIDCS_ENABLE   (1u << 0)
#define SMIDCS_DONE     (1u << 1)
#define SMIDCS_START    (1u << 3)
#define SMIDCS_WRITE    (1u << 5)

/* GPIO */
#define GPLEV0          (0x34 / 4)

static void print_smics ( uint32_t v )
{
  printf ( "  SMICS   = 0x%08X\n", v );
  printf ( "    enable=%u done=%u active=%u start=%u clear=%u write=%u\n",
    !!(v & SMICS_ENABLE), !!(v & SMICS_DONE), !!(v & SMICS_ACTIVE),
    !!(v & SMICS_START),  !!(v & SMICS_CLEAR), !!(v & SMICS_WRITE) );
  printf ( "    teen=%u intd=%u intt=%u intr=%u pvmode=%u seterr=%u pxldat=%u edreq=%u\n",
    !!(v & SMICS_TEEN), !!(v & SMICS_INTD), !!(v & SMICS_INTT), !!(v & SMICS_INTR),
    !!(v & SMICS_PVMODE), !!(v & SMICS_SETERR), !!(v & SMICS_PXLDAT), !!(v & SMICS_EDREQ) );
  printf ( "    aferr=%u txw=%u rxr=%u txd=%u rxd=%u txe=%u rxf=%u\n",
    !!(v & SMICS_AFERR), !!(v & SMICS_TXW), !!(v & SMICS_RXR),
    !!(v & SMICS_TXD), !!(v & SMICS_RXD), !!(v & SMICS_TXE), !!(v & SMICS_RXF) );
}

static void print_smidcs ( uint32_t v )
{
  printf ( "  SMIDCS  = 0x%08X\n", v );
  printf ( "    enable=%u done=%u start=%u write=%u\n",
    !!(v & SMIDCS_ENABLE), !!(v & SMIDCS_DONE),
    !!(v & SMIDCS_START),  !!(v & SMIDCS_WRITE) );
}

static void print_timing ( const char *name, uint32_t v )
{
  uint32_t width  = (v >> 30) & 0x3;
  uint32_t setup  = (v >> 24) & 0x3F;
  uint32_t strobe = (v >> 16) & 0xFF;
  uint32_t hold   = (v >>  8) & 0x3F;
  uint32_t pace   = (v      ) & 0xFF;
  const char *widths[] = { "8-bit", "16-bit", "18-bit", "9-bit" };
  printf ( "  %-8s= 0x%08X  width=%s setup=%u strobe=%u hold=%u pace=%u\n",
           name, v, widths[width], setup, strobe, hold, pace );
}

static void print_cm ( uint32_t ctl, uint32_t div )
{
  static const char *srcs[] = {
    "GND","OSC","testdebug0","testdebug1",
    "PLLA","PLLC","PLLD","HDMI aux"
  };
  uint32_t src   =  ctl & 0xF;
  uint32_t enab  = !!(ctl & CM_CTL_ENAB);
  uint32_t kill  = !!(ctl & CM_CTL_KILL);
  uint32_t busy  = !!(ctl & CM_CTL_BUSY);
  uint32_t divi  =  (div >> 12) & 0xFFF;
  uint32_t divf  =  div & 0xFFF;
  printf ( "  CM_SMICTL = 0x%08X  src=%s enab=%u kill=%u busy=%u\n",
           ctl, (src < 8 ? srcs[src] : "?"), enab, kill, busy );
  printf ( "  CM_SMIDIV = 0x%08X  DIVI=%u DIVF=%u\n", div, divi, divf );
  if ( enab && divi > 0 )
  {
    /* PLLD = 500 MHz, OSC = 19.2 MHz */
    uint32_t base = (src == 6) ? 500 : (src == 1) ? 19 : 0;
    if ( base )
      printf ( "  -> SMI clock ~%u MHz\n", base / divi );
  }
}

int main ( void )
{
  int fd = open ( "/dev/mem", O_RDWR | O_SYNC );
  if ( fd < 0 ) { perror ( "open /dev/mem" ); return 1; }

  volatile uint32_t *smi  = mmap ( NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, PERIPH_BASE + SMI_OFFSET );
  volatile uint32_t *cm   = mmap ( NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, PERIPH_BASE + CM_OFFSET );
  volatile uint32_t *gpio = mmap ( NULL, MAP_SIZE, PROT_READ,
                                    MAP_SHARED, fd, PERIPH_BASE + GPIO_OFFSET );
  close ( fd );

  if ( smi == MAP_FAILED || cm == MAP_FAILED || gpio == MAP_FAILED )
  {
    perror ( "mmap" ); return 1;
  }

  printf ( "\n=== Clock Manager ===\n" );
  print_cm ( cm[CM_SMICTL], cm[CM_SMIDIV] );

  printf ( "\n=== SMI DMA-path registers ===\n" );
  print_smics ( smi[SMICS] );
  printf ( "  SMIL    = 0x%08X  (transfer length)\n", smi[SMIL] );
  printf ( "  SMIA    = 0x%08X  (address)\n",          smi[SMIA] );
  printf ( "  SMID    = 0x%08X  (data FIFO)\n",        smi[SMID] );
  print_timing ( "SMIDSR0", smi[SMIDSR0] );
  print_timing ( "SMIDSW0", smi[SMIDSW0] );

  printf ( "\n=== SMI Direct-mode registers ===\n" );
  print_smidcs ( smi[SMIDCS] );
  printf ( "  SMIDA   = 0x%08X  (direct address)\n", smi[SMIDA] );
  printf ( "  SMIDD   = 0x%08X  (direct data)\n",    smi[SMIDD] );

  printf ( "\n=== GPIO pin 0-7 (GPLEV0 low byte) ===\n" );
  uint32_t lev = gpio[GPLEV0];
  printf ( "  GPLEV0  = 0x%08X\n", lev );
  printf ( "  PI_TXN_IN_PROGRESS (bit 0) = %u\n", lev & 1 );
  printf ( "  BERR               (bit 7) = %u\n", !!(lev & 0x80) );
  printf ( "  IPL                (bits 5,6) = %u\n", (lev >> 4) & 0x7 );

  printf ( "\n=== Diagnosis ===\n" );
  uint32_t cs  = smi[SMICS];
  uint32_t dcs = smi[SMIDCS];
  uint32_t ctl = cm[CM_SMICTL];

  /* SMICS = 0x54000000 (TXE|TXD|TXW set) is the NORMAL idle state —
   * TX FIFO empty and ready. This is NOT an error. */
  uint32_t smics_idle = SMICS_TXE | SMICS_TXD | SMICS_TXW;
  if ( (cs & ~smics_idle) == 0 )
    printf ( "  SMICS: idle/normal (TXE+TXD+TXW = TX FIFO empty, expected)\n" );
  else if ( cs & SMICS_ACTIVE )
    printf ( "  SMICS: DMA-path transfer ACTIVE (stuck)\n" );
  else
    printf ( "  SMICS: 0x%08X — check for unexpected bits\n", cs );

  if ( !(ctl & CM_CTL_ENAB) )
    printf ( "  SMI clock NOT enabled — call ps_smi_init first\n" );
  else
    printf ( "  SMI clock running OK\n" );

  if ( dcs & SMIDCS_START && !(dcs & SMIDCS_DONE) )
    printf ( "  SMIDCS: START set but DONE not set — direct-mode transfer stuck\n" );
  else if ( dcs & SMIDCS_ENABLE )
    printf ( "  SMIDCS: enabled and idle — ready\n" );
  else
    printf ( "  SMIDCS: disabled — needs ps_smi_init\n" );

  if ( lev & 1 )
    printf ( "  PI_TXN_IN_PROGRESS asserted on GPIO 0 — CPLD busy or stuck\n" );
  else
    printf ( "  PI_TXN_IN_PROGRESS clear — CPLD idle\n" );

  if ( lev & 0x80 )
    printf ( "  BERR asserted — clear with ps_reset_state_machine() after init\n" );

  printf ( "\n=== Write test ===\n" );
  /* Try writing SMICS and read back to see if writes are taking effect */
  uint32_t smics_before = smi[SMICS];
  smi[SMICS] = (1u << 0);              /* write ENABLE */
  uint32_t smics_after_enable = smi[SMICS];
  smi[SMICS] = (1u << 0) | (1u << 4); /* write ENABLE | CLEAR */
  usleep(100);
  uint32_t smics_after_clear = smi[SMICS];
  smi[SMICS] = 0;
  uint32_t smics_after_zero = smi[SMICS];

  printf ( "  SMICS before write:       0x%08X\n", smics_before );
  printf ( "  SMICS after ENABLE:       0x%08X  (bit 0 should be 1)\n", smics_after_enable );
  printf ( "  SMICS after ENABLE|CLEAR: 0x%08X  (bits 28,30 should clear)\n", smics_after_clear );
  printf ( "  SMICS after zero:         0x%08X  (should be ~0x14000000 if RXF/TXE only)\n", smics_after_zero );

  if ( smics_after_enable == smics_before )
    printf ( "  *** SMICS is NOT writable — kernel SMI driver likely loaded ***\n" );
  else
    printf ( "  SMICS is writable OK\n" );

  /* Check for kernel smi driver */
  printf ( "\n=== Kernel SMI driver check ===\n" );
  if ( access("/dev/smi", F_OK) == 0 )
    printf ( "  /dev/smi exists — kernel driver is active, unload with: sudo modprobe -r bcm2835_smi\n" );
  else
    printf ( "  /dev/smi not present\n" );

  FILE *mods = popen("lsmod 2>/dev/null | grep smi", "r");
  if (mods) {
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), mods)) {
      printf ( "  lsmod: %s", line );
      found = 1;
    }
    if (!found) printf ( "  No SMI kernel module loaded\n" );
    pclose(mods);
  }

  printf ( "\n" );
  return 0;
}
