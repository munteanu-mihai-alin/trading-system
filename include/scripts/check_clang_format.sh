#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

mapfile -t FILES < <(find include src tests -type f \( -name '*.hpp' -o -name '*.h' -o -name '*.cpp' \) | sort)

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "No C++ source files found."
  exit 0
fi

FAILED=0
for f in "${FILES[@]}"; do
  if ! diff -u "${f}" <(clang-format -style=file "${f}") >/dev/null; then
    echo "Formatting mismatch: ${f}"
    FAILED=1
  fi
done

if [ "${FAILED}" -ne 0 ]; then
  echo "clang-format check failed."
  exit 1
fi

echo "clang-format check passed."
