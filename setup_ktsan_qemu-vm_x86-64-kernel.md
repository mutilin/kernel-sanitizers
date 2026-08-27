# Setup: QEMU vm, x86-64 kernel

These are the instructions on how to run the instrumented with KTSAN x86-64 kernel in a QEMU with Debian Bullseye in the QEMU instances.

In the instructions below, the `$VAR` notation (e.g. `$IMAGE`, `$KERNEL`, etc.) is used to denote paths to directories that are either created when executing the instructions (e.g. when unpacking GCC archive, a directory will be created), or that you have to create yourself before running the instructions. Substitute the values for those variables manually.


## Install Prerequisites

Command:
``` bash
sudo apt update
sudo apt install make gcc flex bison libncurses-dev libelf-dev libssl-dev
```

## Kernel

### Checkout Linux Kernel source

Command:
``` bash
git clone --branch ktsan-v7.1 git@github.com:mutilin/kernel-sanitizers.git $KERNEL
```

### Generate default configs

Command:
``` bash
cd $KERNEL
make defconfig
make kvm_guest.config
```

You can also specify a compiler by adding `LLVM=1` in each `make` (by default GCC).

### Enable required config options

``` bash
# Race condition detector
./scripts/config --enable KTSAN
./scripts/config --enable CONFIG_CONFIGFS_FS
./scripts/config --enable CONFIG_SECURITYFS

./scripts/config --enable KTSAN_DEBUG
```
Or

Edit `.config` file manually and enable them (or do that through `make menuconfig` if you prefer).

Since enabling these options results in more sub options being available, we need to regenerate config:

Command:
``` bash
make olddefconfig
```

### Build the Kernel

Command:
``` bash
make -j$(nproc)
```

Now you should have `vmlinux` (kernel binary) and `bzImage` (packed kernel image):

Command:
``` bash
ls $KERNEL/vmlinux
# sample output - $KERNEL/vmlinux
ls $KERNEL/arch/x86/boot/bzImage
# sample output - $KERNEL/arch/x86/boot/bzImage
```

## Image

### Install debootstrap

Command:
``` bash
sudo apt install debootstrap
```

### Create Debian Bullseye Linux image

Create a Debian Bullseye Linux image with the minimal set of required packages.

Command:
``` bash
mkdir $IMAGE
cd $IMAGE/
wget https://raw.githubusercontent.com/google/syzkaller/master/tools/create-image.sh -O create-image.sh
chmod +x create-image.sh
./create-image.sh
```

The result should be `$IMAGE/bullseye.img` disk image.

### OR Create Debian Linux image with a different version

To create a Debian image with a different version (e.g. bookworm, bullseye, trixie, sid), specify the `--distribution` option.

Command:
``` bash
./create-image.sh --distribution bookworm
```

## QEMU

### Install QEMU

Command:
``` bash
sudo apt install qemu-system-x86
```

### Verify

Make sure the kernel boots and `sshd` starts.

Command:
``` bash
qemu-system-x86_64 \
	-m 6G \
	-smp 2 \
	-kernel $KERNEL/arch/x86/boot/bzImage \
	-append "console=ttyS0 root=/dev/sda earlyprintk=serial net.ifnames=0" \
	-drive file=$IMAGE/bullseye.img,format=raw \
    -net user,host=10.0.2.10,hostfwd=tcp:127.0.0.1:10022-:22 \
    -net nic,model=e1000 \
    -enable-kvm -nographic -pidfile vm.pid \
    2>&1 | tee vm.log
```

After -m you put any RAM size you are willing to implement.

``` text
early console in setup code
early console in extract_kernel
input_data: 0x0000000005d9e276
input_len: 0x0000000001da5af3
output: 0x0000000001000000
output_len: 0x00000000058799f8
kernel_total_size: 0x0000000006b63000

Decompressing Linux... Parsing ELF... done.
Booting the kernel.
[    0.000000] Linux version 7.1.0-ktsan+ ...
[    0.000000] Command line: console=ttyS0 root=/dev/sda debug earlyprintk=serial
...
[ ok ] Starting enhanced syslogd: rsyslogd.
[ ok ] Starting periodic command scheduler: cron.
[ ok ] Starting OpenBSD Secure Shell server: sshd.
```

After that you should be able to ssh to QEMU instance in another terminal.

Command:
``` bash
ssh -i $IMAGE/bullseye.id_rsa -p 10022 -o "StrictHostKeyChecking no" root@localhost
```

To terminate the QEMU instance:

```
<Ctrl+A> then X
```

or

``` bash
kill $(cat vm.pid)
```
