# SpoofVMM v4.9.2

**macOS Tahoe Stage 2 Firmware & Root Hash Manifest Fix for dropped T2 Macs (2018–2019). Includes scoped per-process VMM spoofing, bridge-model swap, opt-in Stage 2 Board-ID swap, and in-kernel code validation bypass for `OSIUpdateFirmwareElement`.**

## What it does

1. **`kern.hv_vmm_present = 1`, per process** — `1` only to software-update / installer helpers (`softwareupdated`, `com.apple.Mobile`, etc.), real bare-metal value to everyone else so APFS disk crypto and Secure Enclave remain unaffected.
2. **bridge-model SWAP** — overwrite `bridge-model` on every IODeviceTree node that has it with a **valid supported-Mac value** (`J215AP`, override with `spoofvmm-bridge=`). **ON by default.** This clears the Stage 1 compatibility gate.
3. **board-id SWAP (Stage 2 Root Hash Manifest fix)** — overwrite `board-id` with a valid Tahoe-supported value (`Mac-5F9802EFE386AA28`, Albert's MacBookPro16,2). During Stage 2 prepare, MobileSoftwareUpdate (MSU) queries `board-id` to look for `boot/Firmware/<Board-ID>.dmg.root_hash` and mtree manifests. Tahoe dropped boards lack these manifests and fail with `NSPOSIXErrorDomain:2` / `com.apple.osinstall:-81`. **Opt-in for installation/Stage 2** (`-spoofvmmbid` / `-spoofvmmstage2`) so normal desktop boots leave Wi-Fi antenna calibration untouched.
4. **In-Kernel Stage 2 Firmware Bypass (`_cs_validate_page`)** — intercepts binary code pages for `OSInstaller.framework` and `osinstallersetupd` at the kernel validation boundary. Binary-patches the entry point of `-[OSIUpdateFirmwareElement runReturningError:]` with `b0 01 c3` (`mov al, 1; ret`) so BridgeOS firmware verification succeeds automatically without userland dylibs or SIP modifications.

## Boot arguments

| Arg | Effect | Default |
|-----|--------|---------|
| `-spoofvmmoff` | disable the plugin entirely | — |
| `-spoofvmmdbg` | debug logging (Debug build only) | — |
| `-spoofvmmbeta` | allow loading outside the Tahoe kernel range | — |
| `spoofvmm-bridge=XXXX` | bridge-model value written by the swap | `J215AP` |
| `-spoofvmmnoswap` | disable the bridge-model swap | swap on |
| `-spoofvmmdelete` | **diagnostic:** delete bridge-model instead of keeping the swap | off |
| `-spoofvmmbid` / `-spoofvmmstage2` | **Stage 2 fix:** enable Board-ID swap to supported Mac | off (on normal boot) |
| `spoofvmm-board=XXXX` | custom Board-ID value for swap | `Mac-5F9802EFE386AA28` |
| `-spoofvmmnobid` | explicitly disable Board-ID swap | disabled |
| `-spoofvmmnocs` | disable `_cs_validate_page` in-kernel installer binary patch | patch on |

## The key finding (fully confirmed on hardware, all parts)

- The compatibility gate keys off **bridge-model** only.
- A bridge-model that is **present and set to a supported-Mac value** passes
  the gate and boots fine.
- **Board-id spoofing is not needed for the gate** and was the sole cause of
  the WiFi connect failure across v4.1–v4.8. Simply not touching it fixes
  WiFi with no fix-up required.
- **This build does NO model spoofing.** Model identity is supplied by
  **OpenCore** (PlatformInfo/SMBIOS → MacBookPro16,4).
- **Deleting** bridge-model boots but does **not** pass the gate.

## Build (no Xcode required, on a real Mac)

```sh
./bootstrap.sh
make                    # release -> build/SpoofVMM.kext
make CONFIG=Debug       # debug build (enables -spoofvmmdbg logging)
```

Type log commands directly in Terminal rather than pasting — copying from a
rich-text source can turn `--` into a single em-dash and break flag parsing:

```sh
log show --last boot --predicate 'eventMessage CONTAINS "svmm"' --style compact
```

Verify:

```sh
ioreg -l -p IODeviceTree | grep -iE "board-id|bridge-model"
```

`board-id` should now show the machine's **real** value everywhere (since
it's never touched at all), while `bridge-model` shows the spoofed swap
value. WiFi should work normally.

Expected log, in order:
```
registered (v4.9: VMM + bridge-model swap only - board-id and the WiFi fix-up have been removed entirely, see notes), waiting for kernel patcher
bridge-model: swap=on value=J215AP (N) | delete=off (0)
route installed (per-process spoof, real value for others)
```

## Confidence level

**Everything in this build is confirmed on hardware** — the VMM spoof and
bridge-model swap (unchanged since v4.5), and now the removal of board-id,
validated by the `-spoofvmmnobid` test in v4.8. This is the first version
since v4.5 with no untested or known-ineffective code paths in it.
