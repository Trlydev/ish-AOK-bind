# Release Notes Since `builds/iSH-AOK_558`

173 commits. This build brings sandboxes to the guest: user, mount, pid and
network namespaces, so bubblewrap, `unshare` and Codex CLI's command sandbox
start and confine. The arm64 and riscv64 guests run fused pairs of
instructions as one gadget and are 10-45% faster than 558, and a new setting
keeps translated code across programs so short ones start about a fifth
faster. Around that: nine GitHub issues answered, OpenGL on the GPU with a
current Mesa, and a set of crash fixes from 557's and 558's reports.

## Before you update

**Saved sessions from 558 will not restore.** An image is tied to the build
that saved it. The guest boots fresh, and the resume picker marks such an
image "can't be restored". Finish anything that lives only in a saved session
before you update.

**A session with a sandbox in it cannot be saved.** A process in a namespace
of its own would no longer be confined if it were restored outside its
namespaces, so the save is refused while one runs. Save Anyway does not leave
such processes out yet, and the automatic save refuses too. On a root that
runs systemd with sandboxed services (Arch: logind and others use
`PrivateTmp=` and friends) that means no session can be saved at all for now.
Devuan and Alpine are affected only while you run a sandbox yourself.

**The arm64 guest no longer says it is an Apple M1.** `/proc/cpuinfo` reads
implementer `0x00`, the code Arm reserves for software, part `0xa0c`. LLVM's
JIT took the old answer at its word and emitted half-precision arithmetic the
guest does not have (Mesa's llvmpipe died of `SIGILL` in its first shader).
Software that reads MIDR to pick a code path now picks a generic one.

## Sandboxes: namespaces (experimental)

`/AOK/docs/namespaces.md` has the details.

- **User namespaces.** `unshare -r` makes a process root in a world of its
  own and nobody new in the real one: uid and gid maps, `setgroups`, the
  capabilities a namespace's root is given, and file ownership as the
  namespace sees it.
- **Mount namespaces**, with propagation (shared, slave, private,
  unbindable), mounts over mounts, `mount_setattr` and `pivot_root`. A bind
  mount made in a sandbox stays in it.
- **Pid namespaces**: a set of processes that see only each other, the first
  of them as pid 1, in `/proc` too; `setns` and `nsenter` join them.
  Accounting and taskstats give pids as the namespace in question numbers
  them.
- **Network namespaces**: a private one has its own `lo`, and veth pairs and
  dummy links between sandboxes (`ip link add ... type veth`), with TCP, UDP
  and ping across them. A sandbox reaches none of the device's network.
- **What runs**: `bwrap`, including `--unshare-all`; `codex sandbox`, so Codex
  CLI runs its commands confined instead of needing the sandbox turned off;
  systemd's `PrivateTmp=`, `ProtectHostname=` and the like are real.
- **FUSE in a sandbox**: root of a user namespace may mount a FUSE filesystem
  served by its own daemon, which sees uids, gids and pids in the sandbox's
  terms. With it comes Linux's access rule, which AOK never had: a FUSE mount
  is for the user who made it unless it was mounted `allow_other`. Anyone
  else, root included, gets `EACCES`.
- **`RLIMIT_NPROC`** counts a namespace's processes against its creator's
  limit outside, so raising the limit inside a sandbox does not raise it.

Not in this build: veth ends in the machine's own namespace, forwarding and
bridges. Creating such a link is `EOPNOTSUPP`, so a container tool that wants
a bridged network (podman's or lxc's) cannot set one up. cgroup and time
namespaces are not offered either, and `/proc/<pid>/ns` no longer lists
them, so bubblewrap and systemd do not ask.

## Speed

- **arm64 guest: fused pairs.** Two adjacent instructions that commonly go
  together run as one gadget with one dispatch, and where the second uses the
  first's result it takes it from a host register. With a one-compare TLB
  probe, a dispatch that updates the stream pointer once, and direct
  add-with-carry and multiply gadgets, the arm64 guest is about 18% faster
  than 558 on 7-Zip and 15-45% on Python on an M4 iPad, about 9% and 15-25%
  on an A10X.
- **riscv64 guest: the same**: fused pairs, forwarding and the TLB probe. On
  the M4, 7-Zip is about 20% and Python 8-45% faster than 558.
- **Cache Translated Code (arm64)**, in Settings → Translated Code, off by
  default. It keeps code translated for one program after it exits, so the
  next program that loads the same library or binary starts faster: on the
  Mac a hundred `/bin/true`s take 111 ms instead of 144, a hundred
  `dash -c :` 129 instead of 175, three gcc compile-and-links 13% less. One
  long-running program gains little. **It turns the guest's ASLR off**,
  because a cached translation is found by the address its code was at, and
  the setting says so: memory-corruption bugs in programs you run here are
  easier to exploit while it is on. Up to 96 MB.
- **Task switching**: a wait that would sleep tries again for up to 25 µs
  first. Two tasks passing a byte each way, on the M4: a pipe round trip 9.4
  → 2.4 µs, a unix socket 11.3 → 2.7, a condition variable 9.0 → 2.9; on the
  A10X 39.5 → 24.8, 53.1 → 19.4 and 29.2 → 9.9.
- **A system call on musl costs a third of what it did.** 558 asked the host
  for the CPU number on every return to guest code for a thread with no rseq
  area, which is every musl thread: `getppid` took 960 ns on the Alpine roots
  of all four architectures and takes 335.
- **A JIT lookup no longer takes the whole JIT's lock.** Under eight
  sysbench clients, 77% of MariaDB's lock waiting was that one line; a lookup
  now takes one of 64 bucket locks.

## Issues answered

- **Home Screen quick actions**
  ([#577](https://github.com/emkey1/ish-AOK/issues/577)): long-press the icon
  for Workspace, Wayland Display, Session Shell or System Console, for that
  one launch, whatever Startup Mode says.
- **Settings → Appearance → Bottom Padding**
  ([#616](https://github.com/emkey1/ish-AOK/issues/616)): space below the
  terminal, 0-120 points, so its last rows are clear of the Home indicator,
  while no on-screen keyboard is up.
- **Desktop menu entries that write `Terminal=True` get a terminal**
  ([#620](https://github.com/emkey1/ish-AOK/issues/620)): Debian's bsdgames
  capitalises it in all 38 entries and GLib read that as no, so Air Traffic
  Controller and BSD Tetris started with nothing to draw in.
- **Browse Files no longer crashes a sideloaded copy**
  ([#625](https://github.com/emkey1/ish-AOK/issues/625)). A copy re-signed
  without the app's App Group has no Files location to open; the row now
  says so in an alert. Export still works.
- **Touchpad Mode** for the Wayland display
  ([#628](https://github.com/emkey1/ish-AOK/issues/628)), in its ··· menu: a
  finger moves the pointer as on a laptop's touchpad instead of putting it
  under itself.
- **A connection iOS takes away reads as reset**
  ([#631](https://github.com/emkey1/ish-AOK/issues/631)): `ECONNRESET`, what
  Linux says and what a reconnecting client waits for, not "Bad file
  descriptor". The disconnect itself is iOS suspending the app;
  `/AOK/docs/networking.md` says how to keep it running.
- **Settings → Wayland Desktop → Install a Desktop…**
  ([#639](https://github.com/emkey1/ish-AOK/issues/639)) lists the desktops
  (labwc, Xfce, Wayfire, sway), what is installed in the running root and
  which one starts, and installs one with a tap.
- **Workspace Storage is a list you can go down**
  ([#640](https://github.com/emkey1/ish-AOK/issues/640)), largest first,
  instead of one total per filesystem.
- **Chinese**: "Build" in the version line was translated as "version"
  ([#643](https://github.com/emkey1/ish-AOK/issues/643)); it is 构建 / 建置
  now.

## Crash fixes

- **The 557 crash in `sockrestart_on_resume`.** A wait on a set of sockets
  registered for the resume once per socket and unregistered once per socket,
  so another thread changing an epoll set in between left a freed task
  listed, and the next resume wrote through it. One registration per wait
  now, and a backstop where a task is freed.
- **iOS killing the app as it suspends (`0xdead10cc`)**, the commonest
  signature over 549-555. The filesystem's write gate was lifted before the
  background assertion that should hold off the suspension was taken; it now
  opens only under an assertion with at least ten seconds left.
- **A native program's `abort()` took the app with it.** It is the
  program's now: it dies of `SIGABRT` and the shell says "Aborted". This was
  558's one new crash, native helix in a tokio panic, and the kqueue shim
  tokio runs on had four faults of its own: queues shared between programs,
  a drained descriptor never watched again, a registration that did not wake
  the sleeper, and two threads collecting one signal.
- **A bad descriptor number in `SCM_RIGHTS`** (a closed one, or a failed
  open's -1) crashed the emulator. It is `EBADF`, as on Linux.
- **Native applets ran under each other's names** now and then: Darwin's
  `basename()` answers from one buffer for the whole app, and two applets
  starting together could read each other's. `basename`, `dirname`,
  `strtok`, `gmtime` and `asctime` are per thread for native programs.
- **No gadget holds a vector register across a call into C** any more.
  558's A10X blocker was one that did (the arm64 memset kept its fill
  pattern in a register a helper could change, and gcc died one compile in
  five); a checker now reads every built gadget object for the same mistake,
  in the test suite, and found nothing else.

## Kernel and conformance

- **Guest faults reach the kernel log only when they kill the program**, as
  on Linux, ratelimited. A JVM's null checks, or a test that traps 61,440
  times, used to write a full report each: three suite runs filled a phone.
  `/proc/ish/fault_reports` sets the mode (`unhandled`, `all`, `none`).
- **The host's descriptor limit is raised at boot.** Every guest file,
  socket and pipe is a descriptor of the one app process; it started at 4864
  on an M4 iPad and is raised to 10240. `/proc/ish/host_fds` reports the
  count by kind.
- **realfs follows no host symlink anywhere in a path.** A directory swapped
  for a symlink between the guest's walk and the host call was followed on
  the host, out of a chroot.
- **Misaligned `LOCK` operations no longer starve their siblings** on x86
  guests (558's known issue): `amd64_xchg_mem` took 19 to 528 seconds and
  takes 1 to 3.
- **A timed `FUTEX_WAIT` said it was woken when nobody woke it**: a
  sibling's `munmap` ended the sleep early and returned 0.
- **A thread's reading of its own CPU time** was stale on the A10X (558's
  known issue).
- **Signals**: a group signal from inside a pid namespace reaches the
  group's members outside it, and `si_pid` is the sender as the receiver can
  see it.
- **The arm64 guest's ID registers**: `mrs` of MIDR_EL1, MPIDR_EL1 and
  REVIDR_EL1 read what Linux emulates for EL0, and the rest of the ID space
  reads zero instead of faulting. `FCVTL`/`FCVTN` between half and single
  precision are implemented.

## Wayland and the GPU

- **OpenGL on the GPU with Mesa 25.2 and later.** Alpine 3.24's and Arch's
  Mesa refused to start zink without robustness2's `nullDescriptor`, which
  the GPU path now offers; on those roots every OpenGL program used to render
  in software (558's known issue). `setup-gpu.sh --check` passes on Alpine
  3.24; full OpenGL programs and compositors there have had little testing.
- `setup-wayfire.sh` no longer refuses on Arch, though nobody has run a
  Wayfire session there yet.

## Native programs

- **`passwd`** changes passwords; it said "not supported on this platform".
- **`ping` and `wget` take the options Linux scripts pass them**: iputils'
  `-W`, `-w`, `-i`, `-s`, `-t`, `-I`, `-q`, `-D`; GNU wget's `-T`, `-t`,
  `-U`, `-P`, `-S`, `--spider`, `-o`, with GNU's exit statuses, so a 404 is
  a failure rather than a saved error page. In a sandbox, `ping` falls back
  to a raw socket.

## The app

- **The File Provider is "iSH-AOK" in Files**, not upstream's "iSH".
- **An Arch root says that pacman needs `/AOK/fixes/arch/fix-pacman.sh`** at
  login, for as long as one of the three things it fixes is still unfixed.
- **Keep Screen Turned On** is applied again whenever a window becomes
  active; it was set once at launch.
- **VoiceOver**: the Workspace theme list reads each theme's name, and the
  font picker, theme editor, New Tab and Diagnostics buttons have hints, in
  all nine languages.
- The iPhone 18 Pro and Pro Max are named in `/proc/cpuinfo`.
- `/AOK/docs` has a new `namespaces.md`, and the docs and the book were
  checked against this build.

## Testing

The full regression suite on the five release roots (Alpine 3.24 i386,
amd64, arm64 and riscv64, and Devuan 6 arm64 with glibc) on an M5 Mac:
380-469 passes per root. The one failure on every root was two tests that
still expected the `/proc/<pid>/ns/cgroup` and `ns/time` files this build
deliberately removed; they are corrected. Namespace and FUSE tests were
checked against Linux 6.12 as an unprivileged user first.

## Known issues

- **A session with a sandbox in it cannot be saved**, and Save Anyway does
  not help (see Before you update).
- **Sandboxes cannot reach the network** through a veth: there is no
  forwarding or bridge yet.
- **ssh sessions still drop when the device locks**
  ([#631](https://github.com/emkey1/ish-AOK/issues/631)): iOS takes a
  suspended app's connections. `cat /dev/location > /dev/null &` keeps the
  app running, at some cost in battery; how long iOS honours it on a locked
  device has not been measured.
- **A native program's own threads share one task** and are not stopped
  when the program ends.
- **Native zsh cannot run one command with more than 128 KB of arguments**
  (`rm *` over a few thousand files). `sh` and the tools themselves take
  2 MB.
- **Xfce draws no desktop backdrop**: icons on black.
- **On distribution roots, `who` misses the app terminal's own login.**
