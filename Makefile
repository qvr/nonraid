obj-y := md_nonraid/ raid6/
PWD := $(shell pwd)
KVERSION := $(shell uname -r)
HEADERS := /lib/modules/$(KVERSION)/build/

modules:
	make -C $(HEADERS) M=$(PWD) modules

clean:
	make -C $(HEADERS) M=$(PWD) clean

package: all
	dpkg-buildpackage -b -rfakeroot -us -uc
