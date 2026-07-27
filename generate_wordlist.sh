#!/bin/bash
# Build wordlist.hpp from wordlist.txt.
# Pads to 2^BITS if short so every BITS-bit index is valid.
set -euo pipefail

SRC="${1:-wordlist.txt}"
OUT="${2:-wordlist.hpp}"
BITS=13
TARGET=$((1 << BITS))

mapfile -t WORDS < <(grep -v '^[[:space:]]*$' "$SRC" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
N=${#WORDS[@]}

if (( N == 0 )); then
  echo "error: no words in $SRC" >&2
  exit 1
fi
if (( N > TARGET )); then
  echo "error: $N words exceeds 2^$BITS ($TARGET)" >&2
  exit 1
fi

pad=0
while (( N < TARGET )); do
  w=$(printf 'zzpad%04d' "$pad")
  WORDS+=("$w")
  N=${#WORDS[@]}
  pad=$((pad + 1))
done

{
  cat <<HDR
#pragma once
#include <array>
#include <string_view>

namespace wordlist {

inline constexpr std::size_t WORD_COUNT = ${TARGET};
inline constexpr std::size_t BITS_PER_WORD = ${BITS};

inline constexpr std::array<std::string_view, WORD_COUNT> WORDLIST = {{
HDR
  for w in "${WORDS[@]}"; do
    printf '    "%s",\n' "$w"
  done
  cat <<'FTR'
}};

}  // namespace wordlist
FTR
} > "$OUT"

echo "Generated $OUT with $TARGET words (padded $pad)"
