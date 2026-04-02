#!/bin/bash
set -e
cd "$(dirname "$0")"
OUT="static_files.h"
echo "// Auto-generated — do not edit" > "$OUT"
echo "#pragma once" >> "$OUT"
for f in static/*; do
  name=$(basename "$f" | tr '.-' '__')
  echo "" >> "$OUT"
  echo "static const char static_${name}[] = {" >> "$OUT"
  xxd -i < "$f" >> "$OUT"
  echo "};" >> "$OUT"
  size=$(wc -c < "$f" | tr -d ' ')
  echo "static const unsigned long static_${name}_len = ${size};" >> "$OUT"
done
echo "generated $OUT"
