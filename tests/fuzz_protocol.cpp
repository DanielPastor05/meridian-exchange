#include "meridian/protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
 const std::span<const unsigned char> input(data,size);
 try { (void)meridian::wire::decode_header(input); } catch(const std::runtime_error&) {}
 try { (void)meridian::wire::decode_request(input,1); } catch(const std::runtime_error&) {}
 try { meridian::wire::Reader reader(input);(void)reader.u64();(void)reader.text(32);reader.finish(); } catch(const std::runtime_error&) {}
 return 0;
}
