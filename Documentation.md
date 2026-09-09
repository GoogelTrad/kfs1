# Documentation for KFS-1

This document explains the compiler flags, architectural concepts, boot flow, and
screen display mechanics used to build the KFS-1 kernel.

---

## 1. Compiler & Linker Flags

The entries below follow the order the flags appear in the `Makefile`
(`CFLAGS` first, then `LDFLAGS`).

### `-fno-builtin`

- **What it does?** Stops the compiler from recognizing standard library
  functions (`memset`, `memcpy`, `strlen`, `printf`, ...) as "built-ins" that it
  can inline, constant-fold, or silently replace with a call to another library
  function.
- **Why we need it?** By default the compiler knows the semantics of these
  functions and feels free to rewrite your code: a loop that zeroes an array can
  be turned into a call to `memset`, and `printf("hi\n")` can be downgraded to
  `puts`. In a freestanding kernel those target functions live in a C library we
  don't link, so the substitution produces unresolved-reference errors at link
  time (or infinite recursion when your own `memset` gets "optimized" into a call
  to itself). `-fno-builtin` forces the compiler to emit exactly the calls you
  wrote and use your implementations from `helpers.c`.

### `-fno-exceptions`

- **What it does?** Disables C++ exception handling (`throw` / `try` / `catch`)
  and lets the compiler assume no exception can ever propagate, so it emits no
  stack-unwinding tables (`.eh_frame`) or landing-pad code. For the C files in
  this project it is close to a no-op, but it is kept as a guard so the moment
  any C++ (or `-fexceptions` C) is added, exceptions stay off.
- **Why we need it?** If exceptions were enabled, `throw` would rely on a runtime
  unwinding library that walks the call stack frame by frame
  (`__cxa_throw`, `_Unwind_Resume`, `_Unwind_RaiseException`, provided by
  `libgcc` / `libstdc++`). That library is not linked in a freestanding kernel,
  so an enabled-but-unlinked exception path produces unresolved-reference errors,
  and an actual `throw` at runtime with no unwinder would be an unrecoverable
  crash. Turning the feature off also makes generated code smaller and removes
  the hidden control-flow paths on every call that might throw.

### `-fno-stack-protector`

- **What it does?** Disables the automatically injected stack-canary checks.
  Many host GCC builds default to `-fstack-protector-strong`, so this flag is an
  explicit opt-out rather than a change from a neutral default.
- **Why we need it?** With the protector on, the compiler adds code to the
  *prologue* of at-risk functions that copies a secret value from the global
  `__stack_chk_guard` onto the stack just past the return address, and code to
  the *epilogue* that re-checks it; on mismatch it calls `__stack_chk_fail`.
  Both `__stack_chk_guard` and `__stack_chk_fail` are supplied by the host C
  library, which we do not link, so leaving the protector on gives
  unresolved-reference errors at link time. (You could instead provide your own
  `__stack_chk_guard` / `__stack_chk_fail`, but for KFS-1 it is simpler to turn
  the feature off.)

### `-fno-rtti`

> Currently commented out in the `Makefile` (`#-fno-rtti`). It is a C++-only
> option: passing it while compiling C makes GCC emit the warning
> *"command-line option '-fno-rtti' is valid for C++/ObjC++ but not for C"*.
> Keep it commented until (and unless) the project starts compiling C++.

- **What it does?** Disables Run-Time Type Information (specific to C++).
- **Why we need it?** In C++, RTTI allows features like `dynamic_cast` and
  `typeid` to inspect an object's type at runtime. To do this, the compiler
  generates extra data tables inside the binary and relies on runtime
  type-checking code. Disabling it saves space, speeds up execution, and prevents
  compiler calls to non-existent runtime libraries.

### `-nostdlib`

- **What it does?** A `gcc` driver option for the link step: do not use the
  standard system startup files *and* do not use the standard system libraries.
  It is the union of two finer flags — `-nostartfiles` (drop `crt0.o` /
  `crti.o` / `crtbegin.o` and friends) and `-nodefaultlibs` (drop `libc`,
  `libgcc`, `libm`, `libstdc++`, ...). Only the objects and libraries you name
  explicitly are linked.
- **Why we need it?** Normally the driver injects C-library startup code that
  runs before `main` (sets up `argc`/`argv`, `atexit`, the C runtime) and links
  `libc` for `printf`, `malloc`, `exit`, and so on. All of that reaches the OS
  through Linux system calls. Your kernel *is* the operating system, so it must
  start at its own `_start` (from `boot.s`, selected by `linker.ld`) with no libc
  behind it; linking the host startup files or libraries would drag in
  system-call stubs and prevent the kernel from booting.

### `-nodefaultlibs`

- **What it does?** Disables only the *default libraries* at link time (`libc`,
  `libgcc`, `libm`, `libstdc++`, ...), while still linking the standard startup
  files. It does **not** control `crt0.o` / `crti.o` — that is `-nostartfiles`.
- **Why we need it?** Even with no explicit `-l` flags, the driver appends the
  default libraries automatically; a single call to `memcpy` or a 64-bit
  division the compiler lowers to a `__udivdi3` helper would then pull code out
  of `libc` / `libgcc`. `-nodefaultlibs` blocks that.
- **Note:** Since `LDFLAGS` already passes `-nostdlib`, which *includes*
  `-nodefaultlibs`, listing `-nodefaultlibs` as well is redundant (harmless —
  it just makes the intent explicit).

### `-T linker.ld`

- **What it does?** Replaces the linker's built-in default linker script with
  `linker.ld` from this repo.
- **Why we need it?** The default script lays a binary out for a hosted Linux
  process (high load address, dynamic interpreter, libc startup). Our script
  sets the entry point to `_start`, places the image at `. = 1M`, and forces the
  Multiboot header to the front of `.text` so GRUB accepts the kernel. See
  section 3, *Step 4*, for the details.

### Standard flags (`-m32`, `-O2`, `-Wall`, `-Wextra`, `-std=gnu99`)

These are not kernel-specific, but for completeness:

- **`-m32`** — generate 32-bit x86 code and objects. Must be identical for every
  compile, assemble, and link step, or the objects will not link. See section 2,
  *Target Architecture*.
- **`-O2`** — optimisation level 2. Mostly a normal speed/size choice, with two
  bare-metal caveats: optimisation is what makes the compiler turn loops into
  `memset`/`memcpy` calls (hence `-fno-builtin`), and it can legally reorder or
  drop reads/writes made through a plain pointer to memory-mapped hardware. The
  VGA buffer pointer (`terminal_buffer`) is currently a plain `uint16_t*`; if
  future optimisation ever elides screen writes, the fix is to type it
  `volatile uint16_t*`.
- **`-Wall -Wextra`** — enable the common and extra warning sets. In a kernel a
  warning like "uninitialised variable" or "implicit declaration" often means a
  bug that would triple-fault the CPU with no diagnostics, so these are treated
  as mandatory.
- **`-std=gnu99`** — compile to C99 plus GNU extensions. C99 brings
  `//` comments, mixed declarations and statements, designated initialisers, and
  `long long`; the `gnu` variant additionally allows GCC extensions commonly used
  in kernels (statement expressions, `__attribute__`, inline-asm conveniences).

---

## 2. Core Architectural Concepts

### Target Architecture (i386 / 32-bit x86)

- **What it is?** A 32-bit instruction set architecture in CPU defined by Intel.
- **Why it matters?** Modern 64-bit CPUs (`x86_64` or ARM64) start up in legacy
  modes or use complex 64-bit page tables and calling conventions. Setting `-m32`
  targets pure 32-bit protected mode, which gives you direct access to 4 GB of
  memory without needing complex 64-bit long-mode setup code right out of the
  gate.

### Bare-Metal Environment

- **What it is?** Running code directly on physical hardware (CPU/RAM) without an
  underlying operating system.
- **Why it matters?** You have no `libc`, no system calls, no crash recovery, and
  no standard functions like `printf` or `malloc`. If your code makes a mistake
  (like accessing a bad memory address), the CPU cannot rely on an OS to print a
  nice error — it will simply restart or freeze.

---

## 3. Boot Flow and Execution Pipeline

```
[ Power On ] ──> [ GRUB Bootloader ] ──> [ Multiboot Header ] ──> [ Assembly boot.s ] ──> [ C kernel_main ]
```

### Step 1: GRUB Bootloader & Multiboot Protocol

- **What it is?** GRUB is a universal bootloader that loads operating systems into
  RAM.
- **Why it matters?** Instead of writing a fragile, complex 512-byte BIOS
  bootloader from scratch, we adhere to the Multiboot Specification. GRUB does the
  heavy lifting of initializing hardware into 32-bit protected mode and jumping
  straight to our code.

### Step 2: The Multiboot Header

- **What it is?** A specific block of magic numbers (`0x1BADB002`) placed in the
  binary.
- **Why it matters?** GRUB scans files on the disk looking for this signature. If
  found, GRUB recognizes the binary as a valid OS kernel and boots it; if
  missing, GRUB rejects it.

### Step 3: Assembly Bootstrap (`boot.s`)

- **What it is?** A tiny assembly script that executes before any C code runs.
- **Why it matters?** High-level C code requires a Stack (a designated region of
  memory used to store local variables and function return addresses). Because no
  OS exists to assign a stack, `boot.s` manually reserves 16 KB of raw RAM
  (`.skip 16384`), assigns it to the stack pointer register (`%esp`), and only
  then executes `call kernel_main`.

### Step 4: Custom Linker Script (`linker.ld`)

- **What it is?** A blueprint telling the linker (`ld`) how to assemble the final
  binary layout in physical memory.
- **Why it matters?** Default linkers structure programs to run inside an OS
  environment (like Linux). Our custom script explicitly enforces `. = 1M;`,
  ensuring our kernel binary is loaded at physical address 1 Megabyte
  (`0x100000`). This avoids hitting memory below 1MB, which is occupied by legacy
  BIOS data and hardware buffers.

### How is `kernel_main` called?

The assembler (`as` or `gcc`) doesn't know where `kernel_main` lives in memory. If
you didn't do anything else, it would crash. Because assembly doesn't
automatically look into other files, the assembler marks `kernel_main` as an
**Unresolved Symbol** (a placeholder or blank address) in `boot.o`. It writes a
note inside `boot.o` saying:

> "Hey, whoever links this binary later, please fill in the real memory address of
> `kernel_main` here."

When `gcc` compiles `kernel.c` into `kernel.o`, it creates a **Symbol Table**
inside `kernel.o`. This table acts like an index card saying:

> "This file contains a function named `kernel_main` starting at offset X."

The Linker (`ld`) takes all `.o` files and reads their Symbol Tables:

1. It looks at `boot.o` and sees a blank request: "Where is `kernel_main`?"
2. It looks at `kernel.o` and sees the answer: "I have `kernel_main`!"
3. Based on your `linker.ld` script (which says the kernel starts at 1MB), the
   linker calculates the exact final memory address where `kernel_main` will end
   up in RAM (e.g., `0x00100120`).
4. It replaces the placeholder `call kernel_main` instruction inside the binary
   with the exact physical address instruction: `call 0x00100120`.

**To summarize:** When `boot.s` runs, it executes a hardcoded memory address jump
that was calculated and patched directly into the binary by the Linker during
compilation. The assembly file doesn't need to know file names — it only needs the
Symbol Name to match between C and Assembly.

---

## 4. Screen Display Mechanics (VGA Text Mode)

### VGA Memory Buffer (`0xB8000`)

- **What it is?** A physical memory address mapped directly to the graphics
  display adapter.
- **Why it matters?** Writing data to `0xB8000` bypasses drivers and sends
  character signals directly to the screen hardware.

### Screen Layout & Bit Formatting

- **Dimensions:** Fixed at 80 columns by 25 rows (2,000 character cells). It is
  always this size for the VGA text mode.
- **Data Encoding:** Every cell on the screen uses 16 bits (2 bytes):

  | Byte | Bits | Contents |
  | --- | --- | --- |
  | Lower Byte | 0–7 | ASCII character code (e.g., `'4'`). |
  | Upper Byte | 8–15 | Color attributes (background color shifted left by 4 bits, combined with foreground color). |

- **Indexing Math:** Because RAM is a single flat array, coordinates are mapped
  using:

  ```
  Index = (Row * 80) + Column
  ```
