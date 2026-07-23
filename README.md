# wordlist

Convert any binary file to an EFF large word list — and back.

## Build

```bash
make
```

Requires a C++17 compiler. Compression uses **vendored [zstd](https://github.com/facebook/zstd)** (`third_party/zstd`); no system libraries needed.

Regenerate the embedded list after editing `wordlist.txt`:

```bash
./generate_wordlist.sh
```

## Usage

```bash
wordlist [options] [input-file|-] [output-file]
```

| Direction | Command |
|-----------|---------|
| Binary → words | `wordlist file.bin [out.words]` |
| Words → binary | `wordlist -w file.words [out.bin]` |
| With compression | `wordlist -c file.bin` / `wordlist -w -c file.words` |
| Random words | `wordlist -r 6` / `wordlist -r 6 nospace` |
| From stdin | `cat file.bin \| wordlist` or `wordlist -` |

### Options

| Flag | Description |
|------|-------------|
| `-w`, `--word-to-binary` | Decode words to binary (default: encode) |
| `-c`, `--compression` | Lossless zstd compress before encode / decompress after decode |
| `-r`, `--random N [fmt]` | Print N random words (`space`, `nospace`, or `newline`) |

Flags are non-positional. Omit `input-file` (or use `-`) to read stdin; omit `output-file` to write stdout.

## Examples

```bash
# Encode
./wordlist secret.bin phrase.txt

# Decode
./wordlist -w phrase.txt secret.bin

# Compress + encode, then restore
./wordlist -c data.bin words.txt
./wordlist -w -c words.txt data.bin

# Pipes
printf 'hello' | ./wordlist
cat words.txt | ./wordlist -w > out.bin

# Passphrase-style random words
./wordlist -r 6
```

## Encoding

1. Optional zstd compression of the payload  
2. 4-byte big-endian length prefix + payload  
3. Split into 13-bit groups → EFF large words (list padded to 8192)

Reverse path undoes the same steps.

## Test

Native C++ tests live in `tests/`:

```bash
make test
```
