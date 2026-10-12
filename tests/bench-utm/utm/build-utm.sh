#!/bin/bash
# build-utm.sh [device|sim] -- build UTM SE (UTM's iOS front end, "iOS-SE" scheme) around the sysroot that
# install-qemu-utm.sh prepared, on the Mac that signs. The product is a signed "UTM SE.app" under
# ~/aok-utm/dd/Build/Products/Release-iphoneos (or -iphonesimulator).
#
# How the tree gets here, as it was done:
#   UTM      github.com/utmapp/UTM at 7eadb056 (5.0.6, main of 2026-09-25), cloned on the build Mac into
#            ~/aok-vm-bench/utm/UTM
#   sysroot  the Sysroot-ios-tci-arm64 artifact (Sysroot-ios_simulator-tci-arm64 for the simulator) of that
#            commit's Build run on GitHub Actions, run 36090554968, unpacked in the UTM checkout:
#              gh run download 36090554968 -R utmapp/UTM -n Sysroot-ios-tci-arm64
#   QEMU     build-qemu-utm.sh, then install-qemu-utm.sh, there
#   then     rsync -a --delete BUILDMAC:aok-vm-bench/utm/UTM/ ~/aok-utm/UTM/
#
# UTM at that commit needs the released Xcode 27: Xcode 26 lacks iOS 27 APIs it uses, and Xcode 27 beta 3
# cannot compile the SwiftTerm it depends on. CodeSigning.xcconfig (written here if absent) gives the app a
# bundle id of our own, so that it installs beside a UTM SE from the App Store, and asks for none of the
# entitlements UTM's paid-account configuration adds.
set -e
U=${U:-$HOME/aok-utm}
cd "$U/UTM"
[ -f CodeSigning.xcconfig ] || cat > CodeSigning.xcconfig <<'XC'
DEVELOPMENT_TEAM = UYU5FM4LQ4
PRODUCT_BUNDLE_PREFIX = app.ish.aok
DEVELOPER_ACCOUNT_PAID = NO
DEVELOPER_ACCOUNT_VM_ACCESS = NO
CODE_SIGN_IDENTITY_IOS = Apple Development
CODE_SIGN_STYLE_IOS = Automatic
XC
case ${1:-device} in
    device) dest='generic/platform=iOS'; extra=-allowProvisioningUpdates ;;
    sim)    dest='generic/platform=iOS Simulator'; extra=ONLY_ACTIVE_ARCH=NO ;;
esac
xcodebuild -scheme iOS-SE -destination "$dest" -configuration Release ARCHS=arm64 -derivedDataPath "$U/dd" \
    -skipPackagePluginValidation -skipMacroValidation $extra build > "$U/build-${1:-device}.log" 2>&1 ||
    { grep "error:" "$U/build-${1:-device}.log" | sort -u | head -20; exit 1; }
ls -d "$U"/dd/Build/Products/Release-*/"UTM SE.app"
