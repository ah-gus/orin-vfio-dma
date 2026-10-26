/*
 * vfio_dev.c - hold a PCIe endpoint open through VFIO on Jetson Orin AGX.
 *
 *   1. enables bus mastering (and memory decoding) in the command register
 *   2. enables MSI and reports interrupts delivered through eventfds
 *   3. mmaps BAR2 (control plane registers) into this process
 *   4. allocates a physically contiguous hugetlb DMA buffer and maps it
 *      into the device's IOMMU domain at a fixed IOVA
 *   5. waits for SIGINT/SIGTERM while the device is driven externally
 *      (e.g. devmem on the BAR2 physical address)
 *
 * SIGUSR1 hexdumps the start of the DMA buffer (devmem cannot read RAM
 * on this kernel because CONFIG_STRICT_DEVMEM=y). With -f FILE the buffer
 * lives in a hugetlbfs file that other processes (e.g. dmamem) can mmap.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <linux/mman.h>
#include <linux/pci_regs.h>
#include <linux/vfio.h>

#define SZ_2M  (2ULL << 20)
#define SZ_32M (32ULL << 20)
#define SZ_1G  (1ULL << 30)
#define MAX_VECTORS 32

#define die(...) do { fprintf(stderr, "[-] " __VA_ARGS__); fputc('\n', stderr); exit(EXIT_FAILURE); } while (0)
#define die_errno(msg) die("%s: %s", msg, strerror(errno))

static struct {
	const char *bdf;
	const char *dma_file;	/* NULL = anonymous hugetlb mapping */
	uint64_t dma_size;
	uint64_t iova;		/* 0 = choose automatically */
	unsigned int nvec;
	int reset;
} opt = { .dma_size = SZ_2M, .nvec = 1 };

static int container_fd = -1, group_fd = -1, device_fd = -1;
static uint64_t cfg_off;	/* offset of the config space region in device_fd */

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static uint64_t parse_size(const char *s)
{
	char *end;
	uint64_t v = strtoull(s, &end, 0);

	switch (*end) {
	case 'G': case 'g': v <<= 10; /* fall through */
	case 'M': case 'm': v <<= 10; /* fall through */
	case 'K': case 'k': v <<= 10; end++; break;
	}
	if (*end || !v)
		die("bad size '%s'", s);
	return v;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [options] <BDF>   e.g. %s 0005:01:00.0\n"
		"  -s SIZE   DMA buffer size (default 2M, up to 1G; K/M/G suffixes)\n"
		"  -f FILE   back the DMA buffer with FILE on a hugetlbfs mount, so other\n"
		"            processes can mmap it (page size comes from the mount)\n"
		"  -i IOVA   device-visible DMA address (default: lowest valid, size aligned)\n"
		"  -n NVEC   number of MSI vectors to enable (default 1)\n"
		"  -R        issue VFIO_DEVICE_RESET before setup\n", prog, prog);
	exit(EXIT_FAILURE);
}

/* ---- config space through the VFIO config region ---- */

static void cfg_read(uint64_t off, void *buf, size_t len)
{
	if (pread(device_fd, buf, len, cfg_off + off) != (ssize_t)len)
		die_errno("config read");
}

static void cfg_write(uint64_t off, const void *buf, size_t len)
{
	if (pwrite(device_fd, buf, len, cfg_off + off) != (ssize_t)len)
		die_errno("config write");
}

static uint16_t cfg_read16(uint64_t off) { uint16_t v; cfg_read(off, &v, 2); return v; }
static void cfg_write16(uint64_t off, uint16_t v) { cfg_write(off, &v, 2); }

static unsigned int find_cap(uint8_t id)
{
	uint8_t pos, cap[2];
	int ttl = 48;

	if (!(cfg_read16(PCI_STATUS) & PCI_STATUS_CAP_LIST))
		return 0;
	cfg_read(PCI_CAPABILITY_LIST, &pos, 1);
	while (pos >= 0x40 && ttl--) {
		cfg_read(pos & ~3, cap, 2);
		if (cap[0] == id)
			return pos & ~3;
		pos = cap[1];
	}
	return 0;
}

/* ---- VFIO container / group / device ---- */

static void open_vfio(void)
{
	char path[PATH_MAX], link[PATH_MAX];
	struct vfio_group_status gs = { .argsz = sizeof(gs) };
	ssize_t n;
	char *grp;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", opt.bdf);
	n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		die("%s: %s (wrong BDF, or device not behind the SMMU?)", path, strerror(errno));
	link[n] = '\0';
	grp = strrchr(link, '/') + 1;

	container_fd = open("/dev/vfio/vfio", O_RDWR);
	if (container_fd < 0)
		die_errno("open /dev/vfio/vfio");
	if (ioctl(container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION)
		die("unexpected VFIO API version");

	snprintf(path, sizeof(path), "/dev/vfio/%s", grp);
	group_fd = open(path, O_RDWR);
	if (group_fd < 0)
		die("open %s: %s (is the device bound to vfio-pci? running as root?)",
		    path, strerror(errno));
	if (ioctl(group_fd, VFIO_GROUP_GET_STATUS, &gs) < 0)
		die_errno("VFIO_GROUP_GET_STATUS");
	if (!(gs.flags & VFIO_GROUP_FLAGS_VIABLE))
		die("IOMMU group %s not viable: every device in "
		    "/sys/kernel/iommu_groups/%s/devices must be bound to vfio-pci (or pcieport)",
		    grp, grp);

	if (ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd) < 0)
		die_errno("VFIO_GROUP_SET_CONTAINER");

	int type = ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU) > 0 ?
		   VFIO_TYPE1v2_IOMMU : VFIO_TYPE1_IOMMU;
	if (ioctl(container_fd, VFIO_SET_IOMMU, type) < 0) {
		if (errno == EPERM)
			die("VFIO_SET_IOMMU: EPERM - Orin has no MSI remapping; run:\n"
			    "    echo Y > /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts");
		die_errno("VFIO_SET_IOMMU");
	}

	device_fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, opt.bdf);
	if (device_fd < 0)
		die_errno("VFIO_GROUP_GET_DEVICE_FD");

	struct vfio_device_info di = { .argsz = sizeof(di) };
	if (ioctl(device_fd, VFIO_DEVICE_GET_INFO, &di) < 0)
		die_errno("VFIO_DEVICE_GET_INFO");
	if (!(di.flags & VFIO_DEVICE_FLAGS_PCI))
		die("%s is not a PCI device", opt.bdf);

	struct vfio_region_info ri = { .argsz = sizeof(ri), .index = VFIO_PCI_CONFIG_REGION_INDEX };
	if (ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0)
		die_errno("get config region info");
	cfg_off = ri.offset;

	printf("[+] %s opened via IOMMU group %s (%s), %u regions, %u irq types\n",
	       opt.bdf, grp, type == VFIO_TYPE1v2_IOMMU ? "type1v2" : "type1",
	       di.num_regions, di.num_irqs);

	if (opt.reset) {
		if (!(di.flags & VFIO_DEVICE_FLAGS_RESET))
			die("device does not support reset");
		if (ioctl(device_fd, VFIO_DEVICE_RESET) < 0)
			die_errno("VFIO_DEVICE_RESET");
		printf("[+] device reset\n");
	}
}

/* ---- 1. bus mastering ---- */

static void set_bus_master(int on)
{
	uint16_t cmd = cfg_read16(PCI_COMMAND);

	if (on)
		cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
	else
		cmd &= ~PCI_COMMAND_MASTER;
	cfg_write16(PCI_COMMAND, cmd);
	cmd = cfg_read16(PCI_COMMAND);
	if (on && !(cmd & PCI_COMMAND_MASTER))
		die("bus master bit did not stick (cmd=0x%04x)", cmd);
	if (on)
		printf("[+] bus mastering enabled (PCI_COMMAND=0x%04x)\n", cmd);
}

/* ---- 2. MSI ---- */

static int set_msi(const int *fds, unsigned int count)
{
	size_t sz = sizeof(struct vfio_irq_set) + count * sizeof(int);
	struct vfio_irq_set *s = calloc(1, sz);
	int ret;

	s->argsz = sz;
	s->index = VFIO_PCI_MSI_IRQ_INDEX;
	s->start = 0;
	s->count = count;
	s->flags = VFIO_IRQ_SET_ACTION_TRIGGER |
		   (count ? VFIO_IRQ_SET_DATA_EVENTFD : VFIO_IRQ_SET_DATA_NONE);
	memcpy(s->data, fds, count * sizeof(int));
	ret = ioctl(device_fd, VFIO_DEVICE_SET_IRQS, s);
	free(s);
	return ret;
}

static void enable_msi(int *efd)
{
	struct vfio_irq_info ii = { .argsz = sizeof(ii), .index = VFIO_PCI_MSI_IRQ_INDEX };
	unsigned int cap, i;

	if (ioctl(device_fd, VFIO_DEVICE_GET_IRQ_INFO, &ii) < 0)
		die_errno("VFIO_DEVICE_GET_IRQ_INFO(MSI)");
	if (ii.count == 0)
		die("device has no MSI capability");
	if (opt.nvec > ii.count)
		die("requested %u MSI vectors, device supports %u", opt.nvec, ii.count);

	for (i = 0; i < opt.nvec; i++) {
		efd[i] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (efd[i] < 0)
			die_errno("eventfd");
	}
	if (set_msi(efd, opt.nvec) < 0)
		die_errno("VFIO_DEVICE_SET_IRQS(MSI)");

	/* report what the device's (virtualized) MSI capability now says */
	cap = find_cap(PCI_CAP_ID_MSI);
	if (cap) {
		uint16_t ctl = cfg_read16(cap + PCI_MSI_FLAGS);
		printf("[+] MSI enabled: %u/%u vectors, MSI ctrl=0x%04x (enable=%d, %s-bit)\n",
		       opt.nvec, ii.count, ctl, !!(ctl & PCI_MSI_FLAGS_ENABLE),
		       ctl & PCI_MSI_FLAGS_64BIT ? "64" : "32");
	} else {
		printf("[+] MSI enabled: %u/%u vectors\n", opt.nvec, ii.count);
	}
}

/* ---- 3. BAR2 ---- */

static uint64_t bar_phys(int bar)
{
	char path[PATH_MAX];
	unsigned long long start = 0, end, flags;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", opt.bdf);
	f = fopen(path, "r");
	if (!f)
		return 0;
	for (int i = 0; i <= bar; i++)
		if (fscanf(f, "%llx %llx %llx", &start, &end, &flags) != 3)
			start = 0;
	fclose(f);
	return start;
}

static volatile uint32_t *map_bar2(uint64_t *size)
{
	struct vfio_region_info ri = { .argsz = sizeof(ri), .index = VFIO_PCI_BAR2_REGION_INDEX };
	void *p;

	if (ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0)
		die_errno("get BAR2 region info");
	if (ri.size == 0)
		die("BAR2 is not implemented (64-bit BAR0 would occupy BAR0/1; check lspci -vv)");
	if (!(ri.flags & VFIO_REGION_INFO_FLAG_MMAP))
		die("BAR2 is not mmap-able through VFIO (flags=0x%x)", ri.flags);

	p = mmap(NULL, ri.size, PROT_READ | PROT_WRITE, MAP_SHARED, device_fd, ri.offset);
	if (p == MAP_FAILED)
		die_errno("mmap BAR2");
	*size = ri.size;
	printf("[+] BAR2 mapped: %" PRIu64 " KiB at %p, phys 0x%" PRIx64 " (use this with devmem)\n",
	       (uint64_t)ri.size >> 10, p, bar_phys(2));
	return p;
}

/* ---- 4. hugetlb DMA buffer ---- */

static uint64_t virt_to_phys(void *va)
{
	uint64_t ent;
	long psz = sysconf(_SC_PAGESIZE);
	int fd = open("/proc/self/pagemap", O_RDONLY);

	if (fd < 0)
		return 0;
	if (pread(fd, &ent, 8, ((uintptr_t)va / psz) * 8) != 8)
		ent = 0;
	close(fd);
	if (!(ent & (1ULL << 63)) || !(ent & ((1ULL << 55) - 1)))
		return 0;	/* not present, or PFN hidden (not root) */
	return (ent & ((1ULL << 55) - 1)) * psz + ((uintptr_t)va % psz);
}

/* Pick the smallest hugepage that holds the whole buffer, so it is one
 * physically contiguous page. */
static uint64_t pick_hugepage(uint64_t size)
{
	static const uint64_t sizes[] = { SZ_2M, SZ_32M, SZ_1G };

	for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		if (size <= sizes[i])
			return sizes[i];
	die("DMA size above 1G cannot be one contiguous hugepage");
}

static void *alloc_dma(uint64_t *hpsz)
{
	uint64_t hp;
	void *p;

	if (opt.dma_file) {
		/* a hugetlbfs file, so other processes can mmap the same pages */
		struct statfs sfs;
		int fd = open(opt.dma_file, O_RDWR | O_CREAT | O_CLOEXEC, 0660);

		if (fd < 0)
			die("open %s: %s", opt.dma_file, strerror(errno));
		if (fstatfs(fd, &sfs) < 0)
			die_errno("fstatfs");
		if (sfs.f_type != HUGETLBFS_MAGIC)
			die("%s is not on a hugetlbfs mount", opt.dma_file);
		hp = sfs.f_bsize;	/* set by the mount's pagesize= option */
		if (opt.dma_size > hp)
			die("%s is on a %" PRIu64 "M hugetlbfs mount; a %" PRIu64 "M buffer needs "
			    "a mount with a larger pagesize= (32M or 1G)",
			    opt.dma_file, hp >> 20, opt.dma_size >> 20);
		if (ftruncate(fd, hp) < 0)
			die_errno("ftruncate");
		p = mmap(NULL, hp, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
		close(fd);
	} else {
		hp = pick_hugepage(opt.dma_size);
		p = mmap(NULL, hp, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE |
			 (__builtin_ctzll(hp) << MAP_HUGE_SHIFT), -1, 0);
	}
	if (p == MAP_FAILED)
		die("mmap %" PRIu64 "M hugepage failed: %s\n    reserve one with: "
		    "echo 4 > /sys/kernel/mm/hugepages/hugepages-%" PRIu64 "kB/nr_hugepages",
		    hp >> 20, strerror(errno), hp >> 10);
	/* No memset: the kernel zeroes hugetlb pages when they are first
	 * allocated, and an existing file keeps what another process put there. */
	opt.dma_size = hp;	/* hugetlb mappings are whole pages anyway */
	*hpsz = hp;
	return p;
}

static uint64_t choose_iova(uint64_t size)
{
	struct vfio_iommu_type1_info *info;
	struct vfio_info_cap_header *hdr;
	uint64_t iova = 0;
	uint32_t argsz = sizeof(*info);

	/* first call learns the size needed for the capability chain */
	for (int pass = 0; pass < 2; pass++) {
		info = calloc(1, argsz);
		info->argsz = argsz;
		if (ioctl(container_fd, VFIO_IOMMU_GET_INFO, info) < 0)
			die_errno("VFIO_IOMMU_GET_INFO");
		if (info->argsz <= argsz)
			break;
		argsz = info->argsz;
		free(info);
	}

	if (!(info->flags & VFIO_IOMMU_INFO_CAPS) || !info->cap_offset) {
		free(info);
		return opt.iova ? opt.iova : size;	/* no range info; trust the caller */
	}

	for (hdr = (void *)((char *)info + info->cap_offset);;
	     hdr = (void *)((char *)info + hdr->next)) {
		if (hdr->id == VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE) {
			struct vfio_iommu_type1_info_cap_iova_range *r = (void *)hdr;

			for (uint32_t i = 0; i < r->nr_iovas; i++) {
				uint64_t lo = r->iova_ranges[i].start, hi = r->iova_ranges[i].end;

				if (opt.iova) {
					if (opt.iova >= lo && opt.iova + size - 1 <= hi)
						iova = opt.iova;
				} else if (!iova) {
					/* never hand out IOVA 0: devices often treat it as NULL */
					uint64_t c = align_up(lo ? lo : size, size);
					if (c >= lo && c + size - 1 <= hi)
						iova = c;
				}
			}
		}
		if (!hdr->next)
			break;
	}
	free(info);
	if (!iova && opt.iova)
		die("IOVA 0x%" PRIx64 " is outside the valid IOVA ranges", opt.iova);
	if (!iova)
		die("no valid IOVA range fits %" PRIu64 " bytes", size);
	return iova;
}

static void map_dma(void *va, uint64_t iova, uint64_t size)
{
	struct vfio_iommu_type1_dma_map m = {
		.argsz = sizeof(m),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr = (uintptr_t)va,
		.iova = iova,
		.size = size,
	};

	if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &m) < 0)
		die_errno("VFIO_IOMMU_MAP_DMA");
}

static void unmap_dma(uint64_t iova, uint64_t size)
{
	struct vfio_iommu_type1_dma_unmap u = { .argsz = sizeof(u), .iova = iova, .size = size };

	if (ioctl(container_fd, VFIO_IOMMU_UNMAP_DMA, &u) < 0)
		perror("[-] VFIO_IOMMU_UNMAP_DMA");
}

static void hexdump(const void *buf, size_t len)
{
	const uint32_t *w = buf;

	for (size_t i = 0; i < len / 4; i += 4)
		printf("  %04zx: %08x %08x %08x %08x\n", i * 4, w[i], w[i + 1], w[i + 2], w[i + 3]);
}

/* ---- 5. wait ---- */

static void wait_loop(const int *efd, void *dma)
{
	uint64_t total[MAX_VECTORS] = { 0 };
	struct epoll_event ev, events[MAX_VECTORS + 1];
	sigset_t mask;
	int ep, sfd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGUSR1);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	sfd = signalfd(-1, &mask, SFD_CLOEXEC);
	ep = epoll_create1(EPOLL_CLOEXEC);
	if (sfd < 0 || ep < 0)
		die_errno("signalfd/epoll");

	ev.events = EPOLLIN;
	ev.data.u32 = UINT32_MAX;
	epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
	for (unsigned int i = 0; i < opt.nvec; i++) {
		ev.data.u32 = i;
		epoll_ctl(ep, EPOLL_CTL_ADD, efd[i], &ev);
	}

	printf("\n[*] ready (pid %d). Drive the device with devmem; "
	       "Ctrl+C / SIGTERM to exit, SIGUSR1 to dump the DMA buffer.\n\n", getpid());
	fflush(stdout);

	for (;;) {
		int n = epoll_wait(ep, events, MAX_VECTORS + 1, -1);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			die_errno("epoll_wait");
		}
		for (int k = 0; k < n; k++) {
			uint32_t id = events[k].data.u32;

			if (id == UINT32_MAX) {
				struct signalfd_siginfo si;

				if (read(sfd, &si, sizeof(si)) != sizeof(si))
					continue;
				if (si.ssi_signo == SIGUSR1) {
					printf("[*] DMA buffer, first 256 bytes:\n");
					hexdump(dma, 256);
					fflush(stdout);
					continue;
				}
				printf("\n[*] %s received, shutting down\n", strsignal(si.ssi_signo));
				close(sfd);
				close(ep);
				return;
			}

			uint64_t cnt;
			if (read(efd[id], &cnt, sizeof(cnt)) == sizeof(cnt)) {
				total[id] += cnt;
				printf("[irq] MSI vector %u: +%" PRIu64 " (total %" PRIu64 ")\n",
				       id, cnt, total[id]);
				fflush(stdout);
			}
		}
	}
}

int main(int argc, char **argv)
{
	int efd[MAX_VECTORS], c;
	uint64_t bar2_size, hpsz, iova, pa;
	volatile uint32_t *bar2;
	void *dma;

	while ((c = getopt(argc, argv, "s:f:i:n:Rh")) != -1) {
		switch (c) {
		case 's': opt.dma_size = parse_size(optarg); break;
		case 'f': opt.dma_file = optarg; break;
		case 'i': opt.iova = strtoull(optarg, NULL, 0); break;
		case 'n': opt.nvec = strtoul(optarg, NULL, 0); break;
		case 'R': opt.reset = 1; break;
		default: usage(argv[0]);
		}
	}
	if (optind != argc - 1)
		usage(argv[0]);
	opt.bdf = argv[optind];
	if (opt.nvec < 1 || opt.nvec > MAX_VECTORS)
		die("-n must be 1..%d", MAX_VECTORS);

	setvbuf(stdout, NULL, _IOLBF, 0);
	open_vfio();

	bar2 = map_bar2(&bar2_size);
	printf("    BAR2[0x0] = 0x%08x\n", bar2[0]);

	dma = alloc_dma(&hpsz);
	iova = choose_iova(hpsz);
	if (opt.iova && (opt.iova & (hpsz - 1)))
		printf("[!] IOVA 0x%" PRIx64 " is not %" PRIu64 "M aligned; the SMMU will "
		       "use smaller pages\n", opt.iova, hpsz >> 20);
	map_dma(dma, iova, hpsz);
	pa = virt_to_phys(dma);
	printf("[+] DMA buffer: %" PRIu64 " MiB in one %" PRIu64 "M hugepage at %p\n",
	       hpsz >> 20, hpsz >> 20, dma);
	printf("    IOVA (program this into the device) : 0x%" PRIx64 " - 0x%" PRIx64 "\n",
	       iova, iova + hpsz - 1);
	if (pa)
		printf("    CPU physical (informational only)  : 0x%" PRIx64 "\n", pa);
	if (opt.dma_file)
		printf("    shared file (mmap MAP_SHARED)      : %s\n", opt.dma_file);

	/* MSI and bus mastering last: the IOMMU mapping exists before the
	 * device is allowed to issue any transaction. */
	enable_msi(efd);
	set_bus_master(1);

	wait_loop(efd, dma);

	set_bus_master(0);
	set_msi(NULL, 0);
	for (unsigned int i = 0; i < opt.nvec; i++)
		close(efd[i]);
	unmap_dma(iova, hpsz);
	munmap(dma, hpsz);
	munmap((void *)bar2, bar2_size);
	close(device_fd);
	close(group_fd);
	close(container_fd);
	printf("[+] released\n");
	if (opt.dma_file)
		printf("[*] %s kept; rm it to return the hugepage to the pool\n", opt.dma_file);
	return 0;
}
