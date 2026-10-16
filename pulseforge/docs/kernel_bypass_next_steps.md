# Kernel bypass: what's built here vs. what real hardware buys you

This project runs in WSL2 - a VM, with a virtual NIC, no PCI passthrough
to a real network card. That rules out the deepest kernel-bypass tools
honestly, so this doc is split into "what's actually in this repo and
runnable" and "what a real deployment would add, and why it needs real
hardware."

## Built and runnable here

- **Classic BPF socket filter** (`include/bpf_filter.hpp`,
  `--bpf-filter` on the receiver) - drops invalid datagrams in the
  kernel, before they're copied to userspace. No toolchain beyond the
  compiler; classic BPF has been in Linux since 2.1.
- **`recvmmsg()` batching** (`--batch N`) - one syscall for up to N
  datagrams instead of one syscall per datagram.
- **`SO_BUSY_POLL`** (`--busy-poll US`) - asks the driver to poll for
  packets instead of waiting for an interrupt. Soft-fails cleanly here
  because loopback/virtual NICs don't implement NAPI busy-polling the
  way a real 10/25/100GbE NIC does - the setsockopt succeeds, the
  latency win just isn't there on this hardware.
- **Huge pages** (`include/hugepage_alloc.hpp`) - `MAP_HUGETLB` with a
  measured, honest fallback (see `docs/hugepages.md`).
- **CPU pinning** (`--pin-cpu`, `include/cpu_util.hpp`) - one piece of
  the real isolation story; see below for the rest.

## What real hardware adds, and why this project can't

**eBPF/XDP.** This repo's classic-BPF filter is the spiritual ancestor
of XDP (eXpress Data Path): an eBPF program attached at the NIC driver
level, running before the kernel even allocates an `sk_buff` for the
packet - able to drop, redirect, or forward packets at line rate with
no copy at all. Running real XDP needs: a NIC driver that implements
the XDP hook (most real 10GbE+ NICs do; WSL2's virtual NIC does not),
`libbpf` + `clang` to compile the eBPF bytecode, and (for anything
beyond `DROP`) a `BPF_MAP` for coordination with userspace. The classic
BPF filter in this repo is the closest approximation buildable without
that stack.

**DPDK.** Kernel bypass taken all the way: DPDK maps the NIC's receive
and transmit queues directly into a userspace process via `UIO`/`VFIO`,
skipping the kernel network stack entirely - no syscalls, no
interrupts, no `sk_buff` allocation, just the userspace process polling
NIC descriptor rings directly. This needs a DPDK-supported physical
NIC, `hugepages` (real ones, reserved at boot), and - critically - that
NIC bound to the host, not shared with a VM's virtualized network
stack. None of that is available inside WSL2's architecture: the "NIC"
WSL2 sees is already a software construct, several layers removed from
real hardware. A from-scratch DPDK study would run on bare-metal Linux
with a supported NIC (Intel ixgbe/i40e, Mellanox mlx5, etc.) - genuinely
a different machine, not a flag on this project.

**CPU isolation, the rest of the story.** `--pin-cpu` pins one thread;
a real low-latency deployment goes further at the OS level:
`isolcpus=` (or newer `nohz_full=`/`rcu_nocbs=`) kernel boot parameters
to keep the scheduler and RCU callbacks off specific cores entirely, so
a pinned thread never gets preempted by anything the kernel decides to
run. `taskset -c N ./bin/receiver` is the userspace-only approximation
this project can demonstrate without touching boot parameters.

**The honest summary for an SRE interview:** this repo demonstrates the
*mechanism and the mental model* of kernel bypass (why it helps, what
layer each technique removes) using everything that's actually
reachable from a WSL2 dev box. Production DPDK/XDP debugging is a
different skill built on the same foundation - this project is the
foundation.
