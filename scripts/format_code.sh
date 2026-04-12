#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

mapfile -t FILES < <(find include src tests -type f \( -name '*.hpp' -o -name '*.h' -o -name '*.cpp' \) | sort)

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "No C++ source files found."
  exit 0
fi

clang-format -i -style=file "${FILES[@]}"
echo "Formatted ${#FILES[@]} files using .clang-format"
