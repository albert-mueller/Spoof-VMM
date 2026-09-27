# Reasoning, and what's still uncertain

## Your evidence changed the analysis
No VMM patch -> APFS works. VMM patch -> APFS breaks. That is a clean controlled
test showing the VMM=1 value is the trigger, not a bystander. Earlier I assumed
the T2/APFS failure was independent of the VMM value; that was wrong.

## Mechanism this build assumes
The hooked sysctl is read by USERSPACE, not by apfs.kext. So a global 1 most
likely breaks disk access because a userspace disk daemon (storagekitd /
diskarbitrationd) reads 1, concludes "VM", and uses a VM disk path that fails
against a real T2-backed encrypted SSD. The installer needs 1 only for its
eligibility check. Hence: spoof that one process, tell the disk daemons the
truth.

## What's genuinely uncertain
- The exact process name doing the Tahoe eligibility check in the full installer
  environment. That's why this build LOGS every caller and ships a conservative
  target list. Read the log, then narrow the list.
- Whether the eligibility check and disk access are cleanly separable by process
  at all. If the SAME process both checks eligibility and drives the disk, per
  process scoping can't split them, and this approach won't fully work either.
  The log will show this too (same pid doing both).

## Honest ceiling
If it turns out the failing disk work happens in the KERNEL rather than a
userspace daemon, then this sysctl hook was never what apfs reads, and neither
targeting nor timing changes apfs's own view - the breakage would be coming from
somewhere else entirely, and the log will show no disk daemon among the callers.
In that case the block is back to the T2 layer, which no kext reaches.

Bring me the `svmm` caller log from a Debug boot and we can settle which of these
is actually happening.
