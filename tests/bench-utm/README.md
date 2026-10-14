# Moved: AOK VM is a project of its own

What was here (the engine race, the QEMU gadget plug-in's gates and
benchmarks, the UTM SE build that became the AOK VM app, the ready-made
machines) is the `aok/` directory of the AOK VM repository, emkey1/aok-vm,
since 2026-10-14, with `docs/utm_engine_study.md` as its
`aok/docs/engine-study.md` and its open questions in `aok/TODO.md`. The
engine is emkey1/qemu; the gadget sources that this JIT and that engine
both build are emkey1/aok-gadgets.

`utm/vm-update.sh` stays: a "Devuan Ultimate" machine's `aok-update`
fetches it from this address, and what it installs is this repository's
`opt/AOK/tools`.
