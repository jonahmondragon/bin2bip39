#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wordlist
{

// Binary -> wordlist words. Layout: 4-byte BE length + payload, zero-padded
// to 13-bit groups.
std::vector<std::string_view>
binary_to_words(const std::vector<std::uint8_t>& payload);

// Words -> binary. Inverse of binary_to_words.
bool words_to_binary(const std::vector<std::string>& words,
                     std::vector<std::uint8_t>& payload,
                     std::string& err);

std::vector<std::string> split_words(const std::string& text);

bool compress_zstd(const std::vector<std::uint8_t>& in,
                   std::vector<std::uint8_t>& out,
                   std::string& err);

bool decompress_zstd(const std::vector<std::uint8_t>& in,
                     std::vector<std::uint8_t>& out,
                     std::string& err);

}  // namespace wordlist
