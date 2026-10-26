/*
 * dmamem.c - devmem-style access to the DMA buffer that vfio_dev shares
 * through a hugetlbfs file (vfio_dev -f FILE). devmem itself cannot reach
 * RAM on this kernel (CONFIG_STRICT_DEVMEM=y).
 *
 *   dmamem FILE OFFSET [WIDTH [VALUE]]    WIDTH = 8, 16, 32 (default) or 64
 *
 * Prints the value at OFFSET; with VALUE, writes it first and prints the
 * read-back.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
	unsigned int width = 32;
	int do_write = argc == 5, fd;
	uint64_t off, val = 0;
	struct stat st;
	volatile void *p;
	char *base;

	if (argc < 3 || argc > 5) {
		fprintf(stderr, "usage: %s FILE OFFSET [WIDTH [VALUE]]   WIDTH = 8, 16, 32 or 64\n",
			argv[0]);
		return 1;
	}
	off = strtoull(argv[2], NULL, 0);
	if (argc > 3)
		width = strtoul(argv[3], NULL, 0);
	if (do_write)
		val = strtoull(argv[4], NULL, 0);
	if (width != 8 && width != 16 && width != 32 && width != 64) {
		fprintf(stderr, "WIDTH must be 8, 16, 32 or 64\n");
		return 1;
	}
	if (off % (width / 8)) {
		fprintf(stderr, "OFFSET 0x%" PRIx64 " is not %u-bit aligned\n", off, width);
		return 1;
	}

	fd = open(argv[1], do_write ? O_RDWR : O_RDONLY);
	if (fd < 0 || fstat(fd, &st) < 0) {
		fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	if (off + width / 8 > (uint64_t)st.st_size) {
		fprintf(stderr, "OFFSET 0x%" PRIx64 " is past the end of the %lld-byte buffer\n",
			off, (long long)st.st_size);
		return 1;
	}
	base = mmap(NULL, st.st_size, PROT_READ | (do_write ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
	close(fd);
	if (base == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return 1;
	}
	p = base + off;

	switch (width) {
	case 8:
		if (do_write)
			*(volatile uint8_t *)p = val;
		val = *(volatile uint8_t *)p;
		break;
	case 16:
		if (do_write)
			*(volatile uint16_t *)p = val;
		val = *(volatile uint16_t *)p;
		break;
	case 32:
		if (do_write)
			*(volatile uint32_t *)p = val;
		val = *(volatile uint32_t *)p;
		break;
	case 64:
		if (do_write)
			*(volatile uint64_t *)p = val;
		val = *(volatile uint64_t *)p;
		break;
	}
	printf("0x%0*" PRIx64 "\n", (int)width / 4, val);
	munmap(base, st.st_size);
	return 0;
}
