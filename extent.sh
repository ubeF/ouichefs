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

