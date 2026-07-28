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
if ! printf '%s\n' "Moin" > test.txt; then
    echo "WRITE FAILED"
    exit 1
fi

if ! output=$(cat test.txt); then
    echo "READ FAILED"
    exit 1
fi

if [ "$output" = "Moin" ]; then
    echo "PASS"
 else
    echo "FAIL"
    exit 1
 fi

echo "== Append =="
if ! printf '%s\n' "Servus" >> test.txt; then
    echo "WRITE FAILED"
    exit 1
fi

if ! output=$(cat test.txt); then
    echo "READ FAILED"
    exit 1
fi

if [ "$output" = $'Moin\nServus' ]; then
    echo "PASS"
 else
    echo "FAIL"
    exit 1
 fi

echo "== Overwrite =="
if ! printf '%s\n' "Salet" > test.txt; then
    echo "WRITE FAILED"
    exit 1
fi

if ! output=$(cat test.txt); then
    echo "READ FAILED"
    exit 1
fi

if [ "$output" = "Salet" ]; then
    echo "PASS"
else
    echo "FAIL"
    exit 1
fi

echo ""
echo "==================="
echo "  File with Holes"
echo "==================="

rm -f test.txt

if ! echo -n "Moin" | dd of=test.txt bs=4096 seek=2 status=none; then
    echo "WRITE FAILED"
    exit 1
fi

echo "== Correct Filesize =="
if ! size=$(stat -c '%s' test.txt); then
    echo "READ FAILED"
    exit 1
fi

if [ "$size" = "8196" ]; then
    echo "PASS"
 else
    echo "FAIL"
    exit 1
 fi

echo "== Empty blocks are zero =="
if ! output=$(dd if=test.txt bs=8192 count=1 status=none | tr -d '\0' | wc -c); then
    echo "READ FAILED"
    exit 1
fi

if [ "$output" = "0" ]; then
    echo "PASS"
 else
    echo "FAIL"
    exit 1
 fi

echo "== Data is accessible =="
if ! output=$(tail -c 4 test.txt); then
    echo "READ FAILED"
    exit 1
fi

if [ "$output" = "Moin" ]; then
    echo "PASS"
else
    echo "FAIL"
    exit 1
fi

echo "== Writing into the Hole correct Filesize check =="
if ! old_size=$(stat -c '%s' test.txt); then
    echo "READ FAILED"
    exit 1
fi

if ! printf "TEST" | dd of=test.txt bs=1 seek=$((4096 + 100)) conv=notrunc status=none; then 
    echo "WRITE FAILED" 
    exit 1 
fi

if ! new_size=$(stat -c '%s' test.txt); then
    echo "READ FAILED"
    exit 1
fi

if [ "$new_size" -eq "$old_size" ]; then
    echo "PASS"
else
    echo "FAIL"
    exit 1
fi

echo "== Written data inside hole is correct =="

if dd if=test.txt bs=1 skip=$((4096 + 100)) count=4 status=none |
   cmp -s - <(printf "TEST"); then
    echo "PASS"
else
    echo "FAIL"

    echo "Expected:"
    printf "TEST" | od -An -tx1

    echo "Actual:"
    dd if=test.txt bs=1 skip=$((4096 + 100)) count=4 status=none |
        od -An -tx1

    exit 1
fi

echo "== Original data is still correct =="

if ! output=$(tail -c 4 test.txt); then
    echo "READ FAILED"
    exit 1
fi

if [ "$output" = "Moin" ]; then
    echo "PASS"
else
    echo "FAIL"
    exit 1
fi

echo "== Bytes around written data remain zero =="

if ! output=$(dd if=test.txt bs=1 skip=4096 count=100 status=none |
    tr -d '\0' |
    wc -c); then
    echo "READ FAILED"
    exit 1
fi

if [ "$output" = "0" ]; then
    echo "PASS"
else
    echo "FAIL"
    exit 1
fi

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

truncate -s $((4096)) test.txt

dmesg -C

if ! output=$(/tmp/get_extents_ioctl "test.txt"); then
    echo "Failed to run user program!"
    exit 1
fi

if dmesg | grep -q "1 extent(s)" &&
   dmesg | grep -q "count=1"; then
    echo "PASS"
else
    echo "Wrong truncate extent metadata"
    dmesg
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
if ! cmp ./bigfile ./mnt/bigfile; then
    echo "5MB file not correct written/read"
    echo "FAIL"
    exit 1
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
    echo "FAIL"
    exit 1
fi

cd mnt

rm *

sync

echo ""
echo "====================================================="
echo "  Test Validate Fewer Extents With Block Allocator"
echo "====================================================="

if ! dd if=/dev/zero of=test1.txt bs=32768 count=1 status=none; then
    exit 1
fi

if ! dd if=/dev/zero of=test1.txt bs=32768 count=1 status=none; then
    exit 1
fi

rm test1.txt

if ! dd if=/dev/zero of=test3.txt bs=65536 count=1 status=none; then
    exit 1
fi

dmesg -C

if ! /tmp/get_extents_ioctl "test3.txt"; then
    echo "Failed to run user program!"
    exit 1
fi

if dmesg | grep -q "1 extent(s)"; then
    echo "PASS"
else
    echo "Wrong extents number received"
    exit 1
fi

echo ""
echo "===================================="
echo "  Test Validate Block Reservation"
echo "===================================="

rm *

exec 3>test1.txt
exec 4>test2.txt

for i in 1 2 3 4 5; do
    printf '%4096s' '' >&3 || exit 1
    printf '%4096s' '' >&4 || exit 1
done

exec 3>&-
exec 4>&-

sync

dmesg -C

if ! /tmp/get_extents_ioctl "test1.txt"; then
    echo "Failed inspect test1.txt!"
    exit 1
fi

if ! /tmp/get_extents_ioctl "test2.txt"; then
    echo "Failed inspect test2.txt!"
    exit 1
fi

log=$(dmesg)
printf '%s\n' "$log"

if [ "$(printf '%s\n' "$log" | grep -c "1 extent(s)")" -eq 2 ] &&
   [ "$(printf '%s\n' "$log" | grep -c "count=5")" -eq 2 ]; then
    echo "PASS"
else
    echo "Block reservation test failed"
    exit 1
fi

echo ""
echo "===================================="
echo " Test Reservation GC"
echo "===================================="

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
fi

exec 3>&-

rm -f *

echo ""
echo "===================================="
echo " Test Sysfs"
echo "===================================="

sys_path="/sys/ouichefs/vda"

if ! total_blocks=$(cat "$sys_path/total_blocks"); then
    echo "FAIL: error get total blocks"
    exit 1
else
    echo "total blocks: $total_blocks"
fi

if ! free_blocks=$(cat "$sys_path/free_blocks"); then
    echo "FAIL: error get free blocks"
    exit 1
else 
    echo "free blocks: $free_blocks"
fi

if ! committed_blocks=$(cat "$sys_path/committed_blocks"); then
    echo "FAIL: error get committed blocks"
    exit 1
else
    echo "committed blocks: $committed_blocks"
fi

if ! reserved_blocks=$(cat "$sys_path/reserved_blocks"); then
    echo "FAIL: error get reserved blocks"
    exit 1
else
    echo "reserved blocks: $reserved_blocks"
fi

if ! files=$(cat "$sys_path/files"); then
    echo "FAIL: error get number files"
    exit 1
else
    echo "number files: $files"
fi

if ! total_extents=$(cat "$sys_path/total_extents"); then
    echo "FAIL: error get total extents"
    exit 1
else
    echo "total extents: $total_extents"
fi

if ! avg_extent_size=$(cat "$sys_path/avg_extent_size"); then
    echo "FAIL: error get avg extent size"
    exit 1
else
    echo "avg extent size: $avg_extent_size"
fi

if ! max_file_size=$(cat "$sys_path/max_file_size"); then
    echo "FAIL: error get max file size"
    exit 1
else
    echo "max file size: $max_file_size"
fi

if ! fragmentation=$(cat "$sys_path/fragmentation"); then
    echo "FAIL: error get fragmentation"
    exit 1
else
    echo "fragmentation: $fragmentation"
fi

if ! reservation_size=$(cat "$sys_path/reservation_size"); then
    echo "FAIL: error get reservation size"
    exit 1
else
    echo "reservation: $reservation_size"
fi

if ! gc_runs=$(cat "$sys_path/gc_runs"); then
    echo "FAIL: error get gc runs"
    exit 1
else
    echo "gc runs: $gc_runs"
fi

if (( free_blocks + committed_blocks + reserved_blocks == total_blocks )); then
    echo "PASS"
else
    echo "FAIL: free blocks + commited blocks + reserved blocks not equal total blocks"
    exit 1
fi

echo ""
echo "===================================="
echo " Defragmentation Test"
echo "===================================="

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

# disable reservation for these tests
echo 0 > "/sys/module/ouichefs/parameters/reservation_size"

echo "== Simple defragmentation test =="
# Create a fragmented file
rm *

exec 3>test1.txt
exec 4>test2.txt

for i in 1 2 3 4 5; do
    printf '%4096s' '' >&3 || exit 1
    printf '%4096s' '' >&4 || exit 1
done

exec 3>&-
exec 4>&-

sync

# Check whether the file is fragmented (more than 1 extent) before defrag
dmesg -C
if ! /tmp/get_extents_ioctl "test1.txt"; then
    echo "get_extents ioctl failed"
    echo "FAIL"
    exit 1
fi

extents=$(dmesg | grep -oE '[0-9]+ extent\(s\)' | awk '{print $1}' | head -n1)
if [ -z "$extents" ]; then
    echo "Could not determine extent count from dmesg"
    echo "FAIL"
    exit 1
fi

if [ "$extents" -le 1 ]; then
    echo "file not fragmented (extents=$extents)"
    echo "FAIL"
    exit 1
fi

# Run defrag helper on test1.txt and ensure it reports 5 blocks allocated
if ! output=$(/tmp/defrag_file "test1.txt"); then
    echo "defrag helper failed"
    echo "FAIL"
    exit 1
fi

if ! printf '%s' "$output" | grep -q "defrag succeeded, 5 blocks allocated"; then
    echo "defrag did not return 5"
    echo "Output: $output"
    echo "FAIL"
    exit 1
fi

# Verify extents: expect single extent of size 5
dmesg -C
if ! /tmp/get_extents_ioctl "test1.txt"; then
    echo "get_extents ioctl failed"
    echo "FAIL"
    exit 1
fi

if ! dmesg | grep -q "1 extent(s)" || ! dmesg | grep -q "count=5"; then
    echo "Wrong extents after defrag"
    dmesg
    exit 1
fi

echo "PASS"

echo ""
echo "== Defrag preserves holes test =="

# Create a file with a hole in the middle: data at block 0 and at block 3 (two-block hole)
rm -f *
printf '%s' "A" | dd of=hole.txt bs=4096 count=1 seek=0 conv=notrunc status=none
printf '%s' "B" | dd of=hole.txt bs=4096 count=1 seek=3 conv=notrunc status=none
sync

# Run defrag
if ! output=$(/tmp/defrag_file "hole.txt"); then
    echo "defrag helper failed"
    echo "FAIL"
    exit 1
fi

# Verify hole still present after defrag
dmesg -C
if ! /tmp/get_extents_ioctl "hole.txt"; then
    echo "get_extents ioctl failed"
    exit 1
fi

if ! dmesg | grep -q "start=0 count=2"; then
    echo "Hole was not preserved after defrag"
    dmesg
    exit 1
fi

echo "PASS"

echo ""
echo "== Partial defragmentation under low space =="

# Create fragmented file by writing alternately to two files
rm -f *
exec 3>part1.txt
exec 4>part2.txt
for i in $(seq 1 50); do
    printf '%4096s' '' >&3 || exit 1
    printf '%4096s' '' >&4 || exit 1
done
exec 3>&-
exec 4>&-
sync

# Fill filesystem completely
rm -f filler
while printf '%4096s' '' >> filler; do
    :
done

# Free a small amount of space (8 blocks)
truncate -s $(( $(stat -c %s filler) - 32768 )) filler
sync

# Expected values for this partial-defrag scenario
needed_blocks=50
expected_allocated=8

# Run defrag (should be partial) and verify it moved some blocks and preserved total blocks
if ! output=$(/tmp/defrag_file "part1.txt"); then
    echo "defrag helper failed"
    echo "FAIL"
    exit 1
fi

allocated=$(printf '%s' "$output" | grep -oE '[0-9]+' | tail -n1)
if [ -z "$allocated" ]; then
    echo "defrag returned no allocated blocks"
    echo "Output: $output"
    echo "FAIL"
    exit 1
fi

if [ "$allocated" -ne "$expected_allocated" ]; then
    echo "defrag allocated $allocated blocks, expected $expected_allocated"
    echo "Output: $output"
    echo "FAIL"
    exit 1
fi

# Verify total blocks equal expected (no data loss)
dmesg -C
if ! /tmp/get_extents_ioctl "part1.txt"; then
    echo "get_extents ioctl failed"
    exit 1
fi
after_blocks=$(dmesg | grep -o 'count=[0-9]*' | awk -F= '{sum += $2} END {print sum}')
if [ "$after_blocks" -ne "$needed_blocks" ]; then
    echo "Block count mismatch after defrag: expected=$needed_blocks after=$after_blocks"
    dmesg
    exit 1
fi

echo "PASS"