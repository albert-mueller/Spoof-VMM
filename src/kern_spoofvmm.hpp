//
//  kern_spoofvmm.hpp
//  SpoofVMM - with Stage 2 BridgeOS firmware bypass for dropped T2 Macs
//

#ifndef kern_spoofvmm_hpp
#define kern_spoofvmm_hpp

#include <Headers/kern_patcher.hpp>
#include <sys/sysctl.h>
#include <sys/vnode.h>

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

	// --- IODeviceTree bridge-model swap ---
	void spoofIdentity();

	// --- Stage 2 Code Signing Validation Patch (_cs_validate_page) ---
	static mach_vm_address_t orgCsValidatePage;
	static void csValidatePage(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data, int *validated_p, int *tainted_p, int *nx_p);
	static void performStage2Replacements(vnode_t vp, const void *data, vm_size_t size);
};

#endif /* kern_spoofvmm_hpp */
