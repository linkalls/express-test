# Windows x64 static linking with release-only vcpkg dependencies.
# See: https://learn.microsoft.com/en-us/vcpkg/users/triplets
# Cemu's standard triplet builds both Debug and Release dependencies.
# This custom triplet deliberately skips Debug dependencies.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
