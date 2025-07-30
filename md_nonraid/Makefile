PWD := $(shell pwd)
KVERSION := $(shell uname -r)
HEADERS := /lib/modules/$(KVERSION)/build/

K_MAJOR := $(shell echo $(KVERSION) | cut -f1 -d.)
K_MINOR := $(shell echo $(KVERSION) | cut -f2 -d.)
K_GT_6_8 := $(shell [ $(K_MAJOR) -gt 6 -o \( $(K_MAJOR) -eq 6 -a $(K_MINOR) -gt 8 \) ] && echo true)
K_GE_6_5 := $(shell [ $(K_MAJOR) -gt 6 -o \( $(K_MAJOR) -eq 6 -a $(K_MINOR) -ge 5 \) ] && echo true)

ifeq ($(K_GT_6_8),true)
md-nonraid-m += 6.12/md_unraid.o 6.12/unraid.o
else ifeq ($(K_GE_6_5),true)
md-nonraid-m += 6.6/md_unraid.o 6.6/unraid.o
else
md-nonraid-m += 6.1/md_unraid.o 6.1/unraid.o
endif
obj-m += md-nonraid.o

modules:
	make -C $(HEADERS) M=$(PWD) modules

clean:
	make -C $(HEADERS) M=$(PWD) clean
