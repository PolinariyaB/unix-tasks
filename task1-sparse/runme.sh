#!/bin/sh
set -eu

# Save complete test report.
exec > result.txt 2>&1

cc -O2 -Wall -Wextra -std=c11 -o myprogram myprogram.c
chmod +x ./make_test_A.sh
chmod +x ./runme.sh

rm -f fileA fileB fileC fileD fileA.gz fileB.gz

echo "Test 0: Create fileA"
echo "Expect: size is 4*1024*1024+1 and markers at offsets 0, 10000, end"
./make_test_A.sh

echo "Test 1: Copy fileA -> fileB with default block size"
echo "Expect: content preserved, sparse behavior possible on FS"
./myprogram fileA fileB

echo "Test 2: gzip compression for fileA and fileB"
echo "Expect: .gz files are created"
gzip -kf fileA
gzip -kf fileB

echo "Test 3: Restore from gzip stream to fileC via myprogram"
echo "Expect: fileC content equals original data"
gzip -cd fileB.gz | ./myprogram fileC

echo "Test 4: Copy fileA -> fileD using block size 100"
echo "Expect: works with non-default block size"
./myprogram -b 100 fileA fileD

echo "Test 5: File statistics"
echo "Expect: logical size for A/B/C/D is identical"
echo "=== stat sizes ==="
stat fileA fileA.gz fileB fileB.gz fileC fileD

echo "=== Expected vs Actual ==="
echo "Expected: size(fileA)=size(fileB)=size(fileC)=size(fileD)=4194305 bytes"
echo "Actual:"
stat -c "%n %s bytes" fileA fileB fileC fileD
echo "Expected: gzip archives exist and are much smaller than uncompressed file"
echo "Actual:"
stat -c "%n %s bytes" fileA.gz fileB.gz
