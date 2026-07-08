#!/bin/bash
export MSYSTEM=MINGW64
export PATH="/mingw64/bin:/usr/bin:$PATH"

echo "Installing MINGW64 packages..."
pacman -S --noconfirm --needed make git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-zeromq mingw-w64-x86_64-libsodium jq

echo "Building plugin in MINGW64..."
cd "/e/VSTDEV/JUCE_PROJECTS/VCV MODULES/zmq_plugin"
make clean
make install
