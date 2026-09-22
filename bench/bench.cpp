#include "meridian/engine.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace meridian;
using Clock = std::chrono::steady_clock;

namespace {
std::size_t parse(const char* arg) {
    const std::string_view text(arg);
    std::size_t n = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (error != std::errc{} || end != text.data() + text.size() || n == 0)
        throw std::invalid_argument("expected positive integer");
    return n;
}

std::vector<Command> workload(std::string_view mode, std::size_t count) {
    std::vector<Command> commands;
    commands.reserve(count + 64);
    OrderId next_id = 1;
    while (commands.size() < count) {
        if (mode == "rest_cancel") {
            const auto first = next_id;
            for (int i = 0; i < 32; ++i)
                commands.push_back({Kind::New, next_id++, i % 2 == 0 ? Side::Buy : Side::Sell,
                                    i % 2 == 0 ? 9900 - i : 10100 + i, 10});
            for (OrderId i = 0; i < 32; ++i) commands.push_back(Command::cancel(first + i));
        } else if (mode == "cross") {
            commands.push_back({Kind::New, next_id++, Side::Sell, 10000, 10});
            commands.push_back({Kind::New, next_id++, Side::Buy, 10000, 10});
        } else {
            for (int i = 0; i < 32; ++i)
                commands.push_back({Kind::New, next_id++, Side::Sell, 10000 + i / 4, 10});
            commands.push_back({Kind::New, next_id++, Side::Buy, 10100, 320});
        }
    }
    commands.resize(count);
    return commands;
}

struct Run {
    double ns_per_command{};
    std::uint64_t hash{};
    std::size_t executions{};
};
Run throughput(const std::vector<Command>& commands) {
    Engine engine(4096);
    Result result;
    result.trades.reserve(64);
    std::size_t executions = 0;
    const auto start = Clock::now();
    for (const auto& command : commands) {
        engine.apply(command, result);
        executions += result.trades.size();
        if (result.status != Status::Accepted && result.status != Status::Cancelled)
            throw std::runtime_error("benchmark unexpectedly rejected a command");
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    engine.verify();
    return {elapsed / static_cast<double>(commands.size()), engine.state_hash(), executions};
}

double percentile(const std::vector<double>& sorted, double p) {
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    return sorted.at(rank - 1);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 3) throw std::invalid_argument("usage: exchange_bench [commands=200000] [repetitions=5]");
        const auto count = argc > 1 ? parse(argv[1]) : 200000;
        const auto repeats = argc > 2 ? parse(argv[2]) : 5;
        if (count > 10000000 || repeats > 100) throw std::invalid_argument("maximum: 10000000 commands, 100 repetitions");
        std::cout << "# Core only; single writer; no parser, network, journal or durable acknowledgement.\n"
                  << "# Synthetic workloads, <=32 resting orders. Latency is a separate instrumented pass.\n"
                  << "# Every raw throughput repetition is emitted below; no guaranteed latency target.\n";
#ifdef _MSC_VER
        std::cout << "# compiler=MSVC " << _MSC_VER << '\n';
#elif defined(__clang__)
        std::cout << "# compiler=Clang " << __clang_version__ << '\n';
#elif defined(__GNUC__)
        std::cout << "# compiler=GCC " << __VERSION__ << '\n';
#endif
#ifdef NDEBUG
        std::cout << "# assertions=NDEBUG (use a Release build for performance)\n";
#else
        std::cout << "# WARNING: not a Release build\n";
#endif
        std::cout << "workload,commands,repetitions,median_ns_per_command,median_commands_per_second,"
                  << "instrumented_p50_ns,instrumented_p99_ns,instrumented_p999_ns,executions,state_hash\n";
        std::cout << std::fixed << std::setprecision(2);
        for (const auto mode : {"rest_cancel", "cross", "sweep"}) {
            const auto commands = workload(mode, count);
            const auto expected = throughput(commands); // Warm-up, not a timed sample in the summary.
            std::vector<double> times;
            for (std::size_t i = 0; i < repeats; ++i) {
                const auto run = throughput(commands);
                if (run.hash != expected.hash || run.executions != expected.executions)
                    throw std::runtime_error("benchmark repetitions diverged");
                times.push_back(run.ns_per_command);
                std::cout << "# raw," << mode << ',' << i + 1 << ',' << run.ns_per_command << '\n';
            }
            std::sort(times.begin(), times.end());
            const double median = repeats % 2 == 0 ? (times[repeats / 2 - 1] + times[repeats / 2]) / 2
                                                   : times[repeats / 2];
            Engine engine(4096);
            Result result;
            result.trades.reserve(64);
            std::vector<double> latencies;
            latencies.reserve(count);
            for (const auto& command : commands) {
                const auto start = Clock::now();
                engine.apply(command, result);
                const auto end = Clock::now();
                latencies.push_back(std::chrono::duration<double, std::nano>(end - start).count());
            }
            if (engine.state_hash() != expected.hash) throw std::runtime_error("latency pass diverged");
            std::sort(latencies.begin(), latencies.end());
            std::cout << mode << ',' << count << ',' << repeats << ',' << median << ',' << 1e9 / median << ','
                      << percentile(latencies, 0.50) << ',' << percentile(latencies, 0.99) << ','
                      << percentile(latencies, 0.999) << ',' << expected.executions << ',' << expected.hash << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark: " << error.what() << '\n';
        return 1;
    }
}
