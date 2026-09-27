#!/bin/bash
# Fetches the Lilu SDK (Lilu.kext, prebuilt DEBUG release) and MacKernelSDK
# into this directory using acidanthera's official bootstrap script.
set -e
cd "$(dirname "$0")"

if [ -f Lilu.kext/Contents/Resources/Headers/kern_api.hpp ] && [ -d MacKernelSDK ]; then
  echo "SDKs already present."
  exit 0
fi

rm -rf Lilu.kext MacKernelSDK
src=$(/usr/bin/curl -Lfs https://raw.githubusercontent.com/acidanthera/Lilu/master/Lilu/Scripts/bootstrap.sh) && eval "$src" || exit 1
