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

1. Optional zstd compression of the payload (`-c`)  
2. Payload bits + a trailing **stop bit** (`1`), zero-padded to 13-bit groups  
3. Each 13-bit group → word index in the 8192-word list  

Decode maps words back to bits, finds the last `1` as the stop bit, and drops the pad.  
There is **no** embedded compression marker — use `-c` on both encode and decode when compressed.

Small inputs with `-c` often produce *more* words because zstd frame headers add overhead until the data is large/repetitive enough to shrink.

## Test

Native C++ tests live in `tests/`:

```bash
make test
```
