AUTOMAKE_OPTIONS = -Wno-portability
SHELL=/bin/bash
ARCH=$(shell uname -m)
ARCHPATH=arch/$(ARCH)

all-local: initrd

if WITH_KMODULES
export INCLUDE_KMODULES=1
endif

initrd-local:
	bash ./make-initrd.sh

cbfs-local:
	bash ./make-initrd.sh cbfs

kernel-local:
	bash ./make-kernel.sh
