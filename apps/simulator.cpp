#include "lob/replay.hpp"
#include "lob/runner.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

int main(int argc, char** argv) {
    try {
        std::unordered_map<std::string, std::string> options;
        bool queued = false, baseline = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "lob_sim [--input commands.txt] [--events journal.txt] [--snapshot state.json]\n"
                             "        [--verify expected-journal.txt] [--queued] [--baseline]\n"
                             "Without --input, reads commands from stdin. Events default to stdout.\n";
                return 0;
            }
            if (arg == "--queued") queued = true;
            else if (arg == "--baseline") baseline = true;
            else if (arg == "--input" || arg == "--events" || arg == "--snapshot" || arg == "--verify") {
                if (++i == argc || options.contains(arg)) throw std::invalid_argument("Missing or duplicate option: " + arg);
                options.emplace(arg, argv[i]);
            } else throw std::invalid_argument("Unknown option: " + arg);
        }
        for (const auto& [a, path_a] : options)
            for (const auto& [b, path_b] : options)
                if (a < b && (a == "--events" || a == "--snapshot" || b == "--events" || b == "--snapshot") &&
                    (std::filesystem::weakly_canonical(path_a) == std::filesystem::weakly_canonical(path_b) ||
                     (std::filesystem::exists(path_a) && std::filesystem::exists(path_b) &&
                      std::filesystem::equivalent(path_a, path_b))))
                    throw std::invalid_argument("Output paths must differ from all input and other output paths");
        std::ifstream file;
        if (options.contains("--input")) {
            file.open(options.at("--input"));
            if (!file) throw std::runtime_error("Cannot open input file");
        }
        auto input = lob::read_commands(file.is_open() ? file : std::cin);
        std::string expected;
        if (options.contains("--verify")) {
            std::ifstream verify(options.at("--verify"), std::ios::binary);
            if (!verify) throw std::runtime_error("Cannot open verification journal");
            expected.assign(std::istreambuf_iterator<char>(verify), {});
        }
        lob::EngineConfig config;
        if (baseline) config.allocation = lob::AllocationMode::Baseline;
        lob::Engine engine(input.instruments, config);
        std::ostringstream journal;
        lob::write_journal_header(journal, input.instruments);
        std::uint64_t rejections = 0;
        const auto consume = [&](const lob::Command& c) {
            const auto& events = engine.process(c);
            for (const auto& e : events) if (e.type == lob::EventType::Rejected) ++rejections;
            lob::write_journal_step(journal, c, events, engine.checksum());
        };
        lob::QueueStats stats;
        if (queued) stats = lob::run_queued(input.commands, consume);
        else for (const auto& c : input.commands) consume(c);
        const auto invariant = engine.check_invariants();
        if (!invariant.empty()) throw std::runtime_error("Invariant failure: " + invariant);
        const auto result = journal.str();
        if (options.contains("--verify") && result != expected)
            throw std::runtime_error("Replay verification failed: journal, events, or intermediate checksums differ");
        if (options.contains("--events")) {
            std::ofstream out(options.at("--events"));
            out.exceptions(std::ios::badbit | std::ios::failbit);
            out << result;
            out.close();
        } else std::cout << result;
        if (options.contains("--snapshot")) {
            std::ofstream out(options.at("--snapshot"));
            out.exceptions(std::ios::badbit | std::ios::failbit);
            lob::write_snapshot(out, engine);
            out.close();
        }
        std::cerr << "commands=" << input.commands.size() << " accepted=" << engine.sequence()
                  << " rejected=" << rejections << " active_orders=" << engine.active_orders()
                  << " checksum=" << engine.checksum();
        if (queued) std::cerr << " queue_full_retries=" << stats.full_retries << " max_queue=" << stats.max_occupancy;
        std::cerr << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
