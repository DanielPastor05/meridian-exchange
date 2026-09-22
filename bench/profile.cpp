#include "meridian/engine.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#ifdef _WIN32
#include <malloc.h>
#endif
namespace {
thread_local bool counting=false;
thread_local std::uint64_t allocations=0;
void counted() { if(counting) ++allocations; }
}
void* operator new(std::size_t n) { counted();if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n,std::align_val_t a) {
 counted();const auto alignment=static_cast<std::size_t>(a);n=std::max(n,std::size_t{1});
#ifdef _WIN32
 if(auto p=_aligned_malloc(n,alignment))return p;
#else
 void* p=nullptr;if(posix_memalign(&p,alignment,n)==0)return p;
#endif
 throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void* p,std::align_val_t) noexcept {
#ifdef _WIN32
 _aligned_free(p);
#else
 std::free(p);
#endif
}
void operator delete[](void* p,std::align_val_t a) noexcept { ::operator delete(p,a); }
void operator delete(void* p,std::size_t,std::align_val_t a) noexcept { ::operator delete(p,a); }
void operator delete[](void* p,std::size_t,std::align_val_t a) noexcept { ::operator delete(p,a); }
using namespace meridian;
using Clock=std::chrono::steady_clock;
struct Item {Command command;int operation;};
std::size_t number(const char* text) {
 std::size_t value{};const std::string_view s(text);const auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),value);
 if(error!=std::errc{}||end!=s.data()+s.size()||value==0)throw std::runtime_error("invalid positive integer");
 return value;
}
Command initial(std::size_t i,OrderId id) {
 const auto side=i%2==0?Side::Buy:Side::Sell;
 const auto offset=static_cast<Price>((i/2)%1000);
 return {Kind::New,id,side,side==Side::Buy?9000-offset:11000+offset,10};
}
double percentile(std::vector<double> x,double p) {std::sort(x.begin(),x.end());return x.at(static_cast<std::size_t>(std::ceil(p*static_cast<double>(x.size())))-1);}
int main(int argc,char** argv) {
 try {
  const auto count=argc>1?number(argv[1]):100000;
  const auto repeats=argc>2?number(argv[2]):3;
  if(argc>3||count<5||count>2000000||repeats>20)throw std::runtime_error("usage: exchange_profile [commands 5..2000000] [repetitions 1..20]");
  std::cout<<"# core only; prefill excluded; four operation classes; deterministic rotating cancels; up to 2001 levels\n";
  std::cout<<"# ordinary/array/aligned allocations counted only inside apply; latency instrumentation separate from throughput\n";
  std::cout<<"kind,depth,repetition,operation,samples,mean_ns,p50_ns,p99_ns,p999_ns,allocations,hash\n";
  for(const std::size_t depth:{1000,10000,100000}) {
   std::vector<Item> items;items.reserve(count+5);std::vector<OrderId> live(depth);
   for(std::size_t i=0;i<depth;++i)live[i]=i+1;
   OrderId next=depth+1;
   for(std::size_t i=0;items.size()<count;++i) {
    const auto slot=i%depth;
    items.push_back({Command::cancel(live[slot]),0});live[slot]=next++;
    items.push_back({initial(slot,live[slot]),1});
    const auto price=static_cast<Price>(10000+i%50);
    items.push_back({{Kind::New,next++,Side::Sell,price,10},1});
    items.push_back({{Kind::New,next++,Side::Buy,10049,4},2});
    items.push_back({{Kind::New,next++,Side::Buy,10049,6},3});
   }
   const auto seed=[&](Engine& engine,Result& result) {for(std::size_t i=0;i<depth;++i)engine.apply(initial(i,i+1),result);};
   std::uint64_t expected=0;
   for(std::size_t repetition=0;repetition<=repeats;++repetition) {
    Engine engine(depth+64);Result result;result.trades.reserve(16);seed(engine,result);
    allocations=0;counting=true;const auto start=Clock::now();
    for(const auto& item:items) {
     engine.apply(item.command,result);
     if(result.status!=Status::Accepted&&result.status!=Status::Cancelled)throw std::runtime_error("unexpected rejection");
    }
    const auto elapsed=std::chrono::duration<double,std::nano>(Clock::now()-start).count();counting=false;
    const auto allocation_count=allocations;engine.verify();const auto hash=engine.state_hash();
    if(repetition==0)expected=hash;else if(hash!=expected)throw std::runtime_error("benchmark diverged");
    if(repetition)std::cout<<"throughput,"<<depth<<','<<repetition<<",mixed,"<<items.size()<<','<<elapsed/static_cast<double>(items.size())<<",0,0,0,"<<allocation_count<<','<<hash<<'\n';
   }
   Engine engine(depth+64);Result result;result.trades.reserve(16);seed(engine,result);
   std::vector<double> samples[4];for(auto& v:samples)v.reserve(items.size());
   for(const auto& item:items) {
    const auto start=Clock::now();engine.apply(item.command,result);const auto end=Clock::now();
    samples[item.operation].push_back(std::chrono::duration<double,std::nano>(end-start).count());
   }
   if(engine.state_hash()!=expected)throw std::runtime_error("instrumented run diverged");
   const char* names[]{"cancel","insert","partial_fill","full_fill"};
   for(int i=0;i<4;++i)std::cout<<"latency,"<<depth<<",0,"<<names[i]<<','<<samples[i].size()<<",0,"<<percentile(samples[i],.5)<<','<<percentile(samples[i],.99)<<','<<percentile(samples[i],.999)<<",0,"<<expected<<'\n';
   const auto book=engine.snapshot();std::uint64_t checksum=0;const auto begin=Clock::now();
   for(std::size_t i=0;i<1000000;++i){const auto& order=book[(i*7919)%book.size()];checksum+=engine.find(order.id)->quantity;}
   const auto elapsed=std::chrono::duration<double,std::nano>(Clock::now()-begin).count();
   std::cout<<"lookup,"<<depth<<",0,find,1000000,"<<elapsed/1000000.0<<",0,0,0,0,"<<checksum<<'\n';
  }
 }catch(const std::exception& error){counting=false;std::cerr<<error.what()<<'\n';return 1;}
}
