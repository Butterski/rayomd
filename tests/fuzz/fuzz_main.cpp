// SPDX-License-Identifier: Apache-2.0

// The libFuzzer entry of one target: RAYOMD_FUZZ_TARGET names its function (tests/fuzz/CMakeLists.txt).

#include "fuzz_targets.h"

#ifndef RAYOMD_FUZZ_TARGET
#error "RAYOMD_FUZZ_TARGET names the target function"
#endif

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    RayoMdFuzz::ScratchDir();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return RayoMdFuzz::RunTarget(RayoMdFuzz::RAYOMD_FUZZ_TARGET, data, size);
}
