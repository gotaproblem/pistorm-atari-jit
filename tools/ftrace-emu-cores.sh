#!/bin/bash
# ftrace-emu-cores.sh - record what the kernel does on the emulator's cores
# (2 = CPU thread, 3 = ipl_task): hard interrupts, IPIs, softirqs, timers,
# task switches. Timestamps use trace_clock mono_raw so they line up with a
# PISTORM_RASTER_TRACE file (its "# clocks" header line).
#
#   sudo tools/ftrace-emu-cores.sh start
#   ... run the emulator ...
#   sudo tools/ftrace-emu-cores.sh stop /home/pistorm/ftrace.txt

T=/sys/kernel/tracing
[ -d "$T/events" ] || T=/sys/kernel/debug/tracing
[ -d "$T/events" ] || { echo "no tracefs (mount -t tracefs nodev /sys/kernel/tracing)"; exit 1; }

case "$1" in
start)
    echo 0 > $T/tracing_on
    echo nop > $T/current_tracer
    echo > $T/trace
    echo mono_raw > $T/trace_clock || { echo "trace_clock mono_raw not available"; exit 1; }
    echo c > $T/tracing_cpumask
    echo 32768 > $T/buffer_size_kb
    echo 0 > $T/events/enable
    on=""
    for e in irq/irq_handler_entry irq/irq_handler_exit irq/softirq_entry irq/softirq_exit \
             ipi/ipi_entry ipi/ipi_exit ipi/ipi_raise \
             timer/hrtimer_expire_entry timer/hrtimer_expire_exit \
             sched/sched_switch; do
        if [ -e $T/events/$e/enable ]; then echo 1 > $T/events/$e/enable; on="$on $e"; fi
    done
    echo 1 > $T/tracing_on
    echo "[FTRACE] on, cores 2-3:$on"
    ;;
stop)
    out=${2:-/home/pistorm/ftrace.txt}
    echo 0 > $T/tracing_on
    cat $T/trace > "$out"
    echo 0 > $T/events/enable
    echo > $T/trace
    echo ff > $T/tracing_cpumask 2>/dev/null
    echo local > $T/trace_clock
    echo "[FTRACE] off, $(grep -vc '^#' "$out") events -> $out"
    ;;
*)
    echo "usage: $0 start | stop [file]"; exit 1 ;;
esac
