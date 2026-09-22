#include "meridian/net.hpp"
#include <charconv>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

using namespace meridian;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
std::uint64_t number(std::string_view text) {
    std::uint64_t value{};
    const auto [end,error] = std::from_chars(text.data(),text.data()+text.size(),value);
    if (error != std::errc{} || end != text.data()+text.size()) throw std::invalid_argument("invalid integer option");
    return value;
}
struct Counters {
    std::atomic<std::uint64_t> connections{}, rejected_connections{}, frames{}, authentication_failures{}, disconnect_errors{}, responses{};
};
struct Worker { std::shared_ptr<std::atomic<bool>> done; std::jthread thread; };
} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path journal, accounts;
        std::string host="127.0.0.1", fault_stage;
        std::uint64_t port=9000, capacity=65536, clients=32, timeout=5000, fault_account=1, fault_request=1;
        Durability durability=Durability::Sync;
        for (int i=1;i<argc;++i) {
            const std::string key=argv[i];
            if (key=="--help") {
                std::cout << "exchange_server --journal PATH --accounts FILE [--host 127.0.0.1] [--port 9000] "
                             "[--capacity 65536] [--max-clients 32] [--timeout-ms 5000] [--durability sync|buffered]\n";
                return 0;
            }
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
        DurableExchange exchange(journal,static_cast<std::size_t>(capacity),read_accounts(accounts),durability,fault);
        net::Runtime runtime;
        auto listener=net::listen(host,static_cast<std::uint16_t>(port),static_cast<int>(clients));
        std::mutex mutex;
        Counters counters;
        std::signal(SIGINT,interrupt); std::signal(SIGTERM,interrupt);
        std::cout << "{\"type\":\"ready\",\"port\":" << listener.port << ",\"sequence\":"
                  << exchange.state().sequence() << ",\"durability\":\""
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
                    {
                        std::lock_guard lock(mutex);
                        wire::Reader reader(input.payload);
                        if (account==0) {
                            if (input.type!=wire::Hello) { output=wire::error(2); close_after=true; }
                            else {
                                const auto id=reader.u64(); const auto token=reader.text(32); reader.finish();
                                if (!exchange.state().authenticate(id,token)) {
                                    ++counters.authentication_failures; output=wire::error(2); close_after=true;
                                } else { account=id; output=wire::account(id,exchange.state(),wire::Hello); }
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
                            const auto current=exchange.state().sequence();
                            const bool changed=(offset!=0 || version!=0) && version!=current;
                            out.u64(changed); out.u64(current);
                            if (changed) { out.u64(0);out.u64(offset);out.u64(0); }
                            else {
                                const auto book=exchange.state().snapshot();
                                const auto start=std::min(offset,static_cast<std::uint64_t>(book.size()));
                                const auto count=std::min(limit,static_cast<std::uint64_t>(book.size())-start);
                                out.u64(book.size());out.u64(start);out.u64(count);
                                for (auto j=start;j<start+count;++j) {
                                    const auto& o=book[static_cast<std::size_t>(j)];
                                    out.u64(o.id);out.u64(static_cast<std::uint64_t>(o.side));out.u64(static_cast<std::uint64_t>(o.price));out.u64(o.quantity);
                                }
                            }
                            output={static_cast<std::uint16_t>(wire::Book|0x8000),std::move(out.bytes)};
                        } else if (input.type==wire::Ping) { reader.finish(); output={static_cast<std::uint16_t>(wire::Ping|0x8000),{}}; }
                        else if (input.type==wire::Metrics) {
                            reader.finish(); wire::Writer out;
                            for (auto v:{counters.connections.load(),counters.rejected_connections.load(),counters.frames.load(),
                                         counters.authentication_failures.load(),counters.disconnect_errors.load(),counters.responses.load(),
                                         exchange.state().sequence()}) out.u64(v);
                            output={static_cast<std::uint16_t>(wire::Metrics|0x8000),std::move(out.bytes)};
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
            done->store(true);
        };
        while (!interrupted && !stopping.load()) {
            for (auto it=workers.begin();it!=workers.end();) {
                if (it->done->load()) { it->thread.join(); it=workers.erase(it); } else ++it;
            }
            if (!net::readable(listener.socket.get(),50)) continue;
            auto socket=net::accept(listener.socket.get());
            if (socket.get()==net::invalid) continue;
            ++counters.connections;
            if (workers.size()>=clients) { ++counters.rejected_connections; continue; }
            auto done=std::make_shared<std::atomic<bool>>(false);
            workers.push_back({done,std::jthread(serve,std::move(socket),done)});
        }
        stopping.store(true);
        for (auto& worker:workers) worker.thread.join();
        std::cout << "{\"type\":\"stopped\",\"sequence\":" << exchange.state().sequence() << "}\n";
        return interrupted ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << "server: " << error.what() << '\n'; return 1; }
}
