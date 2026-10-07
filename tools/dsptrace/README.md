# Falcon DSP host-port trace

This is phase 1 of the unified DSP host-interface design. It records every
event on the host port, on both the 68k side and the DSP side, without
changing how the port behaves. A trace from the Pi can then be compared
with one from Hatari, word by word.

## On the Pi

```
PISTORM_DSP_TRACE=4194304 ./emulator ...
```

| Variable | Meaning |
|---|---|
| `PISTORM_DSP_TRACE` | Size of the trace ring in events, rounded up to a power of two (maximum 16M). Each event takes 24 bytes, so 4194304 events take 96 MB, about 30 s of a busy port. Values below 1024 (for example `1`) give 8192 events. `0` or unset turns tracing off; the port then costs one test per event. |
| `PISTORM_DSP_TRACE_FIRST` | Set to `1` to keep the first events instead of the last. Recording stops when the ring is full, and that dump is written straight away. Use this to capture a program's start without having to time a SIGUSR2. |
| `PISTORM_DSP_TRACE_FILE` | Prefix for dump files. The default is `/tmp/dsptrace`, giving `/tmp/dsptrace-000.txt`, `-001`, and so on. |

The trace is written to a file in three cases:

- **On demand:** run `kill -USR2 <emulator pid>` from another shell. This writes the whole ring. Writing 4M lines takes a few seconds, and the sound stutters while it does.
- **On a stall:** when the 68k has kept polling the port for 0.3 s and nothing has moved either way, the last 512 events are written once per stall.
- **At exit:** the whole ring is written.

While tracing is on, a `[DSPPORT] 5 s:` line is printed every 5 seconds. It shows what each sync point cost:

- RX-read waits;
- empty RX reads, where the 68k got the old word back;
- same-thread DSP runs;
- host-command latency, rewrites while a command was pending, and withdrawals;
- queue depths and dropped words.

Each line of a dump is:

```
seq  ms  frame  type  value  x  dsp-pc
```

The event types are listed at the end of every dump.

## Hatari reference

`hatari-dsptrace.patch` applies to Hatari `f4d65f4` (2026-09-30). It writes
the same events in the same format, plus the 68k pc. Time is in emulated
milliseconds.

```
HATARI_DSPTRACE=/tmp/htrace.txt HATARI_DSPTRACE_MAX=40000000 hatari --machine falcon --dsp emu ...
```

## Comparing

```
tools/dsptrace/dsptrace_cmp.py /tmp/dsptrace-000.txt /tmp/htrace.txt
```

The script compares these streams one at a time:

- 68k→DSP words;
- DSP→68k words;
- words the 68k read;
- host commands asked;
- host commands taken.

It lines each stream up on a run of words that occurs only once in Hatari's
trace, then prints the first word that differs, with both sides' context.
Interleaving between the streams is timing, not data, so the streams are
not compared against each other.
