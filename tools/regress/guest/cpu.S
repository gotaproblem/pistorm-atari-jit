| cpu.S - CPU.PRG: the 68000's speed at cpu_speed 8mhz (see cpu.inc).

        .include "lib.S"

progname: .asciz "CPU"
        .even

TARGET  =       8000

        .include "cpu.inc"
