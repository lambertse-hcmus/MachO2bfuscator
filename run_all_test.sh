#!/bin/bash

TEST_DIR="build/tests"

./build.sh -debug
for file in "$TEST_DIR"/test_*; do
    if [ -f "$file" ] && [ -x "$file" ]; then
        echo "Executing $file..."
        "$file"
    fi
done

