//
//  kern_spoofvmm.cpp
//  SpoofVMM v4.9 - board-id functionality REMOVED ENTIRELY.
//
//  On a MacBookAir8,1 (identity-spoofed to MacBookPro16,4 via OpenCore) this
//  build boots cleanly AND clears the Tahoe installer's compatibility UI
//  gate, allowing an in-place upgrade of the internal SSD. Confirmed on
//  hardware.
//
//  What it does, all at Lilu patcher-load:
//    1. kern.hv_vmm_present = 1, PER PROCESS - returned only to the installer /
//       software-update helpers, real value to everyone else (so storagekitd /
//       diskarbitrationd are unaffected and APFS/disk access is not broken).
//    2. bridge-model SWAP - overwrite bridge-model on every IODeviceTree node
//       that has it with a VALID supported-Mac value (spoofvmm-bridge=,
//       default kDefaultBridge). ON by default. This is the operation that
//       clears the compatibility gate.
//
//  KEY FINDING: the gate keys off bridge-model, not board-id and not
//  hw.target. A bridge-model that is PRESENT and set to a supported-Mac code
//  passes the gate and boots fine. This build does NO model spoofing - model
//  identity is supplied by OpenCore (PlatformInfo/SMBIOS). The kext writing
//  model/Model/product-name/compatible in-OS (old v4.2/Stable) was redundant
//  with OpenCore AND hung early boot; the bridge-model swap itself is
//  boot-safe. DELETING bridge-model boots but does NOT pass the gate.
//
//  bridge-model DELETE is still available but is OPT-IN (-spoofvmmdelete),
//  for diagnostics only. Swap can be turned off with -spoofvmmnoswap. If both
//  swap and delete are on, order is swap-then-delete (net = delete), so do not
//  combine them unless you want deletion.
//
//  REMOVED IN v4.9 (was present in v4.1-v4.8): board-id override in
//  IODeviceTree, and the WiFi node-restore fix-up (v4.6, then v4.7 with a
//  different hook point) that tried to compensate for it. Both are gone.
//  Why: board-id spoofing broke WiFi (the AirPort/Broadcom driver's
//  personality/calibration matching reads board-id, and on this hardware it
//  can apparently only ever read the SAME shared root-level board-id
//  property that everything else reads - ioreg confirmed only two board-id
//  properties exist anywhere in IODeviceTree, root and /efi/platform, and the
//  WiFi controller's own device-tree node has none of its own - so there was
//  never a way to give WiFi a different value than the rest of the system
//  sees via property overriding). On-hardware A/B testing with board-id left
//  completely untouched (-spoofvmmnobid in v4.8) confirmed the gate STILL
//  clears - board-id was never load-bearing for it. So as of v4.9, board-id
//  is simply never touched, permanently, with no flag needed to disable it,
//  and the now-pointless WiFi fix-up is deleted rather than kept dormant.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <libkern/libkern.h>
#include <libkern/c++/OSData.h>
#include <sys/proc.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSIterator.h>
#include <pexpert/pexpert.h>

#include "kern_spoofvmm.hpp"

#define MODULE_SHORT "svmm"

// Value written to bridge-model by the SWAP operation. CONFIRMED WORKING: this
// value clears the Tahoe compatibility gate on a MacBookAir8,1 spoofed to
// MacBookPro16,4 and boots without hanging. Override with spoofvmm-bridge= only
// if targeting a different supported Mac.
static const char *kDefaultBridge = "J215AP";

#ifndef OID_MUTABLE_ANCHOR
#define OID_MUTABLE_ANCHOR (INT_MIN)
#endif

static mach_vm_address_t gOrgHandler = 0;   // original kern.hv_vmm_present handler

// Processes told they are in a VM (prefix match). Everyone else gets the real
// value. These are the software-update / installer helpers.
static const char *kSpoofTargets[] = {
	"softwareupdated",
	"osinstallersetup",
        "osinstallersetupd",
	"OSISetup",
	"com.apple.Mobile",
	"system_installd",
	"installd",
	"InstallAssistant",
};

// Log callers that are NOT spoofed too (debug build + -spoofvmmdbg).
static const bool kLogAllCallers = true;

void VMMSpoof::init() {
	auto err = lilu.onPatcherLoad(onPatcherLoad, this);
	if (err != LiluAPI::Error::NoError)
		SYSLOG(MODULE_SHORT, "failed to register onPatcherLoad: %d", err);
	else
		SYSLOG(MODULE_SHORT, "registered (v4.9: VMM + bridge-model swap only - board-id and the WiFi fix-up have been removed entirely, see notes), waiting for kernel patcher");
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

	if (shouldSpoofFor(procname)) {
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

void VMMSpoof::spoofIdentity() {
	// board-id override REMOVED in v4.9 (was here in v4.1-v4.8). Confirmed
	// on hardware to be unnecessary for the compatibility gate, and it was
	// the sole cause of the WiFi connect failure. Nothing in IODeviceTree's
	// board-id property is touched anymore.

	// --- bridge-model controls (the confirmed gate-clear mechanism) ---
	// swap is ON by default (this is what clears the gate); disable it with
	// -spoofvmmnoswap. delete is OPT-IN (diagnostics only) via -spoofvmmdelete.
	bool doSwap   = !checkKernelArgument("-spoofvmmnoswap");
	bool doDelete = checkKernelArgument("-spoofvmmdelete");

	char bridge[64] = {};
	if (!PE_parse_boot_argn("spoofvmm-bridge", bridge, sizeof(bridge)) || bridge[0] == '\0')
		strlcpy(bridge, kDefaultBridge, sizeof(bridge));

	OSData *bridgeData = nullptr;
	if (doSwap) {
		bridgeData = OSData::withBytes(bridge, static_cast<unsigned>(strlen(bridge) + 1));
		if (!bridgeData)
			SYSLOG(MODULE_SHORT, "bridge: swap OSData alloc failed, swap disabled");
	}

	// Single walk of the device tree handling bridge-model only now. On a
	// real Mac this lives in more than one node (root + /efi/platform), so
	// every node is visited. Per node: bridge swap, then bridge delete
	// (delete last, so BOTH-on nets to deletion).
	unsigned brSwap = 0, brDel = 0;
	if (doSwap || doDelete) {
		IORegistryIterator *it = IORegistryIterator::iterateOver(gIODTPlane, kIORegistryIterateRecursively);
		if (it) {
			IORegistryEntry *entry = nullptr;
			while ((entry = it->getNextObjectRecursive()) != nullptr) {
				if (entry->getProperty("bridge-model")) {
					if (doSwap && bridgeData && entry->setProperty("bridge-model", bridgeData))
						brSwap++;
					if (doDelete) {
						entry->removeProperty("bridge-model");
						brDel++;
					}
				}
			}
			it->release();
		}
	}

	SYSLOG(MODULE_SHORT, "bridge-model: swap=%s value=%s (%u) | delete=%s (%u)",
	       doSwap ? "on" : "off", bridge, brSwap, doDelete ? "on" : "off", brDel);
	if (doSwap && doDelete)
		SYSLOG(MODULE_SHORT, "bridge-model: swap+delete both on -> net effect is DELETE");

	if (bridgeData)
		bridgeData->release();
}

void VMMSpoof::processKernel(KernelPatcher &patcher) {
	// 1) bridge-model swap (IODeviceTree) - the confirmed gate-clear step.
	spoofIdentity();

	// 2) per-process kern.hv_vmm_present route.
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
