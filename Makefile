obj-m += snd-usb-audiobox96.o
snd-usb-audiobox96-y := audiobox96.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
