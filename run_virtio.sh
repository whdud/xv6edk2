#!/bin/bash
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
	-drive if=ide,file=fat:rw:image,index=0,media=disk \
	-device virtio-blk-pci,drive=hd1,disable-modern=on \
	-drive id=hd1,file=xv6/fs.img,format=raw,if=none \
	-m 2048 -smp 4 \
	-serial mon:stdio \
	-vga std
