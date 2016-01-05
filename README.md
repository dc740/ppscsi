# ppscsi
ppSCSI 0.92 modifications for newer kernels (>3.13)


# Steps to compile

make
sudo cp ppscsi.ko epst.ko /lib/modules/`uname -r`/kernel/drivers/parport
sudo depmod -a
sudo /bin/sh -c 'echo "SUBSYSTEM==\"scsi_generic\",ATTRS{type}==\"3\", SYMLINK=\"scanner%n\", MODE=\"0777\", GROUP=\"scanner\"" >> /etc/udev/rules.d/45-libsane.rules'
sudo service udev restart
sudo modprobe ppscsi
sudo modprobe epst

# Scan:
sudo sane-find-scanner
sudo scanimage -d hp:/dev/sg1 --mode Color > scanImage
