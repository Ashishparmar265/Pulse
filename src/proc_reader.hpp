#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <sys/types.h>

struct MemInfo {
    unsigned long long totalKB = 0;
    unsigned long long usedKB = 0;
    unsigned long long availableKB = 0;
    unsigned long long swapTotalKB = 0;
    unsigned long long swapUsedKB = 0;
};

struct ProcessInfo {
    pid_t pid = 0;
    pid_t ppid = 0;
    std::string name;
    unsigned long long cpuTime = 0; // utime + stime in clock ticks
    unsigned long long vmRSS = 0;   // VmRSS in KB
    double cpuPercent = 0.0;        // CPU usage percentage
};

struct TreeRow {
    ProcessInfo info;
    int depth = 0;
    std::string prefix;
    bool has_children = false;
    bool collapsed = false;
};

struct ProcessDetails {
    bool exists = false;
    pid_t pid = 0;
    std::string name;
    std::string cmdline;
    std::string exePath;
    std::string state;
    pid_t ppid = 0;
    unsigned long long vmRSS = 0;
    unsigned long long vmSize = 0;
    int threads = 0;
    int fdCount = -1; // -1 on permission error
};

struct NetStats {
    double rxBytesSec = 0.0;
    double txBytesSec = 0.0;
};

struct DiskStats {
    double readBytesSec = 0.0;
    double writeBytesSec = 0.0;
};

struct BatteryInfo {
    double capacity = 0.0; // percent
    std::string status;
};

class ProcReader {
private:
    struct CpuSample {
        bool valid = false;
        unsigned long long idle = 0;
        unsigned long long total = 0;
    };

    struct NetSample {
        bool valid = false;
        std::chrono::steady_clock::time_point timestamp;
        unsigned long long rxBytes = 0;
        unsigned long long txBytes = 0;
    };

    struct DiskSample {
        bool valid = false;
        std::chrono::steady_clock::time_point timestamp;
        unsigned long long sectorsRead = 0;
        unsigned long long sectorsWritten = 0;
    };

    struct PrevProcSample {
        unsigned long long cpuTime = 0;
        std::chrono::steady_clock::time_point timestamp;
    };

    CpuSample prev_cpu_;
    NetSample prev_net_;
    DiskSample prev_disk_;
    std::unordered_map<pid_t, PrevProcSample> prev_procs_;

    // Helper functions for reading samples
    bool readCpuSample(CpuSample& sample);
    bool readNetSample(NetSample& sample);
    bool readDiskSample(DiskSample& sample);

public:
    ProcReader();

    double cpuPercent();
    MemInfo memInfo();
    std::vector<ProcessInfo> topProcesses(int n);
    std::vector<ProcessInfo> allProcesses();
    ProcessDetails processDetails(pid_t pid);
    NetStats netStats();
    DiskStats diskIoRate();
    std::optional<BatteryInfo> batteryInfo();
};
