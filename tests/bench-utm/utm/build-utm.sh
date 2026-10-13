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
# bundle id of our own, so that it installs beside a UTM SE from the App Store, and UTM's paid-account
# entitlements (Platform/iOS/iOS.entitlements): the App Group group.<prefix>.UTM, the increased memory limit
# and extended virtual addressing. Automatic signing registers them on the team at the first build. The
# App Group is not optional for Vulkan: on a device a sandboxed app may shm_open() only names inside a
# group it is entitled to, UTM's virglrenderer makes the shared memory of a venus context that way
# (APP_SANDBOX_GROUP_ID, which the front end sets from Info.plist whether or not the app is entitled), and
# built without the group every guest's vkCreateInstance failed with "proxy: failed to pre-initialize
# context" in the VM's log -- on an iPad only: the simulator does not enforce it.
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
#   - GCMouse is listened to only while the pointer is locked. Unlocked, an M4 iPad delivered every
#     movement and button twice, as GCMouse's and as the system pointer's; one asked for the guest's mouse
#     and the other for its tablet, and UTM switched between them up to 150 times a second.
#   - a tap whose touch made UTM switch the guest between its mouse and its tablet is clicked when the
#     switch is through, where the finger landed. UTM sent it at once: pressed on the old device, wherever
#     its cursor was, and released on the new one (the guest's mouse kept its button down and the desktop
#     ignored every later click), or, if the switch was already through, not at all.
#   - a scheme "AOK VM" for Xcode, and the settings page's patch at every build (both below)
#   - the app is "AOK VM" on the home screen and on its welcome page, which says what it is based on. A
#     fork is not UTM's to be named for (its licence grants the code, not the name), and UTM SE is on the
#     App Store under that name.
# Its icon is icon/AppIcon.icon (beside this script; copy the directory to ~/aok-utm/icon), which replaces
# UTM's Icon Composer document of the same name. The bundle id, the App Group and the product's file name
# ("UTM SE.app") are still the ones above.
set -e
U=${U:-$HOME/aok-utm}
cd "$U/UTM"
if [ -f "$U/utm-frontend.patch" ] && git apply --check "$U/utm-frontend.patch" 2> /dev/null; then
    git apply "$U/utm-frontend.patch"
    echo "applied utm-frontend.patch"
fi
if [ -d "$U/icon/AppIcon.icon" ]; then
    rsync -a --delete "$U/icon/AppIcon.icon/" Platform/AppIcon.icon/
fi
[ -f CodeSigning.xcconfig ] || cat > CodeSigning.xcconfig <<'XC'
DEVELOPMENT_TEAM = UYU5FM4LQ4
PRODUCT_BUNDLE_PREFIX = app.ish.aok
DEVELOPER_ACCOUNT_PAID = YES
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
# whichever platform's entry came last. The patch makes that phase run at every build (it changes nothing
# the second time); the check below stays.
#
# From Xcode, with no script: once this has run (the patch, the icon and CodeSigning.xcconfig are then in
# the checkout), open ~/aok-utm/UTM/UTM.xcodeproj, choose the scheme "AOK VM" (the patch adds it: the
# iOS-SE target, with Run building the Release configuration, which is the one on the devices) and a
# device, and press Run. What Xcode does not build is the engines: they are in the sysroot, from
# build-qemu-utm.sh and install-qemu-utm.sh.
xcodebuild -scheme "AOK VM" -destination "$dest" -configuration Release ARCHS=arm64 -derivedDataPath "$U/dd" \
    -skipPackagePluginValidation -skipMacroValidation $extra build > "$U/build-${1:-device}.log" 2>&1 ||
    { grep "error:" "$U/build-${1:-device}.log" | sort -u | head -20; exit 1; }
if plutil -p "$U/dd/Build/Products/$products/UTM SE.app/Settings.bundle/Root.plist" | grep -q 'ExcludeTargets\|"Platform"'; then
    echo "the settings page was not patched for this target (Patch Settings bundle did not run)"; exit 1
fi
ls -d "$U/dd/Build/Products/$products/UTM SE.app"
