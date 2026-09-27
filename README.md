# SpoofVMM (Targeted)

Process-scoped `kern.hv_vmm_present` spoof. Returns **1 only to selected
userspace processes** (the software-update / installer helpers) and the **real
value to everyone else at all times** - so disk daemons (`storagekitd`,
`diskarbitrationd`) never see 1 and APFS is left alone. Every caller of the
sysctl is logged so you can find the exact process that needs the spoof.

## Build (Mac with Xcode)

```sh
./bootstrap.sh
make                 # -> build/Release/SpoofVMM.kext
make CONFIG=Debug    # verbose caller logging (-spoofvmmdbg)
make zip
```

## Use it as a probe first

1. Build **Debug**, install, boot the Tahoe installer with `-v` and
   `-spoofvmmdbg`.
2. Read the log:
   ```sh
   log show --last boot --predicate 'eventMessage CONTAINS "svmm"' --style compact
   ```
   You'll see lines like:
   ```
   svmm caller pid=123 name=storagekitd -> passthrough (real value)
   svmm caller pid=456 name=osinstallersetup -> spoofing 1
   ```
3. Find which process reads the sysctl right when the eligibility check happens,
   and which disk daemons read it around formatting. Then edit
   `kSpoofTargets[]` in `src/kern_spoofvmm.cpp` to spoof only the former.

## Why targeted, not global or time-gated

- **Global 1** breaks disk access (a disk daemon believes it's in a VM).
- **Time gate (launchd)** still exposes disk daemons to 1 during install, since
  formatting happens long after launchd.
- **Per process** lets the eligibility check see 1 while disk daemons always see
  the truth - simultaneously, no timing to tune.

See NOTES.md for the full reasoning and honest caveats.
