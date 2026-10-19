// SPDX-License-Identifier: Apache-2.0
// The dynamic Linux CLI's C++ exception runtime: a throw ends the process. RayoMD is built
// without exceptions and catches nothing, so what libstdc++ throws (std::bad_alloc, a
// std::length_error) ended it before, after unwinding the stack for a handler that is not
// there. That search linked libgcc's unwinder, the personality routine and the emergency
// exception pool (41 KB of code and tables, and a 73 KB allocation at every start): these
// definitions take their place, so those archive members are never linked. A throw prints what
// libstdc++'s terminate handler printed (the type as the ABI names it) and aborts; the landing
// pads and handlers of libstdc++'s own code, which only an unwind would reach, abort too.
// The nothrow operator new returns null when memory runs out, without the throw and catch
// libstdc++'s makes of it.
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
    std::fprintf(stderr, "terminate called after throwing an instance of '%s'\n  what():  %s\n", type->name(),
        static_cast<const std::exception*>(object)->what());
    std::abort();
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

extern "C" _Unwind_Reason_Code __gxx_personality_v0(int, _Unwind_Action, _Unwind_Exception_Class, _Unwind_Exception*,
    _Unwind_Context*) {
    Fail("terminate called without an active exception\n");
}

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

extern "C" void _Unwind_Resume(_Unwind_Exception*) {
    Fail("terminate called without an active exception\n");
}

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
