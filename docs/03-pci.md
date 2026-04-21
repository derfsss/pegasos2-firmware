# 03 — PCI: host bridges, configuration cycles, enumeration

This is the largest and most safety-critical chapter of the spec
because bug #2 (PCI walker ignoring bridges) motivates half of the
rewrite. The new implementation must produce a PCI device tree
complete enough that OSes can find every reachable device.

## Host bridges (MV64361)

The Pegasos II has two independent PCI host bridges, both
integrated in the MV64361. The firmware sees them as `/pci0` and
`/pci1` in the device tree (per Pegasos II convention) but they are
otherwise symmetric. The host bridges differ only in the base
address of their internal register banks.

Each host bridge drives a single primary PCI bus. PCI0 is typically
used for on-board devices (VT8231 southbridge, on-board Ethernet,
firmware flash bridging). PCI1 typically hosts the AGP slot and
external PCI slots. Both layouts are community conventions, not
architectural requirements — the firmware MUST NOT hard-code device
placement beyond the minimum needed to initialise the console.

## Address map seen by the CPU

From the MPC7447 the CPU sees the MV64361 resources as:

| Region (CPU-side)            | Direction | Purpose |
|------------------------------|-----------|---------|
| `0xF1000000..0xF100FFFF`     | reads/writes | MV64361 internal register bank — not a PCI resource |
| `0xF1000CF8` (4 B)           | write-only* | **PCI0 Configuration Address** register (external config cycles) |
| `0xF1000CFC` (4 B)           | read/write  | **PCI0 Configuration Data** register |
| `0xF1008CF8` (4 B)           | write-only* | **PCI1 Configuration Address** register |
| `0xF1008CFC` (4 B)           | read/write  | **PCI1 Configuration Data** register |
| `0xF1000C78` (4 B)           | read/write  | PCI0 "internal" self-config register — alternate path that lets the host bridge access its OWN PCI configuration space without emitting a cycle on the bus |
| `0xF1000C7C` (4 B)           | read/write  | PCI0 internal self-config data |
| `0xF1008C78` / `0xF1008C7C`  | read/write  | PCI1 internal self-config registers (same pair, mirrored at +0x8000) |

\* Reads from CfgAddress are legal but meaningless.

The Configuration Address register layout follows PCI 2.3:

```
bit 31        : enable (must be 1 to issue a cycle)
bits 30..24   : reserved (write 0)
bits 23..16   : bus number
bits 15..11   : device number
bits 10..8    : function number
bits  7..2    : register number (dword-aligned)
bits  1..0    : cycle type (00 = Type 0, 01 = Type 1)
```

## Type 0 vs Type 1 configuration cycles

Per the PCI 2.3 specification:

- **Type 0** cycles address a device on the bridge's **primary** bus.
  The host bridge decodes bits 15..11 (device number) to generate
  the IDSEL strobe for the corresponding slot.
- **Type 1** cycles address a device on a **non-primary** bus. The
  host bridge does not decode the device number itself; it just
  passes the cycle to downstream PCI-to-PCI bridges, which either
  service the cycle (if the bus number matches their secondary
  bus) or forward it further.

**Consequence for the firmware:** a walker that issues only Type 0
cycles can only discover devices on the two primary buses (pci0.0
and pci1.0). Devices behind any PCI-to-PCI bridge (including AGP
risers, PCI-to-PCIe converters, multi-slot backplanes) are
unreachable.

The new BIOS MUST issue Type 0 cycles for devices on the host
bridge's primary bus and Type 1 cycles for all other buses. The
selection is a bit-0 flip in the Configuration Address word.

### Machdep `config-*` primitives

The Forth layer exposes six config-access words per IEEE-1275:

| Word        | Stack effect              | Width |
|-------------|----------------------------|-------|
| `config-b@` | ( config-addr -- byte )    | 8-bit |
| `config-b!` | ( byte config-addr -- )    | 8-bit |
| `config-w@` | ( config-addr -- word )    | 16-bit |
| `config-w!` | ( word config-addr -- )    | 16-bit |
| `config-l@` | ( config-addr -- quadlet ) | 32-bit |
| `config-l!` | ( quadlet config-addr -- ) | 32-bit |

`config-addr` is the IEEE-1275 PCI configuration address encoding:
bits 23..16 bus, 15..11 device, 10..8 function, 7..2 register
(bits 31..24 and 1..0 zero). The primitive's implementation MUST:

1. Determine the correct host bridge (pci0 or pci1) from context.
   The Forth word is invoked through a device package that knows
   which host it belongs to.
2. If the bus number equals the host bridge's primary bus AND the
   device is the host bridge itself (dev=0, fn=0), use the internal
   self-config register pair (`+0xC78/+0xC7C`). This avoids routing
   a cycle that addresses the host bridge to its own PCI bus.
3. Otherwise, write the full Config Address word to `+0xCF8` (bit 31
   set, type bit correctly set from bus==primary), then read or
   write `+0xCFC`.

Pseudocode (new impl — written from scratch, NOT paraphrased from
the original):

```
word config-l@ ( config-addr -- quadlet )
    unpack config-addr into (bus, dev, fn, reg)
    if bus == host.primary and (dev, fn) == (0, 0):
        return mv64361_reg_l@( host.internal_base + 0xC7C )
    else:
        cycle_type = (bus == host.primary) ? 0 : 1
        ca = (1<<31) | (bus<<16) | (dev<<11) | (fn<<8) | (reg & 0xFC) | cycle_type
        mv64361_reg_l!( host.cfg_addr, ca )
        return mv64361_reg_l@( host.cfg_data )
```

`config-b@` / `config-w@` must issue a 32-bit config-data read and
extract the requested byte-lane; OR use the MV64361's byte-enable
feature in the Config Address byte-enable field (bits 1..0 when the
chip is configured for partial transfers — consult the MV64361
datasheet §14.1). The firmware MAY pick either approach; the OS-
visible behaviour must match a correct PCI 2.3 implementation.

## PCI enumeration algorithm

This section replaces §"Required behaviour" of bug #2 in
`09-known-bugs.md` with a complete reference algorithm. The
implementation MUST be behaviourally equivalent.

```
ENUMERATE_HOST(host_bridge):
    host.primary_bus       = 0
    host.next_bus_number   = 1
    /* host bridge itself appears at (bus=0, dev=0, fn=0) */
    CREATE_NODE_FOR_BRIDGE(host_bridge, bus=0)
    ENUMERATE_BUS(host, bus=0)

ENUMERATE_BUS(host, bus):
    for dev in 0..31:
        vendor = CONFIG_L_READ(host, bus, dev, fn=0, reg=0x00)
        if (vendor & 0xFFFF) == 0xFFFF:
            continue             /* no device in this slot */
        header_type = CONFIG_B_READ(host, bus, dev, fn=0, reg=0x0E)
        is_multi_function = (header_type & 0x80) != 0
        for fn in 0..(is_multi_function ? 7 : 0):
            if fn > 0:
                vendor = CONFIG_L_READ(host, bus, dev, fn, reg=0x00)
                if (vendor & 0xFFFF) == 0xFFFF:
                    continue
                header_type = CONFIG_B_READ(host, bus, dev, fn, reg=0x0E)
            class_code = CONFIG_L_READ(host, bus, dev, fn, reg=0x08) >> 8
            CREATE_DEVICE_TREE_NODE(host, bus, dev, fn, vendor, class_code, header_type)
            if (header_type & 0x7F) == 0x01 and class_code == 0x060400:
                /* PCI-to-PCI bridge */
                RECURSE_INTO_BRIDGE(host, bus, dev, fn)

RECURSE_INTO_BRIDGE(host, bus, dev, fn):
    secondary_bus = host.next_bus_number
    host.next_bus_number += 1

    /* Programme the bridge's bus-number registers BEFORE probing the far side. */
    CONFIG_B_WRITE(host, bus, dev, fn, reg=0x18, value=bus)               /* PRIMARY */
    CONFIG_B_WRITE(host, bus, dev, fn, reg=0x19, value=secondary_bus)    /* SECONDARY */
    CONFIG_B_WRITE(host, bus, dev, fn, reg=0x1A, value=0xFF)              /* SUBORDINATE (temp) */

    /* Temporarily flush the CPU side of any cached ConfigAddress state. */
    mv64361_reg_l!( host.cfg_addr, 0 )

    /* Walk the secondary bus. */
    ENUMERATE_BUS(host, bus=secondary_bus)

    /* Collapse subordinate back to the largest bus number actually
       assigned below this bridge. */
    CONFIG_B_WRITE(host, bus, dev, fn, reg=0x1A, value=host.next_bus_number - 1)

    /* Update device-tree properties on the bridge node now that we
       know its real children. */
    UPDATE_BUS_RANGE_PROPERTY(bridge_node, secondary_bus, host.next_bus_number - 1)
```

### Notes on the algorithm

1. `next_bus_number` is per-host-bridge; the two hosts have
   independent numbering spaces. Real hardware has distinct bus-
   number counters per host.
2. The temporary SUBORDINATE=0xFF during recursion is required so
   that Type 1 cycles for not-yet-assigned-bus-numbers pass through
   the bridge. Collapse it afterward so OS view is accurate.
3. The `mv64361_reg_l!( host.cfg_addr, 0 )` between bus-number
   programming and the recursive probe is a defensive flush — some
   MV64361 revisions latch the most-recent Config Address value for
   an extra cycle. Writing 0 ensures the next primitive starts
   fresh.
4. Header-type bit 7 ("multi-function") must be read from function
   0 before attempting functions 1..7. If it's clear, single-
   function; skip the inner loop.
5. Class code `0x060400` (PCI-to-PCI bridge, Normal Decode) is the
   one we must recurse into. `0x060401` (Subtractive Decode) should
   also recurse but typically has no BARs to programme.
6. PCI-X and PCIe bridges present themselves as PCI-to-PCI via the
   same class code; no special handling is required to walk through
   a PCI-to-PCIe converter.

## Required device-tree nodes for PCI

Per IEEE-1275 PCI binding and Pegasos II convention:

### Host bridge node

Path: `/pci@<base-address>` (e.g. `/pci@80000000` for PCI0,
`/pci@C0000000` for PCI1 — pick per the MV64361 address-decode
window base).

Required properties:

| Property name       | Encoding | Contents |
|---------------------|----------|----------|
| `name`              | string   | `"pci"` |
| `device_type`       | string   | `"pci"` |
| `compatible`        | string-list | `"pegasos2-host", "mv64361-pcihost", "pci"` |
| `#address-cells`    | int      | 3 (bus/dev/fn, unit address, offset) |
| `#size-cells`       | int      | 2 (size is 64-bit) |
| `#interrupt-cells`  | int      | 1 |
| `reg`               | prop-encoded-array | the MV64361 register bank + the host's config-space window |
| `ranges`            | prop-encoded-array | maps PCI (bus-centric) addresses to CPU-physical addresses for I/O, mem32, mem64 |
| `bus-range`         | 2 ints   | `{0, host.max_bus_assigned}` |
| `interrupt-map`     | prop-encoded-array | per-slot IRQ routing (4 pins × slots) |
| `interrupt-map-mask`| prop-encoded-array | mask for matching interrupt-map |
| `clock-frequency`   | int      | PCI bus clock (typically 33 MHz) |

### Per-device node

Path: `/pci@<host>/<device_type>@<dev>[,<fn>]`

Device-type naming follows IEEE-1275 PCI binding:
- `ethernet` (class 0x02xx)
- `display` (class 0x03xx)
- `scsi` (class 0x0100)
- `ide` (class 0x0101)
- `usb` (class 0x0C03)
- `pci-bridge` or simply `pci` (class 0x0604)
- else class-string from the class code.

Required properties:

| Property         | Contents |
|------------------|----------|
| `name`           | Device-type string from above |
| `vendor-id`      | u32 from config offset 0x00 (low 16 bits) |
| `device-id`      | u32 from config offset 0x02 (low 16 bits) |
| `revision-id`    | u32 from config offset 0x08 |
| `class-code`     | u32 = (class<<16)|(subclass<<8)|prog-if |
| `subsystem-vendor-id` | u32, optional per PCI spec (offset 0x2C) |
| `subsystem-id`   | u32, optional (offset 0x2E) |
| `reg`            | prop-encoded-array: BAR descriptors (see IEEE-1275 PCI binding §4) |
| `assigned-addresses` | prop-encoded-array: BARs after firmware assignment |
| `interrupts`     | prop-encoded-array: interrupt-specifier |
| `device_type`    | Same as `name` for most devices |

### Per-bridge node (class 0x0604)

Additional properties over the per-device set:

| Property           | Contents |
|--------------------|----------|
| `#address-cells`   | 3 (same as host bridge) |
| `#size-cells`      | 2 |
| `bus-range`        | 2 ints: `{secondary, subordinate}` |
| `ranges`           | prop-encoded-array: translates this bridge's bus-child addresses to parent-bus addresses; populated from MEM_BASE/MEM_LIMIT/IO_BASE/IO_LIMIT registers |
| `pci-bridge-number`| the bridge's own bus number (the secondary bus of the parent, equal to this bridge's own "bus" in the 3-cell address) |

The presence of `pci-bridge-number` on bridge nodes is what the
stock firmware's downstream ancestor-walking code looks for (the
string is present in `.rodata` with two reader xrefs even in the
buggy stock firmware — it just never gets written because the
enumerator never creates bridge nodes). The new BIOS must write it.

## BAR allocation

For each device, the firmware allocates I/O and memory ranges from
the host bridge's windows. The algorithm is standard:

```
for each BAR on each device:
    probe size by writing 0xFFFFFFFF and reading back
    (size = ~masked_value + 1 for memory BARs; different mask for I/O)
    choose alignment = size, chosen from the appropriate free list
      (I/O < 4 GiB, mem32 < 4 GiB, prefetch-mem32, mem64)
    write the assigned base back to the BAR
    populate the `assigned-addresses` property

for each bridge:
    choose MEM_BASE = lowest-addressed assigned memory in subtree,
          rounded down to 1 MiB
    choose MEM_LIMIT = highest-addressed + size, rounded up - 1
    same for I/O (4 KiB alignment)
    programme bridge config regs 0x20..0x23 (mem), 0x1C..0x1D (io)
    populate bridge's `ranges` property accordingly
```

Pegasos II slots support 32-bit PCI at 33 MHz. 64-bit BARs exist
only on later-revision cards behind the PCIe riser; the firmware
must handle them in `ranges` by declaring a 64-bit prefetchable
child range on any bridge that's behind a 64-bit-capable slot.

## Interrupt routing

Pegasos II routes PCI `INTA#..INTD#` through the MV64361's internal
interrupt controller. The firmware builds `interrupt-map` on each
PCI host bridge such that:

```
child-unit-address (3 cells), child-interrupt-specifier (1 cell)
  -> parent-phandle, parent-interrupt-specifier (n cells)
```

For the MV64361's internal IC, `parent-interrupt-specifier` is the
IC input number (documented in the Marvell datasheet's interrupt
cause/mask section). The specific slot-to-INTA/B/C/D routing is
Pegasos II board-specific and documented in the service manual;
the new firmware MUST derive it from a per-board constants table,
not from device-tree metadata.

## Required Forth words

Beyond the `config-*` primitives already covered, the following
IEEE-1275 PCI binding words must be implemented:

| Word                   | Purpose |
|------------------------|---------|
| `map-in` / `map-out`   | CPU-side map of a device BAR to a virtual address |
| `dma-alloc` / `dma-free` | Allocate a DMA-coherent buffer |
| `dma-map-in` / `dma-map-out` | Build/tear-down a scatter-gather map |
| `dma-push` / `dma-pull` | Cache maintenance for non-coherent DMA |
| `decode-unit` / `encode-unit` | Parse/format a PCI unit-address string |
| `open` / `close`       | IEEE-1275 package instantiation |
| `probe-all`            | Run the enumeration algorithm above |
| `probe-self`           | Run enumeration for one device, installing FCode if present |

## Tests the implementation must pass

1. **Single-bridge visibility.** With QEMU
   `-device pci-bridge,id=pbr1,bus=pci.1,chassis_nr=1,addr=0x5
    -device e1000,bus=pbr1,addr=0x1`, running `show-devs /pci@<host>`
   must list both the bridge and the Ethernet controller behind it.
   The bridge node must have `bus-range {1 1}` and
   `pci-bridge-number 1`.

2. **Nested-bridge visibility.** With an additional
   `-device pci-bridge,id=pbr2,bus=pbr1,chassis_nr=2 -device
    some-device,bus=pbr2`, the top-level bridge must have
   `bus-range {1 2}` and the sub-bridge `{2 2}`. Both device nodes
   must appear.

3. **Onboard still works.** The baseline set of onboard devices
   (VT8231 SuperIO, IDE, USB, AC97, MC97; SysKonnect GbE on real
   HW or e1000 in QEMU) must still be discovered, with the same
   device-type names and property shapes as current community
   tools expect.

4. **BAR conflicts.** A synthetic test with multiple devices whose
   BAR sizes exceed the host bridge's memory window must fail
   gracefully — the firmware logs a warning and continues without
   that device, rather than overlapping allocations.

5. **Type 1 address construction.** A unit test at the primitive
   level writes a known ConfigAddress for (bus=3, dev=5, fn=2,
   reg=0x10) and reads it back via a mocked MV64361; the stored
   value must have bit 31 set, bit 0 set (type 1), and the
   bus/dev/fn/reg fields correctly encoded.

6. **Onboard Ethernet DHCP.** With `-netdev user,id=net0 -device
   rtl8139,netdev=net0,bus=pci.1` the firmware's NET boot path
   (via `boot net`) must be able to issue a DHCP request and
   receive a reply.

## What NOT to do

- Do not copy the stock firmware's two-tiered Config-access
  mechanism (the internal vs external register pairs) blindly.
  Follow the MV64361 datasheet; the original's pair selection is
  correct but its upper layers miss the Type-1 bit entirely.
- Do not hard-code device placements per Pegasos II board
  revision. Enumerate; build the device tree from what's found.
- Do not assume single host bridge. The two hosts have independent
  bus-number counters and their own device trees.
