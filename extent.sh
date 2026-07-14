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

echo ""
echo "==================="
echo "  File with Holes"
echo "==================="

rm -f test.txt
echo -n "Moin" | dd of=test.txt bs=4096 seek=2 status=none

echo "== Correct Filesize =="
if [ "$(stat -c '%s' test.txt)" = "8196" ]; then
    echo "PASS"
else
    echo "FAIL"
fi

echo "== Empty blocks are zero =="
if [ "$(dd if=test.txt bs=8192 count=1 status=none | tr -d '\0' | wc -c)" = "0" ]; then
    echo "PASS"
else
    echo "FAIL"
fi

echo "== Data is accessible =="
if [ "$(tail -c 4 test.txt)" = "Moin" ]; then
    echo "PASS"
else
    echo "FAIL"
fi

