# XP SP1 ARM64 port sources

This repository contains an audited subset of the native ARM64 port and its test
sources. It covers the UEFI boot path, AArch64 exception entry and context switching,
kernel and process address spaces, native user-mode dispatch, and software rendering.

The integration build requires separately authorized Windows XP SP1 source and
port overlays. This export does not contain the complete build inputs or a bootable
image. Integrated results from the development tree were obtained in QEMU.
Nintendo Switch Lite hardware execution is pending.

## Standalone controls on ARM64 macOS

From the repository root, compile and run the atomic operations and context-capture
controls with the Apple toolchain.

```sh
mkdir -p out
clang -arch arm64 -march=armv8-a -std=c11 -O2 -Wall -Wextra -Werror -pthread \
  tests/interlocked_test.c base/ntos/ex/arm64/interlocked.S -o out/interlocked-test
./out/interlocked-test

clang -arch arm64 -march=armv8-a -std=c11 -O2 -Wall -Wextra -Werror -pthread \
  -Ipublic/sdk/inc tests/context_test.c tests/capture_fixture.S \
  base/ntos/rtl/arm64/capture.S -o out/context-test
./out/context-test
```

## Third-party source

The ARM64 unwind implementation uses .NET Runtime code. The upstream files, pinned
source references, and MIT notice are in [third_party/dotnet-unwind](third_party/dotnet-unwind).
