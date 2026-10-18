/*
 * hw_topology.cpp - dump THIS machine's CPU / cache / NUMA / NIC layout
 * as JSON, for the visualizer's "open up the machine" view.
 *
 * Responsibility: the first thing anyone tuning a low-latency box does
 * is learn its topology - which logical CPUs are hyperthread siblings,
 * which caches they share, which NUMA node the NIC hangs off. Tools like
 * `lscpu`, `lstopo` and `numactl --hardware` all read the same Linux
 * sysfs files this program reads directly, so the code doubles as a map
 * of where that information lives:
 *
 *   /sys/devices/system/cpu/online                    which CPUs exist
 *   /sys/devices/system/cpu/cpuN/topology/            socket + core ids
 *   /sys/devices/system/cpu/cpuN/cache/indexK/        L1d/L1i/L2/L3 info
 *   /sys/devices/system/cpu/isolated                  isolcpus= set
 *   /sys/devices/system/node/nodeK/cpulist, meminfo   NUMA layout
 *   /sys/class/net/IF/                                NICs, queues, driver
 *
 * Usage:  ./bin/hw_topology > visualizer/data/hw_topology.json
 *         (or just `make topology`)
 *
 * Vocabulary the output uses (and that interviews will use):
 *   - logical CPU: what the scheduler sees; what --pin-cpu takes.
 *   - core: one physical execution core. With SMT ("hyperthreading")
 *     two logical CPUs share one core - and its L1/L2 - so pinning two
 *     hot threads onto siblings makes them fight over the same caches.
 *   - package/socket: one physical CPU chip. Multi-socket servers have
 *     one NUMA node per socket; memory attached to the other socket is
 *     slower to reach.
 *
 * Error handling: missing files are normal (VMs and containers hide a
 * lot of sysfs), so every read is optional and missing fields are
 * omitted from the JSON instead of failing the whole program.
 */

#include "cpu_list.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

/* Reads a whole (small) sysfs/procfs file and strips the trailing
 * newline. std::ifstream closes itself when it goes out of scope (RAII),
 * so there is no cleanup path to forget. */
std::optional<std::string> read_file(const fs::path &path) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

std::optional<int> read_int(const fs::path &path) {
    auto s = read_file(path);
    if (!s) {
        return std::nullopt;
    }
    try {
        return std::stoi(*s);
    } catch (...) {
        return std::nullopt; /* e.g. "speed" on a down link reads "-1" or errors */
    }
}

/* Cache sizes are reported like "48K" or "16384K". */
std::optional<int> parse_size_kb(const std::string &s) {
    if (s.empty()) {
        return std::nullopt;
    }
    int multiplier = 1;
    std::string digits = s;
    char unit = s.back();
    if (unit == 'K') {
        digits.pop_back();
    } else if (unit == 'M') {
        multiplier = 1024;
        digits.pop_back();
    }
    auto v = parse_non_negative_int(digits);
    if (!v) {
        return std::nullopt;
    }
    return *v * multiplier;
}

std::string json_string(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' '; /* control chars never appear in these fields */
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string json_int_array(const std::vector<int> &v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); i++) {
        out += (i ? "," : "") + std::to_string(v[i]);
    }
    return out + "]";
}

struct CacheInfo {
    int              level = 0;
    std::string      type;      /* "Data", "Instruction", "Unified" */
    int              size_kb = 0;
    int              line_size = 0;
    int              ways = 0;
    std::vector<int> shared_cpus;
};

struct CoreInfo {
    int              package_id = 0;
    int              core_id = 0;
    std::vector<int> threads; /* logical CPUs on this physical core */
};

std::string cpu_model() {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("model name", 0) == 0) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                return line.substr(colon + 2);
            }
        }
    }
    return "unknown";
}

std::optional<long> mem_total_kb() {
    std::ifstream in("/proc/meminfo");
    std::string key;
    long value = 0;
    std::string unit;
    while (in >> key >> value >> unit) {
        if (key == "MemTotal:") {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace

int main() {
    const fs::path cpu_root = "/sys/devices/system/cpu";

    auto online_text = read_file(cpu_root / "online");
    auto online = online_text ? parse_cpu_list(*online_text) : std::nullopt;
    if (!online) {
        std::fprintf(stderr, "hw_topology: cannot read %s/online\n", cpu_root.c_str());
        return 1;
    }

    /* Group logical CPUs into physical cores via (package_id, core_id).
     * std::map keeps the output in a stable, sorted order. */
    std::map<std::pair<int, int>, CoreInfo> cores;
    for (int cpu : *online) {
        fs::path topo = cpu_root / ("cpu" + std::to_string(cpu)) / "topology";
        int pkg  = read_int(topo / "physical_package_id").value_or(0);
        int core = read_int(topo / "core_id").value_or(cpu);
        CoreInfo &info = cores[{pkg, core}];
        info.package_id = pkg;
        info.core_id    = core;
        info.threads.push_back(cpu);
    }

    /* Every CPU reports every cache it can reach, so the same L3 shows
     * up 20 times on a 20-CPU box. A cache's identity is really "which
     * CPUs share it", so deduplicate on (level, type, shared_cpu_list). */
    std::map<std::string, CacheInfo> caches;
    for (int cpu : *online) {
        fs::path cache_root = cpu_root / ("cpu" + std::to_string(cpu)) / "cache";
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(cache_root, ec)) {
            if (entry.path().filename().string().rfind("index", 0) != 0) {
                continue;
            }
            CacheInfo c;
            c.level     = read_int(entry.path() / "level").value_or(0);
            c.type      = read_file(entry.path() / "type").value_or("Unknown");
            c.size_kb   = parse_size_kb(read_file(entry.path() / "size").value_or("")).value_or(0);
            c.line_size = read_int(entry.path() / "coherency_line_size").value_or(64);
            c.ways      = read_int(entry.path() / "ways_of_associativity").value_or(0);
            std::string shared = read_file(entry.path() / "shared_cpu_list").value_or("");
            c.shared_cpus = parse_cpu_list(shared).value_or(std::vector<int>{cpu});
            caches.emplace(std::to_string(c.level) + "|" + c.type + "|" + shared, c);
        }
    }

    auto isolated_text = read_file(cpu_root / "isolated");
    std::vector<int> isolated =
        isolated_text ? parse_cpu_list(*isolated_text).value_or(std::vector<int>{})
                      : std::vector<int>{};

    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0) {
        std::snprintf(host, sizeof(host), "unknown");
    }

    std::ostringstream out;
    out << "{\n";
    out << "  \"source\": \"sysfs (generated by bin/hw_topology)\",\n";
    out << "  \"hostname\": " << json_string(host) << ",\n";
    out << "  \"cpu_model\": " << json_string(cpu_model()) << ",\n";
    out << "  \"logical_cpus\": " << online->size() << ",\n";
    out << "  \"physical_cores\": " << cores.size() << ",\n";
    if (auto mem = mem_total_kb()) {
        out << "  \"mem_total_kb\": " << *mem << ",\n";
    }
    out << "  \"isolated_cpus\": " << json_int_array(isolated) << ",\n";

    out << "  \"cores\": [";
    bool first = true;
    for (const auto &[key, c] : cores) {
        out << (first ? "\n" : ",\n") << "    {\"package\": " << c.package_id
            << ", \"core_id\": " << c.core_id
            << ", \"threads\": " << json_int_array(c.threads) << "}";
        first = false;
    }
    out << "\n  ],\n";

    /* Sort caches L1 -> L3 so the visualizer can draw them in order. */
    std::vector<CacheInfo> sorted;
    for (const auto &[key, c] : caches) {
        sorted.push_back(c);
    }
    std::sort(sorted.begin(), sorted.end(), [](const CacheInfo &a, const CacheInfo &b) {
        if (a.level != b.level) return a.level < b.level;
        if (a.type != b.type) return a.type < b.type;
        return a.shared_cpus < b.shared_cpus;
    });
    out << "  \"caches\": [";
    first = true;
    for (const auto &c : sorted) {
        out << (first ? "\n" : ",\n") << "    {\"level\": " << c.level
            << ", \"type\": " << json_string(c.type)
            << ", \"size_kb\": " << c.size_kb
            << ", \"line_size\": " << c.line_size
            << ", \"ways\": " << c.ways
            << ", \"shared_cpus\": " << json_int_array(c.shared_cpus) << "}";
        first = false;
    }
    out << "\n  ],\n";

    out << "  \"numa_nodes\": [";
    first = true;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/sys/devices/system/node", ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("node", 0) != 0 || name.size() <= 4) {
            continue;
        }
        auto id = parse_non_negative_int(name.substr(4));
        auto cpus = parse_cpu_list(read_file(entry.path() / "cpulist").value_or(""));
        if (!id || !cpus) {
            continue;
        }
        out << (first ? "\n" : ",\n") << "    {\"id\": " << *id
            << ", \"cpus\": " << json_int_array(*cpus) << "}";
        first = false;
    }
    out << "\n  ],\n";

    /* NICs: the driver name says a lot. A real kernel-bypass capable NIC
     * shows up as e.g. ixgbe/i40e/mlx5_core/sfc; a VM's synthetic NIC
     * (hv_netvsc on Hyper-V/WSL2, virtio_net on KVM) cannot do DPDK/XDP
     * the way bare metal can. rx-N queues are the NIC's hardware receive
     * rings - RSS spreads flows across them, one IRQ per queue. */
    out << "  \"nics\": [";
    first = true;
    for (const auto &entry : fs::directory_iterator("/sys/class/net", ec)) {
        fs::path p = entry.path();
        std::string name = p.filename().string();
        int rx = 0, tx = 0;
        std::error_code qec;
        for (const auto &q : fs::directory_iterator(p / "queues", qec)) {
            std::string qn = q.path().filename().string();
            rx += qn.rfind("rx-", 0) == 0;
            tx += qn.rfind("tx-", 0) == 0;
        }
        std::error_code lec;
        fs::path drv_link = fs::read_symlink(p / "device" / "driver", lec);
        std::string driver = lec ? "virtual (no device)" : drv_link.filename().string();

        out << (first ? "\n" : ",\n") << "    {\"name\": " << json_string(name)
            << ", \"driver\": " << json_string(driver)
            << ", \"mac\": " << json_string(read_file(p / "address").value_or(""))
            << ", \"mtu\": " << read_int(p / "mtu").value_or(0)
            << ", \"speed_mbps\": " << read_int(p / "speed").value_or(-1)
            << ", \"numa_node\": " << read_int(p / "device" / "numa_node").value_or(-1)
            << ", \"rx_queues\": " << rx << ", \"tx_queues\": " << tx << "}";
        first = false;
    }
    out << "\n  ]\n}\n";

    std::fputs(out.str().c_str(), stdout);
    return 0;
}
