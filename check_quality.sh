#!/bin/bash
# Quick code quality check for Bitcoin Echo
# Run before committing to catch common issues

set -e

echo "Bitcoin Echo — Code Quality Check"
echo "=================================="
echo ""

# Check for trailing whitespace
echo "→ Checking for trailing whitespace..."
if git ls-files '*.c' '*.h' | xargs grep -n ' $' 2>/dev/null; then
    echo "  ⚠ Found trailing whitespace (see above)"
else
    echo "  ✓ No trailing whitespace"
fi
echo ""

# Check for tabs (we use spaces)
echo "→ Checking for tabs in source files..."
if git ls-files '*.c' '*.h' | xargs grep -P '\t' 2>/dev/null; then
    echo "  ⚠ Found tabs (we use 4 spaces)"
else
    echo "  ✓ No tabs found"
fi
echo ""

# Check for TODO/FIXME/XXX comments
echo "→ Checking for TODO/FIXME markers..."
count=$(git ls-files '*.c' '*.h' | xargs grep -i 'TODO\|FIXME\|XXX' 2>/dev/null | wc -l | tr -d ' ')
if [ "$count" -gt 0 ]; then
    echo "  ⚠ Found $count TODO/FIXME markers"
    git ls-files '*.c' '*.h' | xargs grep -n -i 'TODO\|FIXME\|XXX' 2>/dev/null | head -5
else
    echo "  ✓ No pending TODO markers"
fi
echo ""

# Check build
echo "→ Testing build..."
if make -j4 > /dev/null 2>&1; then
    echo "  ✓ Build successful"
else
    echo "  ✗ Build failed"
    exit 1
fi
echo ""

echo "=================================="
echo "Quality check complete!"
