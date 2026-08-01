#!/usr/bin/env bash

# Loop through all files in the directory tree
find . -type f | while read -r file; do
    # Create a temporary cleared version of the file
    awk '
    /\*\*\*/ { flag = !flag; next } 
    !flag { print }
    ' "$file" > "$file.tmp" && mv "$file.tmp" "$file"
done


# Loop through all files in the directory tree
find . -type f | while read -r file; do
    # Filter the file and overwrite it safely
    awk '
