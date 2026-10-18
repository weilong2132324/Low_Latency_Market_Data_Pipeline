/*
 * cpu_list.hpp - parse Linux "CPU list" strings like "0-3,8,10-11".
 *
 * Responsibility: sysfs reports sets of CPUs in this compact range
 * format everywhere - /sys/devices/system/cpu/online, a cache's
 * shared_cpu_list, a NUMA node's cpulist, the isolcpus= boot parameter.
 * Reading any of them (hw_topology.cpp does) needs this one parser.
 *
 * Thread-safety: pure function, no shared state.
 *
 * Errors: returns std::nullopt for malformed input rather than
 * guessing - a half-parsed CPU set is worse than none, because the
 * caller would silently pin threads to the wrong cores.
 */

#ifndef PULSEFORGE_CPU_LIST_HPP
#define PULSEFORGE_CPU_LIST_HPP

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

inline std::optional<int> parse_non_negative_int(std::string_view s) {
    if (s.empty()) {
        return std::nullopt;
    }
    int value = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

inline std::optional<std::vector<int>> parse_cpu_list(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1); /* sysfs files end in '\n' */
    }

    std::vector<int> cpus;
    if (text.empty()) {
        return cpus; /* an empty set is valid (e.g. no isolated CPUs) */
    }

    while (!text.empty()) {
        size_t comma = text.find(',');
        std::string_view item = text.substr(0, comma);
        text = (comma == std::string_view::npos) ? std::string_view{} : text.substr(comma + 1);

        size_t dash = item.find('-');
        if (dash == std::string_view::npos) {
            auto cpu = parse_non_negative_int(item);
            if (!cpu) {
                return std::nullopt;
            }
            cpus.push_back(*cpu);
        } else {
            auto lo = parse_non_negative_int(item.substr(0, dash));
            auto hi = parse_non_negative_int(item.substr(dash + 1));
            if (!lo || !hi || *lo > *hi) {
                return std::nullopt;
            }
            for (int c = *lo; c <= *hi; c++) {
                cpus.push_back(c);
            }
        }
    }
    return cpus;
}

#endif /* PULSEFORGE_CPU_LIST_HPP */
