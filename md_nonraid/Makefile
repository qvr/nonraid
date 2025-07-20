PWD := $(shell pwd)
KVERSION := $(shell uname -r)
HEADERS := /lib/modules/$(KVERSION)/build/
md-nonraid-m += md_unraid.o unraid.o
obj-m += md-nonraid.o

modules:
	make -C $(HEADERS) M=$(PWD) modules

clean:
	make -C $(HEADERS) M=$(PWD) clean
