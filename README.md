# orin-vfio-dma

Userspace control of a PCIe endpoint on **NVIDIA Jetson AGX Orin** through
**VFIO**, with no custom kernel module and no kernel rebuild.

`vfio_dev` takes ownership of a PCIe device, enables bus mastering and MSI,
maps a BAR, allocates a physically contiguous hugepage DMA buffer behind the
SMMU, and then holds everything in place while the device is driven
externally (for example with `devmem`). The BAR and the DMA buffer can both be
published as files, so other processes can `mmap` them.

Developed and tested with an Altera/Intel FPGA endpoint (`1172:cafe`) in the
x16 slot (PCIe controller C5, segment `0005`).

---

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Repository layout](#repository-layout)
- [Why a device-tree overlay is needed](#why-a-device-tree-overlay-is-needed)
- [Installation](#installation)
- [Usage](#usage)
- [Accessing the device from other processes](#accessing-the-device-from-other-processes)
- [Shutdown and DMA safety](#shutdown-and-dma-safety)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)

---

## Features

| Feature | How |
|---|---|
| Bus mastering | Sets `PCI_COMMAND_MEMORY` and `PCI_COMMAND_MASTER` through the VFIO config region, after the DMA mapping exists |
| MSI interrupts | `VFIO_DEVICE_SET_IRQS` with one eventfd per vector; interrupts are counted and printed |
| BAR mapping | Any memory BAR (`-b`, default BAR2), mapped with `mmap` on the VFIO device |
| Contiguous DMA memory | A single hugetlb page (2 MB, 32 MB or 1 GB), so the buffer is always physically contiguous |
| IOMMU mapping | Maps the buffer at a fixed IOVA, either the one you give with `-i` or the lowest valid address the kernel reports |
| Shared DMA buffer | Optional hugetlbfs file (`-f`) that any process can `mmap` |
| Shared BAR | Optional symlink (`-B`) to the BAR's sysfs resource file, which any process can `mmap` |
| Clean shutdown | On `SIGINT`/`SIGTERM`: bus mastering off, MSI off, DMA unmapped, links removed |

---

## Requirements

- Jetson AGX Orin with JetPack 6.2.x (L4T R36, kernel 5.15). The stock kernel
  is used unchanged: `CONFIG_VFIO`, `CONFIG_VFIO_PCI`, `CONFIG_VFIO_IOMMU_TYPE1`
  and `CONFIG_HUGETLBFS` are all built in.
- Root access for the one-time setup (device-tree overlay, `vfio_setup.sh`).
  After setup, `vfio_dev` and `dmamem` run as a normal user.
- Build tools: `gcc`, `make`, and `dtc` (package `device-tree-compiler`).

---

## Repository layout

| File | Purpose |
|---|---|
| `vfio_dev.c` | Main program: VFIO setup, MSI, BAR mapping, hugepage DMA buffer, wait loop |
| `dmamem.c` | `devmem`-style read/write tool for the shared DMA file or BAR link |
| `vfio_setup.sh` | Binds the device to `vfio-pci`, reserves hugepages, mounts hugetlbfs, sets permissions |
| `tegra234-c5-pcie-vfio-overlay.dts` | Device-tree overlay that makes the C5 IOMMU group usable by VFIO |
| `pcie_access.sh` | Example: setup followed by a typical `vfio_dev` run |
| `Makefile` | Builds `vfio_dev`, `dmamem` and the `.dtbo` overlay |

---

## Why a device-tree overlay is needed

On Orin, each PCIe controller tags all of its traffic with one SMMU stream ID.
In the stock device tree, the controller node itself also uses that ID
(`iommus = <&smmu 0x14>`), so the controller ends up in the **same IOMMU group**
as the endpoints behind it:

```
IOMMU group: 0005:00:00.0  (root port, pcieport)
             0005:01:00.0  (endpoint,  vfio-pci)
             141a0000.pcie (controller, tegra194-pcie)   <-- blocks VFIO
```

VFIO only accepts a group if every device in it is a bridge or is bound to
`vfio-pci`/`pci-stub`. The controller is neither, so `vfio_dev` fails with
*"IOMMU group not viable"*.

The overlay sets `iommus` on the controller node to an empty value. The kernel
then attaches the controller to no IOMMU, and the group contains only the root
port and the endpoint. Overlays cannot delete properties, so an empty value is
used instead; it has the same effect. The endpoints' `iommu-map` is not
touched, so the endpoint remains fully translated and isolated by the SMMU.

The overlay only changes the C5 controller (`pcie@141a0000`). To use another
controller, change `target-path` in the `.dts`.

---

## Installation

### 1. Build

```bash
make
```

This produces `vfio_dev`, `dmamem` and `tegra234-c5-pcie-vfio-overlay.dtbo`.

### 2. Install the overlay (once)

```bash
sudo cp tegra234-c5-pcie-vfio-overlay.dtbo /boot/
sudo cp /boot/extlinux/extlinux.conf /boot/extlinux/extlinux.conf.bak
```

In `/boot/extlinux/extlinux.conf`, copy your current default `LABEL` block to a
new label, then add the overlay and the VFIO interrupt parameter to the copy:

```
LABEL VFIO
	MENU LABEL VFIO
	LINUX <same as your current entry>
	FDT <same as your current entry>
	INITRD <same as your current entry>
	APPEND <same as your current entry> vfio_iommu_type1.allow_unsafe_interrupts=1
	OVERLAYS <your existing overlays, if any>,/boot/tegra234-c5-pcie-vfio-overlay.dtbo
```

Set `DEFAULT VFIO` and reboot. Keep the original entry: it can be selected from
the boot menu on the serial console if anything goes wrong.

`allow_unsafe_interrupts` is required because the Orin PCIe MSI controller
does not support interrupt remapping. Without it, `VFIO_SET_IOMMU` fails with
`EPERM`. `vfio_setup.sh` also sets it at runtime.

### 3. Verify after reboot

```bash
stat -c %s /sys/firmware/devicetree/base/bus@0/pcie@141a0000/iommus
# 0 -> overlay applied (8 means it was not applied)

ls /sys/bus/pci/devices/0005:01:00.0/iommu_group/devices
# 0005:00:00.0  0005:01:00.0   (the controller is no longer listed)
```

### 4. Prepare the device (after every boot)

```bash
sudo ./vfio_setup.sh <BDF> [hugepage_kB] [count]
sudo ./vfio_setup.sh 0005:01:00.0 32768 2      # example: two 32 MB hugepages
```

The script:

- binds the device to `vfio-pci` (using `driver_override`) and lists its IOMMU group
- makes `/dev/vfio/<group>` accessible to non-root users
- reserves the requested hugepages
- mounts hugetlbfs at `/mnt/hugepages-<size>kB`, owned by the user who ran `sudo`
- gives that user ownership of the device's BAR files (`resource0`–`resource5`)

Find your device's BDF with `lspci -D`.

---

## Usage

```
vfio_dev [options] <BDF>

  -b BAR    BAR to mmap, 0-5 (default 2)
  -B PATH   publish the BAR at PATH (symlink to its sysfs resource file),
            so other processes can mmap it; removed on exit
  -s SIZE   DMA buffer size (default 2M, up to 1G; K/M/G suffixes)
  -f FILE   back the DMA buffer with FILE on a hugetlbfs mount, so other
            processes can mmap it (page size comes from the mount)
  -i IOVA   device-visible DMA address (default: lowest valid, size aligned)
  -n NVEC   number of MSI vectors to enable (default 1)
  -R        issue VFIO_DEVICE_RESET before setup
```

### Typical run

```bash
./vfio_dev -b 2 -B /tmp/ipb3_bar2 \
           -s 32M -i 0x10000000 -f /mnt/hugepages-32768kB/dma0 \
           0005:01:00.0
```

```
[+] 0005:01:00.0 opened via IOMMU group 41 (type1v2), 9 regions, 5 irq types
[+] BAR2 mapped: 4 KiB at 0xffff9f8f1000, phys 0x2b28005000 (use this with devmem)
    shared link (mmap MAP_SHARED)      : /tmp/ipb3_bar2 -> /sys/bus/pci/devices/0005:01:00.0/resource2
[+] DMA buffer: 32 MiB in one 32M hugepage at 0xffffb6000000
    IOVA (program this into the device) : 0x10000000 - 0x11ffffff
    shared file (mmap MAP_SHARED)      : /mnt/hugepages-32768kB/dma0
[+] MSI enabled: 1/4 vectors, 64-bit address
[+] bus mastering enabled (PCI_COMMAND=0x0006)

[*] ready (pid 14000). Drive the device with devmem; Ctrl+C / SIGTERM to exit, SIGUSR1 to dump the DMA buffer.
```

Program the device with the **IOVA**, not the CPU physical address. All
device DMA goes through the SMMU, which only translates the IOVA range.

While it runs:

- each MSI is printed as `[irq] MSI vector N: +count (total)`
- `kill -USR1 <pid>` prints the first 256 bytes of the DMA buffer
- `Ctrl+C` or `kill <pid>` shuts down cleanly

### Choosing the buffer size

The hugepage size is the smallest one that holds the whole buffer, so the
buffer is always a single, physically contiguous page:

| `-s` | Hugepage used | Reserve with |
|---|---|---|
| ≤ 2M | 2 MB | `vfio_setup.sh <BDF> 2048 <n>` |
| ≤ 32M | 32 MB | `vfio_setup.sh <BDF> 32768 <n>` |
| ≤ 1G | 1 GB | `vfio_setup.sh <BDF> 1048576 <n>` |

With `-f`, the page size is set by the hugetlbfs mount instead, so the file must
be on a mount whose `pagesize=` holds the requested size.

---

## Accessing the device from other processes

### DMA buffer (`-f`)

The buffer is a hugetlbfs file. Map it from any process:

```c
int fd = open("/mnt/hugepages-32768kB/dma0", O_RDWR);
struct stat st;
fstat(fd, &st);
volatile uint32_t *buf = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

- The mapping length must be a whole hugepage, which `st_size` always is.
- The C5 controller is DMA-coherent, so no cache maintenance is needed. For
  ordering, have the device write a completion flag after the data, and read
  that flag before reading the data.
- **Reading** with ordinary file I/O (`read`, `fread`, hex editors) works and
  returns the live contents at the time of the read.
- **Writing** with `write()` is not supported by hugetlbfs (`EINVAL`). Write
  through `mmap`, for example with `dmamem`.
- The file stays after `vfio_dev` exits and keeps its hugepage until you
  delete it. Its contents carry over to the next run.

### BAR (`-B`)

`-B PATH` creates a symlink to `/sys/bus/pci/devices/<BDF>/resourceN`, the
kernel's own mmap-able file for that BAR. A BAR is device memory, so it cannot
be stored in a regular file. Map it the same way as the DMA file, and access it
with aligned `volatile` reads and writes of the register width (no `memcpy`).
Non-root access requires `vfio_setup.sh`, which gives the BAR files to your
user.

### `dmamem`

`devmem` cannot reach RAM on this kernel (`CONFIG_STRICT_DEVMEM=y`). `dmamem`
gives the same read/write style for the shared DMA file and for the BAR link:

```
dmamem FILE OFFSET [WIDTH [VALUE]]      WIDTH = 8, 16, 32 (default) or 64
```

```bash
./dmamem /mnt/hugepages-32768kB/dma0 0x0             # read 32 bits
./dmamem /mnt/hugepages-32768kB/dma0 0x100 32 0x1234 # write, then read back
./dmamem /tmp/ipb3_bar2 0x0                          # read BAR2 register 0
```

`devmem` on the BAR's physical address (printed by `vfio_dev`) also works.

---

## Shutdown and DMA safety

On exit, `vfio_dev` turns off bus mastering first, then MSI, and then removes
the IOMMU mapping. If the device is still doing DMA at that point, a write can
arrive after the mapping is gone. The SMMU blocks it and logs:

```
arm-smmu 12000000.iommu: Unhandled context fault: ... iova=0x1000xxxx ...
tegra-mc 2c00000.memory-controller: pcie5w: secure write ...: VPR violation
```

This is the SMMU protecting memory, not a crash. To avoid it, **stop the
device's DMA engine before stopping `vfio_dev`**.

Also stop other processes that use the BAR link before `vfio_dev` exits.
Their mappings stay in place after the device has been shut down, and accesses
through them can return garbage or fault.

---

## Troubleshooting

| Message | Cause | Fix |
|---|---|---|
| `IOMMU group N not viable` | The PCIe controller shares the endpoint's group | Install the overlay and reboot ([details](#why-a-device-tree-overlay-is-needed)) |
| `VFIO_SET_IOMMU: EPERM` | `allow_unsafe_interrupts` is off | Run `vfio_setup.sh`, or add `vfio_iommu_type1.allow_unsafe_interrupts=1` to the kernel command line |
| `open /dev/vfio/N: No such file or directory` | The device is not bound to `vfio-pci` | Run `vfio_setup.sh` |
| `open /dev/vfio/N: Device or resource busy` | Another process holds the group | Stop the other `vfio_dev` (`fuser -v /dev/vfio/*`) |
| `mmap ... hugepage failed` | No free hugepage of the needed size | `vfio_setup.sh <BDF> <kB> <count>`; check `/sys/kernel/mm/hugepages/*/free_hugepages` |
| `... is not on a hugetlbfs mount` | `-f` points outside hugetlbfs | Use the mount created by `vfio_setup.sh` |
| `[!] .../resourceN is root-only` | BAR files still owned by root | Run `vfio_setup.sh` with `sudo` from your user |
| `BARn is not implemented` | Empty BAR, or the upper half of a 64-bit BAR | Check the layout with `lspci -vv` |
| `arm-smmu ... Unhandled context fault` | Device DMA to an unmapped IOVA | Program the IOVA printed by `vfio_dev`; stop DMA before exiting |
| AER `CmpltTO` with reads returning `0xffffffff` | The device did not answer an MMIO read | Device-side: check that the BAR's logic responds to reads |
| `MSI enable` shows 0 in VFIO's config space | VFIO keeps its own copy of that bit | Expected. `sudo lspci -vv` shows the real `MSI: Enable+` |

---

## Limitations

- MSI only. MSI-X and legacy INTx are not wired up.
- One BAR is mapped per run.
- One DMA buffer per run, at most one 1 GB hugepage.
- The `vfio_setup.sh` binding, hugepage reservation and hugetlbfs mount do not
  persist across reboots. Run the script after each boot.
- After a JetPack update, check that the overlay still applies (see
  [Verify after reboot](#3-verify-after-reboot)).
