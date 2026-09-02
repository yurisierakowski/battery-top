# battery-top

An `nmon`-style live terminal dashboard for battery health, CPU, memory,
and disk I/O — a single, dependency-light C program built on `ncurses`.

```
battery-top   Hostname=thinkpad.example.com   Refresh=2s----------17:22:53
System-----------------------------------------------------------------+
hostname  thinkpad.example.com                                         |
model     LENOVO 20S1SHKG00                                            |
os        Debian GNU/Linux 13 (trixie)                                 |
kernel    Linux 6.12.100+deb13-amd64                                   |
uptime    1d 11h 32m    load avg  2.38, 1.80, 1.14                     |
cpu       Intel(R) Core(TM) i5-10310U CPU @ 1.70GHz (8 threads)        |
gpu       Intel Corporation CometLake-U GT2 [UHD Graphics] (rev 02)    |
CPU Utilization----------------------------------------------------------+
cpu        24% (8 cores)   ####................                        |
Memory-------------------------------------------------------------------+
RAM   13.3G /  15.4G   #####...............                            |
Swap   2.9G /  12.0G   #...................                            |
Disks-------------------------------------------------------------------+
nvme0n1  SSD  NVMe  KINGSTON RBUSNS8154P3256GJ1                        |
read   0.0 KB/s   0 IOPS   write   0.0 KB/s   0 IOPS   busy   0%       |
Network-------------------------------------------------------------------+
I/F Name  Recv=KB/s Trans=KB/s   packin  packout  insize outsize Peak->Recv   Trans|
wlp0s20f3*     12.3       4.1      9.0      6.0    143    112       88.4     22.7  |
Battery------------------------------------------------------------------+
power source: on battery                                               |
BAT0  5B10W13906 (Li-poly)                                              |
 97% ##.......................... Discharging                          |
draw 8.41W   voltage 12.48V   remaining 5h 23m                         |
health 92.3%   cycles 110   charge limits 0%-100%                     |
refresh: 2s   q = quit
```

If the terminal is too short to fit every panel, the footer is replaced
with a warning instead of silently clipping content:

```
Warning: Some statistics may not be visible due to limited number of rows!
```

## Features

Panels appear in this order: **System → CPU → Memory → Disks → Network →
Battery**.

- **System** — FQDN hostname, machine model, OS, kernel, uptime, load
  average, CPU model, and GPU.
- **CPU** — one consolidated utilization bar (all cores combined), in the
  same style as the Memory bars, computed live from `/proc/stat` deltas
  between refreshes (the same technique `top` uses).
- **Memory** — RAM and swap usage bars.
- **Disks** — hardware details (model, SSD/HDD, transport) and *live*
  read/write throughput, IOPS, and %busy per physical disk. Deliberately
  shows no space/capacity numbers — this is a hardware + I/O panel, not a
  `df`.
- **Network** — one row per *physically linked* interface, in `nmon`'s own
  I/F table layout: live receive/transmit KB/s, packets/sec in and out,
  average packet size, and the peak receive/transmit rate seen since the
  program started. Wi-Fi interfaces are marked with a trailing `*`.
  "Physically linked" means two things, both checked live: the interface
  must be backed by real hardware (a bridge/veth/vnet/tun interface from
  Docker, libvirt, etc. is excluded, not just loopback), and it must
  currently have a carrier (cable plugged in / radio associated) —
  unplug a cable or the interface simply drops off the list on the next
  refresh.
- **Battery** — per-battery capacity bar, real-time power draw (W),
  voltage, estimated remaining time, health % (vs. design capacity), cycle
  count, and charge thresholds. Reads only real system batteries
  (`/sys/class/power_supply/BAT*`) — a wireless mouse/keyboard reporting
  its own "battery" over HID++ is correctly ignored, since the kernel never
  names those `BAT*`.
- **Row-limit warning** — if the terminal is too short for every panel to
  fit, the footer is replaced with an explicit warning rather than quietly
  clipping content.
- Flicker-free `ncurses` rendering (only the changed cells are redrawn),
  live terminal-resize handling, `q`/`Q` to quit, and Ctrl+C is caught so
  your terminal is always restored on exit.

## Build

Needs a C compiler and the `ncurses` development headers.

| Distro          | Install command                                  |
|------------------|---------------------------------------------------|
| Debian / Ubuntu  | `sudo apt install build-essential libncurses-dev` |
| Fedora / RHEL    | `sudo dnf install gcc ncurses-devel`               |
| Arch             | `sudo pacman -S base-devel ncurses`                |
| Alpine           | `sudo apk add build-base ncurses-dev`              |

Then:

```
make
./battery-top
```

Or directly, without the Makefile:

```
gcc -O2 -Wall -o battery-top battery-top.c -lncurses
```

`make install` installs to `/usr/local/bin` (override with
`make install PREFIX=~/.local`).

## Usage

```
./battery-top [-i seconds]
```

- `-i seconds` — refresh interval (default: 2)
- `q` / `Q` — quit
- Ctrl+C also exits cleanly

## How it works

Everything is sampled live, straight from the kernel, with no daemon and no
shared state:

- **Battery / AC** — `/sys/class/power_supply/*`. The AC adapter is
  identified by `POWER_SUPPLY_TYPE=Mains` (falling back to `USB`/`Wireless`,
  then to common name prefixes), not by guessing a device name, since
  vendors name it very differently (`AC`, `ADP1`, `ACAD`,
  `CROS_USBPD_CHARGER`, ...).
- **CPU** — `/proc/stat`, comparing successive samples (needs one interval
  to produce its first real reading).
- **Memory** — `/proc/meminfo`.
- **Disks** — `/sys/block/*` for hardware attributes (only devices with a
  real `device` symlink are shown, which naturally excludes virtual devices
  like `dm-*`/`md*`/`loop*`); I/O rates come from delta samples of
  `/proc/diskstats`.
- **Network** — interface list from `/sys/class/net/*`, kept only when a
  `device` symlink resolves to a real (non-`/virtual/`) hardware path, and
  Wi-Fi is detected via a `wireless`/`phy80211` subdirectory; each refresh
  also re-checks `carrier` and drops any interface that isn't currently
  linked. Throughput/packet/peak rates come from delta samples of
  `/proc/net/dev` (peaks are tracked in memory and persist for the life of
  the run).
- **System** — `uname(2)`, `/etc/os-release`, `/proc/cpuinfo`,
  `getaddrinfo()` for the FQDN, DMI (`/sys/class/dmi/id/*`) or
  `/proc/device-tree/model` for the machine model. GPU name is the only
  field that shells out to an external tool (`lspci`).

## Compatibility

Built to degrade gracefully rather than crash when something isn't
present, rather than assume one specific machine:

- No battery at all (desktops) — the panel says so instead of erroring.
- No DMI table (ARM/embedded boards) — falls back to
  `/proc/device-tree/model`.
- `/proc/cpuinfo` without an x86-style `model name` field — tries
  `Hardware`, `Model`, `cpu model`, and `Processor` in turn.
- No `lspci` — the GPU field just shows `unknown` instead of failing.
- No network interfaces currently linked, or no physical block devices —
  each panel says so instead of showing an empty table.
- Any `/proc/diskstats` kernel version (the format has only ever had
  fields *appended*, never reordered, since the fields this program reads
  were introduced).

## License

MIT — see [LICENSE](LICENSE).
