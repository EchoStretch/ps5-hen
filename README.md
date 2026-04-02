# ps5-hen

### Homebrew Enabler for the PlayStation 5.
Defeats the Hypervisor on firmware <= 4.51 and enables supports for homebrew and ps4 fpkg's.

## Download

[![Download](https://img.shields.io/badge/Download-ps5--hen.elf-blue?style=for-the-badge)](https://github.com/cragson/ps5-hen/releases/download/dev-latest/ps5-hen.elf)

## Credits
- [fail0verflow](https://github.com/fail0verflow)
- [flat_z](https://github.com/flatz)
- [c0w](https://github.com/c0w-ar)
- [EchoStretch](https://github.com/EchoStretch)
- [idlesauce](https://github.com/idlesauce)
- [Z80](https://github.com/0x90h)
- [John Törnblom](https://github.com/john-tornblom)
- [Specter](https://gist.github.com/Cryptogenic)
- zeco
- [ChendoChap](https://github.com/ChendoChap)
- everyone who helped testing and debugging

Thanks to every single one of you - without your help it wouldn't be possible.

## How It Works

| Stage | Name | Description |
|-------|------|-------------|
| 0 | Discovery | Detect firmware, locate kernel base, map HV structures |
| 1 | TMR Relaxation | Patch IOMMU for unrestricted memory access |
| 2 | VMCB Discovery | Locate Virtual Machine Control Blocks |
| 3 | VMCB Patching | Disable HV intercepts and nested paging |
| 3b | XOTEXT Removal | Remove execute-only page protections |
| 4 | Verification | Confirm successful HV bypass |
| 5 | Kernel Patching | Apply firmware-specific kernel patches |
| 6 | Kexec Install | Install kernel execution primitive |
| 7 | HEN Payload | Load HEN kernel module for homebrew/ps4 fpkg support |

## Firmware Support

### 1.xx - `1.00` `1.01` `1.02` `1.05` `1.10` `1.11` `1.12` `1.13` `1.14`

> **Note:** HEN may crash on 1.xx firmwares after starting a ps4 fpkg.

### 2.xx - `2.00` `2.20` `2.25` `2.26` `2.30` `2.50` `2.70`

> **Note:** HEN may crash on 2.xx firmwares after starting a ps4 fpkg.

### 3.xx - `3.00` `3.10` `3.20` `3.21`

### 4.xx - `4.00` `4.02` `4.03` `4.50` `4.51`

## Building

Requires the PS5 Payload SDK. 
Set `PS5_PAYLOAD_SDK` to your SDK path.

```bash
make          # build everything
cd hen && make  # build HEN module only
```

## Deploying

```bash
make test     # send payload to PS5
make debug    # deploy with GDB support
```
