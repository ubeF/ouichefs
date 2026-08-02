#!/bin/bash

rm -f *

# Fill filesystem almost completely
while printf '%4096s' '' >> filler; do
    :
done

# Free a small amount of space
truncate -s $(( $(stat -c %s filler) - 32768 )) filler
sync

# Create reservations
exec 3>test1

printf '%4096s' '' >&3

dmesg -C

# This write should be the first to hit ENOSPC and invoke GC
if printf '%4096s' '' > trigger; then
    echo PASS
else
    echo FAIL
    exit 1
fi

exec 3>&-

rm -f *

