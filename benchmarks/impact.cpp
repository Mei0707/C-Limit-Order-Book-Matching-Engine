#include "impact_support.hpp"
#include "lob/spsc_queue.hpp"
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

namespace {
using namespace impact;
using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
std::uint64_t integer(const std::string& s) {
    std::uint64_t result{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    if (ec != std::errc{} || end != s.data() + s.size()) throw std::invalid_argument("Invalid integer: " + s);
    return result;
}
struct Options {
    std::string experiment{"sustain"}, output{"impact.csv"};
    std::size_t active{100000}, commands{200000}, repeats{3};
    std::uint64_t seconds{5}, rate{1000000}, seed{42};
};
struct Result {
    std::uint64_t commands{}, trades{}, rejected{}, retries{}, processed{};
    std::size_t max_queue{};
    double seconds{};
    std::vector<std::uint64_t> service, end_to_end;
    MemoryStats before{}, after{};
    std::uint64_t checksum{};
    std::size_t levels{};
};
std::size_t price_levels(const Engine& engine) {
    std::size_t levels = 0;
    for (auto spec : instruments()) {
        const auto depth = engine.depth(spec.id);
        levels += depth.bids.size() + depth.asks.size();
    }
    return levels;
}
void count_events(const std::vector<Event>& events, Result& result) {
    for (const auto& e : events) {
        if (e.type == EventType::Trade) ++result.trades;
        if (e.type == EventType::Rejected) ++result.rejected;
    }
}
std::unique_ptr<Engine> prepare(const Options& options, AllocationMode mode,
                                const std::vector<Command>& batch) {
    auto engine = std::make_unique<Engine>(instruments(), EngineConfig{mode, options.active + 256, 64});
    populate(*engine, options.active);
    for (const auto& c : batch) engine->process(c);
    verify(*engine, options.active, batch.size(), 0, batch.size() / 4, 0);
    return engine;
}
Result direct(const Options& options, AllocationMode mode, const std::vector<Command>& batch) {
    auto engine = prepare(options, mode, batch);
    Result result;
    result.service.reserve(1000000);
    result.before = engine->memory_stats();
    const auto start = Clock::now();
    do {
        for (const auto& c : batch) {
            if (result.commands % SampleStride == 0) {
                const auto begin = Clock::now();
                const auto& events = engine->process(c);
                const auto finish = Clock::now();
                result.service.push_back(static_cast<std::uint64_t>(std::chrono::duration_cast<Nanoseconds>(finish - begin).count()));
                count_events(events, result);
            } else count_events(engine->process(c), result);
            ++result.commands;
        }
    } while (options.experiment == "sustain" && Clock::now() - start < std::chrono::seconds(options.seconds));
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    result.processed = result.commands;
    result.after = engine->memory_stats();
    verify(*engine, options.active, result.commands, batch.size(), result.trades, result.rejected);
    result.checksum = engine->checksum();
    result.levels = price_levels(*engine);
    return result;
}

// The intended arrival time comes from ordinal/rate, not from successful enqueue.
// This includes producer delay and backpressure in end-to-end latency samples.
struct Packet { Command command; std::uint64_t ordinal; };
Result queued(const Options& options, const std::vector<Command>& batch) {
    auto engine = prepare(options, AllocationMode::Pooled, batch);
    SpscQueue<Packet, 4096> queue;
    std::atomic<bool> ready{false}, start_signal{false}, done{false}, failed{false};
    std::exception_ptr error;
    Result result;
    result.service.reserve(1000000); result.end_to_end.reserve(1000000);
    result.before = engine->memory_stats();
    const auto requested = options.rate * options.seconds;
    const auto count = requested - requested % 8;
    const auto offset = [&](std::uint64_t ordinal) {
        return Nanoseconds((ordinal / options.rate) * 1000000000ULL +
                           ((ordinal % options.rate) * 1000000000ULL) / options.rate);
    };
    Clock::time_point origin, finish;
    std::thread worker([&] {
        ready.store(true, std::memory_order_release);
        while (!start_signal.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            Packet packet;
            for (;;) {
                if (!queue.try_pop(packet)) {
                    if (!done.load(std::memory_order_acquire)) { std::this_thread::yield(); continue; }
                    if (!queue.try_pop(packet)) break;
                }
                if (packet.ordinal != result.processed) throw std::runtime_error("Queue lost, duplicated or reordered a command");
                if (packet.ordinal % SampleStride == 0) {
                    const auto begin = Clock::now();
                    const auto& events = engine->process(packet.command);
                    const auto complete = Clock::now();
                    result.service.push_back(static_cast<std::uint64_t>(std::chrono::duration_cast<Nanoseconds>(complete - begin).count()));
                    result.end_to_end.push_back(static_cast<std::uint64_t>(std::chrono::duration_cast<Nanoseconds>(complete - (origin + offset(packet.ordinal))).count()));
                    count_events(events, result);
                } else count_events(engine->process(packet.command), result);
                ++result.processed;
            }
            finish = Clock::now();
        } catch (...) {
            error = std::current_exception();
            failed.store(true, std::memory_order_release);
        }
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    origin = Clock::now() + std::chrono::milliseconds(2);
    start_signal.store(true, std::memory_order_release);
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto scheduled = origin + offset(i);
        while (Clock::now() < scheduled && !failed.load(std::memory_order_acquire)) std::this_thread::yield();
        while (!failed.load(std::memory_order_acquire) &&
               !queue.try_push({batch[static_cast<std::size_t>(i % batch.size())], i})) {
            ++result.retries;
            std::this_thread::yield();
        }
        if (failed.load(std::memory_order_acquire)) break;
        ++result.commands;
        result.max_queue = std::max(result.max_queue, queue.producer_size());
    }
    done.store(true, std::memory_order_release);
    worker.join();
    if (error) std::rethrow_exception(error);
    if (result.commands != count || result.processed != count) throw std::runtime_error("Queue did not drain all offered commands");
    result.seconds = std::chrono::duration<double>(finish - origin).count();
    result.after = engine->memory_stats();
    verify(*engine, options.active, count, batch.size(), result.trades, result.rejected);
    result.checksum = engine->checksum();
    result.levels = price_levels(*engine);
    return result;
}
void write_result(std::ostream& out, const Options& o, AllocationMode mode, std::size_t run, const Result& r) {
    out << std::setprecision(12) << o.experiment << ',' << (mode == AllocationMode::Pooled ? "pooled" : "baseline")
        << ',' << o.seed << ',' << run << ',' << o.active << ',' << Instruments << ',' << r.commands << ',' << r.processed
        << ',' << r.seconds << ',' << static_cast<double>(r.commands) / r.seconds << ',' << r.service.size()
        << ',' << quantile(r.service, 50) << ',' << quantile(r.service, 95) << ',' << quantile(r.service, 99)
        << ',' << (r.service.empty() ? 0 : *std::max_element(r.service.begin(), r.service.end()))
        << ',' << r.end_to_end.size() << ',' << quantile(r.end_to_end, 50) << ',' << quantile(r.end_to_end, 99)
        << ',' << (r.end_to_end.empty() ? 0 : *std::max_element(r.end_to_end.begin(), r.end_to_end.end()))
        << ',' << r.after.upstream_bytes << ',' << r.after.peak_upstream_bytes
        << ',' << static_cast<double>(r.after.upstream_allocations - r.before.upstream_allocations) / static_cast<double>(r.commands)
        << ',' << r.rejected << ',' << r.trades << ',' << r.max_queue << ',' << r.retries
        << ',' << r.commands - r.processed << ',' << (o.experiment == "queue" ? o.rate : 0)
        << ',' << SampleStride << ',' << r.checksum << ',' << r.levels << '\n';
    out.flush();
}
} // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "lob_impact --experiment sustain|scale|queue --active-orders 100000\n"
                             "           --commands 200000 --seconds 5 --repeats 3 --seed 42\n"
                             "           --rate 1000000 --output impact.csv\n"
                             "Scale measures one fixed batch; sustain repeats it for at least --seconds.\n"
                             "Queue offers rate*seconds commands on a scheduled arrival timeline.\n";
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("Missing value for " + arg);
            const std::string value = argv[i];
            if (arg == "--experiment") options.experiment = value;
            else if (arg == "--output") options.output = value;
            else if (arg == "--active-orders") options.active = integer(value);
            else if (arg == "--commands") options.commands = integer(value);
            else if (arg == "--seconds") options.seconds = integer(value);
            else if (arg == "--repeats") options.repeats = integer(value);
            else if (arg == "--rate") options.rate = integer(value);
            else if (arg == "--seed") options.seed = integer(value);
            else throw std::invalid_argument("Unknown option: " + arg);
        }
        if ((options.experiment != "sustain" && options.experiment != "scale" && options.experiment != "queue") ||
            options.active < 8 || options.active > 2000000 || options.commands < 8 || options.commands > 2000000 ||
            options.commands % 8 || !options.seconds || options.seconds > 60 || !options.repeats || options.repeats > 20 ||
            options.rate < 8 || options.rate > 20000000)
            throw std::invalid_argument("Invalid experiment or bounds (active 8..2M, commands 8..2M divisible by 8, seconds 1..60, repeats 1..20, rate 8..20M)");
        const auto batch = mixed_batch(options.active, options.commands, options.seed);
        std::ofstream out(options.output);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << "experiment,allocator,seed,run,active_orders,instruments,commands,processed,elapsed_seconds,commands_per_second,service_samples,service_p50_ns,service_p95_ns,service_p99_ns,service_sample_max_ns,e2e_samples,e2e_p50_ns,e2e_p99_ns,e2e_sample_max_ns,book_bytes,peak_book_bytes,system_allocations_per_command,rejected,trades,queue_max_occupancy,queue_full_retries,lost_commands,offered_rate,sample_stride,checksum,price_levels\n";
        for (std::size_t run = 1; run <= options.repeats; ++run) {
            if (options.experiment == "queue") {
                const auto result = queued(options, batch);
                write_result(out, options, AllocationMode::Pooled, run, result);
            } else {
                const auto first = run % 2 ? AllocationMode::Baseline : AllocationMode::Pooled;
                const auto second = first == AllocationMode::Baseline ? AllocationMode::Pooled : AllocationMode::Baseline;
                for (auto allocator : {first, second}) {
                    const auto result = direct(options, allocator, batch);
                    write_result(out, options, allocator, run, result);
                }
            }
            std::cerr << options.experiment << " active=" << options.active << " run=" << run << " passed\n";
        }
        out.close();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
