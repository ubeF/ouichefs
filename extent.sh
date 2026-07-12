#!/bin/bash

if ! modprobe ouichefs; then
	echo "Could not load module ouichefs module!"
	exit 1
fi

mkdir mnt

if ! mount /dev/vda mnt/; then
	echo "Could not mount image!"
	exit 1
fi

cd mnt/
rm -rf *

echo "============================"
echo "  Generic read/write tests"
echo "============================"

echo "== Simple write + read =="
echo "Moin" > test.txt
if [ "$(cat test.txt)" = "Moin" ]; then
    echo "PASS"
else
    echo "FAIL"
fi

echo "== Append =="
echo "Servus" >> test.txt
if [ "$(cat test.txt)" = $'Moin\nServus' ]; then
    echo "PASS"
else
    echo "FAIL"
fi

echo "== Overwrite =="
echo "Salü" > test.txt
if [ "$(cat test.txt)" = "Salü" ]; then
    echo "PASS"
else
    echo "FAIL"
fi
