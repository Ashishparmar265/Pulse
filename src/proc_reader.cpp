#include "proc_reader.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <unistd.h>

ProcReader::ProcReader() {
    CpuSample cpu;
    readCpuSample(cpu);
    prev_cpu_ = cpu;

    NetSample net;
    readNetSample(net);
    prev_net_ = net;

    DiskSample disk;
    readDiskSample(disk);
    prev_disk_ = disk;
}

bool ProcReader::readCpuSample(CpuSample& sample) {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return false;
    std::string line;
    if (std::getline(file, line)) {
        if (line.rfind("cpu ", 0) == 0) {
            std::istringstream iss(line);
            std::string label;
            iss >> label; // consume "cpu"
            unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
            if (iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
                sample.idle = idle + iowait;
                sample.total = sample.idle + user + nice + system + irq + softirq + steal;
                sample.valid = true;
                return true;
            }
        }
    }
    return false;
}

double ProcReader::cpuPercent() {
    CpuSample current;
    if (!readCpuSample(current)) {
        return 0.0;
    }
    if (!prev_cpu_.valid) {
        prev_cpu_ = current;
        return 0.0;
    }
    unsigned long long totalDelta = 0;
    unsigned long long idleDelta = 0;

    if (current.total >= prev_cpu_.total && current.idle >= prev_cpu_.idle) {
        totalDelta = current.total - prev_cpu_.total;
        idleDelta = current.idle - prev_cpu_.idle;
    }

    prev_cpu_ = current;

    if (totalDelta == 0) return 0.0;
    double activeDelta = static_cast<double>(totalDelta - idleDelta);
    return (activeDelta / static_cast<double>(totalDelta)) * 100.0;
}

MemInfo ProcReader::memInfo() {
    MemInfo info;
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) return info;
    std::string line;
    bool hasMemTotal = false, hasMemAvailable = false, hasSwapTotal = false, hasSwapFree = false;
    unsigned long long memTotal = 0, memAvailable = 0, swapTotal = 0, swapFree = 0;

    while (std::getline(file, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);

        std::string valStr = line.substr(colon + 1);
        std::istringstream iss(valStr);
        unsigned long long val = 0;
        if (iss >> val) {
            if (key == "MemTotal") {
                memTotal = val;
                hasMemTotal = true;
            } else if (key == "MemAvailable") {
                memAvailable = val;
                hasMemAvailable = true;
            } else if (key == "SwapTotal") {
                swapTotal = val;
                hasSwapTotal = true;
            } else if (key == "SwapFree") {
                swapFree = val;
                hasSwapFree = true;
            }
        }
    }

    if (hasMemTotal) info.totalKB = memTotal;
    if (hasMemAvailable) info.availableKB = memAvailable;
    if (hasMemTotal && hasMemAvailable) {
        info.usedKB = memTotal - memAvailable;
    }
    if (hasSwapTotal) info.swapTotalKB = swapTotal;
    if (hasSwapTotal && hasSwapFree) {
        info.swapUsedKB = swapTotal - swapFree;
    }
    return info;
}

bool ProcReader::readNetSample(NetSample& sample) {
    std::ifstream file("/proc/net/dev");
    if (!file.is_open()) return false;
    std::string line;
    if (!std::getline(file, line) || !std::getline(file, line)) return false;

    unsigned long long totalRx = 0;
    unsigned long long totalTx = 0;

    while (std::getline(file, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string iface = line.substr(0, colon);
        iface.erase(0, iface.find_first_not_of(" \t"));
        iface.erase(iface.find_last_not_of(" \t") + 1);

        if (iface == "lo") continue;

        std::string data = line.substr(colon + 1);
        std::istringstream iss(data);
        unsigned long long rx = 0;
        if (iss >> rx) {
            unsigned long long dummy;
            for (int i = 0; i < 7; ++i) {
                if (!(iss >> dummy)) break;
            }
            unsigned long long tx = 0;
            if (iss >> tx) {
                totalRx += rx;
                totalTx += tx;
            }
        }
    }
    sample.timestamp = std::chrono::steady_clock::now();
    sample.rxBytes = totalRx;
    sample.txBytes = totalTx;
    sample.valid = true;
    return true;
}

NetStats ProcReader::netStats() {
    NetSample current;
    if (!readNetSample(current)) {
        return {0.0, 0.0};
    }
    if (!prev_net_.valid) {
        prev_net_ = current;
        return {0.0, 0.0};
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(current.timestamp - prev_net_.timestamp).count();

    double rxRate = 0.0;
    if (current.rxBytes >= prev_net_.rxBytes) {
        rxRate = static_cast<double>(current.rxBytes - prev_net_.rxBytes) / elapsed;
    }
    double txRate = 0.0;
    if (current.txBytes >= prev_net_.txBytes) {
        txRate = static_cast<double>(current.txBytes - prev_net_.txBytes) / elapsed;
    }

    prev_net_ = current;

    if (elapsed <= 0.0) return {0.0, 0.0};
    return {rxRate, txRate};
}

std::vector<ProcessInfo> ProcReader::topProcesses(int n) {
    std::vector<ProcessInfo> processes;
    std::unordered_map<pid_t, PrevProcSample> next_procs;
    auto now = std::chrono::steady_clock::now();

    static const long clk_tck = sysconf(_SC_CLK_TCK);

    try {
        for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
            if (!entry.is_directory()) continue;

            std::string pidStr = entry.path().filename().string();
            if (!std::all_of(pidStr.begin(), pidStr.end(), ::isdigit)) continue;

            pid_t pid = 0;
            try {
                pid = std::stoi(pidStr);
            } catch (...) {
                continue;
            }

            std::string statPath = "/proc/" + pidStr + "/stat";
            std::ifstream statFile(statPath);
            if (!statFile.is_open()) continue;

            std::string statLine;
            if (!std::getline(statFile, statLine)) continue;

            auto openParen = statLine.find('(');
            auto closeParen = statLine.rfind(')');
            if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen) {
                continue;
            }

            std::string name = statLine.substr(openParen + 1, closeParen - openParen - 1);
            std::string rest = statLine.substr(closeParen + 1);
            std::istringstream iss(rest);

            char state;
            if (!(iss >> state)) continue;

            pid_t ppid = 0;
            if (!(iss >> ppid)) continue;

            long long dummy;
            bool skipFailed = false;
            for (int i = 0; i < 9; ++i) {
                if (!(iss >> dummy)) {
                    skipFailed = true;
                    break;
                }
            }
            if (skipFailed) continue;

            unsigned long long utime = 0;
            unsigned long long stime = 0;
            if (!(iss >> utime >> stime)) continue;

            unsigned long long cpuTime = utime + stime;

            double cpuPercent = 0.0;
            auto it = prev_procs_.find(pid);
            if (it != prev_procs_.end()) {
                double elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(now - it->second.timestamp).count();
                if (elapsedSec > 0.0 && cpuTime >= it->second.cpuTime) {
                    double deltaTicks = static_cast<double>(cpuTime - it->second.cpuTime);
                    double deltaSec = deltaTicks / static_cast<double>(clk_tck);
                    cpuPercent = (deltaSec / elapsedSec) * 100.0;
                }
            }

            next_procs[pid] = PrevProcSample{cpuTime, now};

            unsigned long long vmRSS = 0;
            std::string statusPath = "/proc/" + pidStr + "/status";
            std::ifstream statusFile(statusPath);
            if (statusFile.is_open()) {
                std::string statusLine;
                while (std::getline(statusFile, statusLine)) {
                    if (statusLine.rfind("VmRSS:", 0) == 0) {
                        std::istringstream rssIss(statusLine);
                        std::string label;
                        unsigned long long val = 0;
                        if (rssIss >> label >> val) {
                            vmRSS = val;
                        }
                        break;
                    }
                }
            }

            processes.push_back(ProcessInfo{pid, ppid, name, cpuTime, vmRSS, cpuPercent});
        }
    } catch (...) {
        // Safe fallback in case directory access fails transiently
    }

    prev_procs_ = std::move(next_procs);

    std::sort(processes.begin(), processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return a.vmRSS > b.vmRSS;
    });

    if (n >= 0 && processes.size() > static_cast<size_t>(n)) {
        processes.resize(n);
    }

    return processes;
}

std::vector<ProcessInfo> ProcReader::allProcesses() {
    return topProcesses(-1);
}

ProcessDetails ProcReader::processDetails(pid_t pid) {
    ProcessDetails details;
    details.pid = pid;

    std::string procPath = "/proc/" + std::to_string(pid);
    if (!std::filesystem::exists(procPath)) {
        details.exists = false;
        return details;
    }
    details.exists = true;

    // 1. Read name from stat file (as a baseline fallback)
    std::ifstream statFile(procPath + "/stat");
    if (statFile.is_open()) {
        std::string statLine;
        if (std::getline(statFile, statLine)) {
            auto openParen = statLine.find('(');
            auto closeParen = statLine.rfind(')');
            if (openParen != std::string::npos && closeParen != std::string::npos && closeParen > openParen) {
                details.name = statLine.substr(openParen + 1, closeParen - openParen - 1);
            }
        }
    }

    // 2. Read command line args
    std::ifstream cmdFile(procPath + "/cmdline", std::ios::binary);
    if (cmdFile.is_open()) {
        std::string arg;
        while (std::getline(cmdFile, arg, '\0')) {
            if (!details.cmdline.empty()) details.cmdline += " ";
            details.cmdline += arg;
        }
    }
    // Handle kernel threads (command line is empty) by wrapping name in brackets
    if (details.cmdline.empty() && !details.name.empty()) {
        details.cmdline = "[" + details.name + "]";
    }

    // 3. Resolve executable path
    try {
        details.exePath = std::filesystem::read_symlink(procPath + "/exe").string();
    } catch (const std::filesystem::filesystem_error& e) {
        if (e.code() == std::errc::permission_denied) {
            details.exePath = "[permission denied]";
        } else {
            details.exePath = "[error: " + std::string(e.code().message()) + "]";
        }
    } catch (...) {
        details.exePath = "[unknown error]";
    }

    // 4. Parse status file
    std::ifstream statusFile(procPath + "/status");
    if (statusFile.is_open()) {
        std::string line;
        while (std::getline(statusFile, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            
            // Trim whitespace helper
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);

            if (key == "State") {
                details.state = val;
            } else if (key == "PPid") {
                try {
                    details.ppid = std::stoi(val);
                } catch (...) {
                    details.ppid = 0;
                }
            } else if (key == "VmRSS") {
                std::istringstream iss(val);
                iss >> details.vmRSS;
            } else if (key == "VmSize") {
                std::istringstream iss(val);
                iss >> details.vmSize;
            } else if (key == "Threads") {
                try {
                    details.threads = std::stoi(val);
                } catch (...) {
                    details.threads = 0;
                }
            }
        }
    }

    // 5. Count open file descriptors
    int fdCount = 0;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(procPath + "/fd")) {
            (void)entry;
            fdCount++;
        }
        details.fdCount = fdCount;
    } catch (const std::filesystem::filesystem_error& e) {
        details.fdCount = -1; // Indicates permission denied or read failure
    } catch (...) {
        details.fdCount = -1;
    }

    return details;
}

bool ProcReader::readDiskSample(DiskSample& sample) {
    std::ifstream file("/proc/diskstats");
    if (!file.is_open()) return false;

    unsigned long long totalReadSectors = 0;
    unsigned long long totalWriteSectors = 0;
    std::string line;

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        int major, minor;
        std::string name;
        if (iss >> major >> minor >> name) {
            // Filter partitions and virtual devices
            if (!std::filesystem::exists("/sys/block/" + name)) continue;
            if (name.rfind("loop", 0) == 0) continue;
            if (name.rfind("dm-", 0) == 0) continue;
            if (name.rfind("ram", 0) == 0) continue;
            if (name.rfind("zram", 0) == 0) continue;

            unsigned long long reads = 0, reads_merged = 0, sectors_read = 0;
            unsigned long long time_read = 0, writes = 0, writes_merged = 0, sectors_written = 0;
            if (iss >> reads >> reads_merged >> sectors_read >> time_read >> writes >> writes_merged >> sectors_written) {
                totalReadSectors += sectors_read;
                totalWriteSectors += sectors_written;
            }
        }
    }

    sample.timestamp = std::chrono::steady_clock::now();
    sample.sectorsRead = totalReadSectors;
    sample.sectorsWritten = totalWriteSectors;
    sample.valid = true;
    return true;
}

DiskStats ProcReader::diskIoRate() {
    DiskSample current;
    if (!readDiskSample(current)) {
        return {0.0, 0.0};
    }
    if (!prev_disk_.valid) {
        prev_disk_ = current;
        return {0.0, 0.0};
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(current.timestamp - prev_disk_.timestamp).count();

    double readRate = 0.0;
    if (current.sectorsRead >= prev_disk_.sectorsRead) {
        readRate = static_cast<double>((current.sectorsRead - prev_disk_.sectorsRead) * 512) / elapsed;
    }
    double writeRate = 0.0;
    if (current.sectorsWritten >= prev_disk_.sectorsWritten) {
        writeRate = static_cast<double>((current.sectorsWritten - prev_disk_.sectorsWritten) * 512) / elapsed;
    }

    prev_disk_ = current;

    if (elapsed <= 0.0) return {0.0, 0.0};
    return {readRate, writeRate};
}

std::optional<BatteryInfo> ProcReader::batteryInfo() {
    std::vector<std::string> batteries = {"BAT0", "BAT1"};
    double totalFull = 0.0;
    double totalNow = 0.0;
    std::string finalStatus = "Unknown";
    bool foundAny = false;
    bool anyCharging = false;
    bool anyDischarging = false;
    bool allFull = true;

    for (const auto& bat : batteries) {
        std::string path = "/sys/class/power_supply/" + bat;
        if (!std::filesystem::exists(path)) continue;

        foundAny = true;
        
        // Read status
        std::string status = "Unknown";
        std::ifstream statusFile(path + "/status");
        if (statusFile >> status) {
            if (status == "Charging") anyCharging = true;
            if (status == "Discharging") anyDischarging = true;
            if (status != "Full") allFull = false;
        }

        // Read capacity percentage
        double capPercent = 0.0;
        std::ifstream capFile(path + "/capacity");
        capFile >> capPercent;

        // Try reading charge_full or energy_full
        double full = 0.0;
        std::ifstream cFullFile(path + "/charge_full");
        if (!(cFullFile >> full)) {
            std::ifstream eFullFile(path + "/energy_full");
            eFullFile >> full;
        }

        // Try reading charge_now or energy_now
        double now = 0.0;
        std::ifstream cNowFile(path + "/charge_now");
        if (!(cNowFile >> now)) {
            std::ifstream eNowFile(path + "/energy_now");
            if (!(eNowFile >> now)) {
                // Fallback
                now = capPercent * full / 100.0;
            }
        }

        if (full <= 0.0) {
            full = 1.0;
            now = capPercent / 100.0;
        }

        totalFull += full;
        totalNow += now;
    }

    if (!foundAny) return std::nullopt;

    BatteryInfo info;
    if (totalFull > 0.0) {
        info.capacity = (totalNow / totalFull) * 100.0;
    } else {
        info.capacity = 0.0;
    }

    if (anyCharging) finalStatus = "Charging";
    else if (anyDischarging) finalStatus = "Discharging";
    else if (allFull) finalStatus = "Full";
    else finalStatus = "Unknown";

    info.status = finalStatus;
    return info;
}



