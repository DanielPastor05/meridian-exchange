#include "meridian/engine.hpp"
#include "meridian/journal.hpp"
#include "meridian/service.hpp"
#include "meridian/text.hpp"
#include "meridian/version.hpp"

#include <charconv>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

using namespace meridian;

namespace {
template <class T> T number(const std::string& text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("invalid integer");
    return value;
}

Command parse(const std::vector<std::string>& words) {
    if (words.size() == 2 && words[0] == "CANCEL") return Command::cancel(number<OrderId>(words[1]));
    if (words.size() != 5 || words[0] != "NEW" || (words[2] != "BUY" && words[2] != "SELL"))
        throw std::invalid_argument("expected NEW id BUY|SELL price quantity, CANCEL id, or BOOK");
    return {Kind::New, number<OrderId>(words[1]), words[2] == "BUY" ? Side::Buy : Side::Sell,
            number<Price>(words[3]), number<Quantity>(words[4])};
}

void print_result(const Command& command, const Result& result) {
    std::cout << "{\"type\":\"result\",\"sequence\":\"" << result.sequence
              << "\",\"id\":\"" << command.id << "\",\"status\":\"" << name(result.status)
              << "\",\"remaining\":\"" << result.remaining << "\",\"trades\":[";
    bool first = true;
    for (const auto& trade : result.trades) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"maker\":\"" << trade.maker << "\",\"taker\":\"" << trade.taker
                  << "\",\"price\":\"" << trade.price << "\",\"quantity\":\"" << trade.quantity << "\"}";
    }
    std::cout << "]}\n";
}

void print_book(const Engine& engine) {
    std::cout << "{\"type\":\"book\",\"sequence\":\"" << engine.sequence()
              << "\",\"state_hash\":\"" << engine.state_hash() << "\",\"orders\":[";
    bool first = true;
    for (const auto& order : engine.snapshot()) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"id\":\"" << order.id << "\",\"side\":\""
                  << (order.side == Side::Buy ? "BUY" : "SELL") << "\",\"price\":\""
                  << order.price << "\",\"quantity\":\"" << order.quantity << "\"}";
    }
    std::cout << "]}\n";
}

// Rebuilds an account journal's history from the file alone, without a server and without
// writing anything. The server keeps only the last outcome per account and its feed is a
// ring, so this is the way to answer what happened to an older request, or to one account's
// orders. Executions name both sides: a resting order is filled by someone else's request.
int inspect(const std::filesystem::path& journal, const std::filesystem::path& accounts_file,
            std::optional<AccountId> only) {
    const auto accounts = read_accounts(accounts_file);
    const auto header = scan_journal_requests(journal);
    if (header.configuration != configuration_hash(accounts))
        throw std::runtime_error("journal was written under a different account configuration");
    ExchangeState state(header.capacity, accounts);
    std::unordered_map<OrderId, AccountId> owners;
    std::uint64_t seen = 0;
    const auto owner = [&](OrderId id) {
        const auto found = owners.find(id);
        return found == owners.end() ? AccountId{} : found->second;
    };
    const auto info = scan_journal_requests(journal, [&](auto sequence, const Request& request) {
        const auto outcome = state.apply(request, sequence);
        if (request.command.kind == Kind::New && outcome.code == Code::Accepted)
            owners[request.command.id] = request.account;
        if (!only || *only == request.account) {
            std::cout << "{\"type\":\"outcome\",\"global_sequence\":\"" << outcome.global_sequence
                      << "\",\"account\":\"" << request.account
                      << "\",\"request_sequence\":\"" << outcome.request_sequence
                      << "\",\"order\":\"" << request.command.id
                      << "\",\"status\":\"" << name(outcome.code)
                      << "\",\"filled\":\"" << outcome.filled
                      << "\",\"remaining\":\"" << outcome.remaining
                      << "\",\"cancelled\":\"" << outcome.cancelled << "\"}\n";
        }
        for (;;) {
            const auto feed = state.feed(seen, 256);
            if (feed.gap) {
                // More events from one request than the ring holds. Say so: a silently
                // shorter list would read as those executions never having happened.
                std::cout << "{\"type\":\"executions_lost\",\"global_sequence\":\"" << sequence << "\"}\n";
                seen = feed.latest;
                break;
            }
            if (feed.events.empty()) break;
            for (const auto& event : feed.events) {
                seen = event.sequence;
                if (event.type != 1) continue;
                const auto maker = owner(event.trade.maker), taker = owner(event.trade.taker);
                if (only && *only != maker && *only != taker) continue;
                std::cout << "{\"type\":\"execution\",\"global_sequence\":\"" << event.global_sequence
                          << "\",\"maker\":\"" << event.trade.maker << "\",\"maker_account\":\"" << maker
                          << "\",\"taker\":\"" << event.trade.taker << "\",\"taker_account\":\"" << taker
                          << "\",\"price\":\"" << event.trade.price
                          << "\",\"quantity\":\"" << event.trade.quantity << "\"}\n";
            }
        }
    });
    state.verify();
    std::cout << "{\"type\":\"inspect\",\"records\":" << info.records
              << ",\"incomplete_tail_bytes\":" << info.incomplete_tail_bytes
              << ",\"state_hash\":\"" << state.state_hash() << "\"}\n";
    return 0;
}

void usage() {
    std::cout << "Meridian Exchange " MERIDIAN_VERSION "\n"
              << "  exchange run --journal PATH [--input FILE] [--capacity N] [--durability sync|buffered]\n"
              << "  exchange replay JOURNAL\n"
              << "  exchange inspect JOURNAL ACCOUNTS [--account ID]    read-only history of a server journal\n"
              << "Commands: NEW id BUY|SELL price_ticks quantity; CANCEL id; BOOK (one per line).\n"
              << "Default durability is sync. Use a fresh journal for a fresh session.\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1 || std::string_view(argv[1]) == "--help") { usage(); return 0; }
        if (std::string_view(argv[1]) == "replay") {
            if (argc != 3) throw std::invalid_argument("replay requires one journal path");
            const auto info = scan_journal(argv[2]);
            Engine engine(info.capacity);
            Result result;
            const auto replayed = scan_journal(argv[2], [&](auto sequence, const auto& command) {
                engine.apply(command, result);
                if (result.sequence != sequence) throw std::runtime_error("replay sequence mismatch");
            });
            engine.verify();
            std::cout << "{\"type\":\"replay\",\"records\":" << replayed.records
                      << ",\"incomplete_tail_bytes\":" << replayed.incomplete_tail_bytes << "}\n";
            print_book(engine);
            return 0;
        }
        if (std::string_view(argv[1]) == "inspect") {
            if (argc == 4) return inspect(argv[2], argv[3], std::nullopt);
            if (argc == 6 && std::string_view(argv[4]) == "--account")
                return inspect(argv[2], argv[3], number<AccountId>(argv[5]));
            throw std::invalid_argument("inspect requires a journal and an account file, optionally --account ID");
        }
        if (std::string_view(argv[1]) != "run") throw std::invalid_argument("unknown mode");
        std::filesystem::path path, input_path;
        Durability durability = Durability::Sync;
        std::size_t capacity = 65536;
        bool capacity_given = false;
        for (int i = 2; i < argc; i += 2) {
            if (i + 1 >= argc) throw std::invalid_argument("option requires a value");
            const std::string_view option = argv[i];
            const std::string value = argv[i + 1];
            if (option == "--journal") path = value;
            else if (option == "--input") input_path = value;
            else if (option == "--capacity") { capacity = number<std::size_t>(value); capacity_given = true; }
            else if (option == "--durability" && value == "sync") durability = Durability::Sync;
            else if (option == "--durability" && value == "buffered") durability = Durability::Buffered;
            else throw std::invalid_argument("unknown option or value");
        }
        if (path.empty()) throw std::invalid_argument("--journal is required");
        if (!capacity_given && std::filesystem::exists(path) && std::filesystem::file_size(path) > 0)
            capacity = scan_journal(path).capacity;
        std::ifstream input;
        if (!input_path.empty()) {
            input.open(input_path);
            if (!input) throw std::runtime_error("cannot read input file");
        }
        Journal journal(path, capacity, durability);
        Engine engine(capacity);
        Result result;
        (void)journal.replay([&](auto sequence, const auto& command) {
            engine.apply(command, result);
            if (result.sequence != sequence) throw std::runtime_error("replay sequence mismatch");
        });
        std::istream& stream = input_path.empty() ? std::cin : input;
        std::string line;
        std::size_t line_number = 0, errors = 0;
        bool exceeded = false;
        while (read_bounded_line(stream, line, exceeded)) {
            ++line_number;
            if (exceeded) {
                ++errors;
                std::cerr << "line " << line_number << ": command exceeds 4096 bytes\n";
                continue;
            }
            std::vector<std::string> words;
            std::istringstream tokens(line.substr(0, line.find('#')));
            for (std::string word; tokens >> word;) words.push_back(word);
            if (words.empty()) continue;
            if (words.size() == 1 && words[0] == "BOOK") { print_book(engine); continue; }
            Command command;
            try {
                command = parse(words);
            } catch (const std::invalid_argument& error) {
                ++errors;
                std::cerr << "line " << line_number << ": " << error.what() << '\n';
                continue;
            }
            // Journal before mutation and before the response. An interrupted
            // response has an uncertain outcome; replay includes its command.
            journal.append(command);
            engine.apply(command, result);
            print_result(command, result);
            std::cout.flush();
            if (!std::cout) throw std::runtime_error("response output failed; outcome may be committed");
        }
        if (stream.bad()) throw std::runtime_error("input read failed");
        print_book(engine);
        return errors == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "exchange: " << error.what() << '\n';
        return 1;
    }
}
