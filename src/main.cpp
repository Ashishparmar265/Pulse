#include "proc_reader.hpp"
#include "history.hpp"
#include "renderer.hpp"
#include <csignal>
#include <atomic>
#include <curses.h>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cerrno>

std::atomic<bool> exit_requested{false};

void sigint_handler(int sig) {
    (void)sig; // Suppress unused parameter warning
    exit_requested = true;
}

// Case-insensitive substring search helper
bool containsIgnoreCase(const std::string& str, const std::string& search) {
    if (search.empty()) return true;
    auto it = std::search(
        str.begin(), str.end(),
        search.begin(), search.end(),
        [](unsigned char ch1, unsigned char ch2) {
            return std::tolower(ch1) == std::tolower(ch2);
        }
    );
    return it != str.end();
}

// Recursively checks if a node or any of its children matches the search query
bool matchesSearch(pid_t pid, const std::string& query, 
                    const std::unordered_map<pid_t, std::vector<pid_t>>& tree,
                    const std::unordered_map<pid_t, ProcessInfo>& pid_to_info,
                    std::unordered_map<pid_t, bool>& memo) {
    if (query.empty()) return true;
    auto memo_it = memo.find(pid);
    if (memo_it != memo.end()) return memo_it->second;

    auto info_it = pid_to_info.find(pid);
    if (info_it != pid_to_info.end()) {
        if (containsIgnoreCase(info_it->second.name, query)) {
            memo[pid] = true;
            return true;
        }
    }

    auto children_it = tree.find(pid);
    if (children_it != tree.end()) {
        for (pid_t child : children_it->second) {
            if (matchesSearch(child, query, tree, pid_to_info, memo)) {
                memo[pid] = true;
                return true;
            }
        }
    }

    memo[pid] = false;
    return false;
}

// Performs depth-first traversal to construct visible tree rows
void buildTreeRows(pid_t pid,
                   int depth,
                   const std::string& prefix,
                   bool is_last,
                   const std::unordered_map<pid_t, std::vector<pid_t>>& tree,
                   const std::unordered_map<pid_t, ProcessInfo>& pid_to_info,
                   const std::set<pid_t>& collapsed_pids,
                   std::unordered_map<pid_t, bool>& memo_search,
                   const std::string& query,
                   std::vector<pid_t>& visited,
                   std::vector<TreeRow>& rows) {
    
    // Cycle and depth prevention
    if (std::find(visited.begin(), visited.end(), pid) != visited.end()) {
        return;
    }
    visited.push_back(pid);

    auto info_it = pid_to_info.find(pid);
    if (info_it == pid_to_info.end()) {
        visited.pop_back();
        return;
    }

    if (!matchesSearch(pid, query, tree, pid_to_info, memo_search)) {
        visited.pop_back();
        return;
    }

    TreeRow row;
    row.info = info_it->second;
    row.depth = depth;
    row.collapsed = (collapsed_pids.find(pid) != collapsed_pids.end());

    auto children_it = tree.find(pid);
    std::vector<pid_t> visible_children;
    if (children_it != tree.end()) {
        for (pid_t child : children_it->second) {
            if (matchesSearch(child, query, tree, pid_to_info, memo_search)) {
                visible_children.push_back(child);
            }
        }
    }
    row.has_children = !visible_children.empty();

    if (depth > 0) {
        row.prefix = prefix + (is_last ? "└─ " : "├─ ");
    }
    
    rows.push_back(row);

    if (!row.collapsed && row.has_children) {
        std::string next_prefix = prefix;
        if (depth > 0) {
            next_prefix += (is_last ? "   " : "│  ");
        }
        for (size_t i = 0; i < visible_children.size(); ++i) {
            bool child_is_last = (i == visible_children.size() - 1);
            buildTreeRows(visible_children[i], depth + 1, next_prefix, child_is_last,
                          tree, pid_to_info, collapsed_pids, memo_search, query,
                          visited, rows);
        }
    }

    visited.pop_back();
}

int main(int argc, char* argv[]) {
    int interval = 1;
    int history_len = 60;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "pulse - A lightweight C++20 terminal system monitor with sparklines.\n\n";
            std::cout << "Usage: " << argv[0] << " [options]\n\n";
            std::cout << "Options:\n";
            std::cout << "  -i, --interval <sec>   Refresh interval in seconds (default: 1)\n";
            std::cout << "  -h, --history <count>  History window length in samples (default: 60)\n";
            std::cout << "  --help                 Show this help message and exit\n";
            return 0;
        } else if (arg == "--interval" || arg == "-i") {
            if (i + 1 < argc) {
                try {
                    interval = std::stoi(argv[++i]);
                    if (interval <= 0) {
                        std::cerr << "Error: refresh interval must be a positive integer.\n";
                        return 1;
                    }
                } catch (...) {
                    std::cerr << "Error: invalid integer value for interval.\n";
                    return 1;
                }
            } else {
                std::cerr << "Error: --interval requires an argument.\n";
                return 1;
            }
        } else if (arg == "--history" || arg == "-h") {
            if (i + 1 < argc) {
                try {
                    history_len = std::stoi(argv[++i]);
                    if (history_len <= 0) {
                        std::cerr << "Error: history window length must be a positive integer.\n";
                        return 1;
                    }
                } catch (...) {
                    std::cerr << "Error: invalid integer value for history.\n";
                    return 1;
                }
            } else {
                std::cerr << "Error: --history requires an argument.\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            std::cerr << "Try '" << argv[0] << " --help' for more information.\n";
            return 1;
        }
    }

    // Register signal handler for SIGINT (Ctrl+C)
    std::signal(SIGINT, sigint_handler);

    ProcReader reader;
    Renderer renderer;

    // Instantiate histories dynamically using parsed length
    RingBuffer<double> cpuHistory(history_len);
    RingBuffer<double> rxHistory(history_len);
    RingBuffer<double> txHistory(history_len);
    RingBuffer<double> rdHistory(history_len);
    RingBuffer<double> wrHistory(history_len);

    // Presentation and interactive UI state
    Mode mode = Mode::Normal;
    bool show_all = false;
    bool tree_mode = false;
    pid_t selectedPid = 0;
    int prev_selectedIndex = 0;
    int scrollOffset = 0;
    std::string search_query = "";
    SortKey active_sort = SortKey::Memory;
    
    // Battery refresh configuration (slower cadence check)
    constexpr int kBatteryRefreshIntervalCycles = 20;
    int battery_cycle_counter = 0;
    std::optional<BatteryInfo> cached_battery = std::nullopt;

    // Collapsed nodes cache set (persists across redraw loops)
    std::set<pid_t> collapsed_pids;

    // Status message state (kill errors)
    std::string status_message = "";
    std::chrono::steady_clock::time_point status_message_expiry;

    // Kill confirmation target details
    pid_t confirm_pid = 0;
    std::string confirm_name = "";
    std::string confirm_prompt = "";

    // Set up ncurses and color tables
    renderer.init();

    // Set non-blocking keyboard input with a timeout based on interval
    timeout(interval * 1000);

    while (!exit_requested) {
        // 1. Read system statistics
        double cpu = reader.cpuPercent();
        MemInfo mem = reader.memInfo();
        NetStats net = reader.netStats();
        DiskStats disk = reader.diskIoRate();

        // 2. Slow battery refresh check
        if (battery_cycle_counter % kBatteryRefreshIntervalCycles == 0) {
            cached_battery = reader.batteryInfo();
        }
        battery_cycle_counter++;
        
        // 3. Fetch process list based on active mode
        std::vector<ProcessInfo> raw_processes = reader.allProcesses();

        // 4. Construct tree rows or flat sorted rows
        std::vector<TreeRow> processes;

        if (show_all && tree_mode) {
            // Build tree structures (Parent -> Children vectors)
            std::unordered_map<pid_t, std::vector<pid_t>> tree;
            std::unordered_map<pid_t, ProcessInfo> pid_to_info;
            for (const auto& p : raw_processes) {
                pid_to_info[p.pid] = p;
            }
            for (const auto& p : raw_processes) {
                if (p.ppid != 0 && pid_to_info.find(p.ppid) != pid_to_info.end()) {
                    tree[p.ppid].push_back(p.pid);
                }
            }

            // Sort sibling vectors at each level by active SortKey configurations
            for (auto& [parent, children] : tree) {
                std::sort(children.begin(), children.end(), [&](pid_t a, pid_t b) {
                    const auto& infoA = pid_to_info[a];
                    const auto& infoB = pid_to_info[b];
                    if (active_sort == SortKey::Memory) {
                        return infoA.vmRSS > infoB.vmRSS;
                    } else if (active_sort == SortKey::Cpu) {
                        return infoA.cpuPercent > infoB.cpuPercent;
                    } else if (active_sort == SortKey::Pid) {
                        return infoA.pid < infoB.pid;
                    } else { // SortKey::Name
                        std::string a_lower = infoA.name;
                        std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), [](unsigned char c) { return std::tolower(c); });
                        std::string b_lower = infoB.name;
                        std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), [](unsigned char c) { return std::tolower(c); });
                        return a_lower < b_lower;
                    }
                });
            }

            // Locate roots (processes with no living parent in the scanned set)
            std::vector<pid_t> roots;
            for (const auto& p : raw_processes) {
                if (p.ppid == 0 || pid_to_info.find(p.ppid) == pid_to_info.end()) {
                    roots.push_back(p.pid);
                }
            }

            // Sort roots by the active key, keeping PID 1 first
            std::sort(roots.begin(), roots.end(), [&](pid_t a, pid_t b) {
                if (a == 1 && b != 1) return true;
                if (b == 1 && a != 1) return false;
                const auto& infoA = pid_to_info[a];
                const auto& infoB = pid_to_info[b];
                if (active_sort == SortKey::Memory) {
                    return infoA.vmRSS > infoB.vmRSS;
                } else if (active_sort == SortKey::Cpu) {
                    return infoA.cpuPercent > infoB.cpuPercent;
                } else if (active_sort == SortKey::Pid) {
                    return infoA.pid < infoB.pid;
                } else {
                    std::string a_lower = infoA.name;
                    std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), [](unsigned char c) { return std::tolower(c); });
                    std::string b_lower = infoB.name;
                    std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), [](unsigned char c) { return std::tolower(c); });
                    return a_lower < b_lower;
                }
            });

            // Perform depth-first traversal to construct visible tree rows
            std::unordered_map<pid_t, bool> memo_search;
            std::vector<pid_t> visited;
            for (size_t i = 0; i < roots.size(); ++i) {
                bool root_is_last = (i == roots.size() - 1);
                buildTreeRows(roots[i], 0, "", root_is_last, tree, pid_to_info, 
                              collapsed_pids, memo_search, search_query, visited, processes);
            }

        } else {
            // Flat mode list: get top-5 or full list
            std::vector<ProcessInfo> flat_list;
            if (show_all) {
                flat_list = raw_processes;
            } else {
                if (raw_processes.size() > 5) {
                    flat_list.assign(raw_processes.begin(), raw_processes.begin() + 5);
                } else {
                    flat_list = raw_processes;
                }
            }

            // Apply search substring filter
            std::vector<ProcessInfo> filtered_flat;
            if (!search_query.empty()) {
                for (const auto& p : flat_list) {
                    if (containsIgnoreCase(p.name, search_query)) {
                        filtered_flat.push_back(p);
                    }
                }
            } else {
                filtered_flat = flat_list;
            }

            // Sort flat list by the active sort key configuration
            std::sort(filtered_flat.begin(), filtered_flat.end(), [active_sort](const ProcessInfo& a, const ProcessInfo& b) {
                if (active_sort == SortKey::Memory) {
                    return a.vmRSS > b.vmRSS;
                } else if (active_sort == SortKey::Cpu) {
                    return a.cpuPercent > b.cpuPercent;
                } else if (active_sort == SortKey::Pid) {
                    return a.pid < b.pid;
                } else {
                    std::string a_lower = a.name;
                    std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), [](unsigned char c) { return std::tolower(c); });
                    std::string b_lower = b.name;
                    std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), [](unsigned char c) { return std::tolower(c); });
                    return a_lower < b_lower;
                }
            });

            // Populate processes list as unindented TreeRows
            for (const auto& p : filtered_flat) {
                TreeRow row;
                row.info = p;
                row.depth = 0;
                row.prefix = "";
                row.has_children = false;
                row.collapsed = false;
                processes.push_back(row);
            }
        }

        // 5. Feed metric history ring buffers
        cpuHistory.push_back(cpu);
        rxHistory.push_back(net.rxBytesSec);
        txHistory.push_back(net.txBytesSec);
        rdHistory.push_back(disk.readBytesSec);
        wrHistory.push_back(disk.writeBytesSec);

        // 6. Resolve selectedPid index in the active visible processes list
        int selectedIndex = 0;
        if (!processes.empty()) {
            bool found = false;
            if (selectedPid != 0) {
                for (size_t idx = 0; idx < processes.size(); ++idx) {
                    if (processes[idx].info.pid == selectedPid) {
                        selectedIndex = static_cast<int>(idx);
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                if (prev_selectedIndex >= static_cast<int>(processes.size())) {
                    prev_selectedIndex = static_cast<int>(processes.size()) - 1;
                }
                if (prev_selectedIndex < 0) {
                    prev_selectedIndex = 0;
                }
                selectedIndex = prev_selectedIndex;
                selectedPid = processes[selectedIndex].info.pid;
            }
            prev_selectedIndex = selectedIndex;
        } else {
            selectedPid = 0;
            selectedIndex = 0;
            prev_selectedIndex = 0;
        }

        // 7. Calculate scroll offsets dynamically using terminal height and active layout rows
        int height = 0, width = 0;
        getmaxyx(stdscr, height, width);
        
        int start_row = cached_battery.has_value() ? 13 : 11;
        int num_visible_rows = height - start_row - 4;

        if (show_all && !processes.empty()) {
            if (num_visible_rows > 0) {
                if (selectedIndex < scrollOffset) {
                    scrollOffset = selectedIndex;
                } else if (selectedIndex >= scrollOffset + num_visible_rows) {
                    scrollOffset = selectedIndex - num_visible_rows + 1;
                }
            }
        }

        // Clamp scrollOffset to prevent render boundary leaks when processes shrink
        if (num_visible_rows > 0) {
            int max_scroll = std::max(0, static_cast<int>(processes.size()) - num_visible_rows);
            scrollOffset = std::clamp(scrollOffset, 0, max_scroll);
        } else {
            scrollOffset = 0;
        }

        // Check if the temporary status message has expired
        std::string current_status = "";
        if (!status_message.empty()) {
            if (std::chrono::steady_clock::now() < status_message_expiry) {
                current_status = status_message;
            } else {
                status_message = "";
            }
        }

        // Retrieve process details if inspection view is active
        ProcessDetails details;
        if (mode == Mode::Details && selectedPid != 0) {
            details = reader.processDetails(selectedPid);
        }

        // 8. Draw the layout to terminal screen
        renderer.draw(
            cpu, cpuHistory, mem, net, rxHistory, txHistory, processes, 
            show_all, selectedPid, scrollOffset, mode, 
            (mode == Mode::ConfirmKill ? confirm_prompt : search_query), 
            current_status, active_sort, details,
            disk, rdHistory, wrHistory, cached_battery, tree_mode
        );

        // 9. Handle keyboard input
        int ch = getch();
        if (ch == ERR) {
            continue; // Refresh timeout reached with no user input
        }

        if (mode == Mode::Normal) {
            if (ch == 'q' || ch == 'Q') {
                exit_requested = true;
            } else if (ch == 'r' || ch == 'R') {
                cpuHistory = RingBuffer<double>(history_len);
                rxHistory = RingBuffer<double>(history_len);
                txHistory = RingBuffer<double>(history_len);
                rdHistory = RingBuffer<double>(history_len);
                wrHistory = RingBuffer<double>(history_len);
            } else if (ch == 'a' || ch == 'A') {
                show_all = !show_all;
                // Reset select/scroll indices on view toggle
                selectedPid = 0;
                selectedIndex = 0;
                prev_selectedIndex = 0;
                scrollOffset = 0;
                search_query = "";
            } else if (ch == '/') {
                mode = Mode::Search;
            } else if (ch == 's' || ch == 'S') {
                // Cycle sort keys
                if (active_sort == SortKey::Memory) active_sort = SortKey::Cpu;
                else if (active_sort == SortKey::Cpu) active_sort = SortKey::Pid;
                else if (active_sort == SortKey::Pid) active_sort = SortKey::Name;
                else if (active_sort == SortKey::Name) active_sort = SortKey::Memory;
            } else if (ch == 't' || ch == 'T') {
                if (show_all) {
                    tree_mode = !tree_mode;
                }
            } else if (ch == ' ') { // Space toggles collapsed status for tree view nodes
                if (show_all && tree_mode && selectedPid != 0) {
                    auto it = std::find_if(processes.begin(), processes.end(), [selectedPid](const TreeRow& r) {
                        return r.info.pid == selectedPid;
                    });
                    if (it != processes.end() && it->has_children) {
                        if (collapsed_pids.find(selectedPid) != collapsed_pids.end()) {
                            collapsed_pids.erase(selectedPid);
                        } else {
                            collapsed_pids.insert(selectedPid);
                        }
                    }
                }
            } else if (ch == 10 || ch == 13) { // Enter key
                if (show_all && selectedPid != 0) {
                    mode = Mode::Details;
                }
            } else if (ch == KEY_DOWN || ch == 'j' || ch == 'J') {
                if (show_all && !processes.empty()) {
                    selectedIndex = std::min(selectedIndex + 1, static_cast<int>(processes.size()) - 1);
                    selectedPid = processes[selectedIndex].info.pid;
                    prev_selectedIndex = selectedIndex;
                }
            } else if (ch == KEY_UP || ch == 'k' || ch == 'K') {
                if (show_all && !processes.empty()) {
                    selectedIndex = std::max(0, selectedIndex - 1);
                    selectedPid = processes[selectedIndex].info.pid;
                    prev_selectedIndex = selectedIndex;
                }
            } else if (ch == 'd' || ch == 'D' || ch == KEY_DC) {
                if (show_all && selectedPid != 0) {
                    auto it = std::find_if(processes.begin(), processes.end(), [selectedPid](const TreeRow& p) {
                        return p.info.pid == selectedPid;
                    });
                    if (it != processes.end()) {
                        confirm_pid = it->info.pid;
                        confirm_name = it->info.name;
                        confirm_prompt = "Kill " + confirm_name + " (PID " + std::to_string(confirm_pid) + ")? y/n";
                        mode = Mode::ConfirmKill;
                    }
                }
            } else if (ch == KEY_RESIZE) {
                clear();
            }
        } 
        else if (mode == Mode::Search) {
            if (ch == 27) { // Escape
                search_query = "";
                mode = Mode::Normal;
            } else if (ch == 10 || ch == 13) { // Enter
                mode = Mode::Normal;
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (!search_query.empty()) {
                    search_query.pop_back();
                }
                selectedPid = 0;
                selectedIndex = 0;
                prev_selectedIndex = 0;
                scrollOffset = 0;
            } else if (ch >= 32 && ch < 127) { // Printable characters
                search_query.push_back(static_cast<char>(ch));
                selectedPid = 0;
                selectedIndex = 0;
                prev_selectedIndex = 0;
                scrollOffset = 0;
            } else if (ch == KEY_RESIZE) {
                clear();
            }
        } 
        else if (mode == Mode::ConfirmKill) {
            if (ch == 'y' || ch == 'Y') {
                int res = kill(confirm_pid, SIGTERM);
                if (res < 0) {
                    std::string err_desc = std::strerror(errno);
                    if (errno == EPERM) {
                        err_desc = "permission denied";
                    } else if (errno == ESRCH) {
                        err_desc = "process no longer exists";
                    }
                    status_message = "Kill failed (" + std::to_string(confirm_pid) + "): " + err_desc;
                    status_message_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                }
                mode = Mode::Normal;
            } else if (ch == 'n' || ch == 'N' || ch == 27) { // Cancel
                mode = Mode::Normal;
            } else if (ch == KEY_RESIZE) {
                clear();
            }
        }
        else if (mode == Mode::Details) {
            // Escape or 'q' exits details mode and returns to list view
            if (ch == 27 || ch == 'q' || ch == 'Q') {
                mode = Mode::Normal;
            } else if (ch == KEY_RESIZE) {
                clear();
            }
        }
    }

    // Gracefully restore original terminal color palette and screen mode
    renderer.shutdown();

    return 0;
}
