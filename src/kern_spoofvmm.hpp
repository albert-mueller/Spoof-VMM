//
//  kern_spoofvmm.hpp
//  SpoofVMM (Targeted) - reports kern.hv_vmm_present = 1 ONLY to selected
//  userspace processes, logs every caller, and returns the real value to all
//  others (notably storagekitd / diskarbitrationd, so APFS is unaffected).
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

	static struct sysctl_oid *findChild(struct sysctl_oid_list *list, const char *name);
	static struct sysctl_oid *findVmmPresentOid(KernelPatcher &patcher);

	// Decide whether the calling process should be told it is in a VM.
	static bool shouldSpoofFor(const char *procname);

	static int sysctlVmmPresent(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
};

#endif /* kern_spoofvmm_hpp */
