#!/bin/bash

exec 3>test1.txt
exec 4>test2.txt
for i in $(seq 1 13); do
    printf '%4096s' '' >&3 || exit 1
    printf '%4096s' '' >&4 || exit 1
done
exec 3>&-
exec 4>&-
