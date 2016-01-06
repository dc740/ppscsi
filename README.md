# ppscsi
ppSCSI 0.92 modifications for newer kernels (>3.13)


# Steps to compile

```
make
sudo make install
sudo make load
```
You may want to add this rule to udev:
```
sudo /bin/sh -c 'echo "SUBSYSTEM==\"scsi_generic\",ATTRS{type}==\"3\", SYMLINK=\"scanner%n\", MODE=\"0777\", GROUP=\"scanner\"" >> /etc/udev/rules.d/45-libsane.rules'
sudo service udev restart
```

# Scan
```
sudo sane-find-scanner
scanimage -d hp:/dev/sg1 --mode Color > scanImage
```
OR
```
xsane
```
