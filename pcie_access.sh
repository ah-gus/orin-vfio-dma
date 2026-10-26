#!/bin/bash

cd "$(dirname "$0")"

#echo "$(whoami)"

#[ "$UID" -eq 0 ] || exec sudo "$0" "$@"

sudo ./vfio_setup.sh 0005:01:00.0 32768 2

chmod -w /mnt/hugepages-32768kB/dma0

#./vfio_dev -s 32M -i 0x10000000 0005:01:00.0
./vfio_dev -s 32M -i 0x10000000 -f /mnt/hugepages-32768kB/dma0 0005:01:00.0

