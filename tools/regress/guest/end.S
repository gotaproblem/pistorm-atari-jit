| end.S - END.PRG: the last program in A:\AUTO. Its @END line tells the
| runner the boot's tests are all done, so it can stop the emulator
| instead of waiting out the timeout.
        .include "lib.S"
progname: .asciz "END"
        .even
main:
        PS      "@END"
        bra     pr_flush
