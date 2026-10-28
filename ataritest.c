/*
 *
 * syntax
 * ataritest --peek address=0xff8200 loop=yes
 * ataritest --poke address=0x600 data=0x33
 * ataritest --clearmem size=16536 pattern=0x1234 loop=yes
 * ataritest --dumprom address=0xe00000 size=192 or size=256
 * ataritest --init 512 or 1024 or 2048 or 4096
 * ataritest --memory tests=rwx size=512 loop=yes
 * 
 * 
 */

#include <assert.h>
#include <dirent.h>
#include <endian.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "emulator.h"
#include "gpio/bus_lock.h"
#include "gpio/ps_protocol.h"

#define SIZE_KILO 1024
#define SIZE_MEGA (1024 * 1024)
#define SIZE_GIGA (1024 * 1024 * 1024)

#define OFFSET          0x0600
#define MEM_READ        0
#define MEM_WRITE       1
#define TEST_8          8
#define TEST_8_ODD      81
#define TEST_8_RANDOM   82
#define TEST_16         16
#define TEST_16_ODD     161
#define TEST_16_RANDOM  162
#define TEST_32         32
#define TEST_32_ODD     321
#define TEST_32_RANDOM  322

#define REVERSE_VIDEO   "\033[7m"
#define NORMAL          "\033[0m"


int  parser ( int argc, char **argv );
int  memTest ( int direction, int type, uint32_t startAdd, uint32_t length, uint8_t *garbagePtr );
void clearmem ( uint32_t length, uint32_t *duration, uint16_t pattern, int8_t loop );
void setMemory ( uint32_t size );
void peek ( uint32_t start );
void mfptest ( uint32_t count, int irq, int iack );
void poke ( uint32_t address, uint8_t data );
void dump ( uint32_t ROMsize, uint32_t ROMaddress );
void memspeed ( uint32_t length );
void busprofile ( void );
void hwTest ( void );
void devTest ( int rw );
void atariReset ( void );
void atariHalt ( void );
void arbTest ( void );

#define ARB_SOURCE_NONE    0
#define ARB_SOURCE_BLITTER 1
#define ARB_BLIT_FILL      0
#define ARB_BLIT_COPY      1

int VERBOSE = 0;
int doReads;
int doWrites;
int doRandoms;
uint32_t testSize;
uint32_t memSize;
int totalErrors;
int loopTests;
int errorStop;
uint32_t padd;
uint8_t pdata;
int cmdPeek = 0;
int cmdMfp = 0;
int mfpIrq = 0;
int mfpIack = 0;
uint32_t mfpCount = 100000;
int cmdPoke = 0;
int cmdMem = 0;
int cmdDump = 0;
int cmdClear = 0;
int cmdInit = 0;
int cmdMemSpeed = 0;
int cmdBusProfile = 0;
int targetF = 200;
int cmdHWTEST = 0;
int cmdDevTest = 0;
int rwtest = 0;
int cmdRESET = 0;
int cmdHALT = 0;
int cmdArbTest = 0;
int arbSeconds = 5;
int arbHammer = 0;
int arbSource = ARB_SOURCE_NONE;
int arbBlitOp = ARB_BLIT_FILL;
uint32_t arbBlitEveryMs = 20;
uint32_t arbBlitQuietMs = 250;
uint32_t arbAddress = 0x000600;
uint32_t ROMsize = 192;
uint32_t ROMaddress = 0x00e00000;
uint16_t clrPattern = 0x0000;


uint8_t *garbege_datas;
extern volatile unsigned int *gpio;
extern uint8_t fc;
extern volatile uint32_t g_buserr;

struct timespec f2;

uint32_t mem_fd;
uint32_t errors = 0;
uint8_t  loop_tests = 0;
uint32_t cur_loop;



void sigint_handler ( int sig_num )
{
  printf ( "\nATARITEST aborted\n\n");

  if (mem_fd)
    close(mem_fd);

  exit(0);
}


void ps_reinit () 
{
    ps_pulse_reset();
    ps_reset_state_machine();
}


int check_emulator () 
{

    DIR* dir;
    struct dirent* ent;
    char buf[512];

    long  pid;
    char pname[100] = {0,};
    char state;
    FILE *fp=NULL;
    const char *name = "emulator";

    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc, assuming emulator running");
        return 1;
    }

    while((ent = readdir(dir)) != NULL) {
        long lpid = atol(ent->d_name);
        if(lpid < 0)
            continue;
        snprintf(buf, sizeof(buf), "/proc/%ld/stat", lpid);
        fp = fopen(buf, "r");

        if (fp) {
            if ( (fscanf(fp, "%ld (%[^)]) %c", &pid, pname, &state)) != 3 ){
                /* NOT a reason to bail out: /proc entries come and go
                 * while we scan, so a process exiting mid-read lands
                 * here. Treating that as "emulator running" made the
                 * tool refuse to start for an unrelated passer-by. */
                fclose(fp);
                continue;
            }
            /* Skip the DEAD. A zombie ('Z') is a process-table entry
             * whose process has already exited - it holds no memory, no
             * file descriptors and no GPIO mapping, so it cannot be
             * using the bus. Matching it by name alone locked the tool
             * out until reboot, because a zombie cannot be killed: only
             * its parent reaping it makes it go away. 'X' is likewise a
             * corpse being torn down. */
            if (!strcmp(pname, name) && state != 'Z' && state != 'X') {
                printf("found live '%s' at pid %ld (state %c)\n",
                       pname, lpid, state);
                fclose(fp);
                closedir(dir);
                return 1;
            }
            fclose(fp);
        }
    }

    closedir(dir);
    return 0;
}


int main ( int argc, char *argv[] ) 
{
    uint32_t test_size = 2 * SIZE_KILO;
    uint32_t add;


    cur_loop = 1;

    /* Exclusive claim on the bus (gpio/bus_lock.c). This replaces the
     * old /proc name scan, which counted a ZOMBIE emulator as running -
     * and since a zombie cannot be killed, only reaped by its parent,
     * that locked this tool out until a reboot. A kernel file lock is
     * released whenever the holder dies, zombie or not. check_emulator()
     * is kept below as a fallback for the case where the emulator
     * predates the lock (an old binary still running). */
    if ( pistorm_bus_lock ( "ataritest" ) != 0 )
    {
        printf("PiStorm emulator running, please stop this before running ataritest\n");
        return 1;
    }

    if ( check_emulator () )
    {
        printf("PiStorm emulator running, please stop this before running ataritest\n");
        return 1;
    }

    clock_gettime ( CLOCK_PROCESS_CPUTIME_ID, &f2 );
    srand ( (unsigned int)f2.tv_nsec );

    signal ( SIGINT, sigint_handler );

    //ps_setup_protocol ( targetF );
    //exit(1);
   // ps_reset_state_machine ();
   // ps_pulse_reset ();
    

   // usleep (1500);

	fc = 6; //0b101;
    //write8( 0xff8001, 0b00001010 ); // memory config 512k bank 0
    
    doReads = 0;
    doWrites = 0;
    doRandoms = 0;
    memSize = 0;
    testSize = 512;
    totalErrors = 0;
    loopTests = 0;
    errorStop = 0;
    clrPattern = 0x0000;

   
    if ( parser ( argc, argv ) )
    {        
        ps_setup_protocol ();
       
        if ( VERBOSE )
            ps_get_firmware_revision ();

        if ( !memSize )
        {
            memSize = 512;
        }

        if ( testSize > memSize )
        {
            memSize = ( testSize <= 1024 ? 1024 : ( testSize <= 2048 ? 2048 : 4096) );
        }

        if ( cmdInit )
        {
            /* must be 512 or 1024 or 2048 or 4096 */
            setMemory ( memSize );
        }

        if ( cmdMem )
        {
            /* must be 512 or 1024 or 2048 or 4096 */
            setMemory ( memSize );

            printf ( "\nATARITEST\n" );
            printf ( "ATARI MMU configured to use %d KB\n", memSize );
        }

        if ( cmdClear )
        {
            uint32_t duration;

            /* must be 512 or 1024 or 2048 or 4096 */
            setMemory ( memSize );

            printf ( "\nClearing ATARI ST RAM - %d KB\n", testSize );
            fflush (stdout);

            clearmem ( testSize * SIZE_KILO, &duration, clrPattern, loopTests );
            
            printf ( "\nATARI ST RAM cleared in %d ms @ %.2f MB/s\n\n", 
                //duration, ( (float)((testSize * 1024) - OFFSET) / (float)duration * 1000.0) / 1024 );
                duration, ( ( 1.0 / (float)duration ) * testSize ) );

        }

        if ( cmdPoke )
        {
            poke ( padd, pdata );
           // printf ("poking %x with %x\n", padd, pdata );
        }

        if ( cmdPeek )
        {
            if ( loopTests )
            {
                while ( loopTests )
                {
                    printf ( "\033[2J" );
                    peek ( padd );
                    usleep(50000);  /* 50ms delay to make redraw smoother */
                }
            }

            else
                peek ( padd );
        }

        if ( cmdMfp )
            mfptest ( mfpCount, mfpIrq, mfpIack );

        if ( cmdDump )
        {
            printf ( "Dumping onboard ATARI ROM from 0x%X to file tos.rom\n", ROMaddress );

            dump ( ROMsize, ROMaddress );

            printf ( "ATARI ROM dumped - %d KB\n", ROMsize );
        }

        if ( cmdBusProfile )
            busprofile ();

        if ( cmdMemSpeed )
        {
            printf ( "\nChecking ATARI ST RAM memory bandwidth - %d KB\n", testSize );
            fflush (stdout);

            memspeed ( testSize * SIZE_KILO );
            
            //printf ( "\nATARI ST RAM cleared in %d ms @ %.2f KB/s\n\n", 
            //    duration, ( (float)((testSize * 1024) - OFFSET) / (float)duration * 1000.0) / 1024 );
        }

        if ( cmdMem )
        {
            if ( !doReads && !doWrites && !doRandoms )
            {
                printf ( "No memory tests selected\n" );

                exit (0);        
            }

            test_size = testSize * SIZE_KILO;
            

            garbege_datas = malloc ( test_size );

            if ( !garbege_datas )
            {
                printf ( "Failed to allocate memory for garbege datas\n" );

                return 1;
            }

            printf ( "Testing %d KB of memory - Starting address $%.6X\n", test_size / SIZE_KILO, OFFSET );

            if ( loopTests )
                printf ( "Test looping enabled\n" );

            //if (doWrites)
            {
                printf ( "Priming test data\n");

                add = (uint32_t)OFFSET;

                for ( uint32_t i = 0; add < test_size; i++, add++ ) 
                {
                    garbege_datas [i] = add % 2 ? (add - 1 >> 8) & 0xff : add & 0xff;

                    //if ( i == 0 )
                    //    printf ( "add %.8X = %.2X\n", add, garbege_datas [i] );
                    write8 ( add, garbege_datas [i] );
                }
            }

test_loop:

            if (doReads)
            {
                printf ( "\n%sTesting Reads...%s\n\n", REVERSE_VIDEO, NORMAL );

                if ( ! memTest ( MEM_READ,  TEST_8,         OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_READ,  TEST_16,        OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_READ,  TEST_16_ODD,    OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_READ,  TEST_32,        OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_READ,  TEST_32_ODD,    OFFSET, test_size, garbege_datas ) ) return 1;
            }

            if (doWrites)
            {
                printf ( "\n%sTesting Writes...%s\n\n", REVERSE_VIDEO, NORMAL );

                if ( ! memTest ( MEM_WRITE, TEST_8,         OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_WRITE, TEST_16,        OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_WRITE, TEST_16_ODD,    OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_WRITE, TEST_32,        OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_WRITE, TEST_32_ODD,    OFFSET, test_size, garbege_datas ) ) return 1;
            }

            if (doRandoms)
            {
                printf ( "\n%sTesting Random Reads / Writes...%s\n\n", REVERSE_VIDEO, NORMAL );

                if ( ! memTest ( MEM_READ,  TEST_8_RANDOM,  OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_READ,  TEST_16_RANDOM, OFFSET, test_size, garbege_datas ) ) return 1;
                if ( ! memTest ( MEM_READ,  TEST_32_RANDOM, OFFSET, test_size, garbege_datas ) ) return 1;
            }

            if (loopTests) 
            {
                printf ( "%-20s%sPass %d complete. ", "", REVERSE_VIDEO, cur_loop );
                printf ( "Total errors %d%s\n\n", totalErrors, NORMAL );

                sleep(1);

                printf ( "\n%-20s%sStarting pass %d%s\n", "", REVERSE_VIDEO, cur_loop + 1, NORMAL );                

                printf ( "Priming test data\n" );

                add = (uint32_t)OFFSET;

                for ( uint32_t i = 0; add < test_size; i++, add++ ) 
                {
                    garbege_datas [i] = (uint8_t)(rand() % 0xFF );
                    write8 ( add, (uint32_t) garbege_datas [i] );
                }

                cur_loop++;

                goto test_loop;
            }
        }
    
        if ( cmdHWTEST )
        {
            hwTest ();
        }

        if ( cmdDevTest )
        {
            devTest ( rwtest );
        }

        if ( cmdRESET )
        {
            atariReset ();
        }

        if ( cmdHALT )
        {
            atariHalt ();
        }

        if ( cmdArbTest )
        {
            arbTest ();
        }
    }

    else
    {
        printf ( "ATARITEST syntax error\n"
                 "--clearmem <size=xxx> <pattern=xxxx> <loop=yes>\n"
                 "     fills memory for specified size with 0's or pattern.\n"
                 "     <size> 512 to 4096. If not supplied, 512 is used.\n"
                 "     <pattern> memory will be filled with 16bit pattern\n"
                 "--memory tests=<rwx> <loop=yes> <stop=yes> <size=xxx>\n"
                 "     Run memory tests.\n"
                 "     tests r=read, w=write, x=random reads/writes.\n"
                 "     At least one test must be supplied.\n"
                 "     <loop> repeats tests until CNTRL-C is entered.\n"
                 "     <stop> aborts tests on an error.\n"
                 "--mfp <count=N> <irq=yes> <iack=yes>\n"
                 "     Read-stability test of MFP/ACIA registers (bit errors on\n"
                 "     peripheral reads). Default 100000 reads per register.\n"
                 "     <irq=yes> holds an MFP interrupt asserted during the reads.\n"
                 "     <iack=yes> adds real level-6 IACK cycles with a GPIP read after each.\n"
                 "--peek address=xxxxxx <loop=yes>\n"
                 "     examine address.\n"
                 "     displays 256 bytes from address.\n"
                 "     <loop> continuously reads 256 bytes from address.\n"
                 "--poke address=xxxxxx data=xx\n"
                 "     writes BYTE data to address.\n"
                 "--dumprom address=xxxxxx size=[192 | 256]\n"
                 "     reads memory from address to address+size and writes to file tos.rom\n"
                 "--init <size=xxx>\n"
                 "     configures ATARI MMU for the specified size.\n"
                 "     <size> 512 to 4096. If not supplied, 512 is used\n"
                 "--arbtest <seconds=n> <hammer=yes> <address=xxxxxx> <source=blitter> <op=fill|copy> <blitms=n> <quietms=n>\n"
                 "     polls CPLD arbitration status bit and optionally hammers RAM or starts real blits\n"
        );

        exit (0);
    }

    if ( cmdMem )
    {
        printf ( "\nATARITEST complete\n\n");
    }

    else
        printf ( "\n" );

    return 0;
}


void m68k_set_irq ( unsigned int level ) 
{
}


/* --busprofile: where does one bus access spend its time?
 *
 *   total   ns per read16 / write16 (lock + GPIO phases + waiting)
 *   polls   GPLEV reads per access spent waiting for TXN to drop - the
 *           CPLD running the real 68000 bus cycle on the ST
 *   GPLEV   cost of one GPLEV read, measured here
 *   GPSET   cost of one GPSET write (value 0: changes no pin), measured
 *   lock    cost of one ps_lock_bus/ps_unlock_bus pair, measured
 *
 * waiting ~= polls x GPLEV; the rest of total is the Pi's own work. A
 * 68000 bus cycle at 8 MHz is 500 ns, so waiting near 500 ns with total
 * near 1000 ns says the host side costs as much as the bus cycle itself.
 * Reads/writes go to ST RAM $10000-$8FFFF: the emulator must be stopped. */
#ifdef PS_PROFILE
extern uint64_t ps_prof_rd_spins, ps_prof_wr_spins;
extern void ps_prof_lock_pair ( void );
extern uint16_t ps_prof_read16_nolock ( uint32_t addr );
extern void ps_prof_write16_nolock ( uint32_t addr, uint16_t data );
extern uint16_t ps_prof_read16_ish ( uint32_t addr );
extern void ps_prof_write16_ish ( uint32_t addr, uint16_t data );
extern volatile uint32_t *ioread, *ioset;

static double prof_now_ns ( void )
{
    struct timespec t;
    clock_gettime ( CLOCK_MONOTONIC, &t );
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

void busprofile ( void )
{
    const int N = 262144, M = 1000000;
    double t0, gplev, gpset, lock, rd, wr;
    volatile uint32_t sink = 0;

    printf ( "\nBus access profile (%d word reads, %d word writes)\n", N, N );

    t0 = prof_now_ns ();
    for ( int i = 0; i < M; i++ ) sink += *ioread;
    gplev = ( prof_now_ns () - t0 ) / M;

    t0 = prof_now_ns ();
    for ( int i = 0; i < M; i++ ) *ioset = 0;
    asm volatile ( "dmb sy" ::: "memory" );
    gpset = ( prof_now_ns () - t0 ) / M;

    t0 = prof_now_ns ();
    for ( int i = 0; i < M; i++ ) ps_prof_lock_pair ();
    lock = ( prof_now_ns () - t0 ) / M;

    ps_prof_rd_spins = 0;
    t0 = prof_now_ns ();
    for ( int i = 0; i < N; i++ ) sink += read16 ( 0x10000 + ( (uint32_t)i << 1 ) );
    rd = ( prof_now_ns () - t0 ) / N;

    ps_prof_wr_spins = 0;
    t0 = prof_now_ns ();
    for ( int i = 0; i < N; i++ ) write16 ( 0x10000 + ( (uint32_t)i << 1 ), 0x5a5a );
    wr = ( prof_now_ns () - t0 ) / N;

    double rpolls = (double)ps_prof_rd_spins / N, wpolls = (double)ps_prof_wr_spins / N;
    printf ( "  GPLEV read   %6.1f ns    GPSET write %6.1f ns    lock+unlock %6.1f ns\n",
             gplev, gpset, lock );
    printf ( "  read16  %7.1f ns/access = %.2f MB/s   polls %5.2f -> waiting ~%6.1f ns, Pi side ~%6.1f ns\n",
             rd, 2000.0 / rd, rpolls, rpolls * gplev, rd - rpolls * gplev );
    printf ( "  write16 %7.1f ns/access = %.2f MB/s   polls %5.2f -> waiting ~%6.1f ns, Pi side ~%6.1f ns\n",
             wr, 2000.0 / wr, wpolls, wpolls * gplev, wr - wpolls * gplev );
    printf ( "  (a real 68000 bus cycle at 8 MHz is 500 ns = 4.00 MB/s)\n" );

    /* Same accesses with no bus lock: if access time is quantised to the
     * ST's 500 ns bus slots, removing ~77 ns either changes nothing or
     * drops a whole slot - not 77 ns. */
    t0 = prof_now_ns ();
    for ( int i = 0; i < N; i++ ) sink += ps_prof_read16_nolock ( 0x10000 + ( (uint32_t)i << 1 ) );
    rd = ( prof_now_ns () - t0 ) / N;
    t0 = prof_now_ns ();
    for ( int i = 0; i < N; i++ ) ps_prof_write16_nolock ( 0x10000 + ( (uint32_t)i << 1 ), 0x5a5a );
    wr = ( prof_now_ns () - t0 ) / N;
    printf ( "  no lock:  read16 %7.1f ns   write16 %7.1f ns\n", rd, wr );

    /* lock with dmb ish instead of dmb sy */
    t0 = prof_now_ns ();
    for ( int i = 0; i < N; i++ ) sink += ps_prof_read16_ish ( 0x10000 + ( (uint32_t)i << 1 ) );
    rd = ( prof_now_ns () - t0 ) / N;
    t0 = prof_now_ns ();
    for ( int i = 0; i < N; i++ ) ps_prof_write16_ish ( 0x10000 + ( (uint32_t)i << 1 ), 0x5a5a );
    wr = ( prof_now_ns () - t0 ) / N;
    printf ( "  ish lock: read16 %7.1f ns   write16 %7.1f ns\n", rd, wr );

    /* repeat the main locked runs: is the first read16 figure stable? */
    for ( int r = 0; r < 3; r++ )
    {
        t0 = prof_now_ns ();
        for ( int i = 0; i < N; i++ ) sink += read16 ( 0x10000 + ( (uint32_t)i << 1 ) );
        rd = ( prof_now_ns () - t0 ) / N;
        t0 = prof_now_ns ();
        for ( int i = 0; i < N; i++ ) write16 ( 0x10000 + ( (uint32_t)i << 1 ), 0x5a5a );
        wr = ( prof_now_ns () - t0 ) / N;
        printf ( "  repeat %d: read16 %7.1f ns   write16 %7.1f ns\n", r + 1, rd, wr );
    }

    /* Slack: add k extra GPLEV reads (~75 ns each) of Pi time before each
     * locked read. While the access still lands in the same slot the total
     * grows by nothing; the k where it jumps by ~500 ns is how much Pi
     * time is left before the next slot is missed. */
    printf ( "  added Pi time before each read16 (slot quantisation):\n" );
    for ( int k = 0; k <= 8; k++ )
    {
        t0 = prof_now_ns ();
        for ( int i = 0; i < N / 4; i++ )
        {
            for ( int j = 0; j < k; j++ ) sink += *ioread;
            sink += read16 ( 0x10000 + ( (uint32_t)i << 1 ) );
        }
        rd = ( prof_now_ns () - t0 ) / ( N / 4 );
        printf ( "    +%d GPLEV (~%4.0f ns): %7.1f ns/access\n", k, k * gplev, rd );
    }
    (void)sink;
}
#else
void busprofile ( void ) { printf ( "busprofile: build with -DPS_PROFILE\n" ); }
#endif

void memspeed ( uint32_t length )
{
    uint32_t address;
    struct timespec tmsStart, tmsEnd;
    long int nanoStart;
    long int nanoEnd;


    printf ( "Memory Speed Test\n" );

    /* READ */
    clock_gettime ( CLOCK_REALTIME, &tmsStart );

    for ( address =  0; address < length; address += 2 )
        read16 ( address );

    clock_gettime ( CLOCK_REALTIME, &tmsEnd );

    nanoStart = (tmsStart.tv_sec * 1000) + (tmsStart.tv_nsec / 1000000);
    nanoEnd = (tmsEnd.tv_sec * 1000) + (tmsEnd.tv_nsec / 1000000);

    printf ( "READ:  %d ms = %.2f MB/s\n", (nanoEnd - nanoStart), 
        ( 1.0 / ( (float)(nanoEnd - nanoStart) ) * length ) / 1024 );     /* MB/s */


    /* WRITE */
    clock_gettime ( CLOCK_REALTIME, &tmsStart );
    
    for ( address = 0; address < length; address += 2 )
        write16 ( address, 0x5a5a);

    clock_gettime ( CLOCK_REALTIME, &tmsEnd );

    nanoStart = (tmsStart.tv_sec * 1000) + (tmsStart.tv_nsec / 1000000);
    nanoEnd = (tmsEnd.tv_sec * 1000) + (tmsEnd.tv_nsec / 1000000);

    printf ( "WRITE: %d ms = %.2f MB/s\n", (nanoEnd - nanoStart), 
        ( 1.0 / ( (float)(nanoEnd - nanoStart) ) * length ) / 1024 );     /* MB/s */
}



/* --mfp count=N
 *
 * Bit-error rate on reads of the MFP and the keyboard ACIA, with the 68k
 * halted (PiStorm holds the bus, so nothing else touches these chips).
 * Written for the Pi 3A+: ataritest --memory was clean while the emulator
 * saw GPIP bit 7 read low and IACK vectors read $4F for $45 - both bit
 * errors on D0-D7 of DTACK-terminated 68901 reads, which --memory never
 * exercises. Every register here is static with the CPU halted, so any
 * disagreement between consecutive reads IS a bus read error.
 *   $FFFA01 GPIP  - bit 7 = monitor detect (1 = colour with an ST-SCART lead)
 *   $FFFA03 AER, $FFFA05 DDR, $FFFA07 IERA, $FFFA09 IERB, $FFFA17 VR
 *   $FFFC00 ACIA keyboard status - a VPA / E-clock cycle, the slowest on
 *                the bus and the one the H5 hang was seen on
 *   $000600 RAM byte as the control
 * Prints, per register: distinct values seen with their counts, the number
 * of reads that differed from the majority value, and bus errors.
 *
 * irq=yes: before the loop, Timer D is set to interrupt (TDDR=1, /4
 * prescale, IERB/IMRB bit 4) and is never acknowledged, so the 68901 holds
 * IRQ asserted - IPL=6 on the CPLD - for every read. That is the one
 * condition the halted test lacks and the live emulator always has while
 * the keyboard ISR reads GPIP (it runs straight after the IACK, with the
 * ACIA line active). Registers are restored afterwards. */
void mfptest ( uint32_t count, int irq, int iack )
{
    static const struct { uint32_t addr; const char *name; } regs [] = {
        { 0xFFFA01, "GPIP  " }, { 0xFFFA03, "AER   " }, { 0xFFFA05, "DDR   " },
        { 0xFFFA07, "IERA  " }, { 0xFFFA09, "IERB  " }, { 0xFFFA17, "VR    " },
        { 0xFFFC00, "ACIAst" }, { 0x000600, "RAM   " },
    };
    const int nregs = (int)(sizeof regs / sizeof regs [0]);
    uint32_t total_bad = 0;

    uint8_t sv_ierb = 0, sv_imrb = 0, sv_tcdcr = 0, sv_tddr = 0, sv_vr = 0;
    if ( irq )
    {
        uint8_t b;
        sv_ierb  = ps_read_8_fc ( 0xFFFA09, fc, &b );
        sv_imrb  = ps_read_8_fc ( 0xFFFA15, fc, &b );
        sv_tcdcr = ps_read_8_fc ( 0xFFFA1D, fc, &b );
        sv_tddr  = ps_read_8_fc ( 0xFFFA25, fc, &b );
        sv_vr    = ps_read_8_fc ( 0xFFFA17, fc, &b );
        ps_write_8 ( 0xFFFA1D, sv_tcdcr & 0xF0 );     /* Timer D stopped */
        /* A channel left in-service by a crashed guest (software EOI
         * mode) masks every lower channel: seen ISRB=$40 (keyboard ACIA)
         * blocking Timer D. Clear in-service and pending on both halves. */
        ps_write_8 ( 0xFFFA0F, 0x00 );                /* ISRA */
        ps_write_8 ( 0xFFFA11, 0x00 );                /* ISRB */
        ps_write_8 ( 0xFFFA0B, 0x00 );                /* IPRA */
        ps_write_8 ( 0xFFFA0D, 0x00 );                /* IPRB */
        ps_write_8 ( 0xFFFA17, 0x48 );                /* vector base $40, S=1 (software EOI) */
        ps_write_8 ( 0xFFFA25, 0x01 );                /* TDDR = 1 */
        ps_write_8 ( 0xFFFA09, sv_ierb | 0x10 );      /* IERB bit 4 = Timer D */
        ps_write_8 ( 0xFFFA15, sv_imrb | 0x10 );      /* IMRB bit 4 */
        ps_write_8 ( 0xFFFA1D, (sv_tcdcr & 0xF0) | 0x01 ); /* /4: fires in ~2 us, every 2 us */
        usleep ( 100 );
        {
            uint8_t ipl = 0, ipr = ps_read_8_fc ( 0xFFFA0D, fc, &b ), isr = ps_read_8_fc ( 0xFFFA11, fc, &b );
            ps_read_ipl ( &ipl );
            printf ( "\nIRQ held: MFP IPRB=$%02X ISRB=$%02X, CPLD IPL=%u%s\n", ipr, isr, ipl,
                     ipl == 6 ? "" : "  (expected 6 - the MFP is NOT asserting; results below are without IRQ)" );
        }
    }

    printf ( "\nMFP / ACIA read stability - %u reads per register, CPU halted%s\n\n", count,
             irq ? ", MFP IRQ asserted throughout" : "" );
    printf ( " reg      addr     majority  errors  berr   values seen (value:count)\n" );

    for ( int r = 0; r < nregs; r++ )
    {
        uint32_t hist [256];
        uint32_t berrs = 0;
        memset ( hist, 0, sizeof hist );

        for ( uint32_t i = 0; i < count; i++ )
        {
            uint8_t berr = 0;
            uint8_t v = ps_read_8_fc ( regs [r].addr, fc, &berr );
            if ( berr )
                berrs++;
            else
                hist [v]++;
        }

        int maj = 0;
        for ( int v = 1; v < 256; v++ )
            if ( hist [v] > hist [maj] )
                maj = v;

        uint32_t errors = 0;
        for ( int v = 0; v < 256; v++ )
            if ( v != maj )
                errors += hist [v];
        total_bad += errors + berrs;

        printf ( " %s  $%06X  $%02X       %-7u %-6u", regs [r].name, regs [r].addr,
                 maj, errors, berrs );
        int shown = 0;
        for ( int v = 0; v < 256; v++ )
        {
            if ( !hist [v] )
                continue;
            if ( shown++ == 12 ) { printf ( " ..." ); break; }
            printf ( " $%02X:%u", v, hist [v] );
        }
        printf ( "\n" );

        if ( regs [r].addr == 0xFFFA01 && errors )
        {
            /* which bits flip relative to the majority */
            uint32_t bitflip [8] = { 0 };
            for ( int v = 0; v < 256; v++ )
                if ( hist [v] && v != maj )
                    for ( int b = 0; b < 8; b++ )
                        if ( ((v ^ maj) >> b) & 1 )
                            bitflip [b] += hist [v];
            printf ( "          GPIP bits that read wrong:" );
            for ( int b = 7; b >= 0; b-- )
                if ( bitflip [b] )
                    printf ( " D%d:%u", b, bitflip [b] );
            printf ( "\n" );
        }
    }

    /* iack=yes (needs irq=yes): the real thing the live system does and
     * nothing above does - a level-6 IACK cycle (FC=7, $FFFFFD), vector
     * expected $44 (base $40 + Timer D ch4), then a GPIP read immediately
     * after, as the keyboard ISR does. Software-EOI mode leaves ch4 in
     * service, which drops IRQ; clear ISRB to re-arm, wait for IPRB bit 4
     * (Timer D fires every ~2 us), repeat. */
    if ( irq && iack )
    {
        uint32_t vhist [256], ghist [256], berrs = 0, noirq = 0, gpip_bad = 0, vbad = 0;
        uint32_t n = count / 10 < 1000 ? 1000 : count / 10;   /* each pass is several cycles */
        memset ( vhist, 0, sizeof vhist );
        memset ( ghist, 0, sizeof ghist );

        for ( uint32_t i = 0; i < n; i++ )
        {
            uint8_t b = 0, ipl = 0;
            int w;
            ps_write_8 ( 0xFFFA11, 0x00 );                 /* ISRB: end of service */
            for ( w = 0; w < 1000; w++ )                    /* wait for Timer D to re-pend */
            {
                ps_read_ipl ( &ipl );
                if ( ipl == 6 ) break;
            }
            if ( ipl != 6 ) { noirq++; continue; }

            uint8_t v = ps_read_8_fc ( 0x00FFFFFDu, 7, &b );
            if ( b ) { berrs++; continue; }
            vhist [v]++;
            if ( v != 0x44 ) vbad++;

            uint8_t g = ps_read_8_fc ( 0xFFFA01, fc, &b );
            ghist [g]++;
            if ( g != 0xEF ) gpip_bad++;
        }

        printf ( "\nIACK cycles: %u  vector!=$44: %u  berr: %u  no-IRQ (timer did not re-pend): %u\n",
                 n, vbad, berrs, noirq );
        printf ( "  vectors seen:" );
        for ( int v = 0; v < 256; v++ ) if ( vhist [v] ) printf ( " $%02X:%u", v, vhist [v] );
        printf ( "\n  GPIP read right after IACK: %u != $EF; values:", gpip_bad );
        for ( int v = 0; v < 256; v++ ) if ( ghist [v] ) printf ( " $%02X:%u", v, ghist [v] );
        printf ( "\n" );
        total_bad += vbad + berrs + gpip_bad;
    }

    if ( irq )
    {
        ps_write_8 ( 0xFFFA1D, sv_tcdcr & 0xF0 );
        ps_write_8 ( 0xFFFA09, sv_ierb );
        ps_write_8 ( 0xFFFA15, sv_imrb );
        ps_write_8 ( 0xFFFA0D, 0xEF );                /* clear Timer D pending */
        ps_write_8 ( 0xFFFA11, 0xEF );                /* and in-service */
        ps_write_8 ( 0xFFFA25, sv_tddr );
        ps_write_8 ( 0xFFFA1D, sv_tcdcr );
        ps_write_8 ( 0xFFFA17, sv_vr );
    }

    printf ( "\n%s: %u bad reads in total%s\n\n", total_bad ? "FAIL" : "PASS", total_bad,
             irq ? " (IRQ asserted)" : "" );
}

void peek ( uint32_t start )
{
    char            ascii [17];
    int             i, j;
    uint32_t        address;
    unsigned char   data [0x100];                                   /* 256 byte block */
    int             size = 0x100;
    

        
    address = (start / 16) * 16;                                    /* we want a 16 byte boundary */
 
    ascii[16] = '\0';

    printf ( "\n Address    00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n\n" );

    for ( i = 0; i < size; ++i, address++ ) 
    {
        data [i] = read8 (address);

        //if ( g_buserr )
        //    printf ( "bus error\n" );

        if ( !(address % 16) )
            printf( " $%.6X  | ", address );

        printf( "%02X ", ((unsigned char*)data)[i]);

        if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') 
        {
            ascii[i % 16] = ((unsigned char*)data)[i];
        } 

        else 
        {
            ascii[i % 16] = '.';
        }

        if ((i+1) % 8 == 0 || i+1 == size) 
        {
            //printf(" ");

            if ((i+1) % 16 == 0) 
            {
                printf("|  %s \n", ascii);
            } 

            else if (i+1 == size) 
            {
                ascii[(i+1) % 16] = '\0';

                if ((i+1) % 16 <= 8) 
                {
                    //   printf(" ");
                }

                for (j = (i+1) % 16; j < 16; ++j) 
                {
                    printf("   ");
                }

                printf("|  %s \n", ascii);
            }
        }
    }
}


void poke ( uint32_t address, uint8_t data )
{
    write8 ( address, data );
}


/* configure ATARI MMU for amount of system RAM */
	//write8( 0xff8001, 0b00000100 ); // memory config 512k bank 0
    /*
    #define ATARI_MMU_128K  0b00000000 // bank 0
    #define ATARI_MMU_512K  0b00000100 // bank 0
    #define ATARI_MMU_2M    0b00001000 // bank 0
*/
void setMemory ( uint32_t size )
{
    uint8_t banks;

    switch (size)
    {
        case 512:
            banks = 0b00000100;
        break;

        case 1024:
            banks = 0b00000101;
        break;

        case 2048:
            banks = 0b00001000;
        break;

        case 4096:
            banks = 0b00001010;
        break;

        default:
            banks = 0b00000100;
        break;
    }

    write8 ( ((uint32_t)0xff8001), banks ); 
}


int memTest ( int direction, int type, uint32_t startAdd, uint32_t length, uint8_t *garbagePtr )
{
    uint8_t  d8, rd8;
    uint16_t d16, rd16;
    uint32_t d32, rd32;
    uint32_t radd;
    uint32_t add;
    long int nanoStart;
    long int nanoEnd; 

    char dirStr  [6];
    char typeStr [20];
    char testStr [80];

    struct timespec tmsStart, tmsEnd;
    static int testNumber;
    static int currentPass      = 0;
    int errors                  = 0;
    int thisTestErrors          = 0;
    int passErrors              = 0;


    if ( currentPass != cur_loop )
    {
        currentPass = cur_loop;
        testNumber = 1;
    }

    printf ( "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n" );

    switch (direction) 
    {
        case MEM_READ:

            sprintf ( dirStr, "READ" );

                switch (type)
                {
                    case TEST_8:

                        sprintf ( typeStr, "8:" );
                        sprintf ( testStr, "%s%s ", dirStr, typeStr );

                        printf ( "Test %d\n", testNumber );
                        printf ( "%-20s[BYTE] Reading RAM...\n", testStr );

                        add = startAdd;

                        clock_gettime ( CLOCK_REALTIME, &tmsStart );

                        for ( uint32_t n = 0 ; add < length; n++, add++ ) 
                        {
                            if ( n == 0 )
                                printf ( "%-20sRunning ", testStr );

                            d8 = read8 (add);
                            
                            if ( d8 != garbagePtr [n] ) 
                            {
                                if ( thisTestErrors < 10 )
                                {
                                    if (thisTestErrors == 0)
                                        printf ( "\n" );

                                    printf ( "%-20s%sData mismatch at $%.6X: %.02X should be %.02X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        d8, 
                                        garbagePtr [n],
                                        NORMAL );
                                }

                                thisTestErrors++;
                            }

                            if (thisTestErrors && errorStop)
                            {
                                printf ( "%-20sStopped on error\n", testStr );
                                break;
                            }

                            if ( !thisTestErrors )
                            {
                                /* sanity feedback - one dot per 64 KB */
                                if ( n % (length / 32)  == 0 ) /* print 32 dots regardless of test length */
                                {
                                    printf ( "." );
                                    fflush ( stdout );
                                }
                            }
                        }

                        errors += thisTestErrors;

                        clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                        printf ( "\n" );

                    break;

                    case TEST_16:

                        sprintf ( typeStr, "16:" );
                        sprintf ( testStr, "%s%s ", dirStr, typeStr );

                        printf ( "Test %d\n", testNumber );
                        printf ( "%-20s[WORD] Reading RAM aligned...\n", testStr );

                        add = startAdd;

                        clock_gettime ( CLOCK_REALTIME, &tmsStart );

                        for ( uint32_t n = 0; add < length - 2; n += 2, add += 2 ) 
                        {
                            if ( n == 0 )
                                printf ( "%-20sRunning ", testStr );

                            d16 = be16toh ( read16 (add) );
                            
                            if ( d16 != *( (uint16_t *) &garbagePtr [n] ) )
                            {
                                if ( thisTestErrors < 10 )
                                {
                                    if (thisTestErrors == 0)
                                        printf ( "\n" );

                                    printf ( "%-20s%sData mismatch at $%.6X: %.04X should be %.04X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        d16, 
                                        (uint16_t) garbagePtr [n + 1] << 8 | garbagePtr [n],
                                        NORMAL );
                                }

                                thisTestErrors++;
                            }

                            //errors += thisTestErrors;

                            if (thisTestErrors && errorStop)
                            {
                                printf ( "%-20sStopped on error\n", testStr );
                                break;
                            }

                            if ( !thisTestErrors )
                            {
                                if ( n % (length / 32)  == 0 ) 
                                {
                                    printf ( "." );
                                    fflush ( stdout );
                                }
                            }
                        }

                        errors += thisTestErrors;

                        clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                        printf ( "\n" );

                    break;

                    case TEST_16_ODD:

                        sprintf ( typeStr, "16_ODD:" );
                        sprintf ( testStr, "%s%s ", dirStr, typeStr );

                        printf ( "Test %d\n", testNumber );
                        printf ( "%-20s[WORD] Reading RAM unaligned...\n", testStr );

                        add = startAdd + 1;

                        clock_gettime ( CLOCK_REALTIME, &tmsStart );

                        for ( uint32_t n = 1; add < length - 2; n += 2, add += 2 ) 
                        {
                            if ( n == 1 )
                                printf ( "%-20sRunning ", testStr );

                            d16 = be16toh ( (read8 (add) << 8) | read8 ( add + 1 ) );
                            
                            if ( d16 != *( (uint16_t *) &garbagePtr [n] ) )
                            {
                                if ( thisTestErrors < 10 )
                                {
                                    if (thisTestErrors == 0)
                                        printf ( "\n" );

                                    printf ( "%-20s%sData mismatch at $%.6X: %.04X should be %.04X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        d16, 
                                        *( (uint16_t *) &garbagePtr [n] ),
                                        NORMAL );
                                }

                                thisTestErrors++;
                            }

                           // errors += thisTestErrors;

                            if (thisTestErrors && errorStop)
                            {
                                printf ( "%-20sStopped on error\n", testStr );
                                break;
                            }

                            if ( !thisTestErrors )
                            {
                                if ( (n - 1) % (length / 32)  == 0 ) 
                                {
                                    printf ( "." );
                                    fflush ( stdout );
                                }
                            }
                        }

                        errors += thisTestErrors;

                        clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                        printf ( "\n" );

                    break;

                    case TEST_32:

                        sprintf ( typeStr, "32:" );
                        sprintf ( testStr, "%s%s ", dirStr, typeStr );

                        printf ( "Test %d\n", testNumber );
                        printf ( "%-20s[LONG] Reading RAM aligned...\n", testStr );

                        add = startAdd;

                        clock_gettime ( CLOCK_REALTIME, &tmsStart );

                        for ( uint32_t n = 0; add < length - 4; n += 4, add += 4 ) 
                        {
                            if ( n == 0 )
                                printf ( "%-20sRunning ", testStr );

                            d32 = be32toh ( read32 (add) );
                            
                            if ( d32 != *( (uint32_t *) &garbagePtr [n] ) )
                            {
                                if ( thisTestErrors < 10 )
                                {
                                    if (thisTestErrors == 0)
                                        printf ( "\n" );

                                    printf ( "%-20s%sData mismatch at $%.6X: %.08X should be %.08X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        d32, 
                                        *( (uint32_t *) &garbagePtr [n] ),
                                        NORMAL );
                                }

                                thisTestErrors++;
                            }

                            //errors += thisTestErrors;

                            if (thisTestErrors && errorStop)
                            {
                                printf ( "%-20sStopped on error\n", testStr );
                                break;
                            }

                            if ( !thisTestErrors )
                            {
                                if ( n % (length / 32)  == 0 ) 
                                {
                                    printf ( "." );
                                    fflush ( stdout );
                                }
                            }
                        }

                        errors += thisTestErrors;

                        clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                        printf ( "\n" );

                    break;

                    case TEST_32_ODD:

                        sprintf ( typeStr, "32_ODD:" );
                        sprintf ( testStr, "%s%s ", dirStr, typeStr );

                        printf ( "Test %d\n", testNumber );
                        printf ( "%-20s[LONG] Reading RAM unaligned...\n", testStr );

                        add = startAdd + 1;

                        clock_gettime ( CLOCK_REALTIME, &tmsStart );

                        for ( uint32_t n = 1; add < length - 4; n += 4, add += 4 ) 
                        {
                            if ( n == 1 )
                                printf ( "%-20sRunning ", testStr );

                            d32 = read8 (add);
                            d32 |= (be16toh ( read16 ( add + 1 ) ) << 8);
                            d32 |= (read8 ( add + 3 ) << 24 );
                            
                            if ( d32 != *( (uint32_t *) &garbagePtr [n] ) )
                            {
                                if ( thisTestErrors < 10 )
                                {
                                    if (thisTestErrors == 0)
                                        printf ( "\n" );

                                    printf ( "%-20s%sData mismatch at $%.6X: %.08X should be %.08X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        d32, 
                                        *( (uint32_t *) &garbagePtr [n] ),
                                        NORMAL );
                                }

                                thisTestErrors++;
                            }

                            //errors += thisTestErrors;

                            if (thisTestErrors && errorStop)
                            {
                                printf ( "%-20sStopped on error\n", testStr );
                                break;
                            }

                            if ( !thisTestErrors )
                            {
                                if ( (n - 1) % (length / 32)  == 0 ) 
                                {
                                    printf ( "." );
                                    fflush ( stdout );
                                }
                            }
                        }

                        errors += thisTestErrors;

                        clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                        printf ( "\n" );

                    break;

                    /* random data / random addresses */
                    case TEST_8_RANDOM:

                        srand (length);

                        sprintf ( typeStr, "8_RANDOM_RW:" );
                        sprintf ( testStr, "%s%s ", dirStr, typeStr );

                        printf ( "Test %d\n", testNumber );
                        printf ( "%-20s[BYTE] Writing random data to random addresses...\n", testStr );

                        add = startAdd;

                        clock_gettime ( CLOCK_REALTIME, &tmsStart );

                        for ( uint32_t n = 0; add < length; n++, add++ ) 
                        {
                            if ( n == 0 )
                                printf ( "%-20sRunning ", testStr );

                            rd8  = (uint8_t)  ( rand () % 0xFF );

                            for ( int z = 10; z; z-- ) /* ten retries should be enough especially for small mem size */
                            {
                                radd = (uint32_t) ( rand () % length );

                                if ( radd < startAdd )
                                    continue;

                                break;
                            }

                            write8 ( radd, rd8 );
                            d8 = read8 (radd);
                            
                            if ( d8 != rd8 ) 
                            {
                                if ( thisTestErrors < 10 )
                                {
                                    if (thisTestErrors == 0)
                                        printf ( "\n" );

                                    //printf ( "\n%sData mismatch at $%.6X: %.02X should be %.02X\n", testStr, radd, d8, rd8 );
                                    printf ( "%-20s%sData mismatch at $%.6X: %.02X should be %.02X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        radd, 
                                        d8, 
                                        rd8,
                                        NORMAL );
                                }

                                thisTestErrors++;
                            }

                            //errors += thisTestErrors;

                            if (thisTestErrors && errorStop)
                            {
                                printf ( "%-20sStopped on error\n", testStr );
                                break;
                            }

                            if ( !thisTestErrors )
                            {
                                /* sanity feedback - one dot per 64 KB */
                                if ( n % (length / 32)  == 0 ) 
                                {
                                    printf ( "." );
                                    fflush ( stdout );
                                }
                            }
                        }

                        errors += thisTestErrors;

                        clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                        printf ( "\n" );

                    break;

                    case TEST_16_RANDOM:

                        srand (length);

                        sprintf ( typeStr, "16_RANDOM_RW:" );
                        sprintf ( testStr, "%s%s ", dirStr, typeStr );

                        printf ( "Test %d\n", testNumber );
                        printf ( "%-20s[WORD] Writing random data to random addresses aligned...\n", testStr );

                        clock_gettime ( CLOCK_REALTIME, &tmsStart );

                        for ( uint32_t n = 0, add = startAdd ; add < length - 2; n += 2, add += 2 ) 
                        {
                            if ( n == 0 )
                                printf ( "%-20sRunning ", testStr );

                            rd16  = (uint16_t)  ( rand () % 0xffff );

                            for ( int z = 10; z; z-- ) /* ten retries should be enough especially for small mem size */
                            {
                                radd = (uint32_t) ( rand () % length );

                                if ( radd < startAdd )
                                    continue;

                                break;
                            }

                            write16 ( radd, rd16 );
                            d16 = read16 (radd);
                            //d16 = be16toh ( read16 (add) );
                            
                            if ( d16 != rd16 ) 
                            {
                                if ( thisTestErrors < 10 )
                                {
                                    if (thisTestErrors == 0)
                                        printf ( "\n" );

                                    //printf ( "\n%sData mismatch at $%.6X: %.04X should be %.04X\n", testStr, radd, d16, rd16 );
                                    printf ( "%-20s%sData mismatch at $%.6X: %.04X should be %.04X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        radd, 
                                        d16, 
                                        rd16,
                                        NORMAL );
                                }

                                thisTestErrors++;
                            }

                            //errors += thisTestErrors;

                            if (thisTestErrors && errorStop)
                            {
                                printf ( "%-20sStopped on error\n", testStr );
                                break;
                            }

                            if ( !thisTestErrors )
                            {
                                /* sanity feedback - one dot per 64 KB */
                                if ( n % (length / 32)  == 0 ) 
                                {
                                    printf ( "." );
                                    fflush ( stdout );
                                }
                            }
                        }

                        errors += thisTestErrors;

                        clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                        printf ( "\n" );

                    break;

                    case TEST_32_RANDOM:

                        srand (length);

                        sprintf ( typeStr, "32_RANDOM_RW:" );
                        sprintf ( testStr, "%s%s ", dirStr, typeStr );

                        printf ( "Test %d\n", testNumber );
                        printf ( "%-20s[LONG] Writing random data to random addresses aligned...\n", testStr );

                        add = startAdd;

                        clock_gettime ( CLOCK_REALTIME, &tmsStart );

                        for ( uint32_t n = 0; add < length - 4; n += 4, add += 4 ) 
                        {
                            if ( n == 0 )
                                printf ( "%-20sRunning ", testStr );

                            rd32  = (uint32_t)  ( rand () % 0xffffffff );

                            for ( int z = 10; z; z-- ) /* ten retries should be enough especially for small mem size */
                            {
                                radd = (uint32_t) ( rand () % length );

                                if ( radd < startAdd )
                                    continue;

                                break;
                            }

                            write32 ( radd, rd32 );
                            d32 = read32  ( radd );
                            
                            if ( d32 != rd32 ) 
                            {
                                if ( thisTestErrors < 10 )
                                {
                                    if (thisTestErrors == 0)
                                        printf ( "\n" );

                                    //printf ( "\n%sData mismatch at $%.6X: %.08X should be %.08X\n", testStr, radd, d32, rd32 );
                                    printf ( "%-20s%sData mismatch at $%.6X: %.08X should be %.08X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        radd, 
                                        d32, 
                                        rd32,
                                        NORMAL );
                                }

                                thisTestErrors++;
                            }

                            //errors += thisTestErrors;

                            if (thisTestErrors && errorStop)
                            {
                                printf ( "%-20sStopped on error\n", testStr );
                                break;
                            }

                            if ( !thisTestErrors )
                            {
                                /* sanity feedback - one dot per 64 KB */
                                if ( n % (length / 32)  == 0 ) 
                                {
                                    printf ( "." );
                                    fflush ( stdout );
                                }
                            }
                        }

                        errors += thisTestErrors;

                        clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                        printf ( "\n" );

                    break;
                }
            

        break;

        case MEM_WRITE:

            sprintf ( dirStr, "WRITE" );

            switch (type)
            {
                case TEST_8:

                    sprintf ( typeStr, "8:" );
                    sprintf ( testStr, "%s%s ", dirStr, typeStr );

                    printf ( "Test %d\n", testNumber );
                    printf ( "%-20s[BYTE] Writing to RAM... \n", testStr );

                    clock_gettime ( CLOCK_REALTIME, &tmsStart );

                    add = startAdd;

                    for ( uint32_t n = 0; add < length; n++, add++ ) 
                    {
                        if ( n == 0 )
                            printf ( "%-20sRunning ", testStr );

                        d8 = garbagePtr [n];

                        write8 ( add, d8 );
                        rd8 = read8  ( add );
                        
                        if ( d8 != rd8 ) 
                        {
                            if ( thisTestErrors < 10 )
                            {
                                if (thisTestErrors == 0)
                                    printf ( "\n" );

                                //printf ( "\n%sData mismatch at $%.6X: %.02X should be %.02X\n", testStr, add, rd8, d8 );
                                printf ( "%-20s%sData mismatch at $%.6X: %.02X should be %.02X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        rd8, 
                                        d8,
                                        NORMAL );
                            }

                            thisTestErrors++;
                        }

                        //errors += thisTestErrors;

                        if (thisTestErrors && errorStop)
                        {
                            printf ( "%-20sStopped on error\n", testStr );
                            break;
                        }

                        if ( !thisTestErrors )
                        {
                            if ( n % (length / 32)  == 0 ) 
                            {
                                printf ( "." );
                                fflush ( stdout );
                            }
                        }
                    }

                    errors += thisTestErrors;

                    clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                    printf ( "\n" );

                break;

                case TEST_16:

                    sprintf ( typeStr, "16:" );
                    sprintf ( testStr, "%s%s ", dirStr, typeStr );

                    printf ( "Test %d\n", testNumber );
                    printf ( "%-20s[WORD] Writing to RAM aligned... \n", testStr );

                    add = startAdd;

                    clock_gettime ( CLOCK_REALTIME, &tmsStart );

                    for ( uint32_t n = 0; add < length - 2; n += 2, add += 2) 
                    {
                        if ( n == 0 )
                            printf ( "%-20sRunning ", testStr );

                        d16 = *( (uint16_t *) &garbagePtr [n] );

                        write16 ( add, d16 );
                        rd16 = read16  ( add );
                        
                        if ( d16 != rd16 ) 
                        {
                            if ( thisTestErrors < 10 )
                            {
                                if (thisTestErrors == 0)
                                    printf ( "\n" );

                                //printf ( "\n%sData mismatch at $%.6X: %.04X should be %.04X\n", testStr, add, rd16, d16 );
                                printf ( "%-20s%sData mismatch at $%.6X: %.04X should be %.04X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        rd16, 
                                        d16,
                                        NORMAL );
                            }

                            thisTestErrors++;
                        }

                        //errors += thisTestErrors;

                        if (thisTestErrors && errorStop)
                        {
                            printf ( "%-20sStopped on error\n", testStr );
                            break;
                        }

                        if ( !thisTestErrors )
                        {
                            if ( n % (length / 32)  == 0 ) 
                            {
                                printf ( "." );
                                fflush ( stdout );
                            }
                        }
                    }

                    errors += thisTestErrors;

                    clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                    printf ( "\n" );

                break;

                case TEST_16_ODD:

                    sprintf ( typeStr, "16_ODD:" );
                    sprintf ( testStr, "%s%s ", dirStr, typeStr );

                    printf ( "Test %d\n", testNumber );
                    printf ( "%-20s[WORD] Writing to RAM unaligned... \n", testStr );

                    add = startAdd + 1;

                    clock_gettime ( CLOCK_REALTIME, &tmsStart );

                    for ( uint32_t n = 1; add < length - 2; n += 2, add += 2) 
                    {
                        if ( n == 1 )
                            printf ( "%-20sRunning ", testStr );

                        d16 = *( (uint16_t *) &garbagePtr [n] );

                        write8 ( add, (d16 & 0x00FF) );
                        write8 ( add + 1, (d16 >> 8) );                      
                        rd16 = be16toh ( (read8 (add) << 8) | read8 ( add + 1 ) );

                        if ( d16 != rd16 ) 
                        {
                            if ( thisTestErrors < 10 )
                            {
                                if (thisTestErrors == 0)
                                    printf ( "\n" );

                                //printf ( "\n%sData mismatch at $%.6X: %.04X should be %.04X\n", testStr, add, rd16, d16 );
                                printf ( "%-20s%sData mismatch at $%.6X: %.04X should be %.04X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        rd16, 
                                        d16,
                                        NORMAL );
                            }

                            thisTestErrors++;
                        }

                        //errors += thisTestErrors;

                        if (thisTestErrors && errorStop)
                        {
                            printf ( "%-20sStopped on error\n", testStr );
                            break;
                        }

                        if ( !thisTestErrors )
                        {
                            if ( (n - 1) % (length / 32)  == 0 ) 
                            {
                                printf ( "." );
                                fflush ( stdout );
                            }
                        }
                    }

                    errors += thisTestErrors;

                    clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                    printf ( "\n" );

                break;

                case TEST_32:

                    sprintf ( typeStr, "32:" );
                    sprintf ( testStr, "%s%s ", dirStr, typeStr );

                    printf ( "Test %d\n", testNumber );
                    printf ( "%-20s[LONG] Writing to RAM aligned... \n", testStr );

                    add = startAdd;

                    clock_gettime ( CLOCK_REALTIME, &tmsStart );

                    for ( uint32_t n = 0; add < length - 4; n += 4, add += 4) 
                    {
                        if ( n == 0 )
                                printf ( "%-20sRunning ", testStr );

                        d32 = *( (uint32_t *) &garbagePtr [n] );

                        write32 ( add, d32 );
                        rd32 = read32 ( add );

                        if ( d32 != rd32 ) 
                        {
                            if ( thisTestErrors < 10 )
                            {
                                if (thisTestErrors == 0)
                                    printf ( "\n" );

                                //printf ( "\n%sData mismatch at $%.6X: %.08X should be %.08X\n", testStr, add, rd32, d32 );
                                printf ( "%-20s%sData mismatch at $%.6X: %.08X should be %.08X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        rd32, 
                                        d32,
                                        NORMAL );
                            }

                            thisTestErrors++;
                        }

                        //errors += thisTestErrors;

                        if (thisTestErrors && errorStop)
                        {
                            printf ( "%-20sStopped on error\n", testStr );
                            break;
                        }

                        if ( !thisTestErrors )
                        {
                            if ( n % (length / 32)  == 0 ) 
                            {
                                printf ( "." );
                                fflush ( stdout );
                            }
                        }
                    }

                    errors += thisTestErrors;

                    clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                    printf ( "\n" );

                break;

                case TEST_32_ODD:

                    sprintf ( typeStr, "32_ODD:" );
                    sprintf ( testStr, "%s%s ", dirStr, typeStr );

                    printf ( "Test %d\n", testNumber );
                    printf ( "%-20s[LONG] Writing to RAM unaligned... \n", testStr );

                    add = startAdd + 1;

                    clock_gettime ( CLOCK_REALTIME, &tmsStart );

                    for ( uint32_t n = 1; add < length - 4; n += 4, add += 4) 
                    {
                        if ( n == 1 )
                            printf ( "%-20sRunning ", testStr );

                        d32 = *( (uint32_t *) &garbagePtr [n] );

                        write8  ( add, (d32 & 0x0000FF) );
                        write16 ( add + 1, htobe16 ( ( (d32 & 0x00FFFF00) >> 8) ) );
                        write8  ( add + 3, (d32 & 0xFF000000) >> 24);

                        rd32  = read8 (add);
                        rd32 |= (be16toh ( read16 ( add + 1 ) ) << 8);
                        rd32 |= (read8 ( add + 3 ) << 24 );

                        if ( d32 != rd32 ) 
                        {
                            if ( thisTestErrors < 10 )
                            {
                                if (thisTestErrors == 0)
                                    printf ( "\n" );

                                //printf ( "\n%sData mismatch at $%.6X: %.08X should be %.08X\n", testStr, add, rd32, d32 );
                                printf ( "%-20s%sData mismatch at $%.6X: %.08X should be %.08X%s\n", 
                                        testStr,
                                        REVERSE_VIDEO, 
                                        add, 
                                        rd32, 
                                        d32,
                                        NORMAL );
                            }

                            thisTestErrors++;
                        }

                        //errors += thisTestErrors;

                        if (thisTestErrors && errorStop)
                        {
                            printf ( "%-20sStopped on error\n", testStr );
                            break;
                        }

                        if ( !thisTestErrors )
                        {
                            if ( (n - 1) % (length / 32)  == 0 ) 
                            {
                                printf ( "." );
                                fflush ( stdout );
                            }
                        }
                    }

                    errors += thisTestErrors;

                    clock_gettime ( CLOCK_REALTIME, &tmsEnd );

                    printf ( "\n" );

                break;
            }
                
        break;
    }

    if ( errors && errorStop )
    {
        return 0;
    }

    nanoStart = (tmsStart.tv_sec * 1000) + (tmsStart.tv_nsec / 1000000);
    nanoEnd   = (tmsEnd.tv_sec * 1000) + (tmsEnd.tv_nsec / 1000000);

    uint32_t calcLength = (length - startAdd);

    /* recalculate data transfer size as the MEM_WRITES have a read and write component */
    if ( direction == MEM_WRITE ) //&& (type == TEST_8 || type == TEST_8_ODD || type == TEST_8_RANDOM) )
    {
        calcLength *= 2;
    }

    else if ( direction == MEM_READ && (type == TEST_8_RANDOM || type == TEST_16_RANDOM || type == TEST_32_RANDOM) )
    {
        calcLength *= 2;
    }

    printf ( "%-20sTest %d Completed with %d %s in %d ms (%.2f MB/s)\n", 
        testStr, 
        testNumber,
        thisTestErrors,
        thisTestErrors == 1 ? "error" : "errors",
        (nanoEnd - nanoStart), 
       // (( (float)calcLength / (float)(nanoEnd - nanoStart)) * 1000.0) / 1024 );     /* KB/s */
         ( 1.0 / ( (float)(nanoEnd - nanoStart) ) * calcLength ) / 1024 );     /* MB/s */

        
    
    //if ( thisTestErrors )
    //    printf ( "%-20s%sTest errors = %08d%s\n\n", 
    //        testStr,
    //        thisTestErrors ? REVERSE_VIDEO : "",
    //        thisTestErrors,
    //        thisTestErrors ? NORMAL : "" );

    printf ( "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n\n" );

    testNumber++;
    totalErrors += errors;

    return 1;
}


/* no checks performed - raw performance reported */
void clearmem ( uint32_t length, uint32_t *duration, uint16_t pattern, int8_t loopYN )
{
    struct timespec tmsStart, tmsEnd;
    uint32_t loop = 1;

    if ( loopYN )
        loop = 0xffffffff; 
        
    clock_gettime ( CLOCK_REALTIME, &tmsStart );
    
    while ( loop-- )
    {
        for ( uint32_t n = 8; n < length; n += 2 ) {

            write16 ( n, pattern );

            if ( n % (length / 64)  == 0 ) 
            {
                printf ( "." );
                fflush ( stdout );
            }
        }
    }

    clock_gettime ( CLOCK_REALTIME, &tmsEnd );

    long int nanoStart = (tmsStart.tv_sec * 1000) + (tmsStart.tv_nsec / 1000000);
    long int nanoEnd = (tmsEnd.tv_sec * 1000) + (tmsEnd.tv_nsec / 1000000);

    *duration = (nanoEnd - nanoStart);
}


void dump ( uint32_t ROMsize, uint32_t ROMaddress )
{
    uint8_t  in;
    FILE *out = fopen ("tos.rom", "wb+" );

    if ( out == NULL ) 
    {
        printf ("Failed to open tos.rom for writing.\nTOS has not been dumped.\n");

        return;
    }

    ROMsize = ROMsize * SIZE_KILO;

    for ( int i = 0; i < ROMsize; i++ )
    {
        in = read8 ( (ROMaddress + i) ) ;

        fputc ( in, out );
    }

    fclose (out);
}


/* command line parser */
int parser ( int argc, char **argv )
{
    int valid = 0;
    char commandLine [80];
    char *ptr;
    int syntax = 0;
    char arguments [80];
    char *tptr, *aptr;
    char *cmdptr, *argptr;
    char cmd [80];
    char arg [80];
    char *savePtr, *cmdSave, *strSave;
    char *substring;// [80];
    char *cmdLine;
    char *cmdstr;

    /* rebuild command line */
    cmdLine = calloc ( 80, 1 );
    for ( int a = 1; a < argc; a++ )
    {
        strcat ( cmdLine, argv [a] );
        strcat ( cmdLine, " " );
    }

    cmdstr = strtok_r ( cmdLine, "--", &cmdSave );

    /* loop for all commands */
    for ( int a = 1; cmdstr != NULL; cmdstr = strtok_r ( NULL, "--", &cmdSave ), a++ )
    {        
        //printf ("cmdstr = %s\n", cmdstr );

        cmdptr = strtok_r ( cmdstr, " ", &strSave );
        //printf ( "cmdptr = %s\n", cmdptr );

        if ( strcmp ( cmdptr, "v" ) == 0 )
        {        
            VERBOSE = 1;
            valid = 1; 
        }
      
        if ( strcmp ( cmdptr, "memory" ) == 0 )
        {        
            valid = 1;
            substring = strtok_r ( NULL, " ", &strSave );
            
            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );

                if ( strcmp ( aptr, "tests" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );

                    if ( *tptr == 'r' || *(tptr + 1) == 'r' || *(tptr + 2) == 'r'  )
                        doReads = 1;

                    if ( *tptr == 'w' || *(tptr + 1) == 'w' || *(tptr + 2) == 'w'  )
                        doWrites = 1;

                    if ( *tptr == 'x' || *(tptr + 1) == 'x' || *(tptr + 2) == 'x'  )
                        doRandoms = 1;
                }

                else if ( strcmp ( aptr, "size" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    testSize = atoi (tptr);

                    if ( testSize < 512 )
                        testSize = 512;

                    if ( testSize > 4096 )
                        testSize = 4096;
                }

                else if ( strcmp ( aptr, "loop" ) == 0 )
                {   
                    tptr = strtok ( NULL, "" );

                    if ( strcmp ( tptr, "yes" ) == 0 )
                        loopTests = 1;
                }

                else if ( strcmp ( aptr, "stop" ) == 0 )
                {   
                    tptr = strtok ( NULL, "" );

                    if ( strcmp ( tptr, "yes" ) == 0 )
                        errorStop = 1;
                }

                else
                    valid = 0;

                argptr = strtok ( savePtr, " " );
            }

            if ( valid )
                cmdMem = 1;
        }

        if ( strcmp ( cmdptr, "clearmem" ) == 0 )
        {
            valid = 1; 
            substring = strtok_r ( NULL, " ", &strSave );
            
            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );
        
                if ( strcmp ( aptr, "size" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    testSize = atoi ( tptr );

                    if ( testSize < 512 )
                        testSize = 512;

                    if ( testSize > 4096 )
                        testSize = 4096;
                }

                else if ( strcmp ( aptr, "pattern" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    sscanf ( tptr, "%x", &clrPattern );
                } 

                else if ( strcmp ( aptr, "loop" ) == 0 )
                {   
                    tptr = strtok ( NULL, "" );

                    if ( strcmp ( tptr, "yes" ) == 0 )
                        loopTests = 1;
                }

                else
                    valid = 0;
            }

            if ( valid )
            {
                cmdClear = 1; 
            }   
        }

        if ( strcmp ( cmdptr, "mfp" ) == 0 )
        {
            valid = 1;
            cmdMfp = 1;
            substring = strtok_r ( NULL, " ", &strSave );

            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );

                if ( strcmp ( aptr, "count" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    mfpCount = (uint32_t)atoi ( tptr );
                    if ( mfpCount < 1000 ) mfpCount = 1000;
                }

                else if ( strcmp ( aptr, "irq" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    mfpIrq = ( strcmp ( tptr, "yes" ) == 0 );
                }

                else if ( strcmp ( aptr, "iack" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    mfpIack = ( strcmp ( tptr, "yes" ) == 0 );
                    if ( mfpIack ) mfpIrq = 1;
                }

                else
                    valid = 0;
            }
        }

        if ( strcmp ( cmdptr, "peek" ) == 0 )
        {
            valid = 1;
            substring = strtok_r ( NULL, " ", &strSave );
            
            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );
      
                if ( strcmp ( aptr, "address" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    sscanf ( tptr, "%x", &padd );
                }

                else if ( strcmp ( aptr, "loop" ) == 0 )
                {   
                    tptr = strtok ( NULL, "" );

                    if ( strcmp ( tptr, "yes" ) == 0 )
                        loopTests = 1;
                }

                else
                    valid = 0;

                argptr = strtok ( savePtr, " " );
            }

            if ( valid )
            {
                cmdPeek = 1;
            }
        }

        if ( strcmp ( cmdptr, "poke" ) == 0 )
        {
            valid = 1; 
            substring = strtok_r ( NULL, " ", &strSave );
            
            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );
        
                if ( strcmp ( aptr, "address" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    sscanf ( tptr, "%x", &padd );
                }

                else if ( strcmp ( aptr, "data" ) == 0 )
                {   
                    tptr = strtok ( NULL, "" );
                    sscanf ( tptr, "%x", &pdata );
                }

                else
                    valid = 0;

                argptr = strtok ( savePtr, " " );
            }

            if ( valid )
            {
                cmdPoke = 1;
            }
        }

        //  syntax --dumprom size=256 address=0xe00000 
        if ( strcmp ( cmdptr, "dumprom" ) == 0 )
        {
            valid = 1; 
            substring = strtok_r ( NULL, " ", &strSave );
            
            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );
        
                if ( strcmp ( aptr, "address" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    sscanf ( tptr, "%x", &ROMaddress );
                }

                else if ( strcmp ( aptr, "size" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    ROMsize = atoi (tptr);

                    if ( ROMsize > 256 || ROMsize < 192 )
                        ROMsize = 192;
                }

                else
                    valid = 0;

                argptr = strtok ( savePtr, " " );
            }

            if ( valid )
                cmdDump = 1;
        }

        if ( strcmp ( cmdptr, "init" ) == 0 )
        {
            valid = 1; 
            substring = strtok_r ( NULL, " ", &strSave );
            
            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );

                if ( strcmp ( aptr, "size" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    memSize = atoi ( tptr );

                    if ( memSize < 512 )
                        memSize = 512;

                    if ( memSize > 4096 )
                        memSize = 4096;
                }

                else
                    valid = 0;

                argptr = strtok ( savePtr, " " );
            }

            if ( valid )
                cmdInit = 1;
        }

        if ( strcmp ( cmdptr, "busprofile" ) == 0 )
        {
            valid = 1;
            cmdBusProfile = 1;
        }

        if ( strcmp ( cmdptr, "memspeed" ) == 0 )
        {
            valid = 1; 
            substring = strtok_r ( NULL, " ", &strSave );
            
            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );
        
                if ( strcmp ( aptr, "size" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    testSize = atoi ( tptr );

                    if ( testSize < 512 )
                        testSize = 512;

                    if ( testSize > 4096 )
                        testSize = 4096;
                } 

                else
                    valid = 0;

                argptr = strtok ( savePtr, " " );
            }

            if ( valid )
                cmdMemSpeed = 1;
        }
    /*
        if ( strcmp ( cmdptr, "clock" ) == 0 )
        {
            //valid = 1;
            char *p;
            //printf ( "argv = %s\n", argv[a+1] );
            targetF = strtol ( argv [a+1], &p, 10 );
            //printf ( "targetF = %d\n", targetF );
        }
    */
        if ( strcmp ( cmdptr, "hardware" ) == 0 )
        {
            cmdHWTEST = 1;
            valid = 1;
        }

        if ( strcmp ( cmdptr, "reset" ) == 0 )
        {
            cmdRESET = 1;
            valid = 1;
        }

        if ( strcmp ( cmdptr, "halt" ) == 0 )
        {
            cmdHALT = 1;
            valid = 1;
        }

        if ( strcmp ( cmdptr, "arbtest" ) == 0 )
        {
            valid = 1;
            substring = strtok_r ( NULL, " ", &strSave );

            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );

                if ( strcmp ( aptr, "seconds" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    arbSeconds = atoi ( tptr );
                    if ( arbSeconds < 1 )
                        arbSeconds = 1;
                }

                else if ( strcmp ( aptr, "hammer" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "yes" ) == 0 )
                        arbHammer = 1;
                }

                else if ( strcmp ( aptr, "address" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    sscanf ( tptr, "%x", &arbAddress );
                }

                else if ( strcmp ( aptr, "source" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "blitter" ) == 0 )
                        arbSource = ARB_SOURCE_BLITTER;
                    else if ( strcmp ( tptr, "none" ) == 0 )
                        arbSource = ARB_SOURCE_NONE;
                    else
                        valid = 0;
                }

                else if ( strcmp ( aptr, "op" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "fill" ) == 0 )
                        arbBlitOp = ARB_BLIT_FILL;
                    else if ( strcmp ( tptr, "copy" ) == 0 )
                        arbBlitOp = ARB_BLIT_COPY;
                    else
                        valid = 0;
                }

                else if ( strcmp ( aptr, "blitms" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    arbBlitEveryMs = (uint32_t)atoi ( tptr );
                    if ( arbBlitEveryMs < 1 )
                        arbBlitEveryMs = 1;
                }

                else if ( strcmp ( aptr, "quietms" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    arbBlitQuietMs = (uint32_t)atoi ( tptr );
                    if ( arbBlitQuietMs < 1 )
                        arbBlitQuietMs = 1;
                }

                else
                    valid = 0;
            }

            if ( valid )
                cmdArbTest = 1;
        }

        if ( strcmp ( cmdptr, "dev" ) == 0 )
        {
            valid = 1; 
            substring = strtok_r ( NULL, " ", &strSave );
            
            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );
    
                if ( strcmp ( aptr, "address" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    sscanf ( tptr, "%x", &padd );
                }

                else if ( strcmp ( aptr, "read" ) == 0 )
                {   
                    rwtest = 1;
                }

                else if ( strcmp ( aptr, "write" ) == 0 )
                {   
                    rwtest = 0;
                }

                else
                    valid = 0;

                argptr = strtok ( savePtr, " " );
            }

            if ( valid )
            {
                cmdDevTest = 1;
            }
        }
    }

    return valid | syntax;
}


void atariReset ( void )
{
    ps_pulse_reset ();
    ps_reset_state_machine ();
}


void atariHalt ( void )
{
    ps_pulse_halt ();
    ps_reset_state_machine ();
}

#define ARB_STATUS_BUSY_RAW  (1u << 19)
#define BLITTER_BASE        0x00FF8A00u
#define BLITTER_SRC         0x00020000u
#define BLITTER_DST         0x00030000u
#define BLITTER_XWORDS      160u
#define BLITTER_LINES       200u
#define BLITTER_SAMPLE_WORDS 16u

static uint32_t read32_split ( uint32_t address )
{
    return ( (uint32_t)read16 ( address ) << 16 ) | read16 ( address + 2 );
}

static void write32_split ( uint32_t address, uint32_t value )
{
    write16 ( address, (uint16_t)( value >> 16 ) );
    write16 ( address + 2, (uint16_t)value );
}

static uint64_t monotonic_ms ( void )
{
    struct timespec ts;

    clock_gettime ( CLOCK_MONOTONIC, &ts );

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int arb_blitter_busy ( void )
{
    g_buserr = 0;
    return ( read8 ( BLITTER_BASE + 0x3c ) & 0x80 ) && !g_buserr;
}

static uint32_t arb_blitter_dst_sample ( void )
{
    uint32_t sum = 0;

    g_buserr = 0;

    for ( uint32_t i = 0; i < BLITTER_SAMPLE_WORDS; i++ )
        sum = ( sum << 1 ) ^ read16 ( BLITTER_DST + i * 2u );

    return sum;
}

static void arb_blitter_dump_once ( void )
{
    static int dumped = 0;

    if ( dumped )
        return;

    dumped = 1;
    g_buserr = 0;

    printf ( "[BLTREG] ctrl8=%02X ctrl16=%04X src=%06X dst=%06X x=%04X y=%04X hop=%02X lop=%02X skew=%02X berr=%u\n",
             read8 ( BLITTER_BASE + 0x3c ),
             read16 ( BLITTER_BASE + 0x3c ),
             read32_split ( BLITTER_BASE + 0x24 ) & 0x00ffffffu,
             read32_split ( BLITTER_BASE + 0x32 ) & 0x00ffffffu,
             read16 ( BLITTER_BASE + 0x36 ),
             read16 ( BLITTER_BASE + 0x38 ),
             read8 ( BLITTER_BASE + 0x3a ),
             read8 ( BLITTER_BASE + 0x3b ),
             read8 ( BLITTER_BASE + 0x3d ),
             g_buserr );
}

static int arb_start_blitter ( void )
{
    static int prepared = 0;
    static uint16_t fill = 0x1111;
    uint32_t words = BLITTER_XWORDS * BLITTER_LINES;
    uint16_t yinc = (uint16_t)( BLITTER_XWORDS * 2u );

    if ( !prepared )
    {
        for ( uint32_t i = 0; i < words; i++ )
        {
            write16 ( BLITTER_SRC + i * 2u, (uint16_t)( 0x4000u + i ) );
            write16 ( BLITTER_DST + i * 2u, 0 );
        }
        prepared = 1;
    }

    g_buserr = 0;
    write16 ( BLITTER_BASE + 0x3c, 0x0000 );  /* clear stale control/busy */

    for ( uint32_t i = 0; i < 16; i++ )
        write16 ( BLITTER_BASE + i * 2u, fill );

    if ( arbBlitOp == ARB_BLIT_COPY )
    {
        for ( uint32_t i = 0; i < BLITTER_SAMPLE_WORDS; i++ )
            write16 ( BLITTER_SRC + i * 2u, fill + (uint16_t)i );
    }

    for ( uint32_t i = 0; i < BLITTER_SAMPLE_WORDS; i++ )
        write16 ( BLITTER_DST + i * 2u, 0 );

    write16 ( BLITTER_BASE + 0x20, 2 );       /* source X increment */
    write16 ( BLITTER_BASE + 0x22, yinc );    /* source Y increment */
    write32_split ( BLITTER_BASE + 0x24, BLITTER_SRC );
    write16 ( BLITTER_BASE + 0x28, 0xffff );
    write16 ( BLITTER_BASE + 0x2a, 0xffff );
    write16 ( BLITTER_BASE + 0x2c, 0xffff );
    write16 ( BLITTER_BASE + 0x2e, 2 );       /* destination X increment */
    write16 ( BLITTER_BASE + 0x30, yinc );    /* destination Y increment */
    write32_split ( BLITTER_BASE + 0x32, BLITTER_DST );
    write16 ( BLITTER_BASE + 0x36, BLITTER_XWORDS );
    write16 ( BLITTER_BASE + 0x38, BLITTER_LINES );
    write8  ( BLITTER_BASE + 0x3a, arbBlitOp == ARB_BLIT_COPY ? 2 : 1 );
    write8  ( BLITTER_BASE + 0x3b, 3 );       /* LOP: replace destination */
    write8  ( BLITTER_BASE + 0x3d, 0 );       /* skew */

    arb_blitter_dump_once ();
    write16 ( BLITTER_BASE + 0x3c, 0xc000 );  /* start, hog mode */
    fill += 0x1111;

    return g_buserr ? 0 : 1;
}

void arbTest ( void )
{
    uint8_t saved_fc = fc;
    uint64_t start = monotonic_ms ();
    uint64_t next = start + 1000;
    uint64_t end = start + (uint64_t)arbSeconds * 1000ULL;
    uint64_t next_blit = start;
    uint64_t quiet_until = 0;
    uint64_t blit_start = 0;
    uint64_t samples = 0;
    uint64_t arb_seen = 0;
    uint64_t transitions = 0;
    uint64_t source_inflight = 0;
    uint64_t source_triggers = 0;
    uint64_t source_started_busy = 0;
    uint64_t source_changed = 0;
    uint64_t blit_done = 0;
    uint64_t blit_total_ms = 0;
    uint64_t blit_max_ms = 0;
    uint64_t sec_arb_seen = 0;
    uint64_t sec_samples = 0;
    uint64_t sec_transitions = 0;
    uint64_t sec_source_inflight = 0;
    uint64_t sec_source_triggers = 0;
    uint64_t sec_source_started_busy = 0;
    uint64_t sec_source_changed = 0;
    uint64_t sec_blit_done = 0;
    uint64_t sec_blit_total_ms = 0;
    uint64_t sec_blit_max_ms = 0;
    uint64_t hammer_ops = 0;
    uint64_t hammer_berr = 0;
    uint64_t sec_hammer_ops = 0;
    uint64_t sec_hammer_berr = 0;
    uint32_t last_busy = 0xffffffffu;
    uint32_t blit_before = 0;
    uint32_t blit_last = 0;
    int blit_inflight = 0;
    uint16_t pattern = 0x1234;

    fc = 5; /* Supervisor data space for RAM and MMIO cycles. */

    printf ( "\nATARITEST arbitration monitor\n" );
    printf ( "seconds=%d hammer=%s address=$%06X source=%s op=%s blitms=%u quietms=%u\n",
             arbSeconds, arbHammer ? "yes" : "no", arbAddress,
             arbSource == ARB_SOURCE_BLITTER ? "blitter" : "none",
             arbBlitOp == ARB_BLIT_COPY ? "copy" : "fill",
             arbBlitEveryMs, arbBlitQuietMs );
    printf ( "Arbitration status: raw19=BGACK. PISTORM_ARB_WAIT=force enables slow diagnostic pre-cycle polling.\n\n" );

    while ( monotonic_ms () < end )
    {
        uint64_t now = monotonic_ms ();
        uint32_t status = ps_read_status_reg ();
        uint32_t is_arb = ( status & ARB_STATUS_BUSY_RAW ) ? 1u : 0u;
        int blitter_busy = 0;

        samples++;
        sec_samples++;

        if ( is_arb )
        {
            arb_seen++;
            sec_arb_seen++;
        }

        if ( last_busy != 0xffffffffu && last_busy != is_arb )
        {
            transitions++;
            sec_transitions++;
        }

        last_busy = is_arb;

        if ( arbSource == ARB_SOURCE_BLITTER )
        {
            if ( blit_inflight )
            {
                source_inflight++;
                sec_source_inflight++;

                if ( now >= quiet_until )
                {
                    uint32_t blit_after = arb_blitter_dst_sample ();
                    blitter_busy = arb_blitter_busy ();

                    if ( blit_after != blit_last )
                    {
                        source_changed++;
                        sec_source_changed++;
                        blit_last = blit_after;
                    }

                    if ( blitter_busy )
                    {
                        quiet_until = now + arbBlitQuietMs;
                    }
                    else
                    {
                        uint64_t elapsed = now - blit_start;

                        blit_done++;
                        blit_total_ms += elapsed;
                        if ( elapsed > blit_max_ms )
                            blit_max_ms = elapsed;

                        sec_blit_done++;
                        sec_blit_total_ms += elapsed;
                        if ( elapsed > sec_blit_max_ms )
                            sec_blit_max_ms = elapsed;

                        blit_inflight = 0;
                        next_blit = now + arbBlitEveryMs;
                    }
                }
            }

            if ( !blit_inflight && now >= next_blit )
            {
                blit_before = arb_blitter_dst_sample ();
                blit_last = blit_before;
                if ( arb_start_blitter () )
                {
                    source_triggers++;
                    sec_source_triggers++;
                    source_started_busy++;
                    sec_source_started_busy++;
                    blit_inflight = 1;
                    blit_start = now;
                    quiet_until = now + arbBlitQuietMs;
                }
            }
        }

        if ( arbHammer && !blit_inflight )
        {
            g_buserr = 0;
            write16 ( arbAddress, pattern );
            if ( g_buserr )
            {
                hammer_berr++;
                sec_hammer_berr++;
                g_buserr = 0;
            }

            g_buserr = 0;
            (void)read16 ( arbAddress );
            if ( g_buserr )
            {
                hammer_berr++;
                sec_hammer_berr++;
                g_buserr = 0;
            }

            pattern += 0x1111;
            hammer_ops += 2;
            sec_hammer_ops += 2;
        }

        if ( monotonic_ms () >= next )
        {
            printf ( "[ARB/s] samples=%llu arb=%llu arb_trans=%llu inflight=%llu srctrigger=%llu startok=%llu dstchange=%llu blitdone=%llu blitavg=%llums blitmax=%llums hammer=%llu berr=%llu\n",
                     (unsigned long long)sec_samples,
                     (unsigned long long)sec_arb_seen,
                     (unsigned long long)sec_transitions,
                     (unsigned long long)sec_source_inflight,
                     (unsigned long long)sec_source_triggers,
                     (unsigned long long)sec_source_started_busy,
                     (unsigned long long)sec_source_changed,
                     (unsigned long long)sec_blit_done,
                     (unsigned long long)(sec_blit_done ? sec_blit_total_ms / sec_blit_done : 0),
                     (unsigned long long)sec_blit_max_ms,
                     (unsigned long long)sec_hammer_ops,
                     (unsigned long long)sec_hammer_berr );

            sec_samples = 0;
            sec_arb_seen = 0;
            sec_transitions = 0;
            sec_source_inflight = 0;
            sec_source_triggers = 0;
            sec_source_started_busy = 0;
            sec_source_changed = 0;
            sec_blit_done = 0;
            sec_blit_total_ms = 0;
            sec_blit_max_ms = 0;
            sec_hammer_ops = 0;
            sec_hammer_berr = 0;
            next += 1000;
        }
    }

    printf ( "\n[ARB] total samples=%llu arb=%llu arb_trans=%llu inflight=%llu srctrigger=%llu startok=%llu dstchange=%llu blitdone=%llu blitavg=%llums blitmax=%llums hammer=%llu berr=%llu\n",
             (unsigned long long)samples,
             (unsigned long long)arb_seen,
             (unsigned long long)transitions,
             (unsigned long long)source_inflight,
             (unsigned long long)source_triggers,
             (unsigned long long)source_started_busy,
             (unsigned long long)source_changed,
             (unsigned long long)blit_done,
             (unsigned long long)(blit_done ? blit_total_ms / blit_done : 0),
             (unsigned long long)blit_max_ms,
             (unsigned long long)hammer_ops,
             (unsigned long long)hammer_berr );

    fc = saved_fc;
}


#define SIZE_KILO 1024
#define SIZE_MEGA (1024 * 1024)
#define SIZE_GIGA (1024 * 1024 * 1024)

//uint8_t garbege_datas[4 * SIZE_MEGA];

void hwTest ( void )
{
    int j;
    uint16_t tmp;
    uint32_t test_size = 512 * SIZE_KILO, cur_loop = 0;
    uint8_t loop_tests = 0, total_errors = 0;
    int errors = 0;

    test_size = 512 * SIZE_KILO;
            
    garbege_datas = malloc ( test_size );

    if ( !garbege_datas )
    {
        printf ( "Failed to allocate memory for garbege datas\n" );

        return;
    }

    printf ( "\nTesting PiStorm Hardware\n" );


test_loop:

    printf ( "Data bus short circuit test - D0-D15" );

    uint16_t d;

    for ( uint32_t db = 0; db < 16; db++ )
    {
        write16 ( 0x10, (1 << db) );

        for ( uint32_t s = 0; s < 16; s++ )
        {
            if ( s == db )
                continue;

            if ( ( d = read16 ( 0x10 ) ) != (1 << db) )
            {
                printf ( "\ndata bus short - wrote 0x%X, read 0x%X, between data lines %d and %d", (1 << db), d, db, s );
                errors++;
            }
        }

        write16 ( 0x10, 0x00 );
    }

    if ( !errors )
        printf ( "... ok\n" );

    else
        printf ( "\n" );

    printf ( "Address bus short circuit test - A1-A21" );

    //uint8_t d;
    errors = 0;

    for ( uint32_t a = 1; a < 22; a++ )
    {
        write8 ( 0x10 + (1 << a), a );

        for ( uint32_t s = 2; s < 22; s++ )
        {
            if ( s == a )
                continue;

            if ( ( d = read8 ( 0x10 + (1 << s) ) ) == a )
            {
                printf ( "\naddress bus short at 0x%X and 0x%X - read 0x%X, between address lines A%d and A%d", 
                    0x10 + (1 << a), 0x10 + (1 << s), d, a, s );

                errors++;
            }
        }

        write8 ( 0x10 + (1 << a), 0x00 );
    }

    if ( !errors )
        printf ( "... ok\n" );

    else
        printf ( "\n" );


    printf ( "\nAddress bus test (24 bit)\n" );

    for ( uint32_t j = 0; j < 24; ++j )
    {
      uint32_t ja = 1 << j;
      
      printf ( "address bit: $%.6X... ",ja );

      for (uint32_t i = 0; i < test_size; i++) 
	  {
		  //write 512k writes on each address pin (A1-23)
          write16 ( ja, 0xFFFF );
      }
      printf ( "ok\n" );
    }
    

    
	printf ( "\nData bus test (write)\n" );

	j = 0;
    for ( ; j < 16; ++j )
    {
      printf ( "write16: bit %.4X... ", 1 << j );

      for ( uint32_t i = 0; i < test_size; i++ ) 
	  {
        while(garbege_datas[i] == 0x00)
        {
            garbege_datas[i] = (uint8_t)(rand() % 0xFF);
        }
            
        write16 ( 1 << j, 1 << j );
      }

      printf ( "ok\n" );
    }
    

/*
	//printf("And back down... \n");
	for (j=15;j>=0;--j)
    {
      printf("write16: data = %.4X... ", 1 << j);
      for (uint32_t i = 0; i < test_size; i++) 
	  {
          while(garbege_datas[i] == 0x00)
		  {
              garbege_datas[i] = (uint8_t)(rand() % 0xFF);
          }

		  write32(i, (uint16_t)(1 << j));
      }
      printf ( "ok\n");
    }
*/

	//printf ( "\nThe following test only works on non-A variant flip-flops (373 or 374's not 373A or 374A\n" );
	printf ( "\nData bus test (read/write)\n" );

    for ( j = 0; j < 16; ++j )
    {
	  tmp = 1 << j;

      printf ( "read16/write16: bit %.4X... ", tmp );

      write16 ( j + 0x600, tmp );
	  //sleep(1);
      uint16_t c = read16 ( j + 0x600 );
      
      if (c != tmp) 
	  {
          printf("READ16: write/read data mismatch: read %.2X should be %.2X.\n",  c, tmp);
          errors++;
      }
      printf ( "ok\n");
    }
/*
    printf ( "\nData bus test (read/write)\n" );

	for (j=15;j>=0;--j)
    {
	  uint16_t tmp = 1 << j;
      printf("write and read back data bus: bit %.4X... ", tmp );
      write16(j+1, tmp);
	  //sleep(1);
      uint16_t c = read16(j+1);
      if (c != tmp) 
	  {
          printf("failed read %.2X should be %.2X.\n",  c, tmp);
          errors++;
      }
      else
      {
        printf ( "ok\n");
      }
    }
*/

    printf ( "\nHardware total errors: %d\n", errors );

    total_errors += errors;
    errors = 0;
    sleep (1);

    if (loop_tests) {
        printf ("Loop %d done. Begin loop %d.\n", cur_loop + 1, cur_loop + 2);
        printf ("Current total errors: %d.\n", total_errors);
        goto test_loop;
    }

    return;
}


extern void ps_diag (void);
extern void ps_diag3 (void);
extern void ps_diag4 (void);
extern volatile uint32_t *ioread;

void devTest ( int rw )
{
    uint16_t d;

    printf ( "\nATARITEST - DEV\n%s address 0x%08X\n", rw ? "READ looping" : "WRITE looping", padd );

    //ps_reset_state_machine ();

    if ( rw == 1 )
    {
        //ps_diag ();
       // ps_diag3 ();
      //  ps_diag4 ();
#if (1)
        int status;
       
        while ( 1 )
        {
            status = ps_read_status_reg () & 0x00FFFFFF;
            //if ((status & 0x040000))
            //    printf ("OOR\n");
            printf ( "STATUS 0x%08X\n", status  );
          
        }
    }

    else
    {
        fc = 6;
        // Write a known pattern to 8 consecutive addresses
        for (int i = 0; i < 8; i++) {
            ps_write_8(0xf0 + i, i);  // write value = address offset
        }
        // Read them back  
        for (int i = 0; i < 8; i++) {
            uint8_t val = ps_read_8(0xf0 + i);
            printf("addr 0x%05x: wrote %d, read %d\n", 0x10000 + i, i, val);
        }

        uint32_t raw = *ioread;
        printf("After REG_ADDR_HI: GPLEV0=0x%08x, op_rw(PI_D[9])=%d\n", 
            raw, (raw>>17)&1);

        
    }
#endif
    
}
