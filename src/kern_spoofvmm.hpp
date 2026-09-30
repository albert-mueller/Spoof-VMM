//
//  kern_spoofvmm.hpp
//  SpoofVMM v4.9 - board-id functionality REMOVED ENTIRELY, along with the
//  now-dead WiFi node-restore fix that depended on it.
//
//  On-hardware testing (see T2-Tahoe-FINDINGS.md) proved board-id spoofing
//  was never necessary for the Tahoe compatibility gate - the gate clears on
//  bridge-model alone. Board-id spoofing was pure WiFi-breaking collateral
//  damage with no benefit. This version stops touching board-id at all.
//

#ifndef kern_spoofvmm_hpp
#define kern_spoofvmm_hpp

#include <Headers/kern_patcher.hpp>
#include <sys/sysctl.h>

// The class must NOT be named SpoofVMM: Lilu's plugin_start.hpp already declares
// an IOService subclass named after PRODUCT_NAME (SpoofVMM).
class VMMSpoof {
public:
	void init();

private:
	static void onPatcherLoad(void *user, KernelPatcher &patcher);
	void processKernel(KernelPatcher &patcher);

	// --- per-process VMM spoof ---
	static struct sysctl_oid *findChild(struct sysctl_oid_list *list, const char *name);
	static struct sysctl_oid *findVmmPresentOid(KernelPatcher &patcher);
	static bool shouldSpoofFor(const char *procname);
	static int sysctlVmmPresent(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);

	// --- IODeviceTree bridge-model swap (the confirmed gate-clear mechanism) ---
	// swap to a valid supported-Mac value (spoofvmm-bridge=, on by default, off
	// with -spoofvmmnoswap); diagnostic delete opt-in via -spoofvmmdelete.
	void spoofIdentity();
};

#endif /* kern_spoofvmm_hpp */
