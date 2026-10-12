#!/usr/bin/env python3
"""make-vm.py OUTDIR KERNEL INITRAMFS EFI_VARS -- three ready-made RISC-V virtual machines for UTM.

A .utm bundle is a directory: config.plist (UTM's configuration format, version 4) and Data/ with the files it
names. Copied into the app's Documents, UTM lists it like one made in its own wizard.

  "AOK RISC-V Console.utm"  The race's guest: the Alpine kernel and the benchmark userland, booted directly
                            (OpenSBI, then the kernel), one CPU, no display, the console on UTM's built-in
                            terminal, a virtio network card. It runs the six workloads, printing a RESULT
                            line for each (name, then wall, user and system milliseconds in the guest), and
                            leaves a shell.
  "AOK RISC-V Display.utm"  The same guest with its console on a display: UTM's default display device
                            (virtio-ramfb), a USB keyboard, network, two CPUs. No workloads, a shell.
  "AOK RISC-V UEFI.utm"     What UTM's wizard makes for a RISC-V "virt" machine, with no disk: UEFI firmware,
                            a virtio-ramfb display, USB input, sound, network. It ends at the firmware's own
                            shell, having found nothing to boot. It is there to attach an installer to.

INITRAMFS is the VM image of update-roots.sh (initramfs-riscv64-vm.cpio): the race's userland plus the
kernel's modules, without which the guest has no driver for a network card, a display or a USB keyboard.
All three have UTM's debug log on (Data/debug.log: the QEMU command line and QEMU's messages).
"""
import os
import plistlib
import shutil
import sys
import uuid

WORKLOADS = "coremark,nbody,python,xz,sha256,awk"


def base(name, notes, vm_uuid, cpus, memory):
    return {
        "Backend": "QEMU",
        "ConfigurationVersion": 4,
        "Information": {"Name": name, "IconCustom": False, "Icon": "linux", "Notes": notes, "UUID": vm_uuid},
        "System": {
            "Architecture": "riscv64", "Target": "virt", "CPU": "default",
            "CPUFlagsAdd": [], "CPUFlagsRemove": [],
            "CPUCount": cpus, "ForceMulticore": False, "MemorySize": memory, "JITCacheSize": 0,
        },
        "QEMU": {
            "DebugLog": True, "UEFIBoot": False, "RNGDevice": False, "BalloonDevice": False,
            "TPMDevice": False, "Hypervisor": False, "RTCLocalTime": False, "PS2Controller": False,
            "AdditionalArguments": [],
        },
        "Input": {"UsbBusSupport": "Disabled", "UsbSharing": False, "MaximumUsbShare": 3},
        "Sharing": {"DirectoryShareMode": "None", "DirectoryShareReadOnly": False, "ClipboardSharing": False},
        "Display": [], "Drive": [], "Network": [], "Serial": [], "Sound": [],
    }


def network(mac):
    # "Emulated": QEMU's user-mode network, which is all iOS allows. The guest takes an address by DHCP.
    return {"Mode": "Emulated", "Hardware": "virtio-net-pci", "MacAddress": mac,
            "IsolateFromHost": False, "PortForward": []}


def display():
    return {"Hardware": "virtio-ramfb", "DynamicResolution": True, "UpscalingFilter": "Nearest",
            "DownscalingFilter": "Linear", "NativeResolution": False}


def direct_boot(n):
    """The kernel and initramfs as UTM's -kernel and -initrd "drives"."""
    return [
        {"ImageName": "Image-lts-riscv64", "ImageType": "LinuxKernel", "Interface": "None",
         "InterfaceVersion": 1, "Identifier": str(uuid.UUID(int=0xA0C0A0C0_0000_4000_8000_0000000000A0 + n)).upper(),
         "ReadOnly": True},
        {"ImageName": "initramfs-riscv64.cpio", "ImageType": "LinuxInitrd", "Interface": "None",
         "InterfaceVersion": 1, "Identifier": str(uuid.UUID(int=0xA0C0A0C0_0000_4000_8000_0000000000B0 + n)).upper(),
         "ReadOnly": True},
    ]


def terminal():
    return {
        "Mode": "Terminal", "Target": "Auto",
        "Terminal": {"Font": "Menlo", "FontSize": 12, "CursorBlink": True,
                     "ForegroundColor": "#ffffff", "BackgroundColor": "#000000"},
    }


def write(outdir, config, files):
    bundle = os.path.join(outdir, config["Information"]["Name"] + ".utm")
    shutil.rmtree(bundle, ignore_errors=True)
    os.makedirs(os.path.join(bundle, "Data"))
    for name, source in files.items():
        shutil.copyfile(source, os.path.join(bundle, "Data", name))
    with open(os.path.join(bundle, "config.plist"), "wb") as f:
        plistlib.dump(config, f)
    print(bundle)


def main():
    outdir, kernel, initramfs, efi_vars = sys.argv[1:5]
    os.makedirs(outdir, exist_ok=True)

    # Fixed UUIDs: copying a rebuilt bundle over an installed one keeps it the same machine to UTM.
    console = base("AOK RISC-V Console",
                   "Alpine 6.18 kernel and the benchmark userland, booted directly. Runs six workloads, then "
                   "leaves a shell on the terminal.",
                   "A0C0A0C0-0000-4000-8000-000000000001", cpus=1, memory=1024)
    console["QEMU"]["AdditionalArguments"] = [
        f'-append "console=ttyS0 rdinit=/init quiet bench={WORKLOADS} then=shell"',
    ]
    console["Serial"] = [terminal()]
    console["Network"] = [network("52:54:00:A0:C0:01")]
    console["Drive"] = direct_boot(1)
    write(outdir, console, {"Image-lts-riscv64": kernel, "initramfs-riscv64.cpio": initramfs})

    # console=tty0 last makes the display the console: /init's output and its shell go there, and the
    # kernel's messages to the serial terminal as well.
    shown = base("AOK RISC-V Display",
                 "The Alpine guest with its console on the display (virtio-ramfb) and a USB keyboard.",
                 "A0C0A0C0-0000-4000-8000-000000000003", cpus=2, memory=1024)
    shown["QEMU"]["AdditionalArguments"] = [
        '-append "console=ttyS0 console=tty0 rdinit=/init quiet bench=none then=shell"',
    ]
    shown["QEMU"]["RNGDevice"] = True
    shown["Input"]["UsbBusSupport"] = "3.0"
    shown["Display"] = [display()]
    shown["Network"] = [network("52:54:00:A0:C0:03")]
    shown["Serial"] = [terminal()]
    shown["Drive"] = direct_boot(3)
    write(outdir, shown, {"Image-lts-riscv64": kernel, "initramfs-riscv64.cpio": initramfs})

    # The same machine on UTM's accelerated display device. UTM then starts QEMU with -spice gl=on and,
    # as its settings allow, venus=, neptune= and blob= on the device.
    gl = base("AOK RISC-V Display GL",
              "The Display machine on the GL display device (virtio-ramfb-gl). Needs a QEMU with the GL path.",
              "A0C0A0C0-0000-4000-8000-000000000004", cpus=2, memory=2048)
    gl["QEMU"]["AdditionalArguments"] = list(shown["QEMU"]["AdditionalArguments"])
    gl["QEMU"]["RNGDevice"] = True
    gl["Input"]["UsbBusSupport"] = "3.0"
    gl["Display"] = [dict(display(), Hardware="virtio-ramfb-gl")]
    gl["Network"] = [network("52:54:00:A0:C0:04")]
    gl["Serial"] = [terminal()]
    gl["Drive"] = direct_boot(4)
    write(outdir, gl, {"Image-lts-riscv64": kernel, "initramfs-riscv64.cpio": initramfs})

    uefi = base("AOK RISC-V UEFI",
                "UTM's defaults for a RISC-V virt machine with no disk attached. It ends at the UEFI shell: "
                "attach an installer image to boot something.",
                "A0C0A0C0-0000-4000-8000-000000000002", cpus=2, memory=1024)
    uefi["QEMU"].update({"UEFIBoot": True, "RNGDevice": True})
    uefi["Input"]["UsbBusSupport"] = "3.0"
    uefi["Sharing"]["ClipboardSharing"] = True
    uefi["Display"] = [display()]
    uefi["Network"] = [network("52:54:00:A0:C0:02")]
    uefi["Sound"] = [{"Hardware": "intel-hda"}]
    uefi["Serial"] = [terminal()]
    write(outdir, uefi, {"efi_vars.fd": efi_vars})


if __name__ == "__main__":
    main()
