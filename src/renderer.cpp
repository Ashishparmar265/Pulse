#include "renderer.hpp"
#include <curses.h>
#include <clocale>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <ctime>

void Renderer::init() {
    // Set locale to support wide character rendering (UTF-8 blocks)
    std::setlocale(LC_ALL, "");

    // Initialize ncurses screen
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0); // Hide the text cursor

    // Set up colors
    if (has_colors()) {
        start_color();
        use_default_colors();

        if (can_change_color()) {
            // Save original colors for color ID 17 and 195 to restore on exit
            short r = 0, g = 0, b = 0;
            if (color_content(17, &r, &g, &b) != ERR) {
                original_colors_[17] = OriginalColor{r, g, b};
            }
            if (color_content(195, &r, &g, &b) != ERR) {
                original_colors_[195] = OriginalColor{r, g, b};
            }

            // Set custom navy background and soft light blue-white foreground
            init_color(17, 39, 54, 90);       // Dark navy background
            init_color(195, 820, 956, 1000);   // Soft light blue-white
            colors_changed_ = true;

            bg_color_ = 17;
            fg_default_ = 195;
        } else {
            // Fall back to standard ncurses colors
            bg_color_ = COLOR_BLACK;
            fg_default_ = COLOR_WHITE;
        }

        // Initialize color pairs
        init_pair(1, fg_default_, bg_color_);      // Default text
        init_pair(2, COLOR_GREEN, bg_color_);     // Under 50% / Healthy battery
        init_pair(3, COLOR_YELLOW, bg_color_);    // 50% to 80% / Mid battery
        init_pair(4, COLOR_RED, bg_color_);       // Above 80% / Low battery
        init_pair(5, COLOR_CYAN, bg_color_);      // Cyan (Borders/Accents)
    }

    // Set default background of the main terminal window
    if (has_colors()) {
        bkgd(COLOR_PAIR(1));
    }
}

void Renderer::shutdown() {
    // Restore the user's terminal original colors if we changed them
    if (colors_changed_) {
        for (const auto& [color_id, orig] : original_colors_) {
            init_color(color_id, orig.r, orig.g, orig.b);
        }
    }
    curs_set(1); // Restore the cursor
    endwin();    // End ncurses mode
}

std::string Renderer::makeSparkline(const RingBuffer<double>& history, double max_val, size_t target_width) {
    const char* blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    std::string sparkline = "";
    size_t samples_to_draw = std::min(history.size(), target_width);

    // Padding (leftmost side flat blocks) for startup case
    if (samples_to_draw < target_width) {
        size_t padding = target_width - samples_to_draw;
        for (size_t p = 0; p < padding; ++p) {
            sparkline += blocks[0];
        }
    }

    // Append the most recent samples
    size_t start_idx = history.size() - samples_to_draw;
    for (size_t i = start_idx; i < history.size(); ++i) {
        double val = history[i];
        int idx = 0;
        if (max_val > 1e-9) {
            idx = static_cast<int>((val / max_val) * 7.99);
            idx = std::clamp(idx, 0, 7);
        }
        sparkline += blocks[idx];
    }
    return sparkline;
}

void Renderer::draw(double cpuPercent,
                    const RingBuffer<double>& cpuHistory,
                    const MemInfo& mem,
                    const NetStats& net,
                    const RingBuffer<double>& rxHistory,
                    const RingBuffer<double>& txHistory,
                    const std::vector<TreeRow>& processes,
                    bool showAll,
                    pid_t selectedPid,
                    int scrollOffset,
                    Mode mode,
                    const std::string& searchQuery,
                    const std::string& statusMessage,
                    SortKey activeSort,
                    const ProcessDetails& details,
                    DiskStats diskStats,
                    const RingBuffer<double>& rdHistory,
                    const RingBuffer<double>& wrHistory,
                    std::optional<BatteryInfo> battery,
                    bool treeMode) {
    int height, width;
    getmaxyx(stdscr, height, width);

    // Completely clear the window first
    clear();

    // Enforce minimum terminal size constraints (increased h constraint for new metrics layout)
    const int min_h = 18;
    const int min_w = 55;
    if (height < min_h || width < min_w) {
        attron(COLOR_PAIR(4) | A_BOLD);
        mvprintw(height / 2, std::max(0, (width - 19) / 2), "Terminal too small");
        attroff(COLOR_PAIR(4) | A_BOLD);
        refresh();
        return;
    }

    // 1. Draw border box
    attron(COLOR_PAIR(5));
    box(stdscr, 0, 0);
    mvprintw(0, 2, " pulse ");
    attroff(COLOR_PAIR(5));

    // Draw current time in top-right corner of the border
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm* local = std::localtime(&now_time);
    char time_str[16];
    std::strftime(time_str, sizeof(time_str), "%H:%M:%S", local);
    
    attron(COLOR_PAIR(1));
    mvprintw(0, std::max(0, width - 12), " %s ", time_str);

    int row = 2; // starting row index

    // 2. CPU Row (sparkline and percent text)
    int avail_spark_w = width - 22;
    int cpu_spark_w = std::clamp(avail_spark_w, 10, static_cast<int>(cpuHistory.capacity()));

    short cpu_color = 2; // green
    if (cpuPercent >= 80.0) {
        cpu_color = 4; // red
    } else if (cpuPercent >= 50.0) {
        cpu_color = 3; // yellow
    }

    mvprintw(row, 4, "CPU  [");
    std::string cpu_spark = makeSparkline(cpuHistory, 100.0, cpu_spark_w);
    attron(COLOR_PAIR(cpu_color));
    waddstr(stdscr, cpu_spark.c_str());
    attroff(COLOR_PAIR(cpu_color));
    attron(COLOR_PAIR(1));
    waddstr(stdscr, "] ");

    std::stringstream cpu_ss;
    cpu_ss << std::fixed << std::setprecision(2) << cpuPercent << "%";
    attron(COLOR_PAIR(cpu_color) | A_BOLD);
    waddstr(stdscr, cpu_ss.str().c_str());
    attroff(COLOR_PAIR(cpu_color) | A_BOLD);

    row += 2; // add spacing

    // 3. MEM and SWAP rows
    // MEM
    double mem_used_gb = mem.usedKB / (1024.0 * 1024.0);
    double mem_total_gb = mem.totalKB / (1024.0 * 1024.0);
    double mem_pct = 0.0;
    if (mem.totalKB > 0) {
        mem_pct = (static_cast<double>(mem.usedKB) / mem.totalKB) * 100.0;
    }
    short mem_color = 2;
    if (mem_pct >= 80.0) mem_color = 4;
    else if (mem_pct >= 50.0) mem_color = 3;

    mvprintw(row, 4, "MEM  [");
    int mem_filled = std::clamp(static_cast<int>(mem_pct / 5.0), 0, 20);
    attron(COLOR_PAIR(mem_color));
    for (int j = 0; j < 20; ++j) {
        if (j < mem_filled) {
            waddstr(stdscr, "█");
        } else {
            waddstr(stdscr, "░");
        }
    }
    attroff(COLOR_PAIR(mem_color));
    attron(COLOR_PAIR(1));
    waddstr(stdscr, "] ");

    std::stringstream mem_ss;
    mem_ss << std::fixed << std::setprecision(2) << mem_used_gb << " GB / " << mem_total_gb << " GB (" << mem_pct << "%)";
    waddstr(stdscr, mem_ss.str().c_str());

    row++;

    // SWAP
    double swap_used_gb = mem.swapUsedKB / (1024.0 * 1024.0);
    double swap_total_gb = mem.swapTotalKB / (1024.0 * 1024.0);
    double swap_pct = 0.0;
    if (mem.swapTotalKB > 0) {
        swap_pct = (static_cast<double>(mem.swapUsedKB) / mem.swapTotalKB) * 100.0;
    }
    short swap_color = 2;
    if (swap_pct >= 80.0) swap_color = 4;
    else if (swap_pct >= 50.0) swap_color = 3;

    mvprintw(row, 4, "SWAP [");
    int swap_filled = std::clamp(static_cast<int>(swap_pct / 5.0), 0, 20);
    attron(COLOR_PAIR(swap_color));
    for (int j = 0; j < 20; ++j) {
        if (j < swap_filled) {
            waddstr(stdscr, "█");
        } else {
            waddstr(stdscr, "░");
        }
    }
    attroff(COLOR_PAIR(swap_color));
    attron(COLOR_PAIR(1));
    waddstr(stdscr, "] ");

    std::stringstream swap_ss;
    swap_ss << std::fixed << std::setprecision(2) << swap_used_gb << " GB / " << swap_total_gb << " GB (" << swap_pct << "%)";
    waddstr(stdscr, swap_ss.str().c_str());

    row += 2; // add spacing

    // 4. NET row
    int net_spark_w = std::clamp((width - 52) / 2, 5, static_cast<int>(rxHistory.capacity()));

    auto find_max_rate = [](const RingBuffer<double>& history) {
        double mx = 0.0;
        for (size_t i = 0; i < history.size(); ++i) {
            if (history[i] > mx) mx = history[i];
        }
        return mx;
    };
    double max_rx = find_max_rate(rxHistory);
    double max_tx = find_max_rate(txHistory);

    short rx_color = 2;
    if (max_rx > 1e-9) {
        double rx_pct = (net.rxBytesSec / max_rx) * 100.0;
        if (rx_pct >= 80.0) rx_color = 4;
        else if (rx_pct >= 50.0) rx_color = 3;
    }
    short tx_color = 2;
    if (max_tx > 1e-9) {
        double tx_pct = (net.txBytesSec / max_tx) * 100.0;
        if (tx_pct >= 80.0) tx_color = 4;
        else if (tx_pct >= 50.0) tx_color = 3;
    }

    mvprintw(row, 4, "NET  RX: [");
    std::string rx_spark = makeSparkline(rxHistory, max_rx, net_spark_w);
    attron(COLOR_PAIR(rx_color));
    waddstr(stdscr, rx_spark.c_str());
    attroff(COLOR_PAIR(rx_color));
    attron(COLOR_PAIR(1));
    waddstr(stdscr, "] ");

    auto format_rate = [](double bytesSec) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1);
        if (bytesSec >= 1024.0 * 1024.0) {
            ss << (bytesSec / (1024.0 * 1024.0)) << " MB/s";
        } else {
            ss << (bytesSec / 1024.0) << " KB/s";
        }
        return ss.str();
    };

    attron(COLOR_PAIR(rx_color) | A_BOLD);
    waddstr(stdscr, format_rate(net.rxBytesSec).c_str());
    attroff(COLOR_PAIR(rx_color) | A_BOLD);

    attron(COLOR_PAIR(1));
    waddstr(stdscr, "  TX: [");
    std::string tx_spark = makeSparkline(txHistory, max_tx, net_spark_w);
    attron(COLOR_PAIR(tx_color));
    waddstr(stdscr, tx_spark.c_str());
    attroff(COLOR_PAIR(tx_color));
    attron(COLOR_PAIR(1));
    waddstr(stdscr, "] ");
    attron(COLOR_PAIR(tx_color) | A_BOLD);
    waddstr(stdscr, format_rate(net.txBytesSec).c_str());
    attroff(COLOR_PAIR(tx_color) | A_BOLD);

    row += 2; // spacing

    // 5. DISK row
    int disk_spark_w = std::clamp((width - 52) / 2, 5, static_cast<int>(rdHistory.capacity()));
    double max_rd = find_max_rate(rdHistory);
    double max_wr = find_max_rate(wrHistory);

    short rd_color = 2;
    if (max_rd > 1e-9) {
        double rd_pct = (diskStats.readBytesSec / max_rd) * 100.0;
        if (rd_pct >= 80.0) rd_color = 4;
        else if (rd_pct >= 50.0) rd_color = 3;
    }
    short wr_color = 2;
    if (max_wr > 1e-9) {
        double wr_pct = (diskStats.writeBytesSec / max_wr) * 100.0;
        if (wr_pct >= 80.0) wr_color = 4;
        else if (wr_pct >= 50.0) wr_color = 3;
    }

    mvprintw(row, 4, "DISK RD: [");
    std::string rd_spark = makeSparkline(rdHistory, max_rd, disk_spark_w);
    attron(COLOR_PAIR(rd_color));
    waddstr(stdscr, rd_spark.c_str());
    attroff(COLOR_PAIR(rd_color));
    attron(COLOR_PAIR(1));
    waddstr(stdscr, "] ");

    attron(COLOR_PAIR(rd_color) | A_BOLD);
    waddstr(stdscr, format_rate(diskStats.readBytesSec).c_str());
    attroff(COLOR_PAIR(rd_color) | A_BOLD);

    attron(COLOR_PAIR(1));
    waddstr(stdscr, "  WR: [");
    std::string wr_spark = makeSparkline(wrHistory, max_wr, disk_spark_w);
    attron(COLOR_PAIR(wr_color));
    waddstr(stdscr, wr_spark.c_str());
    attroff(COLOR_PAIR(wr_color));
    attron(COLOR_PAIR(1));
    waddstr(stdscr, "] ");
    attron(COLOR_PAIR(wr_color) | A_BOLD);
    waddstr(stdscr, format_rate(diskStats.writeBytesSec).c_str());
    attroff(COLOR_PAIR(wr_color) | A_BOLD);

    row += 2; // spacing

    // 6. BATTERY row (if present)
    if (battery.has_value()) {
        const auto& bat = battery.value();
        short bat_color = 2; // green
        if (bat.capacity < 20.0) {
            bat_color = 4; // red
        } else if (bat.capacity < 50.0) {
            bat_color = 3; // yellow
        }

        mvprintw(row, 4, "BATT [");
        int bat_filled = std::clamp(static_cast<int>(bat.capacity / 5.0), 0, 20);
        attron(COLOR_PAIR(bat_color));
        for (int j = 0; j < 20; ++j) {
            if (j < bat_filled) {
                waddstr(stdscr, "█");
            } else {
                waddstr(stdscr, "░");
            }
        }
        attroff(COLOR_PAIR(bat_color));
        attron(COLOR_PAIR(1));
        waddstr(stdscr, "] ");

        std::stringstream bat_ss;
        bat_ss << std::fixed << std::setprecision(1) << bat.capacity << "% (" << bat.status << ")";
        attron(COLOR_PAIR(bat_color) | A_BOLD);
        waddstr(stdscr, bat_ss.str().c_str());
        attroff(COLOR_PAIR(bat_color) | A_BOLD);

        row += 2; // spacing
    }

    // 7. PROCESSES section title
    std::string sort_str = "";
    if (activeSort == SortKey::Memory) sort_str = "sort: mem";
    else if (activeSort == SortKey::Cpu) sort_str = "sort: cpu";
    else if (activeSort == SortKey::Pid) sort_str = "sort: pid";
    else if (activeSort == SortKey::Name) sort_str = "sort: name";

    attron(COLOR_PAIR(5) | A_BOLD);
    if (mode == Mode::Details) {
        mvprintw(row, 4, "PROCESS DETAILS");
    } else {
        if (showAll) {
            if (treeMode) {
                mvprintw(row, 4, "PROCESSES (ALL - TREE - %s)", sort_str.c_str());
            } else {
                mvprintw(row, 4, "PROCESSES (ALL - FLAT - %s)", sort_str.c_str());
            }
        } else {
            mvprintw(row, 4, "TOP PROCESSES (TOP 5 - %s)", sort_str.c_str());
        }
    }
    attroff(COLOR_PAIR(5) | A_BOLD);
    row++;

    int name_col_w = std::max(15, width - 38);

    if (mode == Mode::Details) {
        // Draw underline
        attron(COLOR_PAIR(1) | A_UNDERLINE);
        mvprintw(row, 4, "%-*s", width - 8, "");
        attroff(COLOR_PAIR(1) | A_UNDERLINE);
        row++;

        attron(COLOR_PAIR(1));
        if (!details.exists) {
            attron(COLOR_PAIR(4) | A_BOLD);
            mvprintw(row, 4, "Process %d no longer exists.", selectedPid);
            attroff(COLOR_PAIR(4) | A_BOLD);
        } else {
            mvprintw(row, 4, "PID:          %d", details.pid);
            mvprintw(row + 1, 4, "Name:         %s", details.name.c_str());
            mvprintw(row + 2, 4, "State:        %s", details.state.c_str());
            mvprintw(row + 3, 4, "Parent PID:   %d", details.ppid);

            // Clip EXE Path to width safely
            std::string exe = details.exePath;
            if (exe.length() > static_cast<size_t>(width - 22)) {
                exe = exe.substr(0, width - 25) + "...";
            }
            mvprintw(row + 4, 4, "Executable:   %s", exe.c_str());

            // Clip cmdline to width safely
            std::string cmd = details.cmdline;
            if (cmd.length() > static_cast<size_t>(width - 22)) {
                cmd = cmd.substr(0, width - 25) + "...";
            }
            mvprintw(row + 5, 4, "Command Line: %s", cmd.c_str());

            // Memory sizes
            std::stringstream mem_ss;
            mem_ss << std::fixed << std::setprecision(1)
                   << "VmRSS: " << (details.vmRSS / 1024.0) << " MB  |  VmSize: " << (details.vmSize / 1024.0) << " MB";
            mvprintw(row + 6, 4, "Memory:       %s", mem_ss.str().c_str());

            // Threads
            mvprintw(row + 7, 4, "Threads:      %d", details.threads);

            // FDs count
            std::string fd_str = "";
            if (details.fdCount < 0) {
                fd_str = "[permission denied]";
            } else {
                fd_str = std::to_string(details.fdCount) + " open files";
            }
            mvprintw(row + 8, 4, "Open FDs:     %s", fd_str.c_str());
        }
        attroff(COLOR_PAIR(1));
    } else {
        attron(COLOR_PAIR(1) | A_UNDERLINE);
        std::stringstream header_ss;
        header_ss << std::left 
                  << std::setw(8) << "PID"
                  << std::setw(name_col_w) << "NAME"
                  << std::right
                  << std::setw(10) << "CPU%"
                  << std::setw(12) << "RSS";
        mvprintw(row, 4, "%s", header_ss.str().c_str());
        attroff(COLOR_PAIR(1) | A_UNDERLINE);
        row++;

        // Calculate dynamic rows visible using exact layout logic
        int num_visible_rows = (height - 3) - row + 1;
        
        if (showAll) {
            int start = scrollOffset;
            int end = std::min(static_cast<int>(processes.size()), start + num_visible_rows);

            for (int i = start; i < end; ++i) {
                const auto& proc = processes[i];
                bool is_selected = (proc.info.pid == selectedPid);

                // Format the name column dynamically for tree nodes
                std::string p_name = "";
                if (treeMode) {
                    p_name = proc.prefix;
                    if (proc.has_children) {
                        p_name += (proc.collapsed ? "[+] " : "[-] ");
                    } else {
                        p_name += "    ";
                    }
                }
                p_name += proc.info.name;

                if (p_name.length() > static_cast<size_t>(name_col_w - 3)) {
                    p_name = p_name.substr(0, name_col_w - 3) + "...";
                }
                
                std::stringstream cpu_proc;
                cpu_proc << std::fixed << std::setprecision(1) << proc.info.cpuPercent << "%";

                std::stringstream rss_proc;
                rss_proc << std::fixed << std::setprecision(1) << (proc.info.vmRSS / 1024.0) << " MB";

                std::stringstream proc_ss;
                proc_ss << std::left
                        << std::setw(8) << proc.info.pid
                        << std::setw(name_col_w) << p_name
                        << std::right
                        << std::setw(10) << cpu_proc.str()
                        << std::setw(12) << rss_proc.str();

                if (is_selected) {
                    attron(COLOR_PAIR(5) | A_REVERSE);
                } else {
                    attron(COLOR_PAIR(1));
                }
                mvprintw(row, 4, "%s", proc_ss.str().c_str());
                if (is_selected) {
                    attroff(COLOR_PAIR(5) | A_REVERSE);
                } else {
                    attroff(COLOR_PAIR(1));
                }
                row++;
            }
        } else {
            int end = std::min(5, static_cast<int>(processes.size()));
            attron(COLOR_PAIR(1));
            for (int i = 0; i < end; ++i) {
                const auto& proc = processes[i];
                std::string p_name = proc.info.name;
                if (p_name.length() > static_cast<size_t>(name_col_w - 3)) {
                    p_name = p_name.substr(0, name_col_w - 3) + "...";
                }
                
                std::stringstream cpu_proc;
                cpu_proc << std::fixed << std::setprecision(1) << proc.info.cpuPercent << "%";

                std::stringstream rss_proc;
                rss_proc << std::fixed << std::setprecision(1) << (proc.info.vmRSS / 1024.0) << " MB";

                std::stringstream proc_ss;
                proc_ss << std::left
                        << std::setw(8) << proc.info.pid
                        << std::setw(name_col_w) << p_name
                        << std::right
                        << std::setw(10) << cpu_proc.str()
                        << std::setw(12) << rss_proc.str();

                mvprintw(row, 4, "%s", proc_ss.str().c_str());
                row++;
            }
            attroff(COLOR_PAIR(1));
        }
    }

    // 8. Footer Rendering
    if (!statusMessage.empty()) {
        attron(COLOR_PAIR(4) | A_BOLD | A_REVERSE);
        mvprintw(height - 2, 2, "                                                                               ");
        mvprintw(height - 2, 2, " ERROR: %s ", statusMessage.c_str());
        attroff(COLOR_PAIR(4) | A_BOLD | A_REVERSE);
    } else if (mode == Mode::Search) {
        attron(COLOR_PAIR(5) | A_REVERSE);
        mvprintw(height - 2, 2, "                                                                               ");
        mvprintw(height - 2, 2, " ESC cancel  ENTER apply | Filter: %s_", searchQuery.c_str());
        attroff(COLOR_PAIR(5) | A_REVERSE);
    } else if (mode == Mode::ConfirmKill) {
        attron(COLOR_PAIR(4) | A_BOLD | A_REVERSE);
        mvprintw(height - 2, 2, "                                                                               ");
        mvprintw(height - 2, 2, " %s ", searchQuery.c_str());
        attroff(COLOR_PAIR(4) | A_BOLD | A_REVERSE);
    } else if (mode == Mode::Details) {
        attron(COLOR_PAIR(5) | A_REVERSE);
        mvprintw(height - 2, 2, "                                                                               ");
        mvprintw(height - 2, 2, " ESC/q back ");
        attroff(COLOR_PAIR(5) | A_REVERSE);
    } else {
        attron(COLOR_PAIR(5) | A_REVERSE);
        mvprintw(height - 2, 2, "                                                                               ");
        if (showAll) {
            if (treeMode) {
                mvprintw(height - 2, 2, " q quit  r reset  a top-5 view  / search  s sort (%s)  t flat  SPACE collapse  ENTER details ", sort_str.c_str());
            } else {
                mvprintw(height - 2, 2, " q quit  r reset  a top-5 view  / search  s sort (%s)  t tree  d kill  ENTER details ", sort_str.c_str());
            }
        } else {
            mvprintw(height - 2, 2, " q quit  r reset  a full view  / search  s sort (%s) ", sort_str.c_str());
        }
        attroff(COLOR_PAIR(5) | A_REVERSE);
    }

    // Refresh display
    refresh();
}
