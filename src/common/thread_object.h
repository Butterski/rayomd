// SPDX-License-Identifier: Apache-2.0

#pragma once

// Per-thread objects with destructors (buffers kept from build to build). Declare one with
// RAYOMD_THREAD_OBJECT(Type, name) inside a function, never as a plain `static thread_local`:
//
// MinGW's GCC keeps thread_local variables in emulated TLS (libgcc's emutls), whose storage
// winpthreads frees at thread exit (_pthread_cleanup_dest calls emutls_destroy) before the CRT's
// TLS callback runs the C++ destructors registered through __cxa_thread_atexit (mingw-w64-crt's
// tls_atexit.c): those destructors then run on freed memory, and a batch worker or the GUI's build
// thread crashed as it ended (GCC PR 80816, mingw-w64 bugs 527 and 727, msys2 MINGW-packages
// #2519; under Wine every `--batch --workers=4` run). There the thread_local is a plain pointer,
// which emutls may free with nothing to destroy, and the object is on the heap, destroyed by a
// fiber-local storage callback (FlsAlloc) that Windows calls with the object list itself at thread
// exit, never through emutls. Elsewhere it is the `static thread_local Type name;` it reads as.

#ifdef __MINGW32__
namespace RayoMd::Thread {

// `destroy(object)` when the calling thread exits, the last registered first.
void AtExit(void* object, void (*destroy)(void*));

template <class T>
struct Box {
    T value;
};

// The thread's object behind `slot`, made (value-initialized, as thread storage is zeroed before
// its constructor runs) on first use.
template <class T>
T& Object(Box<T>*& slot) {
    if (slot == nullptr) {
        slot = new Box<T>();
        AtExit(slot, [](void* box) { delete static_cast<Box<T>*>(box); });
    }
    return slot->value;
}

template <class T>
using Same = T;

} // namespace RayoMd::Thread

#define RAYOMD_THREAD_OBJECT(Type, name)                                        \
    static thread_local ::RayoMd::Thread::Box<Type>* name##Box_ = nullptr;      \
    ::RayoMd::Thread::Same<Type>& name = ::RayoMd::Thread::Object(name##Box_)
#else
namespace RayoMd::Thread {
template <class T>
using Same = T;
}
#define RAYOMD_THREAD_OBJECT(Type, name) static thread_local ::RayoMd::Thread::Same<Type> name
#endif
