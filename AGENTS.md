# Repository Agent Instructions

These instructions apply to the whole repository.

## Driver Context

This is an out-of-tree Linux kernel driver for NVIDIA LSTP USB devices. Treat
USB URBs, locking, memory lifetime, sysfs/kobject lifetime, firmware nodes, and
subsystem registration as correctness-critical.

The driver must support ASIC, FPGA, and microcontroller-backed LSTP endpoints;
OpenBMC and Ubuntu-style hosts; dynamic firmware-reported channel discovery; and
static firmware-described configuration via device tree or ACPI. Do not narrow
any of those paths unless explicitly asked.

The LSTP protocol and userspace-visible interfaces are already deployed. Packet
formats, command/status values, channel types, names, sysfs hierarchy,
attributes, module parameters, device tree bindings, and device-node behavior
are compatibility surfaces. Extensions must remain backwards compatible with
existing firmware, device trees, scripts, and userspace tools.

## Kernel And Compatibility Work

When changing memory management, locking, lifetime, URB behavior, wait queues,
workqueues, IRQ handling, kobjects, sysfs, fwnodes, or subsystem registration,
verify the relevant kernel implementation directly. Prefer `$KERNEL_SRC`; else
use `/lib/modules/$(uname -r)/build`; for compatibility failures, inspect the
specific kernel tree being tested. For high-risk lifetime or concurrency
changes, mention which kernel source paths or APIs were checked.

Prefer newer kernel APIs when they are safer or cleaner. If older supported
kernels lack them, isolate differences in a compatibility header instead of
spreading version checks through driver code.

## Wire-Bound Structures

Every struct that crosses the LSTP wire is `__packed` with `__le*`/`__be*`
on multi-byte fields and plain `u8` for byte fields (`__u*` is UAPI-only).
Convert via `cpu_to_le*()` / `le*_to_cpu()` at field access.

Field access through a `__packed`-struct pointer is alignment-safe; do not
double-wrap with `get_unaligned_le*()` (use those only on raw byte buffers).

Promote each opcode variant to a top-level
`<subsystem>_<operation>_(req|resp)` struct -- no anonymous-struct unions
over opcodes.

Anchor cross-file wire-size assumptions with `BUILD_BUG_ON` at the consumer,
e.g. `BUILD_BUG_ON(sizeof(struct lstp_header) + sizeof(*req) > LSTP_USB_EP_MIN_SIZE)`.

## Stack-Allocated Request/Response Conventions

Per-call request/response structs use designated initializers in their
declaration, scoped to the branch that uses them. Name the local plainly
(`req`, `resp`); the struct type already carries the subsystem prefix.

## Formatting

The repo ships a `.clang-format` at the root. Run `clang-format-19 -i` on every
`.c`/`.h` file you touch before staging the final commit, and before every
amend that adjusts touched lines. Hand-resolved conflicts and partial rewrites
routinely leave wrap points that no longer match the column limit; reviewers
flag the resulting drift even when the logic is fine.

Limit the format pass to the files this change actually touches -- do not
sweep unrelated source files in the same commit. For touch-only formatting on
changed lines against the parent commit,
`git clang-format --binary clang-format-19 HEAD~1` is also available.
