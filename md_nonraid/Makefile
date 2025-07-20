PWD := $(shell pwd)
HEADERS := /lib/modules/$(shell uname -r)/build/
md-nonraid-m += md_unraid.o unraid.o
obj-m += md-nonraid.o

modules:
	make -C $(HEADERS) M=$(PWD) modules

clean:
	make -C $(HEADERS) M=$(PWD) clean
