pid=$(pidof emulator)
sudo perf record -F 99 -g --call-graph fp -p "$pid" -- sleep 10
sudo perf report -g graph,0.5,caller
#sudo perf report
