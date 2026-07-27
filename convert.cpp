#include "convert.hpp"
#include "wordlist.hpp"

#include <zstd.h>

#include <algorithm>
#include <sstream>
#include <string>

namespace wordlist
{
namespace
{

int get_bit(const std::vector<std::uint8_t>& data, std::size_t bit_index)
{
    const std::size_t byte_index = bit_index / 8;
    if (byte_index >= data.size())
    {
        return 0;
    }
    const int shift = 7 - static_cast<int>(bit_index % 8);
    return (data[byte_index] >> shift) & 1;
}

void set_bit(std::vector<std::uint8_t>& data, std::size_t bit_index, int bit)
{
    const std::size_t byte_index = bit_index / 8;
    if (byte_index >= data.size())
    {
        data.resize(byte_index + 1, 0);
    }
    const int shift = 7 - static_cast<int>(bit_index % 8);
    if (bit)
    {
        data[byte_index] =
            static_cast<std::uint8_t>(data[byte_index] | (1u << shift));
    }
    else
    {
        data[byte_index] =
            static_cast<std::uint8_t>(data[byte_index] & ~(1u << shift));
    }
}

int word_index(std::string_view word)
{
    const auto begin = ::wordlist::WORDLIST.begin();
    const auto end = ::wordlist::WORDLIST.end();
    const auto it = std::lower_bound(begin, end, word);
    if (it == end || *it != word)
    {
        return -1;
    }
    return static_cast<int>(it - begin);
}

std::vector<std::string_view>
bits_to_words(const std::vector<std::uint8_t>& bits, std::size_t nbits)
{
    const std::size_t word_count =
        (nbits + ::wordlist::BITS_PER_WORD - 1) / ::wordlist::BITS_PER_WORD;

    std::vector<std::string_view> words;
    words.reserve(word_count);

    for (std::size_t w = 0; w < word_count; ++w)
    {
        unsigned index = 0;
        for (std::size_t b = 0; b < ::wordlist::BITS_PER_WORD; ++b)
        {
            const std::size_t bit_i = w * ::wordlist::BITS_PER_WORD + b;
            const int bit = (bit_i < nbits) ? get_bit(bits, bit_i) : 0;
            index = (index << 1) | static_cast<unsigned>(bit);
        }
        words.push_back(::wordlist::WORDLIST[index]);
    }
    return words;
}

}  // namespace

std::vector<std::string_view>
binary_to_words(const std::vector<std::uint8_t>& payload)
{
    // [payload bits][stop:1]
    const std::size_t nbits = payload.size() * 8 + 1;
    std::vector<std::uint8_t> bits((nbits + 7) / 8, 0);

    for (std::size_t i = 0; i < payload.size() * 8; ++i)
    {
        set_bit(bits, i, get_bit(payload, i));
    }
    set_bit(bits, payload.size() * 8, 1);  // stop bit

    return bits_to_words(bits, nbits);
}

std::vector<std::string> split_words(const std::string& text)
{
    std::vector<std::string> words;
    std::istringstream iss(text);
    std::string token;
    while (iss >> token)
    {
        words.push_back(token);
    }
    return words;
}

bool words_to_binary(const std::vector<std::string>& words,
                     std::vector<std::uint8_t>& payload,
                     std::string& err)
{
    if (words.empty())
    {
        err = "no words provided";
        return false;
    }

    const std::size_t total_bits = words.size() * ::wordlist::BITS_PER_WORD;
    std::vector<std::uint8_t> bits((total_bits + 7) / 8, 0);

    for (std::size_t w = 0; w < words.size(); ++w)
    {
        const int index = word_index(words[w]);
        if (index < 0)
        {
            err = "unknown word: " + std::string(words[w]);
            return false;
        }
        for (std::size_t b = 0; b < ::wordlist::BITS_PER_WORD; ++b)
        {
            const int bit =
                (index >> (::wordlist::BITS_PER_WORD - 1 - b)) & 1;
            set_bit(bits, w * ::wordlist::BITS_PER_WORD + b, bit);
        }
    }

    // Last 1-bit is the stop bit; trailing zeros are pad.
    std::size_t stop = total_bits;
    while (stop > 0 && get_bit(bits, stop - 1) == 0)
    {
        --stop;
    }
    if (stop == 0)
    {
        err = "missing stop bit in encoding";
        return false;
    }
    --stop;  // index of stop bit

    if (stop % 8 != 0)
    {
        err = "payload bit length is not a multiple of 8";
        return false;
    }

    const std::size_t len = stop / 8;
    payload.assign(len, 0);
    for (std::size_t i = 0; i < stop; ++i)
    {
        set_bit(payload, i, get_bit(bits, i));
    }
    return true;
}

bool compress_zstd(const std::vector<std::uint8_t>& in,
                   std::vector<std::uint8_t>& out,
                   std::string& err)
{
    if (in.empty())
    {
        out.clear();
        return true;
    }

    const std::size_t bound = ZSTD_compressBound(in.size());
    if (ZSTD_isError(bound))
    {
        err = std::string("zstd compress bound: ") + ZSTD_getErrorName(bound);
        return false;
    }

    out.resize(bound);
    const std::size_t written = ZSTD_compress(
        out.data(), out.size(), in.data(), in.size(), ZSTD_maxCLevel());
    if (ZSTD_isError(written))
    {
        err = std::string("zstd compress: ") + ZSTD_getErrorName(written);
        return false;
    }
    out.resize(written);
    return true;
}

bool decompress_zstd(const std::vector<std::uint8_t>& in,
                     std::vector<std::uint8_t>& out,
                     std::string& err)
{
    if (in.empty())
    {
        out.clear();
        return true;
    }

    const unsigned long long decompressed_size =
        ZSTD_getFrameContentSize(in.data(), in.size());
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR)
    {
        err = "zstd decompress: not a valid zstd frame";
        return false;
    }
    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN)
    {
        err = "zstd decompress: unknown content size";
        return false;
    }

    out.resize(static_cast<std::size_t>(decompressed_size));
    const std::size_t written =
        ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(written))
    {
        err = std::string("zstd decompress: ") + ZSTD_getErrorName(written);
        return false;
    }
    out.resize(written);
    return true;
}

}  // namespace wordlist
