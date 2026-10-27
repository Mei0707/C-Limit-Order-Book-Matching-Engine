#include "lob/engine.hpp"
#include "lob/runner.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/resource.h>

namespace {
using namespace lob;
using Clock = std::chrono::steady_clock;
std::uint64_t parse_number(const std::string& text) {
    std::uint64_t n{};
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (ec != std::errc{} || end != text.data() + text.size()) throw std::invalid_argument("Invalid number: " + text);
    return n;
}
std::vector<Command> preload(std::size_t count, bool deep) {
    std::vector<Command> commands;
    commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const bool buy = i % 2 == 0;
        const auto distance = static_cast<Price>(deep ? i % 200000 : i % 32);
        commands.push_back(Command::add(i + 1, 1, buy ? Side::Buy : Side::Sell,
            buy ? 999000 - distance : 1001000 + distance, 100));
    }
    return commands;
}
std::vector<Command> workload(const std::string& profile, std::size_t count, std::size_t active, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Command> commands;
    commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const OrderId id = active + i + 1;
        if (profile == "resting" || profile == "deep") {
            const bool buy = rng() % 2 == 0;
            auto distance = static_cast<Price>(profile == "deep" ? i % 200000 : rng() % 32);
            commands.push_back(Command::add(id, 1, buy ? Side::Buy : Side::Sell,
                buy ? 999000 - distance : 1001000 + distance, 1 + rng() % 100));
        } else if (profile == "cancel") {
            if (i % 2 == 0) commands.push_back(Command::add(id, 1, Side::Buy, 999500, 100));
            else commands.push_back(Command::cancel(id - 1, 1));
        } else if (profile == "market") {
            const bool buy = (i / 2) % 2 == 0;
            if (i % 2 == 0) commands.push_back(Command::add(id, 1, buy ? Side::Sell : Side::Buy,
                buy ? 1000500 : 999500, 100));
            else commands.push_back(Command::add(id, 1, buy ? Side::Buy : Side::Sell,
                0, 100, OrderType::Market, TimeInForce::IOC));
        } else if (profile == "mixed" || profile == "burst") {
            const OrderId first = active + (i / 8) * 8 + 1;
            switch (i % 8) {
            case 0: commands.push_back(Command::add(first, 1, Side::Buy, 999500, 100)); break;
            case 1: commands.push_back(Command::add(first + 1, 1, Side::Sell, 1000500, 100)); break;
            case 2: commands.push_back(Command::modify(first, 1, 999500, 120)); break;
            case 3: commands.push_back(Command::add(id, 1, Side::Buy, 1000500,
                (i / 8) % 3 == 0 ? 200 : 70, OrderType::Limit,
                (i / 8) % 3 == 0 ? TimeInForce::FOK : TimeInForce::IOC)); break;
            case 4: commands.push_back(Command::cancel(first + 1, 1)); break;
            case 5: commands.push_back(Command::add(id, 1, Side::Sell, 0, 60, OrderType::Market, TimeInForce::IOC)); break;
            case 6: commands.push_back(Command::modify(first, 1, 999450, 50)); break;
            case 7: commands.push_back(Command::cancel(first, 1)); break;
            }
        } else throw std::invalid_argument("Unknown profile: " + profile);
    }
    return commands;
}
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, std::size_t percent) {
    // Nearest-rank quantile (ceil(N*p/100) - 1), integer arithmetic.
    const auto rank = (sorted.size() / 100) * percent + ((sorted.size() % 100) * percent + 99) / 100;
    return sorted[std::max<std::size_t>(rank, 1) - 1];
}
std::uint64_t peak_rss() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage)) return 0;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}
void measure(std::ostream& out, const std::string& profile, const std::vector<Command>& initial,
             const std::vector<Command>& commands, AllocationMode allocation, bool queued,
             std::uint64_t seed, std::size_t repetition) {
    Engine engine({{1, 1}}, EngineConfig{allocation, initial.size() + commands.size(), 64});
    // Warm allocator and code with a bounded add/cancel cycle, retaining no orders.
    for (std::size_t i = 0; i < 2000; ++i) {
        engine.process(Command::add(1, 1, Side::Buy, 999000, 100));
        engine.process(Command::cancel(1, 1));
    }
    for (const auto& c : initial) engine.process(c);
    const auto memory_before = engine.memory_stats();
    std::vector<std::uint64_t> latencies;
    latencies.reserve(commands.size());
    std::uint64_t rejected = 0, trades = 0, canceled = 0;
    const auto consume = [&](const auto& c) {
        const auto start = Clock::now();
        const auto& events = engine.process(c);
        const auto stop = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()));
        for (const auto& event : events) {
            if (event.type == EventType::Rejected) ++rejected;
            if (event.type == EventType::Trade) ++trades;
            if (event.type == EventType::Canceled) ++canceled;
        }
    };
    const auto start = Clock::now();
    QueueStats queue;
    if (queued) queue = run_queued(commands, consume);
    else for (const auto& c : commands) consume(c);
    const auto stop = Clock::now();
    const auto elapsed = std::chrono::duration<double>(stop - start).count();
    const auto memory_after = engine.memory_stats();
    if (queued && queue.processed != commands.size()) throw std::runtime_error("Queue lost commands");
    if (!engine.check_invariants().empty()) throw std::runtime_error(engine.check_invariants());
    std::sort(latencies.begin(), latencies.end());
    const auto n = static_cast<double>(commands.size());
    out << (queued ? "queued" : "direct") << ',' << (allocation == AllocationMode::Pooled ? "pooled" : "baseline")
        << ',' << profile << ',' << seed << ',' << repetition << ',' << commands.size() << ',' << initial.size()
        << ',' << engine.active_orders() << ',' << n / elapsed << ',' << percentile(latencies, 50)
        << ',' << percentile(latencies, 95) << ',' << percentile(latencies, 99) << ',' << latencies.back()
        << ',' << peak_rss() << ',' << static_cast<double>(memory_after.logical_allocations - memory_before.logical_allocations) / n
        << ',' << static_cast<double>(memory_after.upstream_allocations - memory_before.upstream_allocations) / n
        << ',' << memory_after.upstream_bytes << ',' << memory_after.peak_upstream_bytes << ',' << queue.max_occupancy
        << ',' << queue.full_retries << ",0," << rejected << ',' << trades << ',' << canceled << ',' << engine.checksum() << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::size_t count = 100000, repeats = 3, active = 10000;
        std::uint64_t seed = 42;
        std::string output = "benchmark.csv", profile = "all";
        for (int i = 1; i < argc; ++i) {
            std::string option = argv[i];
            if (option == "--help") {
                std::cout << "lob_bench [--commands 100000] [--repeats 3] [--active-orders 10000] [--seed 42]\n"
                             "          [--profile all|resting|cancel|market|mixed|deep|burst] [--output benchmark.csv]\n";
                return 0;
            }
            if (i + 1 == argc) throw std::invalid_argument("Missing value for " + option);
            std::string value = argv[++i];
            if (option == "--commands") count = parse_number(value);
            else if (option == "--repeats") repeats = parse_number(value);
            else if (option == "--active-orders") active = parse_number(value);
            else if (option == "--seed") seed = parse_number(value);
            else if (option == "--output") output = value;
            else if (option == "--profile") profile = value;
            else throw std::invalid_argument("Unknown option: " + option);
        }
        if (!count || !repeats || count > 100000000 || active > 10000000 || repeats > 1000)
            throw std::invalid_argument("Use 1..100000000 commands, 1..1000 repeats, and <=10000000 initial orders");
        std::vector<std::string> profiles = profile == "all"
            ? std::vector<std::string>{"resting", "cancel", "market", "mixed", "deep", "burst"}
            : std::vector<std::string>{profile};
        if (profile != "all" && profile != "resting" && profile != "cancel" && profile != "market" &&
            profile != "mixed" && profile != "deep" && profile != "burst") throw std::invalid_argument("Unknown profile: " + profile);
        std::ofstream out(output);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << "mode,allocator,profile,seed,run,commands,active_initial,active_final,commands_per_second,p50_ns,p95_ns,p99_ns,max_ns,process_peak_rss_bytes,logical_allocations_per_command,upstream_allocations_per_command,current_book_bytes,peak_book_bytes,queue_max_occupancy,queue_full_retries,dropped_commands,rejected_commands,trades,cancellations,checksum\n";
        for (const auto& p : profiles) {
            const auto initial = preload(active, p == "deep");
            const auto commands = workload(p, count, active, seed);
            for (std::size_t run = 1; run <= repeats; ++run) {
                // Alternate order to reduce consistent first/second-run bias.
                for (const auto allocator : run % 2 ? std::vector<AllocationMode>{AllocationMode::Baseline, AllocationMode::Pooled}
                                                   : std::vector<AllocationMode>{AllocationMode::Pooled, AllocationMode::Baseline}) {
                    measure(out, p, initial, commands, allocator, false, seed, run);
                    if (p == "burst") measure(out, p, initial, commands, allocator, true, seed, run);
                }
            }
            std::cerr << "Completed " << p << " (" << count << " commands, " << repeats << " repeats)\n";
        }
        out.close();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
