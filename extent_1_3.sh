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

cp ../extent_ioctl.h .

echo "============================"
echo "  Test Get Extents IOCTL"
echo "============================"

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

ls

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
