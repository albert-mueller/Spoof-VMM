//
//  kern_spoofvmm.cpp
//  SpoofVMM (Targeted) - process-scoped kern.hv_vmm_present spoof + caller log
//
//  Rationale (from the MacBookAir8,1 / Tahoe investigation):
//    The sysctl kern.hv_vmm_present is read by USERSPACE, not by apfs.kext in
//    the kernel. Empirically, forcing it to 1 for *everyone* breaks disk access
//    on a real T2 Mac - almost certainly because a userspace disk daemon
//    (storagekitd / diskarbitrationd) reads 1, decides "I am a VM", and drives
//    the T2-backed encrypted SSD down a VM code path that fails on real
//    hardware. The Tahoe installer only needs 1 for its *eligibility check*.
//
//    So instead of a global flip (or a time gate, which still exposes disk
//    daemons to 1 during install), we spoof PER PROCESS:
//      - return 1 only to the eligibility-checking process(es),
//      - forward to the kernel's original handler (real value) for everyone
//        else, at all times - so storagekitd/diskarbitrationd always see the
//        truth and APFS is untouched.
//
//    Because it is not certain which process performs the Tahoe check in the
//    full installer, every caller is logged (pid + name + value returned).
//    Boot once, read the log, then narrow kSpoofTargets.
//
//  The OID is located by walking the exported sysctl root (_sysctl__children),
//  the same technique RestrictEvents uses for revpatch=sbvmm.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <libkern/libkern.h>
#include <sys/proc.h>

#include "kern_spoofvmm.hpp"

#define MODULE_SHORT "svmm"

#ifndef OID_MUTABLE_ANCHOR
#define OID_MUTABLE_ANCHOR (INT_MIN)
#endif

static mach_vm_address_t gOrgHandler = 0;

// Process names that should be told they are in a VM. Start conservative:
// only the known software-update / installer helpers. Adjust after reading the
// caller log (see NOTES.md). An entry matches if it is a prefix of the caller
// name, so "com.apple.Mobile" also covers "com.apple.MobileSoftwareUpdate...".
static const char *kSpoofTargets[] = {
	"softwareupdated",
	"osinstallersetup",
	"OSISetup",
	"com.apple.Mobile",
	"system_installd",
	"installd",
	"InstallAssistant",
    "mobileassetd",
};

// 

// Set to true to also log callers that are NOT spoofed (verbose). With a debug
// build (-spoofvmmdbg) this shows every reader of kern.hv_vmm_present.
static const bool kLogAllCallers = true;

void VMMSpoof::init() {
	auto err = lilu.onPatcherLoad(onPatcherLoad, this);
	if (err != LiluAPI::Error::NoError)
		SYSLOG(MODULE_SHORT, "failed to register onPatcherLoad: %d", err);
	else
		SYSLOG(MODULE_SHORT, "registered (targeted mode), waiting for kernel patcher");
}

void VMMSpoof::onPatcherLoad(void *user, KernelPatcher &patcher) {
	static_cast<VMMSpoof *>(user)->processKernel(patcher);
}

bool VMMSpoof::shouldSpoofFor(const char *procname) {
	if (!procname)
		return false;
	for (auto target : kSpoofTargets) {
		size_t n = strlen(target);
		if (strncmp(procname, target, n) == 0)
			return true;
	}
	return false;
}

int VMMSpoof::sysctlVmmPresent(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req) {
	char procname[64] = { '?', 0 };
	int pid = -1;
	if (req && req->p) {
		pid = proc_pid(req->p);
		proc_name(pid, procname, sizeof(procname));
	}

	bool spoof = shouldSpoofFor(procname);

	if (spoof) {
		SYSLOG(MODULE_SHORT, "caller pid=%d name=%s -> spoofing 1", pid, procname);
		int value = 1;
		return SYSCTL_OUT(req, &value, sizeof(value));
	}

	if (kLogAllCallers)
		DBGLOG(MODULE_SHORT, "caller pid=%d name=%s -> passthrough (real value)", pid, procname);

	if (gOrgHandler) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type"
		auto orig = reinterpret_cast<int (*)(struct sysctl_oid *, void *, int, struct sysctl_req *)>(gOrgHandler);
#pragma clang diagnostic pop
		return orig(oidp, arg1, arg2, req);
	}

	int zero = 0;
	return SYSCTL_OUT(req, &zero, sizeof(zero));
}

struct sysctl_oid *VMMSpoof::findChild(struct sysctl_oid_list *list, const char *name) {
	if (!list)
		return nullptr;

	struct sysctl_oid *first = SLIST_FIRST(list);
	if (!first)
		return nullptr;

	struct sysctl_oid_list *lists[2] { list, nullptr };
	if (first->oid_number == OID_MUTABLE_ANCHOR)
		lists[1] = static_cast<struct sysctl_oid_list *>(first->oid_arg1);

	for (auto l : lists) {
		if (!l)
			continue;
		struct sysctl_oid *oid;
		SLIST_FOREACH(oid, l, oid_link) {
			if (oid->oid_number == OID_MUTABLE_ANCHOR)
				continue;
			if (oid->oid_name && strcmp(oid->oid_name, name) == 0)
				return oid;
		}
	}

	return nullptr;
}

struct sysctl_oid *VMMSpoof::findVmmPresentOid(KernelPatcher &patcher) {
	auto root = reinterpret_cast<struct sysctl_oid_list *>(
		patcher.solveSymbol(KernelPatcher::KernelID, "_sysctl__children"));
	if (!root) {
		SYSLOG(MODULE_SHORT, "failed to solve _sysctl__children: %d", patcher.getError());
		patcher.clearError();
		return nullptr;
	}

	auto kern = findChild(root, "kern");
	if (!kern || (kern->oid_kind & CTLTYPE) != CTLTYPE_NODE) {
		SYSLOG(MODULE_SHORT, "failed to find sysctl node kern");
		return nullptr;
	}

	auto vmm = findChild(static_cast<struct sysctl_oid_list *>(kern->oid_arg1), "hv_vmm_present");
	if (!vmm || !vmm->oid_handler) {
		SYSLOG(MODULE_SHORT, "failed to find kern.hv_vmm_present");
		return nullptr;
	}

	return vmm;
}

void VMMSpoof::processKernel(KernelPatcher &patcher) {
	auto oid = findVmmPresentOid(patcher);
	if (!oid)
		return;

	auto handler = reinterpret_cast<mach_vm_address_t>(oid->oid_handler);
	DBGLOG(MODULE_SHORT, "kern.hv_vmm_present handler at 0x%llx", handler);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	gOrgHandler = patcher.routeFunction(handler, reinterpret_cast<mach_vm_address_t>(sysctlVmmPresent), true);
#pragma clang diagnostic pop

	if (!gOrgHandler) {
		SYSLOG(MODULE_SHORT, "failed to route kern.hv_vmm_present handler: %d", patcher.getError());
		patcher.clearError();
		return;
	}

	SYSLOG(MODULE_SHORT, "route installed (per-process spoof, real value for others)");
}
