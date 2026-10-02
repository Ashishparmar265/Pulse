#pragma once

#include "proc_reader.hpp"
#include "history.hpp"
#include <vector>
#include <unordered_map>
#include <string>
#include <optional>
#include <sys/types.h>

enum class Mode { Normal, Search, ConfirmKill, Details };
enum class SortKey { Memory, Cpu, Pid, Name };

class Renderer {
private:
    struct OriginalColor {
        short r = 0;
        short g = 0;
        short b = 0;
    };

    std::unordered_map<short, OriginalColor> original_colors_;
    bool colors_changed_ = false;
    short bg_color_ = 0;
    short fg_default_ = 0;

    std::string makeSparkline(const RingBuffer<double>& history, double max_val, size_t target_width);

public:
    Renderer() = default;
    ~Renderer() = default;

    void init();
    void shutdown();
    void draw(double cpuPercent,
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
              bool treeMode);
};
