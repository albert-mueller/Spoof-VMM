//
//  kern_start.cpp
//  SpoofVMM - Lilu plugin entry / configuration
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

#include "kern_spoofvmm.hpp"

static VMMSpoof spoofer;

static const char *bootargOff[]   { "-spoofvmmoff" };
static const char *bootargDebug[] { "-spoofvmmdbg" };
static const char *bootargBeta[]  { "-spoofvmmbeta" };

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::Tahoe,   // macOS 26 (Darwin 25) - covers 26.7
	KernelVersion::Tahoe,
	[]() {
		spoofer.init();
	}
};
