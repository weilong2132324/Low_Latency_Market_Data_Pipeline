# Huge pages

`include/hugepage_alloc.hpp`'s `HugePageRegion` asks for 2MiB
`MAP_HUGETLB` pages and falls back to a normal mapping if none are
available. To actually get the huge-page path on Linux/WSL2:

```sh
# Check what's currently reserved:
cat /proc/sys/vm/nr_hugepages

# Reserve 64 huge pages (64 * 2MiB = 128MiB) - needs root, and may fail
# if there isn't enough *contiguous* free physical memory:
echo 64 | sudo tee /proc/sys/vm/nr_hugepages

# Verify:
grep Huge /proc/meminfo
```

Under WSL2 specifically: the setting above applies to the lightweight
VM WSL2 runs your distro in, not your Windows host, and un-reserving
huge pages on `wsl --shutdown` is normal (they are not persisted). If
`nr_hugepages` silently stays at 0 after the `echo`, the VM most likely
doesn't have enough contiguous free memory at that moment - this is
expected sometimes and is exactly why `HugePageRegion` always has a
fallback path instead of treating huge pages as guaranteed.

Run `./bin/test_hugepage_alloc` (via `make test`) to see which path was
actually taken on your machine - it prints `used_huge_pages() = true`
or `false` without failing either way.
