# DossierOS

DossierOS is a hobby operating system project written primarily in C, built using the Limine bootloader. The project aims to develop a custom operating system from the kernel upward, with a focus on low-level systems programming, kernel development, and creating a graphical user interface.

## Getting Started

### Dependencies

DossierOS is developed in a UNIX-like environment. Linux is recommended for building the kernel and generating bootable images.

The project requires the following tools:

- **GNU Make** — runs the build system.
- **Git** — retrieves kernel dependencies.
- **Clang/LLVM or a compatible C toolchain** — compiles the kernel.
- **curl** — downloads the required Limine release and EDK2 OVMF firmware images.
- **A host C compiler** — builds the Limine host utility.
- **xorriso** — generates bootable ISO images.
- **sgdisk** — prepares partition tables for HDD images.
- **mtools** — manages FAT filesystem images.
- **QEMU** — runs and tests the operating system in a virtual machine.

The `nasm` assembler is only required if the project uses `.asm` files. Assembly files with the `.S` extension are compiled using the selected C toolchain.

On Ubuntu or WSL Ubuntu, the required packages can be installed with:

```bash
sudo apt update
sudo apt install build-essential git curl xorriso gdisk mtools qemu-system-x86 clang lld nasm
```

### Toolchain Configuration

DossierOS supports configurable compiler toolchains through the Makefile variables `TOOLCHAIN` and `TOOLCHAIN_PREFIX`.

To build using Clang/LLVM:

```bash
make TOOLCHAIN=llvm
```

To use an x86_64 cross-compilation toolchain:

```bash
make TOOLCHAIN_PREFIX=x86_64-elf-
```

The kernel is linked through the compiler driver. GCC uses its configured default linker, while Clang uses LLD. A different linker can be selected through the `LDFLAGS` variable using the appropriate `-fuse-ld=` option.

Link-time optimization can be enabled through `CFLAGS`, for example:

```bash
make CFLAGS='-g -O2 -pipe -flto'
```

### Supported Architectures

The `ARCH` Makefile variable specifies the target architecture for the kernel and bootable image.

The default architecture is `x86_64`. Other architectures supported by the underlying template include:

- `aarch64`
- `loongarch64`
- `riscv64`

Actual support depends on the project's current configuration and the availability of a compatible toolchain.

### Building DossierOS

Run the following commands from the project root directory.

**Build a bootable ISO image:**

```bash
make all
```

This compiles the kernel in the `kernel/` directory and generates a bootable ISO image.

**Build a raw HDD or USB image:**

```bash
make all-hdd
```

This compiles the kernel and generates a raw disk image suitable for use with a virtual machine or for flashing to a supported storage device.

**Build and run DossierOS in QEMU:**

```bash
make run
```

This builds the kernel and ISO image, then launches the operating system in QEMU.

**Build and run the HDD image:**

```bash
make run-hdd
```

This builds the raw disk image and launches it in QEMU.

For `x86_64`, the `run-bios` and `run-hdd-bios` targets use the default SeaBIOS firmware instead of OVMF.

### Development

DossierOS is under active development. Build instructions and supported features may change as the kernel and system components evolve.

The project is intended for experimentation, learning, and development in operating system architecture and low-level programming.