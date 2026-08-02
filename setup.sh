#!/bin/bash

if ! lsmod | grep -q '^ouichefs'; then
    modprobe ouichefs
fi

mkdir -p mnt

if ! mountpoint -q mnt; then
    mount /dev/vda mnt
fi

cd mnt/

cp ../ouichefs/extent_ioctl.h .

printf "%s" '
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>

#include "extent_ioctl.h"

int main(int argc, char **argv)
{
    int fd = open(argv[1], O_RDONLY);

    if (fd < 0) {
        printf("failed to open file!\n");
        return 1;
    }

    if (ioctl(fd, OUICHEFS_IOC_GET_EXTENTS) < 0) {
        printf("ioctl failed!\n");
        return 1;
    }

    close(fd);
    return 0;
}
' > get_extents_ioctl.c

if ! gcc get_extents_ioctl.c -o /tmp/get_extents_ioctl; then
	echo "Cannot compile user program!"
	exit 1
fi

printf "%s" '
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>

#include "extent_ioctl.h"

int main(int argc, char **argv)
{
    int fd = open(argv[1], O_RDONLY);

    if (fd < 0) {
        printf("failed to open file!\n");
        return 1;
    }

	int ret = ioctl(fd, OUICHEFS_IOC_DEFRAG_FILE);
	if (ret <= 0) {
        printf("defrag failed!\n");
        return 1;
    } else {
		printf("defrag succeeded, %d blocks allocated\n", ret);
	}

    close(fd);
    return 0;
}
' > defrag_file.c

if ! gcc defrag_file.c -o /tmp/defrag_file; then
	echo "Cannot compile user program!"
	exit 1
fi
