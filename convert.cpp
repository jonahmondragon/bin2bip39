#include "convert.hpp"

#include "wordlist.hpp"

#include <zstd.h>

#include <algorithm>
#include <sstream>

namespace bin2bip39
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
    const auto begin = bip39::WORDLIST.begin();
    const auto end = bip39::WORDLIST.end();
    const auto it = std::lower_bound(begin, end, word);
    if (it == end || *it != word)
    {
        return -1;
    }
    return static_cast<int>(it - begin);
}

}  // namespace

std::vector<std::string_view>
binary_to_words(const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> data;
    data.reserve(4 + payload.size());

    const auto len = static_cast<std::uint32_t>(payload.size());
    data.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>(len & 0xFF));
    data.insert(data.end(), payload.begin(), payload.end());

    const std::size_t total_bits = data.size() * 8;
    const std::size_t word_count =
        (total_bits + bip39::BITS_PER_WORD - 1) / bip39::BITS_PER_WORD;

    std::vector<std::string_view> words;
    words.reserve(word_count);

    for (std::size_t w = 0; w < word_count; ++w)
    {
        unsigned index = 0;
        for (std::size_t b = 0; b < bip39::BITS_PER_WORD; ++b)
        {
            index = (index << 1) |
                    static_cast<unsigned>(
                        get_bit(data, w * bip39::BITS_PER_WORD + b));
        }
        words.push_back(bip39::WORDLIST[index]);
    }
    return words;
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

    std::vector<std::uint8_t> data;
    data.reserve((words.size() * bip39::BITS_PER_WORD + 7) / 8);

    for (std::size_t w = 0; w < words.size(); ++w)
    {
        const int index = word_index(words[w]);
        if (index < 0)
        {
            err = "unknown BIP39 word: " + words[w];
            return false;
        }
        for (std::size_t b = 0; b < bip39::BITS_PER_WORD; ++b)
        {
            const int bit =
                (index >> (bip39::BITS_PER_WORD - 1 - b)) & 1;
            set_bit(data, w * bip39::BITS_PER_WORD + b, bit);
        }
    }

    if (data.size() < 4)
    {
        err = "encoded data too short";
        return false;
    }

    const std::uint32_t len =
        (static_cast<std::uint32_t>(data[0]) << 24) |
        (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) |
        static_cast<std::uint32_t>(data[3]);

    if (static_cast<std::size_t>(len) > data.size() - 4)
    {
        err = "invalid payload length in encoding";
        return false;
    }

    const std::size_t used_bits = (4 + static_cast<std::size_t>(len)) * 8;
    const std::size_t total_bits = words.size() * bip39::BITS_PER_WORD;
    for (std::size_t i = used_bits; i < total_bits; ++i)
    {
        if (get_bit(data, i) != 0)
        {
            err = "non-zero padding bits in encoding";
            return false;
        }
    }

    payload.assign(data.begin() + 4,
                   data.begin() + 4 + static_cast<std::ptrdiff_t>(len));
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

}  // namespace bin2bip39
