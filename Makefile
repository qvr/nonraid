obj-y := md_nonraid/ raid6/
PWD := $(shell pwd)
KVERSION := $(shell uname -r)
HEADERS := /lib/modules/$(KVERSION)/build/

modules:
	make -C $(HEADERS) M=$(PWD) modules CONFIG_UBSAN=n

clean:
	make -C $(HEADERS) M=$(PWD) clean

package:
	dpkg-buildpackage -b -rfakeroot -us -uc
