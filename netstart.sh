sudo ip addr flush dev tap0
sudo ip addr add 192.168.50.1/24 dev tap0
sudo ip link set tap0 up

sudo sysctl -w net.ipv4.ip_forward=1

sudo iptables -t nat -A POSTROUTING -s 192.168.50.0/24 -o wlan0 -j MASQUERADE
sudo iptables -A FORWARD -i tap0 -o wlan0 -j ACCEPT
sudo iptables -A FORWARD -i wlan0 -o tap0 -m state --state RELATED,ESTABLISHED -j ACCEPT
