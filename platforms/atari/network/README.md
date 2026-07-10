# PiStorm Atari Network Device

This directory holds emulator-side network support for Atari ST / FreeMiNT
networking.

The preferred FreeMiNT path is to expose an ARAnyM-compatible Native Features
`ETHERNET` service and use FreeMiNT's existing `nfeth.xif` driver. That avoids
starting with a new Atari-side driver and lets MiNT-Net own TCP/IP exactly as it
does on ARAnyM.

The fallback/custom path is the simple `PNET` MMIO Ethernet adapter described
below. It is useful as a small PiStorm-specific interface, but it still needs a
matching FreeMiNT `.xif` driver before the guest can use it.

## Activation Model

Adding these files to the Makefile only makes the host-side code compile. The
network is not active until both the emulator side and Atari side are wired up.

For the existing FreeMiNT driver path:

1. The emulator implements the Native Features probe/call opcodes used by
   FreeMiNT: `0x7300` for `NF_GET_ID` and `0x7301` for `NF_CALL`.
2. `NF_GET_ID("ETHERNET")` returns a non-zero feature base when the emulator
   config or `PISTORM_NET` enables the network core.
3. `NF_CALL(base + op, ...)` implements the `nfeth.xif` API:
   `GET_VERSION`, `XIF_INTLEVEL`, `XIF_IRQ`, `XIF_START`, `XIF_STOP`,
   `XIF_READLENGTH`, `XIF_READBLOCK`, `XIF_WRITEBLOCK`, `XIF_GET_MAC`,
   `XIF_GET_IPHOST`, `XIF_GET_IPATARI`, and `XIF_GET_NETMASK`.
4. FreeMiNT must be built/configured with Native Features enabled so
   `KERNEL->nf_ops` is populated at boot.
5. MiNT-Net/`inet4.xdd` must load `nfeth.xif` from the MiNT folder. The driver
   registers `eth0` after it finds the `ETHERNET` feature and API version
   `0x00000005`.

The existing TAP backend in this directory can serve that NatFeat Ethernet
service: `XIF_WRITEBLOCK` sends a guest Ethernet frame to TAP, and
`XIF_READLENGTH`/`XIF_READBLOCK` drain frames received from TAP.

Receive interrupt delivery is still the integration point to finish. The
NatFeat `XIF_IRQ` state is implemented, but the Atari CPU/MFP interrupt path
must still raise the vector that `nfeth.xif` installs before incoming packets
will be delivered asynchronously.

For the custom MMIO path:

1. FreeMiNT owns TCP/IP through `inet4.xdd` and a new `.xif` packet driver.
2. The `.xif` driver talks to this virtual Ethernet adapter over Atari bus MMIO.
3. The emulator moves Ethernet frames between the virtual adapter and a Pi-side
   backend such as Linux TAP, SLIRP, or pcap.
4. The Pi routes/NATs the TAP side through WiFi. Bridging directly to `wlan0`
   is deliberately not the first target because WiFi client mode is often not a
   transparent Ethernet bridge.

## Register Window

Default base: `0x00F10000`

The first implementation keeps two fixed frame windows. Registers are big-endian
from the 68k guest's point of view.

| Offset | Size | Name | Description |
| --- | --- | --- | --- |
| `0x000` | 32 | ID | Always `0x504E4554` (`PNET`). |
| `0x004` | 32 | VERSION | Interface version, currently `1`. |
| `0x008` | 32 | STATUS | Bit 0 enabled, bit 1 link up, bit 2 RX ready, bit 3 TX busy, bit 4 RX dropped. |
| `0x00C` | 32 | IRQ_STATUS | Reserved for a future interrupt-backed path. |
| `0x010` | 32 | MAC_HI | MAC bytes 0..3. |
| `0x014` | 32 | MAC_LO | MAC bytes 4..5 in the high halfword. |
| `0x018` | 32 | MTU | Payload MTU, normally `1500`. |
| `0x020` | 32 | TX_LEN | Guest writes Ethernet frame length. |
| `0x024` | 32 | TX_KICK | Guest writes non-zero to send the TX frame. |
| `0x028` | 32 | RX_LEN | Length of current received Ethernet frame. |
| `0x02C` | 32 | RX_POP | Guest writes non-zero after reading RX frame. |
| `0x100` | bytes | TX_DATA | Ethernet frame to transmit. |
| `0x800` | bytes | RX_DATA | Current received Ethernet frame. |

The window size is `0x1000` bytes. Frames are limited to 1514 bytes, Ethernet
without FCS.

## Integration Notes

The core is intentionally inert until `pistorm_net_init()` is called with
`enabled = true`.

Expected emulator integration points:

- Config parsing: the emulator config file supports:
  `network enabled`, `network_backend tap`, `network_tap tap0`,
  `network_base 0x00F10000`, `network_mac 52:54:50:4e:45:54`,
  `network_irq 4`, `network_host_ip 192.168.50.1`,
  `network_atari_ip 192.168.50.2`, `network_netmask 255.255.255.0`,
  and `network_debug enabled`. TAP is the default backend when `network` is
  enabled; use `network_backend slirp` only for the user-mode SLIRP backend.
  Environment variables with the old `PISTORM_NET_*` names still override the
  config for quick testing.
- TAP address hints exposed through NatFeat are configurable with
  `PISTORM_NET_IP_HOST`, `PISTORM_NET_IP_ATARI`, and `PISTORM_NET_NETMASK`.
  These should match the Pi/TAP side address, Atari address, and subnet mask
  you configure for your LAN or routed TAP setup.
- Startup: `emulator.c` calls `platform_network_init_from_config()` during
  emulator startup.
- Memory dispatch: if `pistorm_net_owns_address(addr)`, call
  `pistorm_net_read()` / `pistorm_net_write()`.
- Polling: call `pistorm_net_poll()` from an existing periodic path. The TAP
  backend currently receives on its own thread, so polling is a no-op placeholder.

Expected FreeMiNT integration:

- Preferred: use FreeMiNT's existing `nfeth.xif` driver once the emulator
  implements Native Features `ETHERNET`.
- Fallback: build a `pnet.xif` Ethernet packet driver for this MMIO register
  map.
- `output()` writes to `TX_DATA`, sets `TX_LEN`, then writes `TX_KICK`.
- The receive path polls `STATUS` bit 2, reads `RX_LEN` and `RX_DATA`, calls
  `eth_remove_hdr()` and `if_input()`, then writes `RX_POP`.
