#!/bin/bash


if [ $1 ] ; then
  timeout 20s ./scx_tsatw &
  timeout 15s stress-ng -c 10 &
  wait
fi

timeout 30s vng -r -- . ./crash-test.sh crash_test

