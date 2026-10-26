CFLAGS ?= -O2 -g -Wall -Wextra
OVERLAY = tegra234-c5-pcie-vfio-overlay.dtbo

PROGS = vfio_dev dmamem

all: $(PROGS) $(OVERLAY)

$(PROGS): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

%.dtbo: %.dts
	dtc -@ -q -I dts -O dtb -o $@ $<

clean:
	rm -f $(PROGS) $(OVERLAY)

.PHONY: all clean
