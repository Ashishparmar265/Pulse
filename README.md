# pulse

`pulse` is a lightweight, responsive terminal system monitor written in modern C++20. Instead of just showing standard static tables, `pulse` renders real-time scrolling sparkline graphs to show statistical trend histories for CPU usage and Network throughput.

---

## Why Sparkline History?

Static tables of metrics only capture a single instant in time. A CPU spike or network burst can easily go unnoticed, or appear artificially high without context. `pulse` builds dynamic Unicode sparklines in your terminal so you can immediately see the trajectory, frequency, and duration of resource usage over a historical window (e.g. the last 60 seconds).

---

## Backing `/proc` Metrics

`pulse` gathers all data directly from the Linux `/proc` virtual filesystem using idiomatic C++ RAII file streams:

- **CPU Usage**: Reads `/proc/stat`'s aggregate `cpu` line. Computes usage percentage dynamically using active/total time deltas between samples.
- **Memory & Swap**: Parses `/proc/meminfo`.
  - Used Memory is calculated as `MemTotal - MemAvailable` (modern kernels) rather than subtracting Buffers/Cached values.
  - Used Swap is calculated as `SwapTotal - SwapFree`.
- **Network Throughput**: Reads `/proc/net/dev`. Sums incoming (`Receive`) and outgoing (`Transmit`) bytes across all non-loopback network interfaces, computing rates based on high-precision wall-clock time deltas.
- **Disk I/O**: Reads `/proc/diskstats`. Sums sectors read and written across top-level physical block devices found in `/sys/block` (excluding virtual, loop, dm, and zram devices), converting sectors to bytes (512 bytes per sector) to compute read/write bytes/sec.
- **Battery Status**: Checks `/sys/class/power_supply/BAT0` and `BAT1`. Performs a capacity-weighted sum of the battery levels and parses charging state, refreshed at a slower cadence (every 20th cycle).
- **Process Activity**: Scans `/proc/[pid]/` directories.
  - Extracts process names handling space/parentheses boundaries (e.g., matching the first `(` and last `)` in `/proc/[pid]/stat`).
  - Measures process CPU usage via `utime` and `stime` ticks divided by clock speed ticks (`sysconf(_SC_CLK_TCK)`) and elapsed duration.
  - Queries physical memory footprint via `VmRSS` in `/proc/[pid]/status`.


---

## Build Instructions

### Prerequisites (Arch Linux / CachyOS)

To compile and link the application with wide UTF-8 block support, install the base build tools and the wide-ncurses package:

```bash
sudo pacman -S base-devel cmake ncurses
```

### Building with CMake

1. Create a build directory:
   ```bash
   cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
   ```
2. Build the project:
   ```bash
   cmake --build build
   ```
3. Run the binary:
   ```bash
   ./build/pulse
   ```

### Building with Direct `g++` compilation
Alternatively, you can compile the source files directly:
```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic src/main.cpp src/proc_reader.cpp src/renderer.cpp -o pulse -lncursesw
```

---

## Usage

```bash
./pulse [options]
```

### Options
- `-i`, `--interval <seconds>`: Refresh rate interval (default: `1`).
- `-h`, `--history <samples>`: History window length in samples (default: `60`).
- `--help`: Show the command line options.

### Example
Run the monitor with a 2-second refresh interval and a 120-sample history window:
```bash
./pulse --interval 2 --history 120
```

### Interactive Keybindings
- `q`: Quit cleanly and restore normal terminal settings.
- `r`: Reset/clear active sparkline histories and restart the traces.
- `a`: Toggle between the "top 5" summary view and the scrollable "full list" view.
- `j` / `k` or `Arrow Keys`: Scroll selection highlight up and down in the full list view.
- `/`: Enter Search mode. Type to filter processes dynamically by name (case-insensitive).
  - `ESC` inside Search mode clears the filter and returns to normal.
  - `Enter` inside Search mode applies the filter and returns to normal.
- `s`: Cycle the active process list sorting key: **Memory** (descending) -> **CPU** (descending) -> **PID** (ascending) -> **Name** (lexicographical). In tree mode, this sorts sibling nodes at each level of the hierarchy.
- `t`: Toggle the full process list between a flat table and a hierarchical parent-child **Tree View**.
- `Space`: (In tree view) Expand or collapse the selected process branch. Nodes with children show `[+]` (collapsed) or `[-]` (expanded) indicators.
- `Enter`: (In full list view) Open a detailed process card showing PID, name, state, PPID, executable paths, complete arguments list, VmRSS, VmSize, threads, and open file descriptor count.
  - `q` or `ESC` exits the process details card and returns to the process list view cleanly (preserving selection highlight and scroll offset).
- `d` or `Delete`: Trigger inline SIGTERM confirmation for the selected process in the full view.
  - `y` confirms and sends SIGTERM (non-privilege kill failures show errors like "permission denied" or "process no longer exists" in the footer).
  - `n` or `ESC` cancels.
- `Ctrl+C`: Gracefully intercepted by a custom signal handler to ensure clean exit and terminal restoration.


---

## Design Decisions

### Circular Ring Buffer
To record sparkline data efficiently, `pulse` utilizes a custom, template-based circular `RingBuffer<T>` with a dynamic, runtime-configurable capacity. Sizing capacity at runtime allows users to define custom history lengths (`--history`) without re-compilation. Slicing and sequential iterators have been tailored to preserve absolute chronological order regardless of internal array wraparound. All insertion operations run in $O(1)$ constant time with zero heap reallocations.

### Tokyo Night Color Theme
The visual layout features a custom-engineered dark navy background color and a soft light blue-white foreground text matching the popular *Tokyo Night* editor theme. Gauges and sparklines dynamically shift color (Green, Yellow, Red) based on active thresholds (under 50%, 50-80%, and above 80% respectively) relative to usage or maximum history rates. If terminal colors cannot be remapped, it degrades gracefully to standard terminal 8/16-color mappings.
