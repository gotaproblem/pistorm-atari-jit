| cpu16.S - CPU16.PRG: the 68000's speed at cpu_speed 16mhz (see cpu.inc).

        .include "lib.S"

progname: .asciz "CPU16"
        .even

TARGET  =       16000

        .include "cpu.inc"
