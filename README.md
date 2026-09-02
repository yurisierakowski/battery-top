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
shell     /bin/bash                                                    |
pkgs      1906 (dpkg)                                                  |
Disks-------------------------------------------------------------------+
nvme0n1  SSD  NVMe  KINGSTON RBUSNS8154P3256GJ1                        |
read   0.0 KB/s   0 IOPS   write   0.0 KB/s   0 IOPS   busy   0%       |
CPU Utilization----------------------------------------------------------+
cpu 0    ####................   19%                                    |
cpu 1    ##..................   12%                                    |
...
Memory-------------------------------------------------------------------+
RAM   13.3G /  15.4G   #####...............                            |
Swap   2.9G /  12.0G   #...................                            |
Battery------------------------------------------------------------------+
power source: on battery                                               |
BAT0  5B10W13906 (Li-poly)                                              |
 97% ##.......................... Discharging                          |
draw 8.41W   voltage 12.48V   remaining 5h 23m                         |
health 92.3%   cycles 110   charge limits 0%-100%                     |
refresh: 2s   q = quit
```

## Features

- **System** — FQDN hostname, machine model, OS, kernel, uptime, load
  average, CPU model, GPU, shell, and installed package count.
- **Disks** — hardware details (model, SSD/HDD, transport) and *live*
  read/write throughput, IOPS, and %busy per physical disk. Deliberately
  shows no space/capacity numbers — this is a hardware + I/O panel, not a
  `df`.
- **CPU** — per-core utilization bars, colored by load, computed live from
  `/proc/stat` deltas between refreshes (the same technique `top` uses).
- **Memory** — RAM and swap usage bars.
- **Battery** — per-battery capacity bar, real-time power draw (W),
  voltage, estimated remaining time, health % (vs. design capacity), cycle
  count, and charge thresholds. Reads only real system batteries
  (`/sys/class/power_supply/BAT*`) — a wireless mouse/keyboard reporting
  its own "battery" over HID++ is correctly ignored, since the kernel never
  names those `BAT*`.
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
- **System** — `uname(2)`, `/etc/os-release`, `/proc/cpuinfo`,
  `getaddrinfo()` for the FQDN, DMI (`/sys/class/dmi/id/*`) or
  `/proc/device-tree/model` for the machine model. GPU name and installed
  package count are the only two fields that shell out to an external tool
  (`lspci`, and whichever package manager is present).

## Compatibility

Built to degrade gracefully rather than crash when something isn't
present, rather than assume one specific machine:

- No battery at all (desktops) — the panel says so instead of erroring.
- No DMI table (ARM/embedded boards) — falls back to
  `/proc/device-tree/model`.
- `/proc/cpuinfo` without an x86-style `model name` field — tries
  `Hardware`, `Model`, `cpu model`, and `Processor` in turn.
- No `lspci`, or no recognized package manager (`dpkg`, `rpm`, `pacman`,
  `apk`, or Portage's `qlist`) — those two fields just show
  `unknown`/`n/a` instead of failing.
- Any `/proc/diskstats` kernel version (the format has only ever had
  fields *appended*, never reordered, since the fields this program reads
  were introduced).

## License

MIT — see [LICENSE](LICENSE).
