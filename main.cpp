#include "wordlist.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// =============================================================================
// Encoding
//
// Arbitrary binary is consumed thirty bits at a time. Those thirty bits become
// exactly three phrases. The mapping is not "one phrase = ten bits"; it is:
//
//   3 bits  -> which list (Person vs Object) each of the three phrases uses
//   9 bits  -> first  phrase's row in TripleDecimalDigitPeopleObjects
//   9 bits  -> second phrase's row
//   9 bits  -> third  phrase's row
//
// The 9-bit row is stored as three 3-bit digits 0-7, then written as a
// three-digit *decimal* number whose digits happen to all be 0-7:
//
//   bits 010 110 001  ->  digits 2, 6, 1  ->  index 261
//                        (not the 9-bit integer 0b010110001 = 177)
//
// That is why first/second/third look like 261, 735, 602 rather than values
// in 0-511.
//
// Worked example from the spec:
//
//   raw 30 bits: 110 010 110 001 111 011 101 110 000 010
//                |   |         |         |
//                |   first=261 second=735 third=602
//                digit=6
//
//   digit 6 is bits 1,1,0 -> Object, Object, Person
//     TripleDecimalDigitPeopleObjects[261].object = "Enxada"
//     TripleDecimalDigitPeopleObjects[735].object = "Cama elástica (pula pula)"
//     TripleDecimalDigitPeopleObjects[602].person = "Jason (Sexta-Feira 13)"
//
// Decoding walks the other way: each phrase is looked up in the person map
// and the object map (std::map cannot search by value; we use unordered_map).
// The three Person/Object hits rebuild `digit`; the three row numbers rebuild
// first/second/third; those four fields are expanded back into thirty bits.
//
// Phrases contain spaces and commas, so a word file is one phrase per line,
// not space-separated tokens.
// =============================================================================

// Four decimal fields that together describe one 30-bit locus.
//
// Thirty bits are ten groups of three bits. Each 3-bit group is a digit 0-7:
//
//   bits  0- 2  -> digit   (Person/Object ordering of the three words)
//   bits  3-11  -> first   (three digits 0-7, written as a 000-777 decimal)
//   bits 12-20  -> second
//   bits 21-29  -> third
//
// Loci ordering for `digit`:
//   0 Person-Person-Person
//   1 Person-Person-Object
//   2 Person-Object-Person
//   3 Person-Object-Object
//   4 Object-Person-Person
//   5 Object-Person-Object
//   6 Object-Object-Person
//   7 Object-Object-Object
struct DecimalSequence
{
    unsigned int digit;   // 1-digit, 0-7; high bit is phrase 0, low bit phrase 2
    unsigned int first;   // 3-digit, 000-777 (each decimal digit is 0-7)
    unsigned int second;  // 3-digit, 000-777
    unsigned int third;   // 3-digit, 000-777
};

namespace
{

// Each locus is thirty bits: 3 selector bits + 3 * 9 payload bits.
constexpr std::size_t kBitsPerLocus = 30;
// Every stored number is a 3-bit digit in 0-7.
constexpr std::size_t kBitsPerTriple = 3;

// Print CLI help to stderr. argv0 is the program name from argv[0].
void usage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0 << " [options] [input [output]]\n"
        << "  Convert a binary file to a one-phrase-per-line word list, or the reverse.\n"
        << "\n"
        << "  -w, --word-to-binary   Read phrases and write binary\n"
        << "  -h, --help             Show this help\n"
        << "\n"
        << "  Omit a path, or pass '-', to use stdin/stdout.\n"
        << "  Word files are UTF-8, one phrase per line (phrases may contain spaces).\n"
        << "  Every thirty bits become three phrases; leftover bits are padded with 0.\n"
        << "  THIS IS A TEST DOGFART";
}

// Read a single bit from `data` at bitstream index `bit_index`.
//
// Byte 0's most-significant bit is bit 0 of the stream, so the file is read
// left-to-right the way a human writes a bit string. Bits past the last byte
// are defined as 0: that is how a short final locus is padded to 30 bits
// without allocating a second buffer.
int get_bit(const std::vector<std::uint8_t>& data, std::size_t bit_index)
{
    // Which byte holds this bit.
    const std::size_t byte_index = bit_index / 8;
    // Past EOF: treat as a padded zero bit.
    if (byte_index >= data.size())
    {
        return 0;
    }
    // Position inside the byte: 7 for the first bit of the byte, 0 for the last.
    const int shift = 7 - static_cast<int>(bit_index % 8);
    return (data[byte_index] >> shift) & 1;
}

// Write `bit` (0 or 1) at bitstream index `bit_index`, growing `data` with
// zero bytes if the index sits past the current end.
void set_bit(std::vector<std::uint8_t>& data, std::size_t bit_index, int bit)
{
    const std::size_t byte_index = bit_index / 8;
    // First write into a later byte: fill the gap with 0x00.
    if (byte_index >= data.size())
    {
        data.resize(byte_index + 1, 0);
    }
    const int shift = 7 - static_cast<int>(bit_index % 8);
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << shift);
    if (bit)
    {
        // Set the targeted bit, leave the other seven alone.
        data[byte_index] = static_cast<std::uint8_t>(data[byte_index] | mask);
    }
    else
    {
        // Clear the targeted bit, leave the other seven alone.
        data[byte_index] = static_cast<std::uint8_t>(data[byte_index] & static_cast<std::uint8_t>(~mask));
    }
}

// Consume the next three bits as an integer 0-7.
// Bit order is big-endian inside the triple: 110 -> 6, 010 -> 2.
unsigned int triple_digit(const std::vector<std::uint8_t>& data,
                          std::size_t bit_index)
{
    unsigned int value = 0;
    for (std::size_t i = 0; i < kBitsPerTriple; ++i)
    {
        // Shift the partial result left, then OR in the newly read bit.
        value = (value << 1) | static_cast<unsigned int>(get_bit(data, bit_index + i));
    }
    return value;
}

// Write `value` (must already be 0-7) as three bits starting at `bit_index`.
// The high bit of the 3-bit value is written first, matching triple_digit.
void write_triple_digit(std::vector<std::uint8_t>& data,
                        std::size_t bit_index,
                        unsigned int value)
{
    for (std::size_t i = 0; i < kBitsPerTriple; ++i)
    {
        // i=0 takes bit 2 (value 4), i=1 takes bit 1, i=2 takes bit 0.
        const int bit = static_cast<int>((value >> (2 - i)) & 1u);
        set_bit(data, bit_index + i, bit);
    }
}

// Read nine bits as three decimal digits 0-7 and compose n = 100*h + 10*t + o.
//
// This is the 9-bit half of a word: it is *not* "read nine bits as an integer
// 0-511". The spec's example 010 110 001 must become 261, not 177.
unsigned int three_digit(const std::vector<std::uint8_t>& data,
                         std::size_t bit_index)
{
    // Hundreds place: first three bits of this 9-bit group.
    const unsigned int hundreds = triple_digit(data, bit_index);
    // Tens place: next three bits.
    const unsigned int tens = triple_digit(data, bit_index + kBitsPerTriple);
    // Ones place: last three bits.
    const unsigned int ones = triple_digit(data, bit_index + 2 * kBitsPerTriple);
    return hundreds * 100u + tens * 10u + ones;
}

// Inverse of three_digit: split a 000-777 decimal into three 0-7 digits and
// write them as nine bits. Reject any n whose decimal digits include 8 or 9,
// because those cannot fit in a 3-bit triple (the table still has rows 008,
// 009, 018, ... for completeness, but encoding never produces them).
bool write_three_digit(std::vector<std::uint8_t>& data,
                       std::size_t bit_index,
                       unsigned int n,
                       std::string& err)
{
    const unsigned int hundreds = n / 100u;
    const unsigned int tens = (n / 10u) % 10u;
    const unsigned int ones = n % 10u;
    if (hundreds > 7u || tens > 7u || ones > 7u || n > 999u)
    {
        err = "index has a decimal digit outside 0-7: " + std::to_string(n);
        return false;
    }
    write_triple_digit(data, bit_index, hundreds);
    write_triple_digit(data, bit_index + kBitsPerTriple, tens);
    write_triple_digit(data, bit_index + 2 * kBitsPerTriple, ones);
    return true;
}

// Pull one 30-bit locus out of the bitstream starting at `bit_index`.
// get_bit returns 0 past EOF, so a trailing partial locus is zero-padded.
DecimalSequence locus_at(const std::vector<std::uint8_t>& data,
                         std::size_t bit_index)
{
    DecimalSequence seq{};
    // Triple 1 (bits 0-2 of the locus): Person/Object selector 0-7.
    seq.digit = triple_digit(data, bit_index);
    // Triples 2-4 (bits 3-11): first word's 000-777 index.
    seq.first = three_digit(data, bit_index + 1 * kBitsPerTriple);
    // Triples 5-7 (bits 12-20): second word's index.
    seq.second = three_digit(data, bit_index + 4 * kBitsPerTriple);
    // Triples 8-10 (bits 21-29): third word's index.
    seq.third = three_digit(data, bit_index + 7 * kBitsPerTriple);
    return seq;
}

// Write one DecimalSequence back as thirty bits at `bit_index`.
// Order matches locus_at so encode-then-decode is a lossless bit identity
// for any locus whose first/second/third digits are all 0-7.
bool write_locus(std::vector<std::uint8_t>& data,
                 std::size_t bit_index,
                 const DecimalSequence& seq,
                 std::string& err)
{
    if (seq.digit > 7u)
    {
        err = "ordering digit must be 0-7";
        return false;
    }
    write_triple_digit(data, bit_index, seq.digit);
    if (!write_three_digit(data, bit_index + 1 * kBitsPerTriple, seq.first, err))
    {
        return false;
    }
    if (!write_three_digit(data, bit_index + 4 * kBitsPerTriple, seq.second, err))
    {
        return false;
    }
    return write_three_digit(data, bit_index + 7 * kBitsPerTriple, seq.third, err);
}

// Slice the whole file into 30-bit loci. An empty file yields no loci.
// A file whose bit length is not a multiple of 30 gets one extra locus whose
// missing tail bits are 0.
std::vector<DecimalSequence>
binary_to_sequences(const std::vector<std::uint8_t>& payload)
{
    const std::size_t nbits = payload.size() * 8;
    if (nbits == 0)
    {
        return {};
    }

    // Ceiling division: 1..30 bits -> 1 locus, 31..60 bits -> 2 loci, ...
    const std::size_t loci = (nbits + kBitsPerLocus - 1) / kBitsPerLocus;
    std::vector<DecimalSequence> sequences;
    sequences.reserve(loci);
    for (std::size_t i = 0; i < loci; ++i)
    {
        sequences.push_back(locus_at(payload, i * kBitsPerLocus));
    }
    return sequences;
}

// Inverse of binary_to_sequences: concatenate loci into a byte vector.
// 30 * N bits is a multiple of 8 only when N is a multiple of 4; otherwise
// the last byte is padded on the right with zeros (set_bit grows with 0x00).
bool sequences_to_binary(const std::vector<DecimalSequence>& sequences,
                         std::vector<std::uint8_t>& out,
                         std::string& err)
{
    out.clear();
    for (std::size_t i = 0; i < sequences.size(); ++i)
    {
        if (!write_locus(out, i * kBitsPerLocus, sequences[i], err))
        {
            return false;
        }
    }
    return true;
}

// True when this peg (0, 1, or 2) should take the Object string.
//
// `digit` is itself a 3-bit number. Its high bit is peg 0 (first phrase),
// middle bit peg 1, low bit peg 2. 0 means Person, 1 means Object:
//
//   digit 6 = 110b -> Object, Object, Person
bool peg_is_object(unsigned int digit, unsigned int peg)
{
    return ((digit >> (2u - peg)) & 1u) != 0;
}

// Look up first/second/third in TripleDecimalDigitPeopleObjects and store the
// three *rows* on the locus. Each row still holds both the person string and
// the object string; which field is later printed is decided by `seq.digit`
// in loci_to_phrases, not here.
bool sequence_to_loci(const DecimalSequence& seq,
                      ThirtyBinaryDigitLoci& loci,
                      std::string& err)
{
    const unsigned int indices[3] = {seq.first, seq.second, seq.third};
    const auto& table = TripleDecimalDigitPeopleObjects;
    for (unsigned int peg = 0; peg < 3; ++peg)
    {
        const unsigned int n = indices[peg];
        // The table has 1000 rows (000-999). Encoding only produces 000-777,
        // but a hand-written word file could theoretically name a later row.
        if (n >= table.size())
        {
            err = "index out of range: " + std::to_string(n);
            return false;
        }
        loci.loci_list[peg] = table[n];
    }
    return true;
}

// Pick person or object from each of the three stored rows according to digit.
// After this, phrases[0..2] are the three lines that will be written out.
void loci_to_phrases(const ThirtyBinaryDigitLoci& loci,
                     unsigned int digit,
                     std::string phrases[3])
{
    for (unsigned int peg = 0; peg < 3; ++peg)
    {
        const PersonObject& row = loci.loci_list[peg];
        phrases[peg] = peg_is_object(digit, peg) ? row.object : row.person;
    }
}

// Phrase -> (index n, is_object).
//
// std::map<int, PersonObject> can go n -> pair, but it cannot go phrase -> n.
// A vector has the same limitation: you would scan 1000 rows per phrase.
// Two unordered_maps (hash tables) give O(1) reverse lookup on each list.
// We keep person and object in separate maps so a hit also tells us which
// list the phrase came from, which is how we recover the 3 selector bits.
struct ReverseIndex
{
    std::unordered_map<std::string, unsigned int> person_to_n;
    std::unordered_map<std::string, unsigned int> object_to_n;
};

// Build both reverse maps from the 1000-row table. Called once per decode.
ReverseIndex build_reverse_index()
{
    ReverseIndex idx;
    const auto& table = TripleDecimalDigitPeopleObjects;
    idx.person_to_n.reserve(table.size());
    idx.object_to_n.reserve(table.size());
    for (unsigned int n = 0; n < table.size(); ++n)
    {
        idx.person_to_n.emplace(table[n].person, n);
        idx.object_to_n.emplace(table[n].object, n);
    }
    return idx;
}

// Classify one phrase as Person or Object and recover its three-digit index.
// Exact string match against the CSV text, including parenthetical notes.
bool lookup_phrase(const ReverseIndex& idx,
                   const std::string& phrase,
                   unsigned int& n,
                   bool& is_object,
                   std::string& err)
{
    const auto person_it = idx.person_to_n.find(phrase);
    const auto object_it = idx.object_to_n.find(phrase);
    const bool as_person = person_it != idx.person_to_n.end();
    const bool as_object = object_it != idx.object_to_n.end();
    // The generated table has unique persons, unique objects, and no overlap
    // between the two lists. Guard anyway so a later table edit cannot silently
    // pick the wrong 3-bit selector.
    if (as_person && as_object)
    {
        err = "ambiguous phrase (person and object): " + phrase;
        return false;
    }
    if (as_person)
    {
        n = person_it->second;
        is_object = false;
        return true;
    }
    if (as_object)
    {
        n = object_it->second;
        is_object = true;
        return true;
    }
    err = "unknown phrase: " + phrase;
    return false;
}

// Three phrases -> DecimalSequence.
// Each phrase contributes one 9-bit index and one Person/Object bit; the three
// Person/Object bits are packed back into `digit` in peg order.
bool phrases_to_sequence(const ReverseIndex& idx,
                         const std::string phrases[3],
                         DecimalSequence& seq,
                         std::string& err)
{
    unsigned int ns[3] = {0, 0, 0};
    unsigned int digit = 0;
    for (unsigned int peg = 0; peg < 3; ++peg)
    {
        bool is_object = false;
        if (!lookup_phrase(idx, phrases[peg], ns[peg], is_object, err))
        {
            return false;
        }
        if (is_object)
        {
            // Set the same bit peg_is_object would read for this peg.
            digit |= (1u << (2u - peg));
        }
    }
    seq.digit = digit;
    seq.first = ns[0];
    seq.second = ns[1];
    seq.third = ns[2];
    return true;
}

// Empty path or "-" means stdin/stdout rather than a named file.
bool is_stdio_path(const std::string& path)
{
    return path.empty() || path == "-";
}

// Read an istream to EOF as raw bytes. Used for both files and stdin.
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
    // Fail only on a hard stream error, not on the expected EOF that ends the loop.
    return in.eof() || static_cast<bool>(in);
}

// Write raw bytes to an ostream. An empty payload writes nothing.
bool write_stream_binary(std::ostream& out, const std::vector<std::uint8_t>& data)
{
    if (!data.empty())
    {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

// Open `path` as binary, or use stdin when the path is "-" / empty.
bool read_file_binary(const std::string& path, std::vector<std::uint8_t>& out)
{
    if (is_stdio_path(path))
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

// Open `path` as binary, or use stdout when the path is "-" / empty.
bool write_file_binary(const std::string& path, const std::vector<std::uint8_t>& data)
{
    if (is_stdio_path(path))
    {
        return write_stream_binary(std::cout, data);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        return false;
    }
    return write_stream_binary(out, data);
}

// Read a word file as one phrase per line.
// Blank lines are skipped so a blank line between loci is allowed.
// Trailing '\r' is stripped so Windows CRLF files match the Unix table strings.
bool read_phrases(const std::string& path, std::vector<std::string>& phrases)
{
    phrases.clear();
    std::ifstream file;
    std::istream* in = &std::cin;
    if (!is_stdio_path(path))
    {
        file.open(path);
        if (!file)
        {
            return false;
        }
        in = &file;
    }

    std::string line;
    while (std::getline(*in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }
        phrases.push_back(line);
    }
    return true;
}

// Write one phrase per line. No extra blank lines between loci.
bool write_phrases(const std::string& path, const std::vector<std::string>& phrases)
{
    std::ofstream file;
    std::ostream* out = &std::cout;
    if (!is_stdio_path(path))
    {
        file.open(path);
        if (!file)
        {
            return false;
        }
        out = &file;
    }
    for (const std::string& phrase : phrases)
    {
        *out << phrase << '\n';
    }
    return static_cast<bool>(*out);
}

// Binary -> phrases: 30 bits -> DecimalSequence -> ThirtyBinaryDigitLoci ->
// three phrases. The 3 selector bits never appear as their own word; they
// are implied by whether each printed phrase is a person or an object.
bool encode_binary_to_words(const std::vector<std::uint8_t>& payload,
                            std::vector<std::string>& phrases,
                            std::string& err)
{
    phrases.clear();
    const std::vector<DecimalSequence> sequences = binary_to_sequences(payload);
    phrases.reserve(sequences.size() * 3);
    for (const DecimalSequence& seq : sequences)
    {
        ThirtyBinaryDigitLoci loci{};
        if (!sequence_to_loci(seq, loci, err))
        {
            return false;
        }
        std::string three[3];
        loci_to_phrases(loci, seq.digit, three);
        phrases.push_back(three[0]);
        phrases.push_back(three[1]);
        phrases.push_back(three[2]);
    }
    return true;
}

// Phrases -> binary: groups of three lines -> DecimalSequence -> 30 bits.
// Phrase count must be a multiple of three; leftover lines would not fill a
// locus and would mean the file was truncated or hand-edited badly.
bool decode_words_to_binary(const std::vector<std::string>& phrases,
                            std::vector<std::uint8_t>& payload,
                            std::string& err)
{
    if (phrases.size() % 3 != 0)
    {
        err = "word file length is not a multiple of 3";
        return false;
    }

    const ReverseIndex idx = build_reverse_index();
    std::vector<DecimalSequence> sequences;
    sequences.reserve(phrases.size() / 3);
    for (std::size_t i = 0; i < phrases.size(); i += 3)
    {
        const std::string three[3] = {phrases[i], phrases[i + 1], phrases[i + 2]};
        DecimalSequence seq{};
        if (!phrases_to_sequence(idx, three, seq, err))
        {
            return false;
        }
        sequences.push_back(seq);
    }
    return sequences_to_binary(sequences, payload, err);
}

}  // namespace

int main(int argc, char* argv[])
{
    // Default mode is binary -> words. -w flips it.
    bool word_to_binary = false;
    // Default both paths to stdin/stdout so `wordlist < in > out` works.
    std::string input_path = "-";
    std::string output_path = "-";
    bool saw_input = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            usage(argv[0]);
            return 0;
        }
        if (arg == "-w" || arg == "--word-to-binary")
        {
            word_to_binary = true;
            continue;
        }
        // A lone "-" is a path meaning stdin/stdout, not an unknown flag.
        if (!arg.empty() && arg[0] == '-' && arg != "-")
        {
            std::cerr << "error: unknown option: " << arg << '\n';
            usage(argv[0]);
            return 1;
        }
        // First non-option: input path. Second: output path. Third: error.
        if (!saw_input)
        {
            input_path = std::string(arg);
            saw_input = true;
            continue;
        }
        if (!saw_output)
        {
            output_path = std::string(arg);
            saw_output = true;
            continue;
        }
        std::cerr << "error: extra argument: " << arg << '\n';
        usage(argv[0]);
        return 1;
    }

    std::string err;
    if (!word_to_binary)
    {
        // Binary file -> three phrases per 30-bit locus.
        std::vector<std::uint8_t> payload;
        if (!read_file_binary(input_path, payload))
        {
            std::cerr << "error: cannot read input file: " << input_path << '\n';
            return 1;
        }
        std::vector<std::string> phrases;
        if (!encode_binary_to_words(payload, phrases, err))
        {
            std::cerr << "error: " << err << '\n';
            return 1;
        }
        if (!write_phrases(output_path, phrases))
        {
            std::cerr << "error: cannot write output file: " << output_path << '\n';
            return 1;
        }
        return 0;
    }

    // Word file -> binary. Each triple of phrases rebuilds 30 bits, including
    // the 3 selector bits implied by Person vs Object on each phrase.
    std::vector<std::string> phrases;
    if (!read_phrases(input_path, phrases))
    {
        std::cerr << "error: cannot read input file: " << input_path << '\n';
        return 1;
    }
    std::vector<std::uint8_t> payload;
    if (!decode_words_to_binary(phrases, payload, err))
    {
        std::cerr << "error: " << err << '\n';
        return 1;
    }
    if (!write_file_binary(output_path, payload))
    {
        std::cerr << "error: cannot write output file: " << output_path << '\n';
        return 1;
    }
    return 0;
}
