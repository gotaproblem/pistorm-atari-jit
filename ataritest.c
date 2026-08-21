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
void poke ( uint32_t address, uint8_t data );
void dump ( uint32_t ROMsize, uint32_t ROMaddress );
void memspeed ( uint32_t length );
void hwTest ( void );
void devTest ( int rw );
void atariReset ( void );
void atariHalt ( void );
void arbTest ( void );
void acsiTest ( void );
void fdcTest ( void );
void p2diag ( void );

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
int cmdPoke = 0;
int cmdMem = 0;
int cmdDump = 0;
int cmdClear = 0;
int cmdInit = 0;
int cmdMemSpeed = 0;
int targetF = 200;
int cmdHWTEST = 0;
int cmdDevTest = 0;
int rwtest = 0;
int cmdRESET = 0;
int cmdHALT = 0;
int cmdArbTest = 0;
int arbSeconds = 5;

/* --acsitest : standalone real-ACSI DMA loop, no TOS in the picture.
 * Every ACSI result to date came from a single transfer inside one TOS boot,
 * where the timing differs every run and nothing can be repeated. This runs
 * the same register sequence the boot trace shows, N times, and counts
 * outcomes. */
int cmdAcsiTest  = 0;
int acsiLoops    = 100;
int acsiDev      = 0;          /* ACSI target 0-7                        */
uint32_t acsiLba = 0;          /* logical block to read                  */
uint32_t acsiBase = 0x001000u; /* ST-RAM destination (even, < memory)    */
int acsiSectors  = 1;          /* sectors per transfer                   */
int acsiSettleUs = 0;          /* optional delay before the status read  */
int acsiQuiet    = 0;          /* 1 = summary only, no per-loop lines    */
int acsiPollUs   = 100;        /* gap between GPIP polls - see acsiTest  */
int acsiUseIrq   = 1;          /* 0 = skip the IRQ wait, just delay      */
int acsiWaitMs   = 50;         /* fixed wait when irq=no                 */
int acsiGapUs    = 0;          /* delay after each register write        */
int acsiScan     = 0;
int acsiBare = 0;          /* bare=yes: preamble EXACTLY as the working scan */          /* 1 = probe targets 0-7 and stop         */
int acsiFc       = 5;          /* 68000 function code on FC0-2           */

/* --fdctest : does the WD1772 completion interrupt reach MFP GPIP5?
 * EmuTOS's floppy driver issues a command then waits on that bit. In the
 * boot trace it issues FORCE INTERRUPT, then RESTORE, reads status once,
 * sees BUSY - which is correct that soon after a command - and never looks
 * at the floppy again. So either the interrupt never arrives, or it is
 * already asserted and the wait falls straight through. This measures
 * which, with no TOS in the way. */
int cmdFdcTest   = 0;
int cmdP2diag    = 0;
int fdcLoops     = 5;
int fdcDrive     = 0;          /* 0 = A, 1 = B                            */
int acsiCmd      = 0;          /* 0=READ(6) 1=TEST UNIT READY 2=INQUIRY  */
int acsiResetFail= 1;          /* reset after a wedged command: ON       */
int acsiCleanup  = 0;          /* extra bus accesses to tidy up: OFF      */
int acsiLun      = 0;          /* CDB byte 1, bits 7-5                    */
int acsiResetMs  = 100;        /* settle after an ACSI bus reset          */
int acsiResetEvery = 0;        /* reset before EVERY iteration            */
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
                printf("fscanf failed, assuming emulator running\n");
                fclose(fp);
                closedir(dir);
                return 1;
            }
            if (!strcmp(pname, name)) {
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
       
        ps_get_firmware_revision ();     /* always - bring-up needs it */

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

        if ( cmdDump )
        {
            printf ( "Dumping onboard ATARI ROM from 0x%X to file tos.rom\n", ROMaddress );

            dump ( ROMsize, ROMaddress );

            printf ( "ATARI ROM dumped - %d KB\n", ROMsize );
        }

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

        if ( cmdAcsiTest )
        {
            setMemory ( memSize );
            acsiTest ();
        }

        if ( cmdFdcTest )
        {
            setMemory ( memSize );
            fdcTest ();
        }

        if ( cmdP2diag )
        {
            setMemory ( memSize );
            p2diag ();
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
                 "--fdctest <loops=n> <drive=a|b>\n"
                 "     drives the real WD1772 directly: selects the drive via PSG port A,\n"
                 "     issues FORCE INTERRUPT then RESTORE, and times how long MFP GPIP5\n"
                 "     takes to signal completion. Says whether the FDC interrupt reaches\n"
                 "     the host at all - which is what EmuTOS waits on.\n"
                 "--acsitest <loops=n> <dev=0-7> <lba=n> <sectors=n> <base=xxxxxx> <settle=us> <quiet=yes>\n"
                 "     standalone real-ACSI DMA loop, no TOS. Issues READ(6) N times and reports\n"
                 "     the DMA address-counter delta per transfer, BR (re-armed each time),\n"
                 "     the ACSI and DMA status bytes, and how many destination bytes changed.\n"
                 "     Gives a rate rather than a single boot-time anecdote.\n"
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

        if ( strcmp ( cmdptr, "p2diag" ) == 0 )
        {
            valid = 1;
            cmdP2diag = 1;
        }

        if ( strcmp ( cmdptr, "fdctest" ) == 0 )
        {
            valid = 1;
            substring = strtok_r ( NULL, " ", &strSave );

            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );

                if ( strcmp ( aptr, "loops" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    fdcLoops = atoi ( tptr );
                    if ( fdcLoops < 1 )
                        fdcLoops = 1;
                }

                else if ( strcmp ( aptr, "drive" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    fdcDrive = ( *tptr == 'b' || *tptr == 'B' || *tptr == '1' );
                }

                else
                    valid = 0;
            }

            if ( valid )
                cmdFdcTest = 1;
        }

        if ( strcmp ( cmdptr, "acsitest" ) == 0 )
        {
            valid = 1;
            substring = strtok_r ( NULL, " ", &strSave );

            for ( ; substring != NULL; substring = strtok_r ( NULL, " ", &strSave ) )
            {
                aptr = strtok ( substring, "=" );

                if ( strcmp ( aptr, "loops" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiLoops = atoi ( tptr );
                    if ( acsiLoops < 1 )
                        acsiLoops = 1;
                }

                else if ( strcmp ( aptr, "dev" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiDev = atoi ( tptr ) & 7;
                }

                else if ( strcmp ( aptr, "lba" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiLba = (uint32_t)strtoul ( tptr, NULL, 0 ) & 0x001FFFFFu;
                }

                else if ( strcmp ( aptr, "base" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    sscanf ( tptr, "%x", &acsiBase );
                    acsiBase &= ~1u;
                }

                else if ( strcmp ( aptr, "sectors" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiSectors = atoi ( tptr );
                    if ( acsiSectors < 1 )   acsiSectors = 1;
                    if ( acsiSectors > 255 ) acsiSectors = 255;
                }

                else if ( strcmp ( aptr, "settle" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiSettleUs = atoi ( tptr );
                    if ( acsiSettleUs < 0 )
                        acsiSettleUs = 0;
                }

                else if ( strcmp ( aptr, "quiet" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "yes" ) == 0 )
                        acsiQuiet = 1;
                }

                else if ( strcmp ( aptr, "pollus" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiPollUs = atoi ( tptr );
                    if ( acsiPollUs < 0 )
                        acsiPollUs = 0;
                }

                else if ( strcmp ( aptr, "irq" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "no" ) == 0 )
                        acsiUseIrq = 0;
                }

                else if ( strcmp ( aptr, "gapus" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiGapUs = atoi ( tptr );
                    if ( acsiGapUs < 0 )
                        acsiGapUs = 0;
                }

                else if ( strcmp ( aptr, "cmd" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "read" ) == 0 )         acsiCmd = 0;
                    else if ( strcmp ( tptr, "tur" ) == 0 )     acsiCmd = 1;
                    else if ( strcmp ( tptr, "inquiry" ) == 0 ) acsiCmd = 2;
                    else valid = 0;
                }

                else if ( strcmp ( aptr, "resetms" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiResetMs = atoi ( tptr );
                    if ( acsiResetMs < 1 )
                        acsiResetMs = 1;
                }

                else if ( strcmp ( aptr, "resetevery" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "yes" ) == 0 )
                        acsiResetEvery = 1;
                }

                else if ( strcmp ( aptr, "lun" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiLun = atoi ( tptr ) & 7;
                }

                else if ( strcmp ( aptr, "cleanup" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "yes" ) == 0 )
                        acsiCleanup = 1;
                }

                else if ( strcmp ( aptr, "resetonfail" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "yes" ) == 0 )
                        acsiResetFail = 1;
                }

                else if ( strcmp ( aptr, "fc" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiFc = atoi ( tptr ) & 7;
                }

                else if ( strcmp ( aptr, "bare" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "yes" ) == 0 )
                        acsiBare = 1;
                }

                else if ( strcmp ( aptr, "scan" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    if ( strcmp ( tptr, "yes" ) == 0 )
                        acsiScan = 1;
                }

                else if ( strcmp ( aptr, "waitms" ) == 0 )
                {
                    tptr = strtok ( NULL, "" );
                    acsiWaitMs = atoi ( tptr );
                    if ( acsiWaitMs < 1 )
                        acsiWaitMs = 1;
                }

                else
                    valid = 0;
            }

            if ( valid )
                cmdAcsiTest = 1;
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


/* =========================================================================
 * --acsitest : standalone real-ACSI DMA loop
 *
 * WHY THIS EXISTS
 * Every ACSI measurement so far came from a single transfer inside one TOS
 * boot. Boot-to-boot the enumeration path, the timing and the number of
 * transfers all differ, so each run is N=1 and nothing can be attributed to
 * anything. One boot pulled six sectors; the next, same binary and same
 * config, pulled none. That is not a bug report, it is noise.
 *
 * This runs the exact register sequence the boot trace shows - the
 * $0190/$0090 R/W toggle, base, sector count, then a six-byte READ(6) CDB -
 * N times with no TOS in the loop, and counts outcomes. The Pi owns all the
 * timing, so the probes are not competing with EmuTOS for the bus.
 *
 * WHAT IT MEASURES, per iteration:
 *   - the DMA address counter delta, read back off the real chip. 0 means
 *     the controller performed no bus cycles; count*512 means it completed;
 *     anything between means it started and was cut off.
 *   - BR, re-armed before each transfer via status[2], so it means "during
 *     THIS transfer" rather than "at some point since reset".
 *   - the ACSI status byte and the DMA status register.
 *   - how many bytes of the destination window actually changed. The window
 *     is poisoned with a per-iteration pattern first, so "DMA wrote zeros"
 *     and "DMA wrote nothing" are distinguishable - they were not before.
 *
 * The output is a rate, not an anecdote. Vary one thing (firmware revision,
 * settle delay, sector count) and the rate moves or it does not.
 * ========================================================================= */

#define ACSI_STATUS_BR_SEEN  (1u << 19)  /* PI_D[11] -> GPIO19. Same bit as
                                            ARB_STATUS_BUSY_RAW above, which
                                            is labelled BGACK - that comment
                                            predates fw 0.70a, where the bit
                                            became the sticky BR flag. */
#define ACSI_MFP_GPIP        0x00FFFA01u
#define ACSI_GPIP_IRQ        0x20u        /* FDC/HDC interrupt, active low */

#define ACSI_DMA_DATA        0x00FF8604u
#define ACSI_DMA_MODE        0x00FF8606u
#define ACSI_DMA_BASE_HI     0x00FF8609u
#define ACSI_DMA_BASE_MID    0x00FF860Bu
#define ACSI_DMA_BASE_LO     0x00FF860Du

#define ACSI_IRQ_TIMEOUT_MS  1000u    /* whole command incl. data phase */
#define ACSI_ACK_TIMEOUT_MS  20u      /* per command byte               */

static uint32_t acsi_read_base ( void )
{
    uint32_t h = read8 ( ACSI_DMA_BASE_HI );
    uint32_t m = read8 ( ACSI_DMA_BASE_MID );
    uint32_t l = read8 ( ACSI_DMA_BASE_LO );

    return ( ( h & 0xFFu ) << 16 ) | ( ( m & 0xFFu ) << 8 ) | ( l & 0xFFu );
}

/* MFP GPIP bit 5, active low. Returns 1 if it went low before the deadline. */
static int acsi_wait_irq ( uint32_t ms, int poll_us )
{
    uint64_t deadline = monotonic_ms () + ms;

    while ( read8 ( ACSI_MFP_GPIP ) & ACSI_GPIP_IRQ )
    {
        if ( monotonic_ms () > deadline )
            return 0;

        if ( poll_us )
            usleep ( (useconds_t)poll_us );
    }

    return 1;
}

/* One CDB byte. Mode $88 selects the HDC with A1 low - that is the FIRST
 * byte, the one that also asserts the device select. $8A is A1 high, every
 * byte after it. Straight from the boot trace.
 *
 * ACSI IS HANDSHAKED PER BYTE. The device pulls IRQ low to acknowledge each
 * command byte and the host must not send the next one until it does. The
 * first version of this function wrote all six bytes back-to-back at Pi
 * speed, which no real target can follow: the command was never assembled,
 * so there was no completion, no IRQ, and $FF8604 read back floating bus -
 * $FF, $60, $1E, $00 with nothing driving it. Every iteration timed out and
 * it looked like a DMA fault when it was a protocol fault in the test.
 *
 * Returns 0 if the device did not acknowledge.
 */
extern void ps_flush_posted ( void );

/* Real-ST pacing for every ACSI interaction.  A 68000 running TOS puts
 * whole instructions (and landed bus cycles) between each step; posted
 * writes at Pi pace do not.  Every step below LANDS before the next
 * begins, with ~real-world spacing.  "Wait for the handshake" applied
 * to the whole path, not one spot. */
#define ACSI_STEP_US 15

static int acsi_cmd_byte ( uint16_t mode, uint8_t byte, uint32_t ms )
{
    /* ROLLBACK NOTE (measured): the canonical-TOS two-mode-write shape
     * plus step pacing took a 50%-flaky CDB phase to 100% dead at byte
     * 0.  This body is the exact configuration that achieved partial
     * acks; changes go back in ONE AT A TIME against that baseline. */
    if ( mode )
        write16 ( ACSI_DMA_MODE, mode );

    /* `gapus` exists because the Pi issues these two writes microseconds
     * apart, where a 68000 running TOS puts whole instructions between them.
     * If selection only works with a gap, the target cannot follow the Pi's
     * bus rate and that is the finding - not a DMA fault. */
    if ( acsiGapUs )
        usleep ( (useconds_t)acsiGapUs );

    write16 ( ACSI_DMA_DATA, (uint16_t)byte );

    if ( acsiGapUs )
        usleep ( (useconds_t)acsiGapUs );

    return acsi_wait_irq ( ms, 0 );   /* ack arrives in microseconds */
}

/* Put the DMA chip back to a deselected, floppy-owned state. Without this a
 * command that dies part way leaves the target waiting for the rest of its
 * CDB, and the NEXT iteration's first byte is swallowed as a continuation -
 * one failure poisons every attempt after it. */
static void acsi_deselect ( void )
{
    write16 ( ACSI_DMA_MODE, 0x0080 );
}

/* Probe each target with just the select byte. */
static void acsi_scan ( void )
{
    printf ( "scanning ACSI targets 0-7 (select byte only)\n" );

    for ( int d = 0; d < 8; d++ )
    {
        int ack;

        acsi_deselect ();
        write16 ( ACSI_DMA_MODE, 0x0190 );
        write16 ( ACSI_DMA_MODE, 0x0090 );
        write16 ( ACSI_DMA_DATA, 1 );

        ack = acsi_cmd_byte ( 0x0088, (uint8_t)( ( d << 5 ) | 0x00 ),
                              ACSI_ACK_TIMEOUT_MS );   /* TEST UNIT READY */

        printf ( "  target %d: %s  (gpip=$%02X)\n", d,
                 ack ? "ACK - device present" : "no response",
                 read8 ( ACSI_MFP_GPIP ) );

        acsi_deselect ();
        usleep ( 2000 );
    }

    printf ( "\n" );
}


/* ACSI BUS RESET - the only thing known to return the target to a defined
 * state.
 *
 * This matters more than anything else in the file. A command that is
 * abandoned part way leaves the device still selected and still counting
 * bytes, so the NEXT command's select byte is swallowed as a continuation
 * byte. Evidence: the HDC decoded one command as
 *
 *     TEST_UNIT_READY t1:2 (0x00:40:60:80:a0:c0)
 *
 * where $20/$40/$60/$80/$a0/$c0 are the select bytes for six DIFFERENT
 * targets, concatenated into a single CDB by a scan whose acknowledgements
 * never arrived. Whether any given command works then depends on what byte
 * offset the device happens to be sitting at - which is exactly the
 * "random, intermittent" behaviour, and it is not randomness at all.
 *
 * Deselecting with mode $0080 does NOT clear that state; only a bus reset
 * does, and the device reports RESET when it sees one.
 *
 * The reset also clears the MMU configuration, so setMemory() has to run
 * again or every subsequent DMA target address is meaningless.
 */
static void acsi_bus_reset ( const char *why )
{
    uint64_t deadline;

    if ( VERBOSE && why )
        printf ( "      [ACSI bus reset: %s]\n", why );

    atariReset ();
    usleep ( (useconds_t)acsiResetMs * 1000u );

    setMemory ( memSize );          /* reset wiped the MMU config */

    /* Both controllers can be left asserting after a reset - the WD1772
     * finishes a restore and raises IRQ, and the HDC answers the reset.
     * Read each status register once so GPIP5 is released before the next
     * command, otherwise the pre-command check sees a stuck IRQ and skips
     * every iteration. An earlier build pulsed reset and then did NOT do
     * this, and came back with gpip=$CF on all four remaining loops. */
    write16 ( ACSI_DMA_MODE, 0x0080 );      /* FDC status  */
    (void)read16 ( ACSI_DMA_DATA );
    write16 ( ACSI_DMA_MODE, 0x008A );      /* HDC status  */
    (void)read16 ( ACSI_DMA_DATA );
    write16 ( ACSI_DMA_MODE, 0x0080 );

    deadline = monotonic_ms () + 500;

    while ( !( read8 ( ACSI_MFP_GPIP ) & ACSI_GPIP_IRQ ) )
    {
        if ( monotonic_ms () > deadline )
        {
            printf ( "      [ACSI bus reset: GPIP5 still asserted after "
                     "500ms - gpip=$%02X]\n", read8 ( ACSI_MFP_GPIP ) );
            break;
        }

        usleep ( 1000 );
    }
}

void acsiTest ( void )
{
    uint8_t  saved_fc = fc;
    uint32_t len      = (uint32_t)acsiSectors * 512u;   /* poison window */

    /* Bytes the DMA controller should actually move. INQUIRY returns 36,
     * TEST UNIT READY none - classifying either against sectors*512 meant
     * a perfectly good 36-byte transfer would still have printed PARTIAL. */
    uint32_t xfer_len = ( acsiCmd == 1 ) ? 0u
                      : ( acsiCmd == 2 ) ? 36u
                                         : len;

    uint32_t n_nomove = 0, n_partial = 0, n_complete = 0;
    uint32_t n_br     = 0, n_timeout = 0, n_buserr   = 0;
    uint32_t n_status_ok = 0, n_data_changed = 0, n_cmd_fail = 0;
    uint32_t n_cdb_retry = 0;
    uint32_t n_stuck_irq = 0;
    uint64_t total_delta = 0;

    /* 5 = supervisor data, which is what TOS uses for $FF8xxx register
     * access and what arbTest uses. NOTE main() leaves the global at 6
     * (supervisor program) and devTest keeps it there, so results from
     * --dev are not directly comparable. Overridable with fc=n so the
     * function code can be swept against a scope rather than argued about. */
    fc = (uint8_t)acsiFc;

    printf ( "\nATARITEST standalone ACSI DMA loop\n" );
    printf ( "loops=%d dev=%d lba=%u sectors=%d base=$%06X settle=%dus "
             "fc=%d gap=%dus\n",
             acsiLoops, acsiDev, acsiLba, acsiSectors, acsiBase, acsiSettleUs,
             acsiFc, acsiGapUs );
    printf ( "bus accesses: command + status only%s\n",
             acsiCleanup ? ", plus cleanup toggles" : "" );
    printf ( "bus reset: at start%s%s (settle %dms)\n",
             acsiResetFail  ? ", after any failure" : "",
             acsiResetEvery ? ", before every iteration" : "",
             acsiResetMs );
    printf ( "command: %s%s\n",
             acsiCmd == 1 ? "TEST UNIT READY (no data phase)" :
             acsiCmd == 2 ? "INQUIRY (36 bytes)" : "READ(6)",
             acsiResetFail ? ", ST reset pulse after a wedge" : "" );
    printf ( "completion: %s\n", acsiUseIrq
             ? "MFP GPIP bit 5 (poll gap below)" : "fixed delay, IRQ ignored" );
    if ( acsiUseIrq )
        printf ( "poll gap=%dus timeout=%ums\n", acsiPollUs,
                 ACSI_IRQ_TIMEOUT_MS );
    else
        printf ( "wait=%dms\n", acsiWaitMs );
    printf ( "CDB = %02X %02X %02X %02X %02X 00   (target %d, LUN %d)\n",
             (unsigned)( ( acsiDev << 5 ) | ( acsiCmd == 1 ? 0x00 :
                                              acsiCmd == 2 ? 0x12 : 0x08 ) ),
             (unsigned)( acsiCmd == 0
                         ? ( ( acsiLun << 5 ) | ( ( acsiLba >> 16 ) & 0x1F ) )
                         : ( acsiLun << 5 ) ),
             (unsigned)( acsiCmd == 0 ? ( acsiLba >> 8 ) & 0xFF : 0 ),
             (unsigned)( acsiCmd == 0 ? acsiLba & 0xFF : 0 ),
             (unsigned)( acsiCmd == 1 ? 0 : acsiCmd == 2 ? 36 : acsiSectors ),
             acsiDev, acsiLun );
    printf ( "delta is the DMA address counter read back off the real chip; "
             "expecting %u byte%s\n\n", xfer_len, xfer_len == 1 ? "" : "s" );

    if ( acsiScan )
    {
        acsi_scan ();
        fc = saved_fc;
        return;
    }

    /* Start from a defined state, always. Every run before this one began
     * with whatever the previous run left the target holding. */
    /* THE SCAN/LOOP BISECTION LANDED HERE: the scan path (which ACKS)
     * returns above this line and never fires the bus reset; the loop
     * (byte 0 never acked) always did.  Our reset pulse is a 100ms
     * assertion - nothing like a real ST's 16us RESET instruction - and
     * whatever the target does to re-initialise afterwards, it is not
     * listening when the CDBs start.  bare=yes now also skips it. */
    if ( !acsiBare )
        acsi_bus_reset ( "start of run" );

    /* sigint_handler exit()s directly, so there is no abort flag to poll. */
    for ( int loop = 0; loop < acsiLoops; loop++ )
    {
        uint8_t  poison = (uint8_t)( 0xA5u ^ (unsigned)loop );
        uint32_t after, delta, changed = 0;
        uint16_t acsi_status, dma_status;
        uint32_t sr;
        int      timed_out = 0;
        uint64_t deadline  = 0;
        uint8_t  gpip      = 0;
        int      cmd_fail_byte = -1;

        g_buserr = 0;

        if ( acsiResetEvery && loop )
            acsi_bus_reset ( NULL );

        /* IRQ MUST be deasserted before a command starts. If it is already
         * low - left over from a command that failed part way - then every
         * per-byte wait below returns instantly without the device having
         * done anything, and the whole iteration is a false pass. Exactly
         * one iteration in the first 20-loop run "succeeded" this way. */
        if ( !( read8 ( ACSI_MFP_GPIP ) & ACSI_GPIP_IRQ ) )
        {
            /* OBSERVE, DO NOT TIDY - default.
             *
             * This used to try to clear the interrupt with mode toggles and
             * a status read. Those are extra accesses to a target whose
             * state machine is not mine to guess at, and adding them turned
             * a tool that reliably produced one clean INQUIRY into one that
             * was unreliable on every command. Skipping the iteration costs
             * a data point; poking the device costs the whole run.
             *
             * cleanup=yes puts the old behaviour back if it turns out to
             * help, but it has to earn its place on evidence. */
            if ( acsiCleanup )
            {
                write16 ( ACSI_DMA_MODE, 0x0000 );
                ps_flush_posted ();         /* transfer mode LANDED
                                               before we start waiting  */
                write16 ( ACSI_DMA_MODE, 0x008A );
                (void)read16 ( ACSI_DMA_DATA );
                acsi_deselect ();
            }

            usleep ( 1000 );

            if ( !( read8 ( ACSI_MFP_GPIP ) & ACSI_GPIP_IRQ ) )
            {
                n_stuck_irq++;

                if ( !acsiQuiet )
                    printf ( "%4d: IRQ still asserted before command - "
                             "skipped (gpip=$%02X)\n",
                             loop, read8 ( ACSI_MFP_GPIP ) );
                continue;
            }
        }

        /* Poison the destination. Without this a transfer that moves 512
         * zero bytes is indistinguishable from one that moves nothing, and
         * both were being reported as "sig 0000". */
        if ( !acsiBare )
            for ( uint32_t i = 0; i < len; i++ )
                write8 ( acsiBase + i, poison );

        /* Re-arm the sticky BR flag: 0x0004 to clear, 0x0000 to release.
         * status[1:0] stay 00 so HALT/RESET/INIT are untouched. */
        if ( !acsiBare )
        {
            write_reg ( 0x0004 );
            write_reg ( 0x0000 );
        }

        /* DESELECT FIRST - this is the one ordering difference between
         * this loop (byte 0 never acked) and the scan path (acks every
         * time, today, on this stack): the loop used to program the
         * BASE registers while the mode register still held $8A from
         * the previous status read - HDC selected, A1 high.  The scan
         * deselects to $80 before touching anything, and the EmuTOS
         * trace likewise programs base with an FDC-family mode live.
         * A base access with the HDC addressed can hand the target a
         * phantom byte - after which the real select is "byte 1" of a
         * command that never existed. */
        write16 ( ACSI_DMA_MODE, 0x0080 );

        /* Base next - the trace programs it before the mode toggle.
         * bare=yes skips it: the scan (which ACKS on this bench, today)
         * never touches base. */
        if ( !acsiBare )
        {
            write8 ( ACSI_DMA_BASE_LO,  (uint8_t)( acsiBase & 0xFFu ) );
            write8 ( ACSI_DMA_BASE_MID, (uint8_t)( ( acsiBase >> 8 ) & 0xFFu ) );
            write8 ( ACSI_DMA_BASE_HI,  (uint8_t)( ( acsiBase >> 16 ) & 0xFFu ) );
        }

        /* Toggling the R/W bit with SCREG set resets the DMA chip's internal
         * state. $0190 then $0090 leaves direction = read (device -> RAM). */
        write16 ( ACSI_DMA_MODE, 0x0190 );
        write16 ( ACSI_DMA_MODE, 0x0090 );

        /* Sector count, SCREG still set. */
        write16 ( ACSI_DMA_DATA, (uint16_t)acsiSectors );

        /* Six-byte READ(6), each byte acknowledged before the next.
         * The LAST byte is not waited on here - that ack is the end of the
         * whole command including the data phase, and it gets the long
         * timeout below. */
        {
            uint8_t cdb[6];
            int cdb_attempt;

            /* opcode 0x08 READ(6), 0x00 TEST UNIT READY, 0x12 INQUIRY.
             *
             * TEST UNIT READY has NO DATA PHASE, and that is the point of
             * offering it: --scan (which only ever sends TUR) runs happily
             * while the READ loop wedges the target hard enough to need a
             * power cycle. A device that accepts a READ and is then left
             * holding 512 bytes nobody collects has nowhere to go - which
             * is what a DRQ that never gets serviced looks like from the
             * far end. Being able to loop TUR proves the command transport
             * is sound independently of the data phase. */
            /* Byte 0 : device in bits 7-5, opcode in bits 4-0.
             * Byte 1 : LUN in bits 7-5, and for READ(6) the top five bits
             *          of the LBA in bits 4-0.
             *
             * The LUN was previously never written explicitly - it came out
             * as 0 only because lba=0 made (lba >> 16) & 0x1F zero. Correct
             * by accident. Now it is placed deliberately, and a non-zero
             * lba can no longer silently overwrite it. */
            cdb[0] = (uint8_t)( ( acsiDev << 5 ) |
                                ( acsiCmd == 1 ? 0x00 :
                                  acsiCmd == 2 ? 0x12 : 0x08 ) );

            if ( acsiCmd == 0 )         /* READ(6) */
            {
                cdb[1] = (uint8_t)( ( acsiLun << 5 ) |
                                    ( ( acsiLba >> 16 ) & 0x1F ) );
                cdb[2] = (uint8_t)( ( acsiLba >> 8 ) & 0xFF );
                cdb[3] = (uint8_t)( acsiLba & 0xFF );
                cdb[4] = (uint8_t)acsiSectors;
            }

            else                        /* TEST UNIT READY / INQUIRY */
            {
                cdb[1] = (uint8_t)( acsiLun << 5 );   /* EVPD / reserved 0 */
                cdb[2] = 0x00;                        /* page code         */
                cdb[3] = 0x00;
                cdb[4] = (uint8_t)( acsiCmd == 2 ? 36 : 0 );
            }

            cdb[5] = 0x00;              /* control */

            /* CDB RETRY - what every real-world initiator does.  The
             * target's command arming is a sub-microsecond polled
             * window (its own CMD prints skip whole loops), and TOS
             * masks exactly this by silently retrying commands.  Three
             * attempts at ~real-world spacing; retries are COUNTED so
             * the marginality stays visible instead of hidden. */
            for ( cdb_attempt = 0; cdb_attempt < 3; cdb_attempt++ )
            {
                cmd_fail_byte = -1;

                for ( int b = 0; b < 5; b++ )
                {
                    if ( !acsi_cmd_byte ( b ? 0x008A : 0x0088, cdb[b],
                                          ACSI_ACK_TIMEOUT_MS ) )
                    {
                        cmd_fail_byte = b;
                        break;
                    }
                }

                if ( cmd_fail_byte < 0 )
                    break;                       /* CDB accepted        */

                n_cdb_retry++;
                acsi_deselect ();
                usleep ( 2000 );                 /* let the target re-arm */
            }

            if ( cmd_fail_byte < 0 )
            {
                /* final byte: write it, then wait for the transfer */
                write16 ( ACSI_DMA_MODE, 0x008A );
                write16 ( ACSI_DMA_DATA, (uint16_t)cdb[5] );

                /* DATA-PHASE MODE - the EmuTOS trace decoded WITH its
                 * time axis: the "$0000 ... $008A" pair has the ENTIRE
                 * DMA TRANSFER between the two writes.  $0000 (bit7=0)
                 * is the transfer mode - it is what routes the HDC's
                 * DRQ into the DMA engine.  $008A comes only AFTERWARDS,
                 * to address the status byte.  Replaying the pair
                 * back-to-back (as this code did) left the DMA deaf to
                 * the HDC for the whole data phase: DRQs ignored, BR
                 * never asserted, address counter frozen, target wedged
                 * in its no-timeout ACK spin.  Every data-phase symptom
                 * ever logged by --acsitest, from one collapsed trace. */
                write16 ( ACSI_DMA_MODE, 0x0000 );
            }
        }

        if ( cmd_fail_byte >= 0 )
        {
            n_cmd_fail++;

            if ( !acsiQuiet )
                printf ( "%4d: device did not acknowledge CDB byte %d "
                         "(gpip=$%02X) - target %d not responding?\n",
                         loop, cmd_fail_byte, read8 ( ACSI_MFP_GPIP ),
                         acsiDev );

            if ( acsiCleanup )
                acsi_deselect ();

            if ( acsiResetFail )
                acsi_bus_reset ( "CDB not acknowledged" );
            else
                usleep ( 1000 );

            continue;
        }

        /* Wait for the device IRQ on MFP GPIP bit 5, active low.
         *
         * THE GAP IS NOT OPTIONAL. A tight read8() loop here is a solid
         * second of back-to-back Pi bus cycles at exactly the moment the
         * DMA controller needs idle slots to move data in - the poll would
         * be preventing the event it is waiting for, then reporting a
         * timeout. `pollus` puts the bus back down between samples; 100us
         * is ~1000 polls/sec, plenty to catch a completion, and leaves the
         * bus quiet the rest of the time.
         *
         * If it still never fires, `irq=no` skips the wait entirely and
         * just delays `waitms` before reading back - which tells you
         * whether the transfer works and only the IRQ path is broken. */
        if ( acsiUseIrq )
        {
            timed_out = !acsi_wait_irq ( ACSI_IRQ_TIMEOUT_MS, acsiPollUs );
            gpip      = read8 ( ACSI_MFP_GPIP );
            (void)deadline;
        }

        else
        {
            usleep ( (useconds_t)acsiWaitMs * 1000u );
            gpip = read8 ( ACSI_MFP_GPIP );
        }

        if ( acsiSettleUs )
            usleep ( (useconds_t)acsiSettleUs );

        /* Transfer over (or timed out): NOW address the status byte. */
        write16 ( ACSI_DMA_MODE, 0x008A );

        /* Read everything BEFORE anything else touches the bus. */
        acsi_status = read16 ( ACSI_DMA_DATA ) & 0xFFu;
        after       = acsi_read_base ();
        dma_status  = read16 ( ACSI_DMA_MODE ) & 0x07u;
        sr          = read_reg ();

        delta = ( after >= acsiBase ) ? after - acsiBase : 0;
        total_delta += delta;

        for ( uint32_t i = 0; i < len; i++ )
            if ( read8 ( acsiBase + i ) != poison )
                changed++;

        if ( delta == 0 )             n_nomove++;
        else if ( delta >= xfer_len ) n_complete++;
        else                          n_partial++;

        if ( sr & ACSI_STATUS_BR_SEEN ) n_br++;
        if ( timed_out )
        {
            n_timeout++;

            /* A target left mid-data-phase stays there. Deselect, and pulse
             * reset if asked, or every iteration after the first is testing
             * a wedged device rather than the DMA. */
            /* Count it and stop. Same reasoning as the pre-check above:
             * no unsolicited bus accesses on the failure path. */
            if ( acsiCleanup )
            {
                write16 ( ACSI_DMA_MODE, 0x0000 );
                write16 ( ACSI_DMA_MODE, 0x008A );
                (void)read16 ( ACSI_DMA_DATA );
                acsi_deselect ();
            }

            /* A target left mid-data-phase stays there, and deselecting
             * does not move it. Reset is the only thing that does. */
            if ( acsiResetFail )
                acsi_bus_reset ( "transfer timed out" );
            else
                usleep ( 2000 );
        }
        if ( g_buserr )                 n_buserr++;
        if ( acsi_status == 0 )         n_status_ok++;
        if ( changed )                  n_data_changed++;

        if ( !acsiQuiet )
        {
            printf ( "%4d: delta=%-6u %-8s  acsi=$%02X dma=$%02X gpip=$%02X "
                     "BR=%-3s changed=%u/%u%s%s\n",
                     loop, delta,
                     delta == 0 ? "NOMOVE" : ( delta >= xfer_len ? "complete"
                                                                 : "PARTIAL" ),
                     acsi_status, dma_status, gpip,
                     ( sr & ACSI_STATUS_BR_SEEN ) ? "yes" : "no",
                     changed, len,
                     timed_out ? "  TIMEOUT" : "",
                     g_buserr  ? "  BUSERR"  : "" );
            fflush ( stdout );
        }
    }

    printf ( "\n--- %d iterations ---\n", acsiLoops );
    printf ( "complete      %6u  (%.1f%%)\n", n_complete,
             100.0 * n_complete / (double)acsiLoops );
    printf ( "partial       %6u  (%.1f%%)\n", n_partial,
             100.0 * n_partial / (double)acsiLoops );
    printf ( "no movement   %6u  (%.1f%%)\n", n_nomove,
             100.0 * n_nomove / (double)acsiLoops );
    printf ( "mean delta    %6.1f bytes of %u\n",
             (double)total_delta / (double)acsiLoops, xfer_len );
    printf ( "BR asserted   %6u\n", n_br );
    printf ( "acsi status 0 %6u\n", n_status_ok );
    printf ( "data changed  %6u\n", n_data_changed );
    printf ( "irq timeout   %6u\n", n_timeout );
    printf ( "bus error     %6u\n",   n_buserr );
    printf ( "cmd not ack'd %6u  << device never acknowledged a CDB byte\n",
             n_cmd_fail );
    printf ( "cdb retries     %4u  (arming marginality, masked by retry - a real ST's TOS does the same)\n",
             n_cdb_retry );
    printf ( "irq stuck low %6u  << skipped, would have been a false pass\n\n",
             n_stuck_irq );

    if ( n_complete == 0 && n_partial == 0 )
        printf ( "The DMA controller performed no bus cycles in any "
                 "iteration.\n\n" );
    else if ( n_complete == 0 )
        printf ( "Every transfer started and none finished - a hold problem, "
                 "not a grant problem.\n\n" );

    fc = saved_fc;
}


/* =========================================================================
 * --fdctest : does the WD1772 completion interrupt reach MFP GPIP5?
 *
 * EmuTOS's floppy driver issues a command and then waits for GPIP5 to go
 * low. The boot trace shows it issue FORCE INTERRUPT, then RESTORE, read
 * the status once - getting BUSY, which is correct microseconds after a
 * command - and then never touch the floppy again. Two possibilities, and
 * they need separating:
 *
 *   a) the interrupt never arrives, so the wait times out;
 *   b) GPIP5 is ALREADY low when the wait starts, so it falls straight
 *      through and the status is read while the command is still running.
 *
 * This drives the same registers with no TOS involved and reports the state
 * of GPIP5 before the command, and how long it takes to assert after it.
 *
 * The WD1772 status register is Type I here (RESTORE is a Type I command):
 *   b7 MOTOR ON   b6 WRITE PROTECT   b5 SPIN-UP   b4 SEEK ERROR
 *   b3 CRC ERROR  b2 TRACK 00        b1 INDEX     b0 BUSY
 * ========================================================================= */

#define FDC_MODE_REG        0x00FF8606u
#define FDC_DATA            0x00FF8604u
#define FDC_MODE_CMD        0x0080u     /* A1=0 A0=0 -> command/status     */
#define FDC_MODE_TRACK      0x0082u
#define FDC_MODE_DATA       0x0086u

#define PSG_SELECT          0x00FF8800u
#define PSG_WRITE           0x00FF8802u

#define FDC_CMD_FORCE_INT   0xD0u
#define FDC_CMD_RESTORE     0x0Bu

static void fdc_select ( int drive_b )
{
    uint8_t pa;

    write8 ( PSG_SELECT, 14 );          /* port A */
    pa = read8 ( PSG_SELECT );

    pa |= 0x07u;                        /* deselect both, side 0          */
    pa &= (uint8_t)~( drive_b ? 0x04u : 0x02u );   /* select one, active low */

    write8 ( PSG_SELECT, 14 );
    write8 ( PSG_WRITE, pa );
}

static uint8_t fdc_status ( void )
{
    write16 ( FDC_MODE_REG, FDC_MODE_CMD );
    return (uint8_t)( read16 ( FDC_DATA ) & 0xFFu );
}

static void fdc_command ( uint8_t cmd )
{
    write16 ( FDC_MODE_REG, FDC_MODE_CMD );
    write16 ( FDC_DATA, (uint16_t)cmd );
}

static void fdc_decode ( uint8_t st )
{
    printf ( "        status $%02X ="
             " %s%s%s%s%s%s%s%s\n", st,
             ( st & 0x80 ) ? "MOTOR "     : "",
             ( st & 0x40 ) ? "PROTECT "   : "",
             ( st & 0x20 ) ? "SPINUP "    : "",
             ( st & 0x10 ) ? "SEEKERR "   : "",
             ( st & 0x08 ) ? "CRCERR "    : "",
             ( st & 0x04 ) ? "TRACK00 "   : "",
             ( st & 0x02 ) ? "INDEX "     : "",
             ( st & 0x01 ) ? "BUSY"       : "idle" );
}

void fdcTest ( void )
{
    uint8_t saved_fc = fc;
    uint32_t n_already = 0, n_timeout = 0, n_ok = 0;

    fc = 5;

    printf ( "\nATARITEST WD1772 interrupt test\n" );
    printf ( "drive %c, %d loops. GPIP5 (MFP $FFFA01 bit 5) is what EmuTOS\n"
             "waits on for floppy command completion - active low.\n\n",
             fdcDrive ? 'B' : 'A', fdcLoops );

    for ( int loop = 0; loop < fdcLoops; loop++ )
    {
        uint8_t  before, st;
        uint64_t t0, took = 0;
        int      asserted = 0;

        fdc_select ( fdcDrive );

        /* Clear anything outstanding: force interrupt, then read status -
         * reading the status register is what releases the WD1772's INTRQ. */
        fdc_command ( FDC_CMD_FORCE_INT );
        usleep ( 20000 );
        (void)fdc_status ();
        usleep ( 10000 );

        before = read8 ( ACSI_MFP_GPIP );

        if ( !( before & ACSI_GPIP_IRQ ) )
        {
            n_already++;
            printf ( "%3d: GPIP5 ALREADY LOW before the command (gpip=$%02X)"
                     "   << EmuTOS's wait would fall straight through\n",
                     loop, before );
            fdc_decode ( fdc_status () );
            continue;
        }

        /* RESTORE: seek to track 0. Completes in a few ms at track 0, or up
         * to ~250 steps x 30ms if the head is elsewhere. */
        fdc_command ( FDC_CMD_RESTORE );

        t0 = monotonic_ms ();

        while ( monotonic_ms () - t0 < 3000 )
        {
            if ( !( read8 ( ACSI_MFP_GPIP ) & ACSI_GPIP_IRQ ) )
            {
                asserted = 1;
                took = monotonic_ms () - t0;
                break;
            }

            usleep ( 500 );
        }

        st = fdc_status ();

        if ( asserted )
        {
            n_ok++;
            printf ( "%3d: GPIP5 asserted after %llu ms\n",
                     loop, (unsigned long long)took );
        }

        else
        {
            n_timeout++;
            printf ( "%3d: GPIP5 NEVER asserted in 3s (gpip=$%02X)"
                     "   << the FDC interrupt is not reaching the host\n",
                     loop, read8 ( ACSI_MFP_GPIP ) );
        }

        fdc_decode ( st );
        usleep ( 100000 );
    }

    printf ( "\n--- %d loops ---\n", fdcLoops );
    printf ( "completed      %u\n", n_ok );
    printf ( "never asserted %u\n", n_timeout );
    printf ( "already low    %u\n", n_already );

    if ( n_ok == fdcLoops )
        printf ( "\nThe FDC and its interrupt path are fine. The fault is\n"
                 "between GPIP5 and the guest, not in the hardware.\n\n" );
    else if ( n_timeout )
        printf ( "\nThe WD1772 is not signalling completion to the MFP.\n"
                 "Nothing above this can work until that does.\n\n" );
    else
        printf ( "\nGPIP5 is being held low by something else - EmuTOS's\n"
                 "wait returns immediately and it reads BUSY. That matches\n"
                 "the boot trace exactly.\n\n" );

    fc = saved_fc;
}


/* =========================================================================
 * --p2diag : isolate the PSP2 write-misfire variable by variable.
 *
 * Bench evidence so far: ~0.25% of BYTE writes misfire during ataritest's
 * priming (read tests then re-discover the same damaged addresses pass
 * after pass), and some aligned WORD writes never land (memory still holds
 * the previous pattern).  Whether the trigger is data value, address
 * change, back-to-back pacing, or byte-vs-word is EXACTLY what this
 * separates.  Each experiment changes ONE thing.  50k ops each, immediate
 * read-back verify, first 5 mismatches printed with full context.
 * ========================================================================= */
#define P2D_OPS 50000

#include <sched.h>
#include <sys/mman.h>
#include <time.h>

extern uint32_t p2_go_misses;      /* PSP2 driver retry counter */
extern uint32_t p2_dbg_counters(void); /* FW 0x27: {fight,wr[6:0],rd[7:0]} */

static uint64_t p2d_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

static uint32_t p2d_run(const char *name, int exp_no)
{
    uint32_t errs = 0, shown = 0;
    uint32_t a1 = 0x001000, a2 = 0x042000;
    uint32_t d0 = p2_dbg_counters();

    for (uint32_t i = 0; i < P2D_OPS; i++)
    {
        uint32_t addr, got, exp;

        switch (exp_no)
        {
            case 0:                      /* same addr, same data      */
                addr = a1; exp = 0xA55A;
                write16(addr, (uint16_t)exp);  got = read16(addr); break;
            case 1:                      /* same addr, walking data   */
                addr = a1; exp = i & 0xFFFF;
                write16(addr, (uint16_t)exp);  got = read16(addr); break;
            case 2:                      /* alternating addr, walking data
                                            (was data=addr, which made a
                                            stale-address write INVISIBLE -
                                            the old value equalled the
                                            expected one; blind experiment) */
                addr = (i & 1) ? a2 : a1; exp = (addr ^ i) & 0xFFFF;
                write16(addr, (uint16_t)exp);  got = read16(addr); break;
            case 3:                      /* sequential sweep, like WRITE16 */
                addr = 0x001000 + ((i * 2) & 0xFFFF); exp = addr & 0xFFFF;
                write16(addr, (uint16_t)exp);  got = read16(addr); break;
            case 4:                      /* byte even                 */
                addr = 0x001000 + ((i * 2) & 0xFFFF); exp = i & 0xFF;
                write8(addr, (uint16_t)exp);   got = read8(addr);  break;
            default:                     /* byte odd                  */
                addr = 0x001001 + ((i * 2) & 0xFFFF); exp = i & 0xFF;
                write8(addr, (uint16_t)exp);   got = read8(addr);  break;
        }

        if (got != exp)
        {
            static uint64_t last_err_us;
            uint64_t now = p2d_us();
            errs++;
            if (shown < 5)
            {
                uint32_t r2 = (exp_no >= 4) ? read8(addr) : read16(addr);
                uint32_t r3 = (exp_no >= 4) ? read8(addr) : read16(addr);
                shown++;
                printf("  %-22s op %6u  $%06X  wrote %04X  read %04X"
                       "  re-read %04X/%04X  +%llums\n",
                       name, i, addr, exp, got, r2, r3,
                       (unsigned long long)((now - last_err_us) / 1000));
            }
            last_err_us = now;
        }
    }

    {
        /* reconcile EXECUTED cycles (firmware counters) with ISSUED ops.
         * cnt_wr is 7 bits, cnt_rd 8; the CSR reads themselves are not
         * counted (separate engine path).  Reads issued = ops + 2 per
         * printed error (the re-read pair). */
        uint32_t d1 = p2_dbg_counters();
        unsigned wr_got = ((d1 >> 8) - (d0 >> 8)) & 0x7F;
        unsigned wr_exp = P2D_OPS & 0x7F;
        unsigned rd_got = (d1 - d0) & 0xFF;
        unsigned rd_exp = (P2D_OPS + 2 * shown) & 0xFF;
        printf("%-26s %6u / %u errors  (%.3f%%)  timeouts: %u\n"
               "    fw executed: wr %%128 = %u (expect %u)%s   "
               "rd %%256 = %u (expect %u)%s   fight=%s\n",
               name, errs, (unsigned)P2D_OPS, 100.0 * errs / P2D_OPS,
               p2_go_misses,
               wr_got, wr_exp, wr_got == wr_exp ? "" : "  <<< SHORT",
               rd_got, rd_exp, rd_got == rd_exp ? "" : "  <<< OFF",
               (d1 & 0x8000) ? "YES <<< BUS FIGHT" : "no");
    }
    return errs;
}

void p2diag(void)
{
    uint8_t saved_fc = fc;
    fc = 5;

    printf("\nPSP2 write-path diagnostic - one variable per experiment\n\n");

    /* THE PREEMPTION TEST.  Bench evidence: writes vanish in ADJACENT-OP
     * PAIRS every few milliseconds - the cadence of the 250Hz scheduler
     * tick, not of any electrical process.  Lock memory and go SCHED_FIFO:
     * if the error table goes clean under RT priority, the fault is a
     * preemption window in the transaction, located in software in one
     * run.  If errors persist identically, preemption is eliminated. */
    {
        struct sched_param sp = { .sched_priority = 90 };
        mlockall(MCL_CURRENT | MCL_FUTURE);
        if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0)
            printf("running at SCHED_FIFO 90 (preemption suppressed)\n\n");
        else
            printf("SCHED_FIFO unavailable (%s) - running normally\n\n",
                   "need root");
    }
    p2d_run("A same-addr same-data",    0);
    p2d_run("B same-addr walking-data", 1);
    p2d_run("C alternating-addr",       2);
    p2d_run("D sequential sweep",       3);
    p2d_run("E byte even",              4);
    p2d_run("F byte odd",               5);
    printf("\nReading the table: errors only in D = address-phase pacing;"
           "\nonly in B = data-dependent; A clean + others dirty = change-"
           "\ntriggered; E/F only = byte-path; everything dirty = per-"
           "\ntransaction (strobe/GO).  A clean table = writes are fine and"
           "\nthe fault is in sustained-rate patterns only.\n\n");

    fc = saved_fc;
}
