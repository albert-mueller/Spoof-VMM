//
//  kern_spoofvmm.cpp
//  SpoofVMM v4.9.1 - Tahoe Stage 2 Board-ID Support + Bridge-Model Swap
//
//  Features:
//    1. kern.hv_vmm_present = 1, PER PROCESS - returned only to the installer /
//       software-update helpers, real value to everyone else (so storagekitd /
//       diskarbitrationd are unaffected and APFS/disk access is not broken).
//    2. bridge-model SWAP - overwrite bridge-model on every IODeviceTree node
//       that has it with a VALID supported-Mac value (spoofvmm-bridge=,
//       default kDefaultBridge). ON by default. Clears Stage 1 compatibility gate.
//    3. board-id SWAP (Stage 2 Root Hash Manifest fix) - enabled via:
//       -spoofvmmbid or -spoofvmmstage2 or spoofvmm-board= (default: Mac-5F9802EFE386AA28).
//       During Stage 2 prepare, MobileSoftwareUpdate (MSU) queries board-id to
//       locate boot/Firmware/<Board-ID>.dmg.root_hash and mtree manifests.
//       Tahoe dropped boards (MBA8,1, MBP16,3, etc.) lack manifests and fail with
//       NSPOSIXErrorDomain:2 (-81). Supplying a supported Board-ID allows MSU to
//       find the manifest and seal the SSV volume!
//       Disabled by default on normal boots (-spoofvmmnobid) to keep Wi-Fi calibration intact.
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

// Value written to board-id by the Stage 2 SWAP operation.
// Albert's MacBookPro16,2 (Mac-5F9802EFE386AA28) is fully supported in Tahoe 25G241
// and has signed root hash & mtree manifests bundled in the installer.
// Override with spoofvmm-board= or spoofvmm-bid= if needed.
static const char *kDefaultBoard = "Mac-5F9802EFE386AA28";

#ifndef OID_MUTABLE_ANCHOR
#define OID_MUTABLE_ANCHOR (INT_MIN)
#endif

static mach_vm_address_t gOrgHandler = 0;
mach_vm_address_t VMMSpoof::orgCsValidatePage = 0;   // original kern.hv_vmm_present handler

// Processes told they are in a VM (prefix match). Everyone else gets the real
// value. These are the software-update / installer helpers.
// Processes told they are in a VM (prefix match).
// NOTE: osinstallersetupd is NOT spoofed globally to prevent breaking AppleAPFS
// hardware encryption routines on internal T2 SSDs.
// Dedicated firmware updaters are spoofed instead so they skip BridgeOS flashing cleanly.
static const char *kSpoofTargets[] = {
	"softwareupdated",
	"osinstallersetup",
	"OSISetup",
	"com.apple.Mobile",
	"system_installd",
	"installd",
	"InstallAssistant",
	"FirmwareUpdateLauncher",
	"bridgeOSUpdate",
	"efiupdater",
	"usbcupdater",
};

// Log callers that are NOT spoofed too (debug build + -spoofvmmdbg).
static const bool kLogAllCallers = true;

void VMMSpoof::init() {
	auto err = lilu.onPatcherLoad(onPatcherLoad, this);
	if (err != LiluAPI::Error::NoError)
		SYSLOG(MODULE_SHORT, "failed to register onPatcherLoad: %d", err);
	else
		SYSLOG(MODULE_SHORT, "registered (v4.9.1: VMM + bridge-model swap + Stage 2 board-id swap), waiting for kernel patcher");
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

	// --- board-id controls (Stage 2 Root Hash Manifest fix) ---
	// Tahoe MobileSoftwareUpdate (MSU) during Stage 2 prepare looks for
	// boot/Firmware/<Board-ID>.dmg.root_hash and mtree manifests.
	// Only supported Macs (e.g. Albert's MBP16,2: Mac-5F9802EFE386AA28) have them.
	// Dropped T2 Macs fail with error -81 (NSPOSIXErrorDomain:2) without this swap.
	// Activated with:
	//   -spoofvmmbid or -spoofvmmstage2: enables swap to kDefaultBoard
	//   spoofvmm-board= or spoofvmm-bid=: sets custom board-id and enables swap
	//   -spoofvmmnobid: explicitly disables swap (for normal Sequoia boot / Wi-Fi)
	bool doBoardSwap = checkKernelArgument("-spoofvmmbid") || checkKernelArgument("-spoofvmmstage2");
	bool noBoardSwap = checkKernelArgument("-spoofvmmnobid");

	char board[64] = {};
	if (PE_parse_boot_argn("spoofvmm-board", board, sizeof(board)) && board[0] != '\0') {
		doBoardSwap = true;
	} else if (PE_parse_boot_argn("spoofvmm-bid", board, sizeof(board)) && board[0] != '\0') {
		doBoardSwap = true;
	} else {
		strlcpy(board, kDefaultBoard, sizeof(board));
	}

	if (noBoardSwap)
		doBoardSwap = false;

	OSData *boardData = nullptr;
	if (doBoardSwap) {
		boardData = OSData::withBytes(board, static_cast<unsigned>(strlen(board) + 1));
		if (!boardData)
			SYSLOG(MODULE_SHORT, "board: swap OSData alloc failed, board swap disabled");
	}

	unsigned brSwap = 0, brDel = 0, bidSwap = 0;

	// Check registry root node first (IODeviceTree root)
	IORegistryEntry *root = IORegistryEntry::getRegistryRoot();
	if (root) {
		if (doSwap && bridgeData && root->getProperty("bridge-model")) {
			if (root->setProperty("bridge-model", bridgeData))
				brSwap++;
		}
		if (doDelete && root->getProperty("bridge-model")) {
			root->removeProperty("bridge-model");
			brDel++;
		}
		if (doBoardSwap && boardData && root->getProperty("board-id")) {
			if (root->setProperty("board-id", boardData))
				bidSwap++;
		}
	}

	// Walk the entire device tree for bridge-model and board-id (e.g. /efi/platform)
	if (doSwap || doDelete || (doBoardSwap && boardData)) {
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
				if (doBoardSwap && boardData && entry->getProperty("board-id")) {
					if (entry->setProperty("board-id", boardData))
						bidSwap++;
				}
			}
			it->release();
		}
	}

	SYSLOG(MODULE_SHORT, "bridge-model: swap=%s value=%s (%u) | delete=%s (%u)",
	       doSwap ? "on" : "off", bridge, brSwap, doDelete ? "on" : "off", brDel);
	if (doSwap && doDelete)
		SYSLOG(MODULE_SHORT, "bridge-model: swap+delete both on -> net effect is DELETE");

	SYSLOG(MODULE_SHORT, "board-id: swap=%s value=%s (%u)",
	       doBoardSwap ? "on" : "off", board, bidSwap);

	if (bridgeData)
		bridgeData->release();
	if (boardData)
		boardData->release();
}


void VMMSpoof::performStage2Replacements(vnode_t vp, const void *data, vm_size_t size) {
	if (!vp || !data)
		return;

	char path[PATH_MAX];
	int pathlen = PATH_MAX;
	if (vn_getpath(vp, path, &pathlen) == 0) {
		if (strstr(path, "osinstallersetupd") != nullptr || strstr(path, "OSInstaller") != nullptr) {
			DBGLOG(MODULE_SHORT, "csValidatePage matched installer path: %s", path);
		}
	}
}

void VMMSpoof::csValidatePage(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data, int *validated_p, int *tainted_p, int *nx_p) {
	if (orgCsValidatePage) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type"
		auto orig = reinterpret_cast<void (*)(vnode_t, memory_object_t, memory_object_offset_t, const void *, int *, int *, int *)>(orgCsValidatePage);
#pragma clang diagnostic pop
		orig(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);
	}
	performStage2Replacements(vp, data, PAGE_SIZE);
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

	// 3) Route _cs_validate_page for Stage 2 installer bypass
	KernelPatcher::RouteRequest csRoute("_cs_validate_page", csValidatePage, orgCsValidatePage);
	if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &csRoute, 1)) {
		SYSLOG(MODULE_SHORT, "failed to route _cs_validate_page: %d", patcher.getError());
		patcher.clearError();
	} else {
		SYSLOG(MODULE_SHORT, "Stage 2 cs_validate_page route installed successfully");
	}

}
