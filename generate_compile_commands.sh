#!/bin/bash
# Generate compile_commands.json for clangd
# Includes all source files with proper flags

cd "$(dirname "$0")"
PROJECT_ROOT="$(pwd)"

# Common compile flags
FLAGS="-std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude -pthread"

# Start JSON array
echo "[" > compile_commands.json

# Find all .c files in src/ and test/
files=$(find src test -type f -name "*.c" 2>/dev/null | sort)

first=true
for file in $files; do
    if [ "$first" = false ]; then
        echo "," >> compile_commands.json
    fi
    first=false

    echo "  {" >> compile_commands.json
    echo "    \"directory\": \"$PROJECT_ROOT\"," >> compile_commands.json
    echo "    \"command\": \"cc $FLAGS -c $file\"," >> compile_commands.json
    echo "    \"file\": \"$file\"" >> compile_commands.json
    echo -n "  }" >> compile_commands.json
done

echo "" >> compile_commands.json
echo "]" >> compile_commands.json

count=$(echo "$files" | wc -l | tr -d ' ')
echo "Generated compile_commands.json with $count entries"
