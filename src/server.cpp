#include "meridian/net.hpp"
#include "meridian/snapshot.hpp"
#include <algorithm>
#include <charconv>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

using namespace meridian;
namespace {
static_assert(std::atomic<bool>::is_always_lock_free, "signal flag must be lock-free");
std::atomic<bool> interrupted{false};
void interrupt(int) { interrupted.store(true, std::memory_order_relaxed); }
std::uint64_t number(std::string_view text) {
    std::uint64_t value{};
    const auto [end,error] = std::from_chars(text.data(),text.data()+text.size(),value);
    if (error != std::errc{} || end != text.data()+text.size()) throw std::invalid_argument("invalid integer option");
    return value;
}
struct Counters {
    std::atomic<std::uint64_t> connections{}, rejected_connections{}, frames{}, authentication_failures{}, disconnect_errors{}, responses{};
};
struct Worker { std::shared_ptr<std::atomic<bool>> done; std::jthread thread; std::string origin; };
} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path journal, accounts;
        std::string host="127.0.0.1", fault_stage;
        std::uint64_t port=9000, capacity=65536, clients=32, timeout=5000, fault_account=1, fault_request=1;
        Durability durability=Durability::Sync;
        bool allow_remote=false;
        // Zero leaves each of these off.
        std::uint64_t max_account_sessions=0, max_origin_connections=0, metrics_account=0;
        for (int i=1;i<argc;++i) {
            const std::string key=argv[i];
            if (key=="--help") {
                std::cout << "exchange_server --journal PATH --accounts FILE [--host 127.0.0.1] [--port 9000] "
                             "[--capacity 65536] [--max-clients 32] [--timeout-ms 5000] [--durability sync|buffered] "
                             "[--allow-remote] [--max-account-sessions N] [--max-origin-connections N] [--metrics-account ID]\n";
                return 0;
            }
            if (key=="--allow-remote") { allow_remote=true; continue; }
            if (++i>=argc) throw std::invalid_argument("option needs a value");
            const std::string value=argv[i];
            if (key=="--journal") journal=value;
            else if (key=="--accounts") accounts=value;
            else if (key=="--host") host=value;
            else if (key=="--port") port=number(value);
            else if (key=="--capacity") capacity=number(value);
            else if (key=="--max-clients") clients=number(value);
            else if (key=="--timeout-ms") timeout=number(value);
            else if (key=="--durability" && value=="sync") durability=Durability::Sync;
            else if (key=="--durability" && value=="buffered") durability=Durability::Buffered;
            else if (key=="--max-account-sessions") max_account_sessions=number(value);
            else if (key=="--max-origin-connections") max_origin_connections=number(value);
            else if (key=="--metrics-account") metrics_account=number(value);
            else if (key=="--fault-stage") fault_stage=value;
            else if (key=="--fault-account") fault_account=number(value);
            else if (key=="--fault-request") fault_request=number(value);
            else throw std::invalid_argument("unknown option");
        }
        if (journal.empty() || accounts.empty() || port>65535 || clients==0 || clients>128 ||
            capacity==0 || capacity>1000000 || timeout<100 || timeout>30000)
            throw std::invalid_argument("invalid server configuration");
        const std::set<std::string> stages{"before_append","mid_append","after_write","after_sync","after_apply","before_response","after_response"};
        if (!fault_stage.empty() && !stages.contains(fault_stage)) throw std::invalid_argument("unknown fault stage");
        std::atomic<bool> stopping=false, fault_armed=false;
        FaultHook pause;
        FaultHook fault;
        if (!fault_stage.empty()) pause=[&](std::string_view stage) {
            if (stage==fault_stage) {
                std::cout << "{\"type\":\"fault_ready\",\"stage\":\"" << stage << "\"}\n" << std::flush;
                // Test harness terminates this process externally at this exact boundary.
                for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        };
        if (pause) fault=[&](std::string_view stage) { if (fault_armed.load()) pause(stage); };
        const auto configured=read_accounts(accounts);
        // A sentence in the README is not a control. This protocol sends tokens in the clear,
        // so anything but loopback has to be asked for, and never with the demo's tokens.
        if (!host.starts_with("127.")) {
            if (!allow_remote) throw std::invalid_argument("refusing a reachable address without --allow-remote: this protocol has no TLS");
            for (const auto& account:configured) {
                if (account.token.find_first_not_of(account.token.front())==std::string::npos)
                    throw std::invalid_argument("refusing a reachable address with fixture tokens in the account file");
            }
        }
        DurableExchange exchange(journal,static_cast<std::size_t>(capacity),configured,durability,fault);
        net::Runtime runtime;
        auto listener=net::listen(host,static_cast<std::uint16_t>(port),static_cast<int>(clients));
        std::mutex mutex;
        BookSnapshots snapshots;
        Counters counters;
        // Published after each request, so Metrics can report it without taking the lock.
        std::atomic<std::uint64_t> published_sequence{exchange.state().sequence()};
        std::map<AccountId,std::uint64_t> sessions; // Guarded by mutex.
        std::signal(SIGINT,interrupt); std::signal(SIGTERM,interrupt);
        std::cout << "{\"type\":\"ready\",\"port\":" << listener.port << ",\"sequence\":"
                  << exchange.state().sequence() << ",\"state_hash\":\"" << exchange.state().state_hash() << "\",\"durability\":\""
                  << (durability==Durability::Sync?"sync":"buffered") << "\"}\n" << std::flush;
        std::vector<Worker> workers;
        workers.reserve(static_cast<std::size_t>(clients));
        struct StopOnExit { std::atomic<bool>& flag; ~StopOnExit() { flag.store(true); } } stop_on_exit{stopping};
        const auto serve=[&](net::Socket socket, const std::shared_ptr<std::atomic<bool>>& done) {
            AccountId account=0;
            try {
                while (!stopping.load()) {
                    wire::Frame input;
                    if (!net::receive(socket.get(),input,static_cast<int>(timeout),stopping)) break;
                    ++counters.frames;
                    wire::Frame output;
                    bool close_after=false;
                    bool submitted=false;
                    bool target=false;
                    if (account!=0 && (input.type==wire::Ping || input.type==wire::Metrics)) {
                        // Outside the lock on purpose. A writer stuck in fsync holds the lock, and
                        // that is exactly when these two have to keep answering: a health check
                        // queued behind the problem cannot report it.
                        wire::Reader reader(input.payload);
                        reader.finish();
                        if (input.type==wire::Ping) output={static_cast<std::uint16_t>(wire::Ping|0x8000),{}};
                        else if (metrics_account!=0 && account!=metrics_account) output=wire::error(2);
                        else {
                            wire::Writer out;
                            for (auto v:{counters.connections.load(),counters.rejected_connections.load(),counters.frames.load(),
                                         counters.authentication_failures.load(),counters.disconnect_errors.load(),counters.responses.load(),
                                         published_sequence.load()}) out.u64(v);
                            output={static_cast<std::uint16_t>(wire::Metrics|0x8000),std::move(out.bytes)};
                        }
                    } else {
                        std::lock_guard lock(mutex);
                        wire::Reader reader(input.payload);
                        if (account==0) {
                            if (input.type!=wire::Hello) { output=wire::error(2); close_after=true; }
                            else {
                                const auto id=reader.u64(); const auto token=reader.text(32); reader.finish();
                                if (!exchange.state().authenticate(id,token)) {
                                    ++counters.authentication_failures; output=wire::error(2); close_after=true;
                                } else if (max_account_sessions!=0 && sessions[id]>=max_account_sessions) {
                                    output=wire::error(3); close_after=true;
                                } else { account=id; ++sessions[id]; output=wire::account(id,exchange.state(),wire::Hello); }
                            }
                        } else if (input.type==wire::Submit) {
                            const auto request=wire::decode_request(input.payload,account);
                            target=account==fault_account && request.sequence==fault_request;
                            fault_armed.store(target);
                            try { output=wire::outcome(exchange.execute(request)); }
                            catch (const std::exception& error) {
                                // An append/apply error poisons the exchange. No later client may continue.
                                stopping.store(true);
                                std::cerr << "fatal exchange failure: " << error.what() << '\n';
                                throw;
                            }
                            published_sequence.store(exchange.state().sequence());
                            submitted=true;
                        } else if (input.type==wire::Account) {
                            reader.finish(); output=wire::account(account,exchange.state(),wire::Account);
                        } else if (input.type==wire::Events) {
                            const auto after=reader.u64(), limit=reader.u64(); reader.finish();
                            if (limit==0 || limit>256) throw std::runtime_error("invalid event limit");
                            output=wire::feed(exchange.state().feed(after,static_cast<std::size_t>(limit)));
                        } else if (input.type==wire::Book) {
                            const auto offset=reader.u64(), version=reader.u64(), limit=reader.u64(); reader.finish();
                            if (limit==0 || limit>256) throw std::runtime_error("invalid book limit");
                            wire::Writer out;
                            const auto page=snapshots.page(exchange.state(),offset,version,static_cast<std::size_t>(limit));
                            out.u64(page.changed); out.u64(page.version); out.u64(page.total); out.u64(page.offset); out.u64(page.orders.size());
                            for (const auto& o:page.orders) {
                                out.u64(o.id);out.u64(static_cast<std::uint64_t>(o.side));out.u64(static_cast<std::uint64_t>(o.price));out.u64(o.quantity);
                            }
                            output={static_cast<std::uint16_t>(wire::Book|0x8000),std::move(out.bytes)};
                        } else { output=wire::error(1);close_after=true; }
                    }
                    // Socket writes cannot hold up the serialized matching state.
                    if (submitted && target && pause) pause("before_response");
                    net::send(socket.get(),output,static_cast<int>(timeout),stopping);
                    ++counters.responses;
                    if (submitted && target && pause) pause("after_response");
                    if (close_after) break;
                }
            } catch (const std::exception&) { ++counters.disconnect_errors; }
            if (account!=0) { std::lock_guard lock(mutex); --sessions[account]; }
            done->store(true);
        };
        const auto reap=[&] {
            for (auto it=workers.begin();it!=workers.end();) {
                if (it->done->load()) { it->thread.join(); it=workers.erase(it); } else ++it;
            }
        };
        while (!interrupted && !stopping.load()) {
            reap();
            if (!net::readable(listener.socket.get(),50)) continue;
            auto accepted=net::accept(listener.socket.get());
            if (accepted.socket.get()==net::invalid) {
                if (accepted.error==net::AcceptError::ResourcePressure) {
                    std::cerr << "accept resource pressure: " << accepted.system_error << '\n';
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                continue;
            }
            auto socket=std::move(accepted.socket);
            ++counters.connections;
            // Again, because the wait above can be 50 ms long: a connection that has since
            // closed must not count against the one arriving now.
            reap();
            if (workers.size()>=clients) { ++counters.rejected_connections; continue; }
            // Workers are only touched from this thread, so counting them needs no lock.
            const auto from_origin=std::ranges::count_if(workers,[&](const Worker& worker) { return worker.origin==accepted.origin; });
            if (max_origin_connections!=0 && static_cast<std::uint64_t>(from_origin)>=max_origin_connections) { ++counters.rejected_connections; continue; }
            auto done=std::make_shared<std::atomic<bool>>(false);
            workers.push_back({done,std::jthread(serve,std::move(socket),done),accepted.origin});
        }
        stopping.store(true);
        for (auto& worker:workers) worker.thread.join();
        std::cout << "{\"type\":\"stopped\",\"sequence\":" << exchange.state().sequence()
                  << ",\"state_hash\":\"" << exchange.state().state_hash() << "\"}\n";
        return interrupted ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << "server: " << error.what() << '\n'; return 1; }
}
