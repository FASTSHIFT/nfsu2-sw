# CMake toolchain for r36s/Dockerfile.cross: focal's aarch64 cross gcc, target
# libraries from arm64 multiarch (/usr/lib/aarch64-linux-gnu).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

# pkg-config: only the arm64 .pc files (SDL2).
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
