#!/bin/bash
# Utility script for building documentation

# Build documentation for all targets
function build_docs_all() {
    for target in esp32 esp32s3 esp32c3 esp32c6 esp32p4
    do
        echo "Building docs for target: $target"
        build-docs -l en -t $target
    done
}

# Build documentation for a specific target
function build_docs_target() {
    local target=$1
    if [ -z "$target" ]; then
        echo "No target specified, using esp32"
        target="esp32"
    fi
    echo "Building docs for target: $target"
    build-docs -l en -t $target
}

# Run Doxygen separately
function build_doxygen() {
    echo "Building Doxygen documentation"
    cd doxygen && doxygen Doxyfile
}

# Main function
if [ "$1" == "all" ]; then
    build_docs_all
elif [ "$1" == "doxygen" ]; then
    build_doxygen
else
    build_docs_target $1
fi