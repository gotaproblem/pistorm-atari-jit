# STBOX headless harness

Runs the sandbox ST (Musashi + stbox.c) on any host with a synthetic timer -
no Pi, no DRM, no SDL - boots a TOS with a game image, pokes the keyboard
and joystick, and reports the box's own forensics: exceptions with
disassembly, trace count, halts, screen activity. ~1.5 s of host time per
60 guest-seconds. Used to regression-test the whole games-share in a minute
and to bisect which change broke which title.

    tools/stbox-harness/build.sh cur                       # today's code
    tools/stbox-harness/build.sh pre -DSTBOX_NO_TRACE -DSTBOX_OLD_VIDCOUNTER -DSTBOX_OLD_FDC_INTRQ
    ROMS=roms GAMES=games tools/stbox-harness/batch.sh tos206uk 60 cur pre
    HARNESS_STE=1 ... batch.sh ...                          # STE box

Single image, with forensics:

    ./harness-cur --tos roms/tos206uk.rom --ram 4096 --disk game.st --secs 60 -v
      HARNESS_WATCH=lo:hi     write watchpoint (CPU + FDC DMA), hex
      HARNESS_PCHIST=N        PC history (runs) at a fatal exception
      HARNESS_DASM=a:b,c:d    disassemble ranges at a fatal exception
      HARNESS_AT=ppc          only dump for a fault at this ppc
      HARNESS_FDCMAP=1        every sector read: dst, disk offset, command
      HARNESS_FDCCMDS=1       FDC command trace (build with -DSTBOX_FDC_TRACE)
      PISTORM_STBOX_DBG=1     PC window / last hardware read telemetry

Verdicts: RUNNING (screen still changing), STUCK (static >= 10 s - usually a
title screen the pokes do not satisfy: unknown, not broken), CRASH (fatal
exception then static), HALT (double bus fault).
