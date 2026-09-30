# SpoofVMM v4.9

**Board-id functionality removed entirely — permanently, with no flag needed
to disable it. The dead WiFi node-restore fix (v4.6/v4.7) is deleted, not
just turned off. This is the leanest, simplest version of the kext to date.**

## Why

Confirmed on hardware, in order:

1. `ioreg -l -p IODeviceTree | grep -iE board-id` showed only **two**
   `board-id` properties exist anywhere in the entire IODeviceTree plane
   (root, `/efi/platform`) — the WiFi controller's own node has none of its
   own. This made v4.6 (hook on `AirPortBrcmNIC`) and v4.7 (hook on
   `IOSkywalkFamily`, testing a timing hypothesis) both structurally
   incapable of working, regardless of hook point.
2. A full `ioreg -l -p IODeviceTree | grep -B5 -A25 -i "pci14e4"` diff
   between a stock no-kext boot (WiFi working) and a SpoofVMM-active boot
   (WiFi broken) showed the WiFi PCI device's own properties are
   byte-for-byte identical in both states. Nothing local to the WiFi node
   changes — ruling out a WiFi-node-level cause entirely and pointing at the
   shared root-level `board-id` as the only remaining candidate.
3. **v4.8's `-spoofvmmnobid` flag, tested on hardware with board-id left
   completely untouched (confirmed via terminal — the real MacBookAir8,1
   board-id was present): the compatibility gate still cleared.** Board-id
   spoofing was never necessary for the gate. The original finding ("gate
   keys off bridge-model, not board-id") was correct all along — board-id
   spoofing was pure WiFi-breaking collateral damage with zero benefit.

So as of v4.9: board-id is never touched, period. No flag, no option, no
dormant code path — it's simply gone.

## What it does

1. **`kern.hv_vmm_present = 1`, per process** — `1` only to the
   software-update / installer helpers, real value to everyone else (disk
   daemons unaffected). Unchanged since v4.1.
2. **bridge-model SWAP** — overwrite `bridge-model` on every IODeviceTree
   node that has it with a **valid supported-Mac value** (`J215AP`, override
   with `spoofvmm-bridge=`). **ON by default.** This is the confirmed,
   sole gate-clearing operation.

That's it. No board-id, no model spoofing (OpenCore's SMBIOS/PlatformInfo
already supplies model identity), no WiFi fix-up needed because there's
nothing left to fix.

## Boot arguments

| Arg | Effect | Default |
|-----|--------|---------|
| `-spoofvmmoff` | disable the plugin entirely | — |
| `-spoofvmmdbg` | debug logging (Debug build only) | — |
| `-spoofvmmbeta` | allow loading outside the Tahoe kernel range (needed to test on Sonoma) | — |
| `spoofvmm-bridge=XXXX` | bridge-model value written by the swap | `J215AP` |
| `-spoofvmmnoswap` | disable the bridge-model swap | swap on |
| `-spoofvmmdelete` | **diagnostic:** delete bridge-model instead of keeping the swap | off |

All board-id-related and WiFi-fix-related flags from v4.1–v4.8
(`-spoofvmmnobid`, `spoofvmm-bid=`, `-spoofvmmwifi`/`-spoofvmmnowifi`,
`spoofvmm-realbid=`) are **gone** — they no longer exist and will be
silently ignored as unrecognized boot-args if left over in your
config.plist from an earlier version. Remove them for clarity, though
leaving them won't break anything.

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
