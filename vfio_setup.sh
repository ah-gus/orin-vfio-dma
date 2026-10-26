#!/bin/bash
# Bind a PCIe device to vfio-pci and prepare the system for vfio_dev.
# usage: sudo ./vfio_setup.sh <BDF> [hugepage_kB] [count]
#   e.g. sudo ./vfio_setup.sh 0005:01:00.0          (four 2M hugepages)
#        sudo ./vfio_setup.sh 0005:01:00.0 32768 2  (two 32M hugepages)
set -e
BDF=${1:?usage: $0 <BDF> [hugepage_kB] [count]}
HP_KB=${2:-2048}
HP_NR=${3:-4}
DEV=/sys/bus/pci/devices/$BDF
[ -d "$DEV" ] || { echo "no such device $BDF (lspci -D)"; exit 1; }

# Orin's DesignWare MSI controller has no interrupt remapping; without this
# VFIO_SET_IOMMU fails with EPERM.
echo Y > /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts

# Unbind the current driver and bind vfio-pci.
if [ -e "$DEV/driver" ]; then
    [ "$(basename "$(readlink "$DEV/driver")")" = vfio-pci ] || echo "$BDF" > "$DEV/driver/unbind"
fi
echo vfio-pci > "$DEV/driver_override"
[ -e "$DEV/driver" ] || echo "$BDF" > /sys/bus/pci/drivers_probe

GRP=$(basename "$(readlink "$DEV/iommu_group")")
echo "IOMMU group $GRP:"
for d in /sys/kernel/iommu_groups/$GRP/devices/*; do
    drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null || true)
    echo "  $(basename "$d")  driver=${drv:-none}"
done
chmod 666 /dev/vfio/$GRP   # optional: lets a non-root user open the group

echo "$HP_NR" > /sys/kernel/mm/hugepages/hugepages-${HP_KB}kB/nr_hugepages
echo "hugepages-${HP_KB}kB: $(cat /sys/kernel/mm/hugepages/hugepages-${HP_KB}kB/free_hugepages) free"

# hugetlbfs mount at this page size for file-backed DMA buffers (vfio_dev -f),
# owned by the user who ran sudo so they can create and share the file.
MNT=/mnt/hugepages-${HP_KB}kB
mkdir -p "$MNT"
mountpoint -q "$MNT" || mount -t hugetlbfs -o pagesize=${HP_KB}K,uid=${SUDO_UID:-0},gid=${SUDO_GID:-0},mode=0770 none "$MNT"
echo "hugetlbfs mount: $MNT   (e.g. vfio_dev -f $MNT/dma0 ...)"

# Let the user who ran sudo mmap the BAR files that vfio_dev -B links to.
if [ -n "$SUDO_UID" ]; then
    for r in "$DEV"/resource[0-5]; do [ -e "$r" ] && chown "$SUDO_UID:$SUDO_GID" "$r"; done
    echo "BAR files owned by uid $SUDO_UID: $(ls "$DEV" | grep -E '^resource[0-5]$' | tr '\n' ' ')"
fi
