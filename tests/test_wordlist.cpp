#include "convert.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

namespace fs = std::filesystem;

namespace
{

int g_failed = 0;
int g_passed = 0;

void check(bool cond, const char* name)
{
    if (cond)
    {
        ++g_passed;
        std::cout << "  PASS  " << name << '\n';
    }
    else
    {
        ++g_failed;
        std::cout << "  FAIL  " << name << '\n';
    }
}

std::vector<std::uint8_t> bytes_from(std::string_view s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string join_words(const std::vector<std::string_view>& words)
{
    std::ostringstream ss;
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        if (i > 0)
        {
            ss << ' ';
        }
        ss << words[i];
    }
    return ss.str();
}

bool vectors_equal(const std::vector<std::uint8_t>& a,
                   const std::vector<std::uint8_t>& b)
{
    return a == b;
}

void write_binary_file(const fs::path& path,
                       const std::vector<std::uint8_t>& data)
{
    std::ofstream out(path, std::ios::binary);
    if (!data.empty())
    {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
}

std::vector<std::uint8_t> read_binary_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> data;
    char buf[8192];
    while (in)
    {
        in.read(buf, sizeof(buf));
        const auto n = in.gcount();
        if (n > 0)
        {
            data.insert(data.end(), buf, buf + n);
        }
    }
    return data;
}

std::string read_text_file(const fs::path& path)
{
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Run: bin [args...] < stdin_path (optional). Capture stdout to out_path.
// Returns exit code, or -1 on spawn failure.
int run_tool(const fs::path& bin,
             const std::vector<std::string>& args,
             const fs::path& stdin_path,
             const fs::path& stdout_path)
{
    std::ostringstream cmd;
    cmd << bin.string();
    for (const auto& a : args)
    {
        cmd << " '" << a << "'";
    }
    if (!stdin_path.empty())
    {
        cmd << " < '" << stdin_path.string() << "'";
    }
    if (!stdout_path.empty())
    {
        cmd << " > '" << stdout_path.string() << "'";
    }
    cmd << " 2>/dev/null";
    const int status = std::system(cmd.str().c_str());
    if (status < 0)
    {
        return -1;
    }
#if defined(WIFEXITED) && defined(WEXITSTATUS)
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return -1;
#else
    return status;
#endif
}

void test_unit_roundtrip_hello()
{
    std::cout << "unit: round-trip hello\n";
    const auto payload = bytes_from("hello");
    const auto words = wordlist::binary_to_words(payload);
    check(words.size() == 6, "hello encodes to 6 words");
    check(join_words(words) ==
              "abacus abacus broiling deepen stimuli unfasten",
          "hello word phrase matches");

    std::vector<std::string> word_strs(words.begin(), words.end());
    std::vector<std::uint8_t> decoded;
    std::string err;
    const bool ok = wordlist::words_to_binary(word_strs, decoded, err);
    check(ok, "words_to_binary succeeds");
    check(vectors_equal(payload, decoded), "decoded equals original");
}

void test_unit_empty()
{
    std::cout << "unit: empty payload\n";
    const std::vector<std::uint8_t> payload;
    const auto words = wordlist::binary_to_words(payload);
    check(!words.empty(), "empty payload produces words");

    std::vector<std::string> word_strs(words.begin(), words.end());
    std::vector<std::uint8_t> decoded;
    std::string err;
    const bool ok = wordlist::words_to_binary(word_strs, decoded, err);
    check(ok, "empty round-trip succeeds");
    check(decoded.empty(), "decoded empty payload is empty");
}

void test_unit_binary_bytes()
{
    std::cout << "unit: binary bytes 00 01 02 ff\n";
    const std::vector<std::uint8_t> payload = {0x00, 0x01, 0x02, 0xFF};
    const auto words = wordlist::binary_to_words(payload);
    std::vector<std::string> word_strs(words.begin(), words.end());
    std::vector<std::uint8_t> decoded;
    std::string err;
    const bool ok = wordlist::words_to_binary(word_strs, decoded, err);
    check(ok, "binary bytes round-trip succeeds");
    check(vectors_equal(payload, decoded), "binary bytes match");
}

void test_unit_unknown_word()
{
    std::cout << "unit: unknown word rejected\n";
    const std::vector<std::string> words = {"abacus", "notaword"};
    std::vector<std::uint8_t> decoded;
    std::string err;
    const bool ok = wordlist::words_to_binary(words, decoded, err);
    check(!ok, "unknown word fails");
    check(err.find("unknown") != std::string::npos, "error mentions unknown");
}

void test_unit_compression_roundtrip()
{
    std::cout << "unit: zstd compression round-trip\n";
    std::vector<std::uint8_t> payload(4096, 'A');
    const std::string tail = "hello world";
    for (int i = 0; i < 200; ++i)
    {
        payload.insert(payload.end(), tail.begin(), tail.end());
    }

    std::string err;
    std::vector<std::uint8_t> compressed;
    check(wordlist::compress_zstd(payload, compressed, err), "compress ok");
    check(compressed.size() < payload.size(), "compressed smaller");

    const auto words = wordlist::binary_to_words(compressed);
    std::vector<std::string> word_strs(words.begin(), words.end());
    std::vector<std::uint8_t> encoded_payload;
    check(wordlist::words_to_binary(word_strs, encoded_payload, err),
          "decode words ok");
    check(vectors_equal(compressed, encoded_payload),
          "word payload is compressed data");

    std::vector<std::uint8_t> decompressed;
    check(wordlist::decompress_zstd(encoded_payload, decompressed, err),
          "decompress ok");
    check(vectors_equal(payload, decompressed),
          "decompressed equals original");
    check(words.size() == 23, "compressible sample encodes to 23 words");
}

void test_unit_split_words()
{
    std::cout << "unit: split_words\n";
    const auto words =
        wordlist::split_words("  abacus   zoo\nability  \t");
    check(words.size() == 3, "split yields 3 words");
    check(words[0] == "abacus" && words[1] == "zoo" &&
              words[2] == "ability",
          "split tokens correct");
}

void test_cli(const fs::path& bin, const fs::path& tmp)
{
    std::cout << "cli: file round-trip\n";
    const auto in_bin = tmp / "hello.bin";
    const auto words_path = tmp / "hello.words";
    const auto out_bin = tmp / "hello.out";
    const auto out_bin2 = tmp / "hello.out2";

    write_binary_file(in_bin, bytes_from("hello"));

    check(run_tool(bin, {in_bin.string(), words_path.string()}, {}, {}) == 0,
          "encode to file");
    check(run_tool(bin, {"-w", words_path.string(), out_bin.string()}, {},
                   {}) == 0,
          "decode -w to file");
    check(run_tool(bin,
                   {"--word-to-binary", words_path.string(), out_bin2.string()},
                   {}, {}) == 0,
          "decode --word-to-binary to file");
    check(vectors_equal(read_binary_file(in_bin), read_binary_file(out_bin)),
          "round-trip -w matches");
    check(vectors_equal(read_binary_file(in_bin), read_binary_file(out_bin2)),
          "round-trip --word-to-binary matches");
    check(read_text_file(words_path).find(
              "abacus abacus broiling deepen stimuli unfasten") !=
              std::string::npos,
          "cli phrase matches unit");

    std::cout << "cli: compression round-trip\n";
    std::vector<std::uint8_t> big(4096, 'A');
    const std::string tail = "hello world";
    for (int i = 0; i < 200; ++i)
    {
        big.insert(big.end(), tail.begin(), tail.end());
    }
    const auto big_in = tmp / "big.bin";
    const auto big_words = tmp / "big.words";
    const auto big_out = tmp / "big.out";
    const auto big_words2 = tmp / "big2.words";
    const auto big_out2 = tmp / "big2.out";
    write_binary_file(big_in, big);

    check(run_tool(bin, {"-c", big_in.string(), big_words.string()}, {},
                   {}) == 0,
          "encode -c");
    check(run_tool(bin, {"-w", "-c", big_words.string(), big_out.string()}, {},
                   {}) == 0,
          "decode -w -c");
    check(vectors_equal(read_binary_file(big_in), read_binary_file(big_out)),
          "compressed round-trip matches");

    check(run_tool(bin,
                   {"--compression", big_in.string(), big_words2.string()}, {},
                   {}) == 0,
          "encode --compression");
    check(run_tool(bin,
                   {"--word-to-binary", "--compression", big_words2.string(),
                    big_out2.string()},
                   {}, {}) == 0,
          "decode --word-to-binary --compression");
    check(vectors_equal(read_binary_file(big_in), read_binary_file(big_out2)),
          "long-flag compressed round-trip matches");

    std::cout << "cli: stdin pipes\n";
    const auto pipe_words = tmp / "pipe.words";
    const auto pipe_words2 = tmp / "pipe2.words";
    const auto pipe_bin = tmp / "pipe.bin";

    check(run_tool(bin, {"-"}, in_bin, pipe_words) == 0,
          "stdin via explicit -");
    check(read_text_file(pipe_words) == read_text_file(words_path),
          "pipe - matches file encode");

    check(run_tool(bin, {}, in_bin, pipe_words2) == 0,
          "stdin with no args");
    check(read_text_file(pipe_words2) == read_text_file(words_path),
          "no-args pipe matches file encode");

    check(run_tool(bin, {"-w", "-"}, words_path, pipe_bin) == 0,
          "words stdin via -w -");
    check(vectors_equal(read_binary_file(in_bin), read_binary_file(pipe_bin)),
          "pipe decode matches original");
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <path-to-wordlist>\n";
        return 2;
    }

    const fs::path bin = argv[1];
    if (!fs::exists(bin))
    {
        std::cerr << "error: binary not found: " << bin << '\n';
        return 2;
    }

    const fs::path tmp =
        fs::temp_directory_path() / "wordlist_tests";
    fs::create_directories(tmp);

    std::cout << "wordlist tests\n";
    test_unit_roundtrip_hello();
    test_unit_empty();
    test_unit_binary_bytes();
    test_unit_unknown_word();
    test_unit_compression_roundtrip();
    test_unit_split_words();
    test_cli(bin, tmp);

    std::cout << '\n'
              << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
