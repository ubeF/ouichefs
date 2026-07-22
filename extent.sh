#!/bin/bash

if ! lsmod | grep -q '^ouichefs'; then
    modprobe ouichefs
fi

mkdir -p mnt

if ! mountpoint -q mnt; then
    mount /dev/vda mnt
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
echo "Salet" > test.txt
if [ "$(cat test.txt)" = "Salet" ]; then
    echo "PASS"
else
    echo "FAIL"
fi

#echo ""
#echo "==================="
#echo "  File with Holes"
#echo "==================="
#
#rm -f test.txt
#echo -n "Moin" | dd of=test.txt bs=4096 seek=2 status=none
#
#echo "== Correct Filesize =="
#if [ "$(stat -c '%s' test.txt)" = "8196" ]; then
#    echo "PASS"
#else
#    echo "FAIL"
#fi
#
#echo "== Empty blocks are zero =="
#if [ "$(dd if=test.txt bs=8192 count=1 status=none | tr -d '\0' | wc -c)" = "0" ]; then
#    echo "PASS"
#else
#    echo "FAIL"
#fi
#
#echo "== Data is accessible =="
#if [ "$(tail -c 4 test.txt)" = "Moin" ]; then
#    echo "PASS"
#else
#    echo "FAIL"
#fi

echo ""
echo "============================"
echo "  Test Get Extents IOCTL"
echo "============================"

rm -f test.txt
cp ../ouichefs/extent_ioctl.h .

printf "%s" '
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "extent_ioctl.h"

int main(int argc, char **argv)
{
    int fd = open(argv[1], O_RDONLY);

    if (fd < 0)
        return 1;

    if (ioctl(fd, OUICHEFS_IOC_GET_EXTENTS) < 0)
        return 1;

    close(fd);
    return 0;
}
' > get_extents_ioctl.c

if ! gcc get_extents_ioctl.c -o /tmp/get_extents_ioctl; then
	echo "Cannot compile user program!"
	exit 1
fi

echo "Moin" > test.txt

if ! output=$(/tmp/get_extents_ioctl "test.txt"); then
    echo "Failed to run user program!"
    exit 1
else
    echo "PASS"
fi

echo ""
echo "============================"
echo "  Test Extents Edge Cases"
echo "============================"

echo "== Test Truncate File =="

dd if=/dev/zero of=test.txt bs=4096 count=8 status=none

truncate -s $((4*4096)) test.txt

dmesg -C

if ! output=$(/tmp/get_extents_ioctl "test.txt"); then
    echo "Failed to run user program!"
    exit 1
fi

if dmesg | grep -q "count=4"; then
    echo "PASS"
else
    echo "Wrong block count received"
    exit 1
fi

echo "== Test Truncate to Size Zero =="

dd if=/dev/zero of=test.txt bs=4096 count=8 status=none

truncate -s 0 test.txt

dmesg -C

if ! output=$(/tmp/get_extents_ioctl "test.txt"); then
    echo "Failed to run user program!"
    exit 1
fi

if dmesg | grep -q "0 extent(s)"; then
    echo "PASS"
else
    echo "Wrong extents number received"
    exit 1
fi

# The following test is for 1.5.3 Verification
echo "== Test Extents Big File Size =="

cd ..
dd if=/dev/urandom of=bigfile bs=1M count=5
cp bigfile mnt/
if cmp ./bigfile ./mnt/bigfile; then
    echo "PASS"
else
    echo "5MB file not correct written/read"
    echo "FAIL"
fi

dmesg -C

if ! /tmp/get_extents_ioctl "mnt/bigfile"; then
    echo "Failed to run user program!"
    exit 1
fi

if [ "$(dmesg | grep "count=" | awk -F'count=' '{sum += $2} END {print sum}')" -eq 1280 ]; then
    echo "PASS"
else
    echo "Did not return correct block count"
    exit 1
fi