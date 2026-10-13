// The arm64 guest says who it is the same way everywhere, and it is not an
// Apple M1.
//
// /proc/cpuinfo gave "CPU implementer : 0x61, CPU part : 0x023" -- the M1's
// Firestorm core, "the silicon underneath". A program that goes by that
// generates code for an M1: LLVM's JIT takes the host CPU from those two
// lines, and llvmpipe (Mesa 26, LLVM 22) then emitted a half-precision FADD,
// which this guest has never had, and died of SIGILL. The guest now carries
// an identity of its own: implementer 0x00, the code Arm reserves for
// software (QEMU's own CPU uses it), and a part number no CPU has.
//
// With it, the three things that say so agree:
//
//   AT_HWCAP has CPUID (bit 11), which /proc/cpuinfo's Features line has
//     named all along while the bit libraries test was missing;
//   `mrs` reads MIDR_EL1, MPIDR_EL1 and REVIDR_EL1 from EL0, as Linux
//     emulates them for a process (MIDR was SIGILL): MPIDR as Linux's safe
//     value 0x80000000, REVIDR as 0;
//   /proc/cpuinfo's implementer, variant, part and revision are MIDR_EL1's
//     fields, the same on every CPU listed;
//   an ID register Linux keeps no value for reads as zero rather than
//     faulting (ID_AA64AFR0_EL1), and the ones that carry features still do
//     (ID_AA64ISAR0_EL1 has AES).
//
// arm64 only.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__aarch64__)
int main(void) { printf("cpu_identity: SKIP (arm64 only)\n"); return 0; }
#else
#include <sys/auxv.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("cpu_identity: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

int main(void) {
    unsigned long hwcap = getauxval(AT_HWCAP);
    CHECK(hwcap & (1ul << 11), "AT_HWCAP %#lx has no CPUID bit", hwcap);
    if (!(hwcap & (1ul << 11))) {
        // Without it the reads below are not promised to work.
        printf("cpu_identity: FAIL failures=%d\n", failures);
        return 1;
    }
    uint64_t midr, mpidr, revidr, isar0, afr0;
    __asm__("mrs %0, midr_el1" : "=r"(midr));
    __asm__("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__("mrs %0, revidr_el1" : "=r"(revidr));
    __asm__("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    __asm__("mrs %0, s3_0_c0_c5_4" : "=r"(afr0)); // ID_AA64AFR0_EL1
    unsigned implementer = (unsigned) (midr >> 24) & 0xff, variant = (unsigned) (midr >> 20) & 0xf;
    unsigned part = (unsigned) (midr >> 4) & 0xfff, revision = (unsigned) midr & 0xf;

    CHECK(mpidr == 0x80000000, "MPIDR_EL1 reads %#llx, wanted 0x80000000", (unsigned long long) mpidr);
    CHECK(revidr == 0, "REVIDR_EL1 reads %#llx, wanted 0", (unsigned long long) revidr);
    CHECK(afr0 == 0, "ID_AA64AFR0_EL1 reads %#llx, wanted 0", (unsigned long long) afr0);
    CHECK(((isar0 >> 4) & 0xf) != 0, "ID_AA64ISAR0_EL1 %#llx says no AES, which AT_HWCAP has", (unsigned long long) isar0);
    CHECK(implementer != 0x61, "MIDR_EL1 %#llx names Apple (0x61) as the implementer", (unsigned long long) midr);
    CHECK(implementer == 0x00, "MIDR_EL1 %#llx: implementer %#x, wanted 0x00 (software)", (unsigned long long) midr, implementer);
    CHECK(((midr >> 16) & 0xf) == 0xf, "MIDR_EL1 %#llx: architecture field is not 0xf", (unsigned long long) midr);

    FILE *f = fopen("/proc/cpuinfo", "r");
    CHECK(f != NULL, "cannot read /proc/cpuinfo");
    int cpus = 0, features_cpuid = 0;
    char line[512];
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        char *colon = strchr(line, ':');
        if (colon == NULL)
            continue;
        unsigned long value = strtoul(colon + 1, NULL, 0);
        if (strncmp(line, "CPU implementer", 15) == 0) {
            cpus++;
            CHECK(value == implementer, "cpuinfo implementer %#lx, MIDR_EL1 says %#x", value, implementer);
        } else if (strncmp(line, "CPU variant", 11) == 0) {
            CHECK(value == variant, "cpuinfo variant %#lx, MIDR_EL1 says %#x", value, variant);
        } else if (strncmp(line, "CPU part", 8) == 0) {
            CHECK(value == part, "cpuinfo part %#lx, MIDR_EL1 says %#x", value, part);
        } else if (strncmp(line, "CPU revision", 12) == 0) {
            CHECK(value == revision, "cpuinfo revision %lu, MIDR_EL1 says %u", value, revision);
        } else if (strncmp(line, "Features", 8) == 0) {
            if (strstr(colon, " cpuid") != NULL)
                features_cpuid++;
            // What the guest does not have must not be claimed either: these
            // are the ones a JIT would act on.
            CHECK(strstr(colon, " fphp") == NULL && strstr(colon, " asimdhp") == NULL,
                  "cpuinfo claims half-precision arithmetic (fphp/asimdhp)");
        }
    }
    if (f != NULL)
        fclose(f);
    CHECK(cpus >= 1, "/proc/cpuinfo has no CPU implementer line");
    CHECK(features_cpuid == cpus, "%d of %d CPUs list cpuid in Features", features_cpuid, cpus);

    if (failures != 0) {
        printf("cpu_identity: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("cpu_identity: PASS (MIDR_EL1 %#llx)\n", (unsigned long long) midr);
    return 0;
}
#endif
