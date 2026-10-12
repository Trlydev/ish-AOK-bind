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
#
# The front end is UTM's with utm-frontend.patch (beside this script; copy it to ~/aok-utm) applied when
# it is there and not applied yet. What it changes, all in Platform/iOS:
#   - a mouse or trackpad positions the guest's cursor through the system's pointer, with no pointer lock.
#     UTM asks for the lock whenever a VM runs and takes movement from GCMouse, which reports none unless
#     the system grants it; on an M4 iPad the trackpad of a keyboard did nothing. Capture (relative
#     movement) remains for a guest without a tablet, and as a setting ("Capture Mouse and Trackpad").
#   - what it asks for and what the system granted goes to the VM's debug.log ("pointer lock: ...")
#   - every ancestor view controller is told which child decides on the lock, not only the parent
#   - the keyboard's power key (HID usage 0x66) is not passed to the guest: an iPad keyboard's key arrived
#     as one and the guest shut down under its user
#   - a tap whose touch made UTM switch the guest between its mouse and its tablet is clicked when the
#     switch is through, where the finger landed. UTM sent it at once: pressed on the old device, wherever
#     its cursor was, and released on the new one (the guest's mouse kept its button down and the desktop
#     ignored every later click), or, if the switch was already through, not at all.
set -e
U=${U:-$HOME/aok-utm}
cd "$U/UTM"
if [ -f "$U/utm-frontend.patch" ] && git apply --check "$U/utm-frontend.patch" 2> /dev/null; then
    git apply "$U/utm-frontend.patch"
    echo "applied utm-frontend.patch"
fi
[ -f CodeSigning.xcconfig ] || cat > CodeSigning.xcconfig <<'XC'
DEVELOPMENT_TEAM = UYU5FM4LQ4
PRODUCT_BUNDLE_PREFIX = app.ish.aok
DEVELOPER_ACCOUNT_PAID = NO
DEVELOPER_ACCOUNT_VM_ACCESS = NO
CODE_SIGN_IDENTITY_IOS = Apple Development
CODE_SIGN_STYLE_IOS = Automatic
XC
case ${1:-device} in
    device) dest='generic/platform=iOS'; extra=-allowProvisioningUpdates; products=Release-iphoneos ;;
    sim)    dest='generic/platform=iOS Simulator'; extra=ONLY_ACTIVE_ARCH=NO; products=Release-iphonesimulator ;;
esac
# The settings page (Settings.bundle) holds every edition's and platform's entries; UTM's "Patch Settings
# bundle" build phase removes the ones that are not this target's. Its declared input is the bundle's
# directory, so after an edit to Root.plist an incremental build copied the file again and did not patch
# it: the SE app then showed JitStreamer and other entries of the JIT editions, and took its defaults from
# whichever platform's entry came last. Make both steps run, and check below that they did.
touch Platform/iOS/Settings.bundle
rm -rf "$U/dd/Build/Products/$products/UTM SE.app/Settings.bundle"
xcodebuild -scheme iOS-SE -destination "$dest" -configuration Release ARCHS=arm64 -derivedDataPath "$U/dd" \
    -skipPackagePluginValidation -skipMacroValidation $extra build > "$U/build-${1:-device}.log" 2>&1 ||
    { grep "error:" "$U/build-${1:-device}.log" | sort -u | head -20; exit 1; }
if plutil -p "$U/dd/Build/Products/$products/UTM SE.app/Settings.bundle/Root.plist" | grep -q 'ExcludeTargets\|"Platform"'; then
    echo "the settings page was not patched for this target (Patch Settings bundle did not run)"; exit 1
fi
ls -d "$U/dd/Build/Products/$products/UTM SE.app"
