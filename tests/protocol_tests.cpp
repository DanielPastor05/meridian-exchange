#include "meridian/protocol.hpp"
#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace meridian;
void check(bool ok) { if (!ok) throw std::runtime_error("protocol check failed"); }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch(const std::runtime_error&) { rejected=true; } check(rejected); }
int main() {
 try {
  wire::Writer out; for (auto v:{1ULL,1ULL,42ULL,2ULL,100ULL,8ULL}) out.u64(v);
  const auto request=wire::decode_request(out.bytes,7);
  check(request==Request{7,1,{Kind::New,42,Side::Sell,100,8}});
  auto encoded=wire::encode({wire::Submit,out.bytes});
  const auto header=wire::decode_header(std::span(encoded).first(16));
  check(header.type==wire::Submit && header.length==48);
  for(std::size_t n=0;n<48;++n) rejects([&] { (void)wire::decode_request(std::span(out.bytes).first(n),7); });
  auto extra=out.bytes;extra.push_back(0);rejects([&] { (void)wire::decode_request(extra,7); });
  for(std::size_t n=0;n<16;++n) rejects([&] { (void)wire::decode_header(std::span(encoded).first(n)); });
  for(auto byte:{0,1,2,3,4,5,12,13,14,15}) {
   auto corrupt=encoded;corrupt[static_cast<std::size_t>(byte)]^=128;
   rejects([&] { (void)wire::decode_header(std::span(corrupt).first(16)); });
  }
  encoded[8]=0;encoded[9]=1;encoded[10]=0;encoded[11]=1;
  rejects([&] { (void)wire::decode_header(std::span(encoded).first(16)); });
  rejects([] { (void)wire::encode({wire::Submit,std::vector<unsigned char>(65537)}); });
  // Structured mutation reaches both accepted and rejected decoding paths.
  std::mt19937_64 random(0xF022);std::size_t accepted=0,rejected=0;
  for(int i=0;i<100000;++i) {
   auto input=out.bytes;input[static_cast<std::size_t>(random()%input.size())]^=static_cast<unsigned char>(random());
   try { const auto r=wire::decode_request(input,7);check(r.account==7);++accepted; }
   catch(const std::runtime_error&) { ++rejected; }
  }
  check(accepted>0 && rejected>0);
  std::cout<<"PASS protocol boundaries and 100000 structured parser mutations\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
