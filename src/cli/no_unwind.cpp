// SPDX-License-Identifier: Apache-2.0
// The release CLI's C++ exception runtime (Linux, dynamic and static, and MinGW): a throw ends
// the process.
// RayoMD is built without exceptions and catches nothing, so what libstdc++ throws
// (std::bad_alloc, a std::length_error) ended it before, after unwinding the stack for a handler
// that is not there. That search linked libgcc's unwinder, the personality routine and the
// emergency exception pool (41 KB of code and tables on Linux, and a 73 KB allocation at every
// start; 12 KB of rayomd.exe): these definitions take their place, so those archive members are
// never linked. A throw prints what libstdc++'s terminate handler printed (the type as the ABI
// names it) and aborts; the landing pads and handlers of libstdc++'s own code, which only an
// unwind would reach, abort too. The nothrow operator new returns null when memory runs out,
// without the throw and catch libstdc++'s makes of it.
#include <cxxabi.h>
#include <unwind.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <typeinfo>

namespace {

[[noreturn]] void Fail(const char* message) {
    std::fputs(message, stderr);
    std::abort();
}

} // namespace

namespace __cxxabiv1 {

extern "C" void* __cxa_allocate_exception(std::size_t size) noexcept {
    void* const object = std::malloc(size);
    if (object == nullptr) Fail("terminate called after throwing an instance of 'St9bad_alloc'\n");
    return object;
}

extern "C" void __cxa_free_exception(void* object) noexcept {
    std::free(object);
}

// Every type libstdc++ throws derives from std::exception, at the start of the object.
extern "C" void __cxa_throw(void* object, std::type_info* type, void (*)(void*)) {
    std::fputs("terminate called after throwing an instance of '", stderr);
    std::fputs(type->name(), stderr);
    std::fputs("'\n  what():  ", stderr);
    std::fputs(static_cast<const std::exception*>(object)->what(), stderr);
    Fail("\n");
}

extern "C" void __cxa_rethrow() {
    Fail("terminate called without an active exception\n");
}

extern "C" void* __cxa_begin_catch(void*) noexcept {
    Fail("terminate called without an active exception\n");
}

extern "C" void __cxa_end_catch() {
    Fail("terminate called without an active exception\n");
}

#ifdef __SEH__
// MinGW's personality routine for Windows' structured exception handling.
extern "C" int __gxx_personality_seh0(void*, void*, void*, void*) {
    Fail("terminate called without an active exception\n");
}
#else
extern "C" _Unwind_Reason_Code __gxx_personality_v0(int, _Unwind_Action, _Unwind_Exception_Class, _Unwind_Exception*,
    _Unwind_Context*) {
    Fail("terminate called without an active exception\n");
}
#endif

extern "C" void __cxa_call_unexpected(void*) {
    Fail("terminate called without an active exception\n");
}

extern "C" void* __cxa_get_exception_ptr(void*) noexcept {
    Fail("terminate called without an active exception\n");
}

// No exception is ever in flight or caught.
extern "C" std::type_info* __cxa_current_exception_type() noexcept {
    return nullptr;
}

} // namespace __cxxabiv1

namespace std {

bool uncaught_exception() noexcept {
    return false;
}

int uncaught_exceptions() noexcept {
    return 0;
}

} // namespace std

namespace __gnu_cxx {

// std::terminate's handler (a joinable std::thread destroyed): no exception is ever active, so
// terminate.cpp's handler, which reads the active exception through a catch, is not linked.
void __verbose_terminate_handler() {
    Fail("terminate called without an active exception\n");
}

} // namespace __gnu_cxx

extern "C" void _Unwind_Resume(_Unwind_Exception*) {
    Fail("terminate called without an active exception\n");
}

#ifndef __SEH__
// The static build: glibc's libc.a has cleanup regions in its stdio, run by C's personality
// routine during an unwind, and unwinds a thread in pthread_exit and at a cancellation
// (_Unwind_ForcedUnwind, _Unwind_GetCFA). RayoMD neither exits nor cancels a thread and nothing
// unwinds, so with these libgcc's unwinder (libgcc_eh.a) and the unwind tables it would read are
// not linked there either. The dynamic build's libc unwinds through libgcc_s.so, which it loads.
extern "C" _Unwind_Reason_Code __gcc_personality_v0(int, _Unwind_Action, _Unwind_Exception_Class,
    _Unwind_Exception*, _Unwind_Context*) {
    Fail("terminate called without an active exception\n");
}

extern "C" _Unwind_Reason_Code _Unwind_ForcedUnwind(_Unwind_Exception*, _Unwind_Stop_Fn, void*) {
    Fail("rayomd: a thread exited or was cancelled, which needs an unwinder\n");
}

extern "C" _Unwind_Word _Unwind_GetCFA(_Unwind_Context*) {
    Fail("rayomd: a thread exited or was cancelled, which needs an unwinder\n");
}
#endif

#ifndef RAYOMD_ENABLE_PROFILING
// As libstdc++'s operator new: the new-handler is called while malloc fails; without one, null.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (size == 0) size = 1;
    for (;;) {
        if (void* memory = std::malloc(size)) return memory;
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) return nullptr;
        handler();
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
#endif
