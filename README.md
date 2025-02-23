# Celerity Runtime

The Celerity distributed runtime and API aims to bring the power and ease of
use of SYCL to distributed memory clusters.

**NOTE**: Celerity is a research project first and foremost, and is still in
early development. While it does work for certain applications, it probably
does not fully support your use case just yet. We'd however love for you to
give it a try and tell us about how you could imagine using Celerity for your
projects in the future.

## Dependencies

* [Boost](http://www.boost.org) (tested with version 1.65 - 1.68)
* A supported SYCL implementation, either
	- [hipSYCL](https://github.com/illuhad/hipsycl), or
	- [ComputeCpp](https://www.codeplay.com/products/computesuite/computecpp)
* A MPI 2 implementation (tested with OpenMPI 4.0 and MSMPI 10.0)
* [CMake](https://www.cmake.org)
* A C++14 compiler (tested with MSVC 14, gcc 7 and Clang 8)

### Automatically Downloaded Dependencies

These dependencies are downloaded automatically during the CMake build process,
for convenience:

* [Catch2](https://github.com/catchorg/Catch2) for testing
* The [spdlog](https://github.com/gabime/spdlog) logging library

## Building

Building is as simple as calling `cmake && make`, depending on your setup you
might however also have to provide some library paths etc.

## Building Examples

The runtime comes with several examples that are built automatically when
the `CELERITY_BUILD_EXAMPLES` CMake option is set (true by default).

## Using Celerity as a Library

Simply run `make install` (or equivalent, depending on build system) to copy all
relevant header files and libraries to the `CMAKE_INSTALL_PREFIX`. This includes
a CMake [package configuration
file](https://cmake.org/cmake/help/latest/manual/cmake-packages.7.html#package-configuration-file)
which is placed inside the `lib/cmake` directory. Once included in a CMake
project, you can use the
`add_celerity_to_target(TARGET target SOURCES source1 source2...)` function to
set everything up.

## Environment Variables

* `CELERITY_DEVICES` can be used to assign different compute devices to CELERITY
  nodes on a single host. The syntax is as follows:
  `CELERITY_DEVICES="<platform_id> <first device_id> <second device_id> ... <nth device_id>"`.
* `CELERITY_FORCE_WG=<work_group_size>` can be used to force a particular work
   group size for *every kernel* and *every dimension*.
* `CELERITY_PROFILE_OCL` controls whether OpenCL-level profiling information
  should be used or not (currently not supported when using hipSYCL).
* `CELERITY_LOG_LEVEL` controls the logging output level. One of `trace`, `debug`,
  `info`, `warn`, `err`, `critical`, or `off`.