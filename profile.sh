#!/bin/sh
set -xe
cd build
cmake .. -DDISABLE_MONTE_CARLO=1 -DFIXED_BOARD_SIZE=9 -DDUSE_VALGRIND=1 -DHAVE_CONFIG_H=1 -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j
valgrind --tool=callgrind interface/gnugo -b10

