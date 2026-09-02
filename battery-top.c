/*
 * battery-top - an nmon-style live dashboard for battery health, CPU and
 * memory usage, and basic system/hardware info.
 *
 * Reads live data straight from /proc and /sys/class/power_supply (BAT*
 * only -- peripheral "batteries" such as HID++ mice/keyboards are ignored
 * since the kernel never names them BAT*). No shared state with any other
 * tool; everything is sampled fresh every refresh.
 *
 * Build:  gcc -O2 -Wall -o battery-top battery-top.c -lncurses
 * Usage:  ./battery-top [-i seconds]
 *         press 'q' to quit.
 */

#define _GNU_SOURCE
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <strings.h>

#define PS_DIR "/sys/class/power_supply"
#define MAX_CORES 256
#define MAX_BATT 4

static int g_interval = 2; /* seconds */

/* ---------- small read helpers ---------- */

static long read_long(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void read_str(const char *path, char *buf, size_t n) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(buf, (int)n, f)) {
        size_t l = strlen(buf);
        while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
    }
    fclose(f);
}

static void trim(char *s) {
    size_t l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, l - i + 1);
}

/* ---------- system info (queried once at startup) ---------- */

typedef struct {
    char hostname[256];
    char model[128];
    char os[256];
    char kernel[256];
    char cpu_model[256];
    int  cpu_threads;
    char gpu[256];
    char shell[128];
    char pkgs[64];
} sysinfo_t;

static void run_capture(const char *cmd, char *buf, size_t n) {
    buf[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) return;
    if (fgets(buf, (int)n, p)) trim(buf);
    pclose(p);
}

static void cache_system_info(sysinfo_t *s) {
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(s->kernel, sizeof(s->kernel), "%s %s", u.sysname, u.release);
    } else {
        strcpy(s->kernel, "unknown");
    }

    if (gethostname(s->hostname, sizeof(s->hostname)) != 0) strcpy(s->hostname, "unknown");
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_flags = AI_CANONNAME;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(s->hostname, NULL, &hints, &res) == 0 && res && res->ai_canonname) {
        strncpy(s->hostname, res->ai_canonname, sizeof(s->hostname) - 1);
        s->hostname[sizeof(s->hostname) - 1] = '\0';
    }
    if (res) freeaddrinfo(res);

    /* DMI (sys_vendor/product_name) only exists on x86 systems with a BIOS/
     * UEFI SMBIOS table. ARM/embedded boards (Raspberry Pi and similar)
     * instead expose a single board name via the device tree. */
    char vendor[64] = "", product[64] = "";
    read_str("/sys/class/dmi/id/sys_vendor", vendor, sizeof(vendor));
    read_str("/sys/class/dmi/id/product_name", product, sizeof(product));
    trim(vendor);
    trim(product);
    if (vendor[0] || product[0]) {
        snprintf(s->model, sizeof(s->model), "%s %s", vendor, product);
    } else {
        read_str("/proc/device-tree/model", s->model, sizeof(s->model));
        trim(s->model);
        if (!s->model[0]) strcpy(s->model, "unknown");
    }

    strcpy(s->os, "unknown");
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                char *v = line + 12;
                trim(v);
                size_t l = strlen(v);
                if (l >= 2 && v[0] == '"' && v[l - 1] == '"') { v[l - 1] = '\0'; v++; }
                strncpy(s->os, v, sizeof(s->os) - 1);
                s->os[sizeof(s->os) - 1] = '\0';
                break;
            }
        }
        fclose(f);
    }

    /* x86 exposes "model name"; ARM/other architectures use "Hardware",
     * "Model", "cpu model", or "Processor" instead, depending on kernel
     * version -- try each, in the order most likely to be present. */
    strcpy(s->cpu_model, "unknown");
    static const char *cpu_fields[] = {
        "model name", "Hardware", "Model", "cpu model", "Processor", "cpu\t"
    };
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            for (size_t i = 0; i < sizeof(cpu_fields) / sizeof(cpu_fields[0]); i++) {
                size_t flen = strlen(cpu_fields[i]);
                if (strncmp(line, cpu_fields[i], flen) == 0) {
                    char *v = strchr(line, ':');
                    if (v) {
                        v++;
                        trim(v);
                        if (v[0]) {
                            strncpy(s->cpu_model, v, sizeof(s->cpu_model) - 1);
                            s->cpu_model[sizeof(s->cpu_model) - 1] = '\0';
                        }
                    }
                    break;
                }
            }
            if (strcmp(s->cpu_model, "unknown") != 0) break;
        }
        fclose(f);
    }
    s->cpu_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (s->cpu_threads < 1) s->cpu_threads = 1;

    run_capture("lspci 2>/dev/null | grep -iE 'vga|3d|display' | head -1 | cut -d: -f3-", s->gpu, sizeof(s->gpu));
    if (!s->gpu[0]) strcpy(s->gpu, "unknown");
    trim(s->gpu);

    const char *shell = getenv("SHELL");
    if (!shell) shell = "/bin/sh";
    strncpy(s->shell, shell, sizeof(s->shell) - 1);

    strcpy(s->pkgs, "n/a");
    char out[32];
    if (!system("command -v dpkg-query >/dev/null 2>&1")) {
        run_capture("dpkg-query -f '.\\n' -W 2>/dev/null | wc -l", out, sizeof(out));
        if (out[0]) snprintf(s->pkgs, sizeof(s->pkgs), "%s (dpkg)", out);
    } else if (!system("command -v rpm >/dev/null 2>&1")) {
        run_capture("rpm -qa 2>/dev/null | wc -l", out, sizeof(out));
        if (out[0]) snprintf(s->pkgs, sizeof(s->pkgs), "%s (rpm)", out);
    } else if (!system("command -v pacman >/dev/null 2>&1")) {
        run_capture("pacman -Qq 2>/dev/null | wc -l", out, sizeof(out));
        if (out[0]) snprintf(s->pkgs, sizeof(s->pkgs), "%s (pacman)", out);
    } else if (!system("command -v apk >/dev/null 2>&1")) {
        run_capture("apk info 2>/dev/null | wc -l", out, sizeof(out));
        if (out[0]) snprintf(s->pkgs, sizeof(s->pkgs), "%s (apk)", out);
    } else if (!system("command -v equery >/dev/null 2>&1")) {
        /* Gentoo */
        run_capture("qlist -I 2>/dev/null | wc -l", out, sizeof(out));
        if (out[0]) snprintf(s->pkgs, sizeof(s->pkgs), "%s (portage)", out);
    }
}

/* ---------- CPU utilization ---------- */

typedef struct {
    unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
} cpu_raw_t;

typedef struct {
    double user_pct, sys_pct, wait_pct, idle_pct;
} cpu_pct_t;

/* index 0 = aggregate ("cpu "), 1..ncores = per-core ("cpu0".."cpuN") */
static int read_cpu_raw(cpu_raw_t *out, int max) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        if (!isdigit((unsigned char)line[3]) && line[3] != ' ') break;
        char tag[16];
        cpu_raw_t r = {0};
        int got = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                          tag, &r.user, &r.nice, &r.sys, &r.idle,
                          &r.iowait, &r.irq, &r.softirq, &r.steal);
        if (got < 5) continue;
        out[n++] = r;
    }
    fclose(f);
    return n;
}

static cpu_raw_t g_prev_cpu[MAX_CORES + 1];
static int g_prev_cpu_valid = 0;
static cpu_pct_t g_cpu_pct[MAX_CORES + 1];
static int g_ncores = 0; /* entries in g_cpu_pct beyond index 0 */

static void update_cpu_usage(void) {
    cpu_raw_t cur[MAX_CORES + 1];
    int n = read_cpu_raw(cur, MAX_CORES + 1);
    if (n <= 0) return;
    g_ncores = n - 1;

    for (int i = 0; i < n; i++) {
        if (g_prev_cpu_valid) {
            unsigned long long pu = g_prev_cpu[i].user, pn = g_prev_cpu[i].nice,
                               ps = g_prev_cpu[i].sys, pi = g_prev_cpu[i].idle,
                               piow = g_prev_cpu[i].iowait, pir = g_prev_cpu[i].irq,
                               psir = g_prev_cpu[i].softirq, pst = g_prev_cpu[i].steal;
            unsigned long long du = cur[i].user - pu, dn = cur[i].nice - pn,
                               ds = cur[i].sys - ps, di = cur[i].idle - pi,
                               diow = cur[i].iowait - piow, dir = cur[i].irq - pir,
                               dsir = cur[i].softirq - psir, dst = cur[i].steal - pst;
            unsigned long long total = du + dn + ds + di + diow + dir + dsir + dst;
            if (total > 0) {
                g_cpu_pct[i].user_pct = 100.0 * (du + dn) / total;
                g_cpu_pct[i].sys_pct  = 100.0 * (ds + dir + dsir) / total;
                g_cpu_pct[i].wait_pct = 100.0 * diow / total;
                g_cpu_pct[i].idle_pct = 100.0 * (di + dst) / total;
            } else {
                memset(&g_cpu_pct[i], 0, sizeof(cpu_pct_t));
            }
        } else {
            memset(&g_cpu_pct[i], 0, sizeof(cpu_pct_t));
        }
    }
    memcpy(g_prev_cpu, cur, sizeof(cpu_raw_t) * n);
    g_prev_cpu_valid = 1;
}

/* ---------- memory ---------- */

typedef struct {
    long total_kb, avail_kb, swap_total_kb, swap_free_kb;
} mem_t;

static void read_mem(mem_t *m) {
    memset(m, 0, sizeof(*m));
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char key[64];
    long val;
    while (fscanf(f, "%63s %ld", key, &val) == 2) {
        if (!strcmp(key, "MemTotal:")) m->total_kb = val;
        else if (!strcmp(key, "MemAvailable:")) m->avail_kb = val;
        else if (!strcmp(key, "SwapTotal:")) m->swap_total_kb = val;
        else if (!strcmp(key, "SwapFree:")) m->swap_free_kb = val;
        while (fgetc(f) != '\n' && !feof(f)) {}
    }
    fclose(f);
}

/* ---------- disks (hardware + live I/O, no space usage) ---------- */

#define MAX_DISKS 16

typedef struct {
    char name[32], model[64], transport[16];
    int rotational; /* 0=SSD, 1=HDD, -1=unknown */
} disk_info_t;

typedef struct {
    unsigned long long rd_ios, rd_sectors, wr_ios, wr_sectors, io_ticks_ms;
} disk_raw_t;

typedef struct {
    double read_kBps, write_kBps, read_iops, write_iops, busy_pct;
} disk_rate_t;

static disk_info_t g_disks[MAX_DISKS];
static int g_ndisks = 0;

/* real hardware block devices only: skip loop/ram/zram/dm/md (no "device" link) */
static void cache_disks(void) {
    DIR *d = opendir("/sys/block");
    if (!d) return;
    struct dirent *e;
    while (g_ndisks < MAX_DISKS && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char devpath[300];
        snprintf(devpath, sizeof(devpath), "/sys/block/%s/device", e->d_name);
        char resolved[512];
        ssize_t rl = readlink(devpath, resolved, sizeof(resolved) - 1);
        if (rl < 0) continue; /* no "device" link -> virtual (dm-*, md*, loop*, zram*) */
        resolved[rl] = '\0';

        disk_info_t *di = &g_disks[g_ndisks];
        memset(di, 0, sizeof(*di));
        /* block device names (nvme0n1, sda, ...) are always short; the
         * fixed field just needs safe truncation, which snprintf guarantees. */
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(di->name, sizeof(di->name), "%s", e->d_name);
        #pragma GCC diagnostic pop

        char path[350];
        snprintf(path, sizeof(path), "/sys/block/%s/device/model", e->d_name);
        read_str(path, di->model, sizeof(di->model));
        trim(di->model);
        if (!di->model[0]) strcpy(di->model, "unknown");

        snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", e->d_name);
        di->rotational = (int)read_long(path);

        if (!strncmp(e->d_name, "nvme", 4)) strcpy(di->transport, "NVMe");
        else if (!strncmp(e->d_name, "mmcblk", 6)) strcpy(di->transport, "SD/eMMC");
        else if (strstr(resolved, "/usb")) strcpy(di->transport, "USB");
        else strcpy(di->transport, "SATA/SCSI");

        g_ndisks++;
    }
    closedir(d);
}

static int read_disk_raw(const char *name, disk_raw_t *out) {
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        int maj, min;
        char devname[64];
        unsigned long long rd_ios, rd_merges, rd_sectors, rd_ticks;
        unsigned long long wr_ios, wr_merges, wr_sectors, wr_ticks;
        unsigned long long in_progress, io_ticks;
        /* /proc/diskstats: maj min name rd_ios rd_merges rd_sectors rd_ticks
         * wr_ios wr_merges wr_sectors wr_ticks in_progress io_ticks ...
         * (later kernels append discard/flush fields -- irrelevant here). */
        int got = sscanf(line, "%d %d %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                          &maj, &min, devname, &rd_ios, &rd_merges, &rd_sectors, &rd_ticks,
                          &wr_ios, &wr_merges, &wr_sectors, &wr_ticks, &in_progress, &io_ticks);
        if (got != 13) continue;
        if (!strcmp(devname, name)) {
            out->rd_ios = rd_ios; out->rd_sectors = rd_sectors;
            out->wr_ios = wr_ios; out->wr_sectors = wr_sectors;
            out->io_ticks_ms = io_ticks;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static disk_raw_t g_prev_disk[MAX_DISKS];
static struct timespec g_prev_disk_ts;
static int g_prev_disk_valid = 0;
static disk_rate_t g_disk_rate[MAX_DISKS];

static void update_disk_io(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = 0;
    if (g_prev_disk_valid) {
        elapsed = (now.tv_sec - g_prev_disk_ts.tv_sec) +
                  (now.tv_nsec - g_prev_disk_ts.tv_nsec) / 1e9;
    }

    for (int i = 0; i < g_ndisks; i++) {
        disk_raw_t cur;
        if (!read_disk_raw(g_disks[i].name, &cur)) { memset(&g_disk_rate[i], 0, sizeof(disk_rate_t)); continue; }

        if (g_prev_disk_valid && elapsed > 0) {
            unsigned long long drd_ios = cur.rd_ios - g_prev_disk[i].rd_ios;
            unsigned long long drd_sec = cur.rd_sectors - g_prev_disk[i].rd_sectors;
            unsigned long long dwr_ios = cur.wr_ios - g_prev_disk[i].wr_ios;
            unsigned long long dwr_sec = cur.wr_sectors - g_prev_disk[i].wr_sectors;
            unsigned long long dticks = cur.io_ticks_ms - g_prev_disk[i].io_ticks_ms;

            g_disk_rate[i].read_kBps = (drd_sec * 512.0 / 1024.0) / elapsed;
            g_disk_rate[i].write_kBps = (dwr_sec * 512.0 / 1024.0) / elapsed;
            g_disk_rate[i].read_iops = drd_ios / elapsed;
            g_disk_rate[i].write_iops = dwr_ios / elapsed;
            g_disk_rate[i].busy_pct = (dticks / (elapsed * 1000.0)) * 100.0;
            if (g_disk_rate[i].busy_pct > 100) g_disk_rate[i].busy_pct = 100;
        } else {
            memset(&g_disk_rate[i], 0, sizeof(disk_rate_t));
        }
        g_prev_disk[i] = cur;
    }
    g_prev_disk_ts = now;
    g_prev_disk_valid = 1;
}

/* ---------- battery ---------- */

typedef struct {
    char name[16], model[64], technology[32], status[16];
    int capacity, cycle_count;
    long energy_now, energy_full, energy_full_design;
    long power_now, voltage_now, current_now;
    int start_thresh, end_thresh; /* -1 if unsupported */
} battery_t;

static int read_batteries(battery_t *arr, int max) {
    DIR *d = opendir(PS_DIR);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while (n < max && (e = readdir(d))) {
        if (strncmp(e->d_name, "BAT", 3) != 0) continue;
        battery_t *b = &arr[n];
        memset(b, 0, sizeof(*b));
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(b->name, sizeof(b->name), "%s", e->d_name);
        #pragma GCC diagnostic pop

        char path[512];
        #define BP(field) (snprintf(path, sizeof(path), PS_DIR "/%s/" field, e->d_name), path)

        read_str(BP("model_name"), b->model, sizeof(b->model));
        read_str(BP("technology"), b->technology, sizeof(b->technology));
        read_str(BP("status"), b->status, sizeof(b->status));
        b->capacity = (int)read_long(BP("capacity"));
        b->cycle_count = (int)read_long(BP("cycle_count"));
        b->energy_now = read_long(BP("energy_now"));
        b->energy_full = read_long(BP("energy_full"));
        b->energy_full_design = read_long(BP("energy_full_design"));
        b->power_now = read_long(BP("power_now"));
        b->voltage_now = read_long(BP("voltage_now"));
        b->current_now = read_long(BP("current_now"));

        if (b->power_now <= 0 && b->current_now > 0 && b->voltage_now > 0)
            b->power_now = b->current_now / 1000000 * (b->voltage_now / 1000000);

        if (b->energy_now < 0) {
            b->energy_now = read_long(BP("charge_now"));
            b->energy_full = read_long(BP("charge_full"));
            b->energy_full_design = read_long(BP("charge_full_design"));
        }

        b->start_thresh = (int)read_long(BP("charge_control_start_threshold"));
        b->end_thresh = (int)read_long(BP("charge_control_end_threshold"));
        #undef BP
        n++;
    }
    closedir(d);
    return n;
}

/* An AC/mains supply can be named almost anything depending on vendor and
 * kernel driver (AC, AC0, ADP1, ACAD, CROS_USBPD_CHARGER, ...). The portable
 * way to identify one is POWER_SUPPLY_TYPE=Mains, per the kernel's power
 * supply class documentation -- not the device name. Fall back to the
 * common name prefixes only if no device declares that type. */
static int ac_online(void) {
    DIR *d = opendir(PS_DIR);
    if (!d) return -1;
    struct dirent *e;
    int result = -1;
    int fallback = -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        char type[32];
        snprintf(path, sizeof(path), PS_DIR "/%s/type", e->d_name);
        read_str(path, type, sizeof(type));

        int is_ac_name = (strncmp(e->d_name, "AC", 2) == 0 || strncmp(e->d_name, "ADP", 3) == 0);
        if (!strcasecmp(type, "Mains") || !strcasecmp(type, "USB") || !strcasecmp(type, "Wireless")) {
            snprintf(path, sizeof(path), PS_DIR "/%s/online", e->d_name);
            long v = read_long(path);
            if (v >= 0) { result = (int)v; break; }
        } else if (is_ac_name && fallback < 0) {
            snprintf(path, sizeof(path), PS_DIR "/%s/online", e->d_name);
            long v = read_long(path);
            if (v >= 0) fallback = (int)v;
        }
    }
    closedir(d);
    return result >= 0 ? result : fallback;
}

/* ---------- rendering ---------- */

enum { CP_GREEN = 1, CP_YELLOW, CP_RED, CP_CYAN, CP_BLUE, CP_DIM, CP_TITLE };

static void init_colors(void) {
    start_color();
    use_default_colors();
    init_pair(CP_GREEN, COLOR_GREEN, -1);
    init_pair(CP_YELLOW, COLOR_YELLOW, -1);
    init_pair(CP_RED, COLOR_RED, -1);
    init_pair(CP_CYAN, COLOR_CYAN, -1);
    init_pair(CP_BLUE, COLOR_BLUE, -1);
    init_pair(CP_DIM, COLOR_WHITE, -1);
    init_pair(CP_TITLE, COLOR_CYAN, -1);
}

static int level_color_capacity(int pct) {
    if (pct <= 20) return CP_RED;
    if (pct <= 50) return CP_YELLOW;
    return CP_GREEN;
}

static int level_color_load(int pct) {
    if (pct >= 80) return CP_RED;
    if (pct >= 50) return CP_YELLOW;
    return CP_GREEN;
}

/* single-color bar, e.g. battery capacity or memory usage */
static void draw_simple_bar(int y, int x, int width, int pct, int inverted) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = pct * width / 100;
    int color = inverted ? level_color_load(pct) : level_color_capacity(pct);
    move(y, x);
    attron(COLOR_PAIR(color) | A_BOLD);
    for (int i = 0; i < filled; i++) addch('#');
    attroff(COLOR_PAIR(color) | A_BOLD);
    attron(COLOR_PAIR(CP_DIM) | A_DIM);
    for (int i = filled; i < width; i++) addch('.');
    attroff(COLOR_PAIR(CP_DIM) | A_DIM);
}

static void row_border(int y, int cols) {
    if (cols > 0) mvaddch(y, cols - 1, '|');
}

static void section_rule(int *y, int cols, const char *label) {
    if (*y >= LINES) { (*y)++; return; }
    move(*y, 0);
    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    printw("%s", label);
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);
    int used = (int)strlen(label);
    attron(COLOR_PAIR(CP_DIM));
    for (int i = used; i < cols - 1; i++) addch('-');
    attroff(COLOR_PAIR(CP_DIM));
    mvaddch(*y, cols - 1, '+');
    (*y)++;
}

static const char *human_time(long secs, char *buf, size_t n) {
    if (secs <= 0) { snprintf(buf, n, "n/a"); return buf; }
    long h = secs / 3600, m = (secs % 3600) / 60;
    snprintf(buf, n, "%ldh %02ldm", h, m);
    return buf;
}

static void human_uptime(char *buf, size_t n) {
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0;
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    long secs = (long)up;
    long days = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
    if (days > 0) snprintf(buf, n, "%ldd %ldh %ldm", days, h, m);
    else snprintf(buf, n, "%ldh %ldm", h, m);
}

static void read_loadavg(char *buf, size_t n) {
    FILE *f = fopen("/proc/loadavg", "r");
    buf[0] = '\0';
    if (!f) return;
    double a, b, c;
    if (fscanf(f, "%lf %lf %lf", &a, &b, &c) == 3)
        snprintf(buf, n, "%.2f, %.2f, %.2f", a, b, c);
    fclose(f);
}

static volatile sig_atomic_t g_resized = 0;
static void on_winch(int sig) { (void)sig; g_resized = 1; }

static void on_terminate(int sig) {
    (void)sig;
    endwin();
    _exit(0);
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "i:h")) != -1) {
        switch (opt) {
            case 'i': g_interval = atoi(optarg); if (g_interval < 1) g_interval = 1; break;
            default:
                fprintf(stderr, "Usage: %s [-i seconds]\n", argv[0]);
                return 1;
        }
    }

    sysinfo_t sys;
    cache_system_info(&sys);
    cache_disks();

    signal(SIGWINCH, on_winch);
    signal(SIGINT, on_terminate);
    signal(SIGTERM, on_terminate);

    initscr();
    if (has_colors()) init_colors();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(g_interval * 1000);

    while (1) {
        if (g_resized) {
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
                resizeterm(ws.ws_row, ws.ws_col);
            g_resized = 0;
        }

        update_cpu_usage();
        update_disk_io();
        mem_t mem; read_mem(&mem);
        battery_t batteries[MAX_BATT];
        int nbat = read_batteries(batteries, MAX_BATT);
        int ac = ac_online();

        int cols = COLS, lines = LINES;
        erase();
        int y = 0;

        /* header */
        char timebuf[32];
        time_t now = time(NULL);
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", localtime(&now));
        char left[256];
        snprintf(left, sizeof(left), "battery-top   Hostname=%s   Refresh=%ds", sys.hostname, g_interval);
        move(y, 0);
        attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
        printw("%s", left);
        attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);
        int pad = cols - (int)strlen(left) - (int)strlen(timebuf) - 1;
        attron(COLOR_PAIR(CP_DIM));
        for (int i = 0; i < pad; i++) addch('-');
        attroff(COLOR_PAIR(CP_DIM));
        if (pad >= 0) printw("%s", timebuf);
        y++;
        y++; /* blank */

        /* System info */
        section_rule(&y, cols, "System");
        char uptime_s[64], load_s[64];
        human_uptime(uptime_s, sizeof(uptime_s));
        read_loadavg(load_s, sizeof(load_s));
        struct { const char *k; const char *v; } sysrows[] = {
            {"hostname", sys.hostname}, {"model", sys.model}, {"os", sys.os},
            {"kernel", sys.kernel}, {"uptime", uptime_s}, {"load avg", load_s},
            {"cpu", sys.cpu_model}, {"gpu", sys.gpu}, {"shell", sys.shell}, {"pkgs", sys.pkgs},
        };
        for (size_t i = 0; i < sizeof(sysrows) / sizeof(sysrows[0]) && y < lines - 1; i++, y++) {
            move(y, 0);
            attron(COLOR_PAIR(CP_DIM));
            printw("%-10s", sysrows[i].k);
            attroff(COLOR_PAIR(CP_DIM));
            printw("%.*s", cols - 13, sysrows[i].v);
            row_border(y, cols);
        }
        y++;

        /* Disks: hardware + live I/O (no space/usage info) */
        section_rule(&y, cols, "Disks");
        if (g_ndisks == 0 && y < lines) {
            move(y, 0);
            printw("No physical block devices found");
            row_border(y, cols);
            y++;
        }
        for (int i = 0; i < g_ndisks && y < lines - 1; i++) {
            disk_info_t *di = &g_disks[i];
            disk_rate_t *r = &g_disk_rate[i];
            const char *kind = di->rotational == 0 ? "SSD" : di->rotational == 1 ? "HDD" : "?";

            move(y, 0);
            attron(A_BOLD);
            printw("%-10s", di->name);
            attroff(A_BOLD);
            printw("%-6s %-9s %.*s", kind, di->transport, cols - 30, di->model);
            row_border(y, cols);
            y++;

            if (y < lines) {
                char line[128];
                snprintf(line, sizeof(line), "  read  %8.1f KB/s %6.0f IOPS", r->read_kBps, r->read_iops);
                move(y, 0);
                printw("%.*s", cols - 1, line);
                row_border(y, cols);
                y++;
            }
            if (y < lines) {
                char line[128];
                snprintf(line, sizeof(line), "  write %8.1f KB/s %6.0f IOPS", r->write_kBps, r->write_iops);
                move(y, 0);
                printw("%.*s", cols - 1, line);
                row_border(y, cols);
                y++;
            }
            if (y < lines) {
                int busy = (int)(r->busy_pct + 0.5);
                int busy_bar_x = 9; /* after "  busy   " */
                int busy_bar_w = cols - busy_bar_x - 8;
                if (busy_bar_w < 6) busy_bar_w = 6;
                move(y, 0);
                printw("  busy");
                draw_simple_bar(y, busy_bar_x, busy_bar_w, busy, 1);
                mvprintw(y, busy_bar_x + busy_bar_w + 1, "%3d%%", busy);
                row_border(y, cols);
                y++;
            }
        }
        y++;

        /* CPU Utilization */
        section_rule(&y, cols, "CPU Utilization");
        int cpu_bar_x = 9; /* after "cpu 123 " */
        int cpu_bar_w = cols - cpu_bar_x - 8; /* leave room for " 100%" + border */
        if (cpu_bar_w < 10) cpu_bar_w = 10;

        for (int c = 1; c <= g_ncores && y < lines - 1; c++, y++) {
            cpu_pct_t *p = &g_cpu_pct[c];
            int pct = (int)(p->user_pct + p->sys_pct + p->wait_pct + 0.5);
            if (pct > 100) pct = 100;
            move(y, 0);
            printw("cpu %-3d ", c - 1);
            draw_simple_bar(y, cpu_bar_x, cpu_bar_w, pct, 1);
            mvprintw(y, cpu_bar_x + cpu_bar_w + 1, "%3d%%", pct);
            row_border(y, cols);
        }
        if (y < lines) {
            cpu_pct_t *p = &g_cpu_pct[0];
            int pct = (int)(p->user_pct + p->sys_pct + p->wait_pct + 0.5);
            if (pct > 100) pct = 100;
            move(y, 0);
            attron(A_BOLD);
            printw("avg     ");
            attroff(A_BOLD);
            draw_simple_bar(y, cpu_bar_x, cpu_bar_w, pct, 1);
            attron(A_BOLD);
            mvprintw(y, cpu_bar_x + cpu_bar_w + 1, "%3d%%", pct);
            attroff(A_BOLD);
            row_border(y, cols);
            y++;
        }
        y++;

        /* Memory */
        section_rule(&y, cols, "Memory");
        if (mem.total_kb > 0 && y < lines) {
            long used_kb = mem.total_kb - mem.avail_kb;
            int mem_pct = (int)(100.0 * used_kb / mem.total_kb);
            move(y, 0);
            printw("%-10s%6.1fG / %5.1fG  ", "RAM", used_kb / 1024.0 / 1024.0, mem.total_kb / 1024.0 / 1024.0);
            draw_simple_bar(y, 30, cols - 32, mem_pct, 1);
            row_border(y, cols);
            y++;
        }
        if (mem.swap_total_kb > 0 && y < lines) {
            long swap_used = mem.swap_total_kb - mem.swap_free_kb;
            int swap_pct = (int)(100.0 * swap_used / mem.swap_total_kb);
            move(y, 0);
            printw("%-10s%6.1fG / %5.1fG  ", "Swap", swap_used / 1024.0 / 1024.0, mem.swap_total_kb / 1024.0 / 1024.0);
            draw_simple_bar(y, 30, cols - 32, swap_pct, 1);
            row_border(y, cols);
            y++;
        }
        y++;

        /* Battery */
        section_rule(&y, cols, "Battery");
        if (ac >= 0 && y < lines) {
            move(y, 0);
            printw("power source: ");
            attron(COLOR_PAIR(ac ? CP_GREEN : CP_YELLOW) | A_BOLD);
            printw("%s", ac ? "plugged in" : "on battery");
            attroff(COLOR_PAIR(ac ? CP_GREEN : CP_YELLOW) | A_BOLD);
            row_border(y, cols);
            y++;
        }
        if (nbat == 0 && y < lines) {
            move(y, 0);
            printw("No system battery (BAT*) found");
            row_border(y, cols);
            y++;
        }
        for (int i = 0; i < nbat && y < lines - 4; i++) {
            battery_t *b = &batteries[i];
            move(y, 0);
            attron(A_BOLD);
            printw("%s", b->name);
            attroff(A_BOLD);
            printw("  %s (%s)", b->model, b->technology);
            row_border(y, cols);
            y++;

            int batt_bar_w = cols - 5 - 14;
            if (batt_bar_w < 10) batt_bar_w = 10;
            if (batt_bar_w > 40) batt_bar_w = 40;

            move(y, 0);
            printw("%3d%% ", b->capacity);
            draw_simple_bar(y, 5, batt_bar_w, b->capacity, 0);
            move(y, 5 + batt_bar_w + 1);
            int scolor = !strcmp(b->status, "Charging") ? CP_GREEN :
                         !strcmp(b->status, "Full") ? CP_CYAN : CP_YELLOW;
            attron(COLOR_PAIR(scolor));
            printw("%s", b->status);
            attroff(COLOR_PAIR(scolor));
            row_border(y, cols);
            y++;

            double watts = b->power_now > 0 ? b->power_now / 1000000.0 : -1;
            double volts = b->voltage_now > 0 ? b->voltage_now / 1000000.0 : -1;
            char remaining[48] = "n/a";
            if (b->power_now > 0 && b->energy_now >= 0) {
                if (!strcmp(b->status, "Discharging")) {
                    long secs = (long)((double)b->energy_now / b->power_now * 3600);
                    human_time(secs, remaining, sizeof(remaining));
                } else if (!strcmp(b->status, "Charging") && b->energy_full > 0) {
                    long secs = (long)((double)(b->energy_full - b->energy_now) / b->power_now * 3600);
                    char tmp[32];
                    human_time(secs, tmp, sizeof(tmp));
                    snprintf(remaining, sizeof(remaining), "%s to full", tmp);
                }
            }
            move(y, 0);
            if (watts >= 0) printw("draw %.2fW   ", watts); else printw("draw n/a     ");
            if (volts >= 0) printw("voltage %.2fV   ", volts); else printw("voltage n/a       ");
            printw("remaining %s", remaining);
            row_border(y, cols);
            y++;

            double health = (b->energy_full > 0 && b->energy_full_design > 0)
                ? 100.0 * b->energy_full / b->energy_full_design : -1;
            move(y, 0);
            if (health >= 0) printw("health %.1f%%   ", health); else printw("health n/a     ");
            printw("cycles %d", b->cycle_count);
            if (b->start_thresh >= 0 && b->end_thresh >= 0)
                printw("   charge limits %d%%-%d%%", b->start_thresh, b->end_thresh);
            row_border(y, cols);
            y++;
        }

        if (lines > 0) {
            move(lines - 1, 0);
            attron(COLOR_PAIR(CP_DIM));
            printw("refresh: %ds   q = quit", g_interval);
            attroff(COLOR_PAIR(CP_DIM));
        }

        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;
    }

    endwin();
    return 0;
}
