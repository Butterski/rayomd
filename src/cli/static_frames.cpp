// SPDX-License-Identifier: Apache-2.0
// RAYOMD_STATIC with GCC 13 or newer: crtbeginT.o registers .eh_frame with libgcc, which since
// GCC 13 sorts every FDE into a b-tree at startup (about 0.4 M instructions, 40 µs a process).
// Linked with --eh-frame-hdr, the unwinder finds the FDEs through PT_GNU_EH_FRAME instead, as
// in the dynamic build; --wrap sends crtbeginT's two calls here.
struct object;
extern "C" void __wrap___register_frame_info(const void*, object*) {}
extern "C" void* __wrap___deregister_frame_info(const void*) { return nullptr; }
