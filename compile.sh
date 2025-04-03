#!/bin/sh

echo "Building cr3 tools..."
for file in ./*.c; do
    make FILENAME="$(basename "$file" .c)"
done
echo "Done"