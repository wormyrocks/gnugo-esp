#!/bin/sh
set -xe
mkdir -p build
cd build
cmake .. -DCONFIG_DISABLE_MONTE_CARLO=1 -DCONST_PATTERNS -DUSE_VALGRIND=1 -DHAVE_CONFIG_H=1 -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j
interface/gnugo -b1000 -S -T --showtime
#valgrind --tool=callgrind interface/gnugo -b10
