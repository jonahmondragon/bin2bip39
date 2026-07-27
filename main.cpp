#include "convert.hpp"
#include "wordlist.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0
              << " [options] [input-file|-] [output-file]\n"
              << "  input-file may be omitted or '-' to read from stdin\n"
              << "Options (non-positional):\n"
              << "  -w, --word-to-binary   words -> binary"
              << " (default: binary -> words)\n"
              << "  -c, --compression     lossless zstd compress before encode /\n"
              << "                        decompress after decode\n"
              << "  -r, --random N [fmt]  output N random words (fmt: space|nospace|newline)\n";
}

bool parse_u64(std::string_view s, std::uint64_t& out)
{
    if (s.empty())
    {
        return false;
    }
    std::uint64_t v = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10)
        {
            return false;
        }
        v = v * 10 + d;
    }
    out = v;
    return true;
}

bool write_random_words(std::ostream& out, std::uint64_t n, std::string_view fmt)
{
    // Prefer real dictionary words; skip synthetic zzpad* fillers.
    std::size_t real_count = ::wordlist::WORD_COUNT;
    while (real_count > 0)
    {
        const auto w = ::wordlist::WORDLIST[real_count - 1];
        if (w.size() < 5 || w.substr(0, 5) != "zzpad")
        {
            break;
        }
        --real_count;
    }
    if (real_count == 0)
    {
        real_count = ::wordlist::WORD_COUNT;
    }
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<std::size_t> dist(0, real_count - 1);

    const char* sep = " ";
    bool newline_each = false;
    if (fmt == "nospace")
    {
        sep = "";
    }
    else if (fmt == "newline")
    {
        sep = "\n";
        newline_each = true;
    }
    else if (fmt != "space" && !fmt.empty())
    {
        return false;
    }

    for (std::uint64_t i = 0; i < n; ++i)
    {
        if (i > 0 && !newline_each)
        {
            out << sep;
        }
        out << ::wordlist::WORDLIST[dist(gen)];
        if (newline_each)
        {
            out << '\n';
        }
    }
    if (!newline_each && n > 0)
    {
        out << '\n';
    }
    return static_cast<bool>(out);
}

bool is_stdin_path(const std::string& path)
{
    return path.empty() || path == "-";
}

bool read_stream_binary(std::istream& in, std::vector<std::uint8_t>& out)
{
    out.clear();
    char buf[8192];
    while (in)
    {
        in.read(buf, sizeof(buf));
        const auto n = in.gcount();
        if (n > 0)
        {
            out.insert(out.end(), buf, buf + n);
        }
    }
    return in.eof() || static_cast<bool>(in);
}

bool read_file(const std::string& path, std::vector<std::uint8_t>& out)
{
    if (is_stdin_path(path))
    {
        return read_stream_binary(std::cin, out);
    }

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    return read_stream_binary(in, out);
}

bool read_text_file(const std::string& path, std::string& out)
{
    if (is_stdin_path(path))
    {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        out = ss.str();
        return true;
    }

    std::ifstream in(path);
    if (!in)
    {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in && !in.eof())
    {
        return false;
    }
    out = ss.str();
    return true;
}

bool write_words(std::ostream& out, const std::vector<std::string_view>& words)
{
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        if (i > 0)
        {
            out << ' ';
        }
        out << words[i];
    }
    out << '\n';
    return static_cast<bool>(out);
}

bool write_binary(std::ostream& out, const std::vector<std::uint8_t>& data)
{
    if (!data.empty())
    {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

bool is_reverse_flag(std::string_view arg)
{
    return arg == "-w" || arg == "--word-to-binary";
}

bool is_compression_flag(std::string_view arg)
{
    return arg == "-c" || arg == "--compression";
}

}  // namespace

int main(int argc, char* argv[])
{
    bool reverse = false;
    bool compress = false;
    bool random_mode = false;
    std::uint64_t random_count = 0;
    std::string_view random_fmt = "space";
    std::vector<std::string> positionals;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (is_reverse_flag(arg))
        {
            reverse = true;
            continue;
        }
        if (is_compression_flag(arg))
        {
            compress = true;
            continue;
        }
        if (arg == "-r" || arg == "--random")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: " << arg << " requires N\n";
                usage(argv[0]);
                return 1;
            }
            ++i;
            if (!parse_u64(argv[i], random_count) || random_count == 0)
            {
                std::cerr << "error: invalid random count: " << argv[i] << '\n';
                return 1;
            }
            random_mode = true;
            if (i + 1 < argc)
            {
                const std::string_view maybe = argv[i + 1];
                if (maybe == "space" || maybe == "nospace" || maybe == "newline")
                {
                    random_fmt = maybe;
                    ++i;
                }
            }
            continue;
        }
        if (arg == "-")
        {
            positionals.emplace_back("-");
            continue;
        }
        if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "error: unknown option: " << arg << '\n';
            usage(argv[0]);
            return 1;
        }
        positionals.emplace_back(arg);
    }

    if (random_mode)
    {
        if (reverse || compress || !positionals.empty())
        {
            std::cerr << "error: -r/--random cannot be combined with other modes\n";
            return 1;
        }
        if (!write_random_words(std::cout, random_count, random_fmt))
        {
            std::cerr << "error: invalid random format (use space|nospace|newline)\n";
            return 1;
        }
        return 0;
    }

    if (positionals.size() > 2)
    {
        usage(argv[0]);
        return 1;
    }

    // No input path, or explicit "-": read from stdin.
    const std::string input_path =
        positionals.empty() ? std::string("-") : positionals[0];
    const char* output_path =
        (positionals.size() == 2) ? positionals[1].c_str() : nullptr;

    if (!reverse)
    {
        std::vector<std::uint8_t> payload;
        if (!read_file(input_path, payload))
        {
            std::cerr << "error: cannot read input file: " << input_path
                      << '\n';
            return 1;
        }

        if (compress)
        {
            std::vector<std::uint8_t> compressed;
            std::string err;
            if (!wordlist::compress_zstd(payload, compressed, err))
            {
                std::cerr << "error: " << err << '\n';
                return 1;
            }
            payload.swap(compressed);
        }

        const auto words = wordlist::binary_to_words(payload);

        if (output_path)
        {
            std::ofstream out(output_path);
            if (!out || !write_words(out, words))
            {
                std::cerr << "error: cannot write output file: " << output_path
                          << '\n';
                return 1;
            }
        }
        else if (!write_words(std::cout, words))
        {
            std::cerr << "error: failed writing to stdout\n";
            return 1;
        }
        return 0;
    }

    std::string text;
    if (!read_text_file(input_path, text))
    {
        std::cerr << "error: cannot read input file: " << input_path << '\n';
        return 1;
    }

    const auto words = wordlist::split_words(text);
    std::vector<std::uint8_t> payload;
    std::string err;
    if (!wordlist::words_to_binary(words, payload, err))
    {
        std::cerr << "error: " << err << '\n';
        return 1;
    }

    if (compress)
    {
        std::vector<std::uint8_t> decompressed;
        if (!wordlist::decompress_zstd(payload, decompressed, err))
        {
            std::cerr << "error: " << err << '\n';
            return 1;
        }
        payload.swap(decompressed);
    }

    if (output_path)
    {
        std::ofstream out(output_path, std::ios::binary);
        if (!out || !write_binary(out, payload))
        {
            std::cerr << "error: cannot write output file: " << output_path
                      << '\n';
            return 1;
        }
    }
    else if (!write_binary(std::cout, payload))
    {
        std::cerr << "error: failed writing to stdout\n";
        return 1;
    }

    return 0;
}
