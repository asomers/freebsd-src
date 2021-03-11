Demonstrates the use of KTLS with zfs recv

Prerequisites
=============

```
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365
pkg install socat
```

Startup
=======

```
sudo sysctl kern.ipc.mb_use_ext_pgs=1
sudo sysctl kern.ipc.tls.enable=1
sudo sysctl kern.ipc.tls.cbc_enable=1
sudo kldload ktls_ocf
```

Usage
=====

```
make

truncate -s 1g /tmp/zfs.img
sudo zpool create testpool /tmp/zfs.img
sudo zfs create testpool/src
sudo dd if=/dev/zero of=/testpool/src/zerofile bs=1m count=128
sudo zfs snapshot testpool/src@1

sudo ./zfs-sslrecv  -p 4443 -s testpool/dst
sudo zfs send testpool/src@1 | socat - SSL:localhost:4443,verify=0
```
