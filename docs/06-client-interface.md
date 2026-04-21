# 06 — IEEE-1275 client interface

After Phase 3 hands control to the OS, the firmware persists as a
callable service through the client interface (CI). The booted OS
invokes firmware services by calling the CI entry pointer passed
in r5 at boot-time with a single argument: a pointer to a
structured call descriptor.

This chapter specifies the calling convention, the required
services, and the Pegasos2-specific behaviour.

## Entry convention

At boot-time, the firmware publishes the CI entry:

```
r3 = 0
r4 = 0
r5 = &ci_handler      /* firmware-supplied function pointer */
```

The OS saves r5 somewhere (typically a well-known kernel global)
and invokes the CI by:

```
ci_handler(struct ci_call *args)
    where args has this shape:
    struct ci_call {
        u32 service;            /* pointer to null-terminated ASCII name */
        u32 nargs;              /* count of args entries */
        u32 nrets;              /* count of rets entries */
        u32 arg_or_ret[nargs + nrets];
    };
```

Register convention during a CI call:

- r3 = args pointer (the call descriptor).
- Return value: CI handler sets r3 = 0 for success, non-zero for
  "service not supported". The actual service return values go
  into the `arg_or_ret[nargs..nargs+nrets-1]` slots BEFORE the
  handler returns.
- All other GPRs preserved by the CI handler.
- No FPR / VR usage by CI.
- MSR state preserved.

The handler runs in the OS's context — same MSR, same stack. That
means the CI handler MUST:

- Not trigger an external interrupt with its own state leaking.
- Not assume a private stack (reserve firmware scratch via r1
  adjustment if needed).
- Tolerate being called from any CPU mode the OS might use
  (supervisor mode almost always; if the OS ever calls CI from
  user mode — unusual but the standard allows it — return an
  error).

## Required services

Per IEEE-1275 §6 and its Pegasos2 bindings. Each is invoked by
name via the `service` field.

### Device tree

| Service name            | Args                 | Returns        |
|--------------------------|----------------------|----------------|
| `finddevice`             | path                 | phandle        |
| `getproplen`             | phandle, name        | length         |
| `getprop`                | phandle, name, buf, buflen | actual length |
| `nextprop`               | phandle, prev-name, buf | flag       |
| `setprop`                | phandle, name, buf, len | actual length |
| `canon`                  | path, buf, buflen    | actual length  |
| `instance-to-path`       | ihandle, buf, buflen | actual length  |
| `instance-to-package`    | ihandle              | phandle        |
| `package-to-path`        | phandle, buf, buflen | actual length  |
| `child`                  | phandle              | phandle        |
| `peer`                   | phandle              | phandle        |
| `parent`                 | phandle              | phandle        |

### Device I/O

| Service | Args | Returns |
|---------|------|---------|
| `open`    | path              | ihandle     |
| `close`   | ihandle           | —           |
| `read`    | ihandle, buf, len | actual      |
| `write`   | ihandle, buf, len | actual      |
| `seek`    | ihandle, pos-hi, pos-lo | pos-hi, pos-lo |
| `call-method` | method-name, ihandle, args... | catchresult, rets... |

### Memory

| Service | Args | Returns |
|---------|------|---------|
| `claim`   | virt, size, align | base |
| `release` | virt, size | — |

### Control and configuration

| Service | Args | Returns |
|---------|------|---------|
| `boot`      | bootspec | — |
| `enter`     | — | — |
| `exit`      | — | — |
| `chain`     | virt, size, entry, args | — |
| `interpret` | cmd-string, args... | results... |
| `quiesce`   | — | — |
| `set-callback` | newfunc | oldfunc |
| `set-symbol-lookup` | func-ptr | — |

### Time of day

| Service | Args | Returns |
|---------|------|---------|
| `get-time-of-day` | — | year, month, day, hour, minute, second |
| `set-time-of-day` | year, month, day, hour, minute, second | — |
| `milliseconds`    | — | millis |

### SMP (multi-CPU — not used on Pegasos2 but defined for
compatibility)

| Service | Args | Returns |
|---------|------|---------|
| `start-cpu` | phandle, pc, arg | — |
| `stop-self` | — | — |
| `idle-self` | — | — |
| `resume-cpu`| phandle | — |

On Pegasos2 the CPU count is always 1; `start-cpu` for any phandle
other than `/cpus/cpu@0` returns an error.

## Quiesce semantics

When the OS calls `quiesce`:

1. The CI entry pointer remains valid but services are limited
   to `set-callback`, `interpret`, `exit`, and `chain`.
2. All device packages close (open ihandles remain valid but
   device-tree walking stops returning updated state).
3. Decrementer interrupts within firmware context stop firing.
4. The OS is responsible for all subsequent interrupts and the
   device state.

This is a one-way transition: after `quiesce`, `open` and most
services return errors. The OS uses `chain` to re-boot a
different image (the firmware retains enough state to load and
jump to a new boot image without re-initialising hardware).

## AmigaOS 4 amigaboot.of contract

Beyond the standard CI, AmigaOS 4's `amigaboot.of` expects:

1. A command-line argument `bootdevice=<partition-name>` passed
   via `boot hd:0 amigaboot.of bootdevice=DHY`.
2. The firmware's `boot` service passes this after the filename,
   in the `bootspec` arg.
3. `amigaboot.of` reads it from `/chosen/bootargs` after boot (the
   firmware must populate `/chosen/bootargs` with everything after
   the filename).
4. `amigaboot.of` uses `interpret` to execute Forth words
   `mounter`, `peg2ide.device:xxx`, etc. The firmware's
   `interpret` service must support these AOS-specific words
   (they're registered by AOS-aware device packages at boot).

The rewrite must preserve this contract exactly. See
`07-boot-loader.md` for how the boot-image flow and argument
parsing interact with amigaboot.of.

## Pegasos2-specific extensions

### `"screen-info"` package method

Pegasos2 Forth builds expose a `screen-info` method on the
`/packages/disk-label` node (legacy convention). It returns the
current resolution and colour depth of the console display, if
any. AOS 4's Workbench uses this during early setup.

### `"probe-self"` deferral

Some AOS drivers want to re-invoke `probe-self` on a device node
after OS boot. The firmware must keep the probe-self method
callable via `call-method` even after `quiesce` (with the caveat
that hardware state may have changed).

### RTC access

Pegasos2 conventions use `get-time-of-day` / `set-time-of-day`;
these must be backed by the M48T59 chip (see `08-nvram.md`).

## Error codes

CI-level errors are reported via r3 from the CI handler:

- 0 = success, check `arg_or_ret` for actual service result.
- -1 = service name unknown.
- -2 = invalid argument count.
- -3 = internal firmware error (handler should also have printed
  a diagnostic to the console).

Service-level errors are typically reported via a service-specific
"catch result" return value. For `call-method`, this is the first
return slot: 0 for success, non-zero for failure.

## Tests

1. Boot AmigaOS 4 install CD; MediaToolbox runs; HDD partitions
   are created and formatted; AOS installation completes; reboot
   brings up the installed system.
2. Boot Linux (Debian PowerPC); kernel reaches userspace; /proc/
   cpuinfo reflects the correct CPU model.
3. Boot MorphOS; desktop appears.
4. A synthetic CI harness (via `bootgen` or similar) calls every
   listed service at least once; each returns sensible values or
   the expected error.
5. `quiesce` followed by an attempt to use `open` returns the
   right error without crashing.
6. `chain` successfully re-boots an alternative kernel image.
