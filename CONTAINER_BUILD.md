# Containerized development build

The container build mirrors upstream's Linux CMake CI baseline: GCC 14,
CMake, Ninja, a Debug configuration, and CTest. The source tree is copied into
the image, so compilation and test output never changes ownership or modes in
the host checkout.

Build the current source snapshot and run all tests with:

```sh
./scripts/container-build
```

The wrapper uses Podman by default. Use Docker or another compatible engine
with `CONTAINER_ENGINE=docker`. Extra arguments are forwarded to CMake, for
example:

```sh
./scripts/container-build -DBTOP_GPU=OFF
```

The image name defaults to `btop-build:local` and can be changed with
`BTOP_BUILD_IMAGE`. `BTOP_BUILD_TYPE` and `BTOP_BUILD_JOBS` control the CMake
build type and build parallelism inside the container. The default GPU build
defines `_GNU_SOURCE` for C sources so the vendored Intel GPU collector builds
with GCC 14 and current glibc.

Because the image contains a source snapshot, rerun the wrapper after changing
source files. Docker's layer cache keeps the toolchain layer reusable.

The image includes Python and pyte. CTest runs the real executable in a
pseudo-terminal and inspects completed terminal frames at 36×6, 80×24, and
160×40, including interface editing and process-filter interaction. A local
CMake build registers this test when Python and pyte are available.

These are native Linux builds. Building the macOS and BSD collectors requires
the corresponding platform SDKs, headers, and libraries; the Linux container
does not provide them. No remote Actions runs or publication are performed by
these scripts.
