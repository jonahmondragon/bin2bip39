# bin2bip39

Convert any binary file to a BIP39 English word list — and back.

## Build

```bash
make
```

Requires a C++17 compiler. Compression uses **vendored [zstd](https://github.com/facebook/zstd)** (`third_party/zstd`); no system libraries needed.

## Usage

```bash
bin2bip39 [options] [input-file|-] [output-file]
```

| Direction | Command |
|-----------|---------|
| Binary → words | `bin2bip39 file.bin [out.words]` |
| Words → binary | `bin2bip39 -w file.words [out.bin]` |
| With compression | `bin2bip39 -c file.bin` / `bin2bip39 -w -c file.words` |
| From stdin | `cat file.bin \| bin2bip39` or `bin2bip39 -` |

### Options

| Flag | Description |
|------|-------------|
| `-w`, `--word-to-binary` | Decode words to binary (default: encode) |
| `-c`, `--compression` | Lossless zstd compress before encode / decompress after decode |

Flags are non-positional. Omit `input-file` (or use `-`) to read stdin; omit `output-file` to write stdout.

## Examples

```bash
# Encode
./bin2bip39 secret.bin phrase.txt

# Decode
./bin2bip39 -w phrase.txt secret.bin

# Compress + encode, then restore
./bin2bip39 -c data.bin words.txt
./bin2bip39 -w -c words.txt data.bin

# Pipes
printf 'hello' | ./bin2bip39
cat words.txt | ./bin2bip39 -w > out.bin
```

## Encoding

1. Optional zstd compression of the payload  
2. 4-byte big-endian length prefix + payload  
3. Split into 11-bit groups → BIP39 English words (zero-padded)

Reverse path undoes the same steps.

## Test

Native C++ tests live in `tests/`:

```bash
make test
```
