#!/usr/bin/env bash
sox FDV_offair.wav \
-t raw -b 16 -r 8000 -e signed-integer - | \
../build/tools/webrx_rade_decode |sox -t raw -r 8000 -b 16 -e signed-integer -c 1 - output.wav
