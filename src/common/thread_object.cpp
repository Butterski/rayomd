// SPDX-License-Identifier: Apache-2.0

#include "thread_object.h"

#ifdef __MINGW32__
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <new>

namespace RayoMd::Thread {
namespace {

struct Registered {
    void* object;
    void (*destroy)(void*);
    Registered* next;
};

// Called by Windows as a thread exits, with the thread's list (thread_object.h).
void NTAPI DestroyAll(void* list) {
    for (Registered* entry = static_cast<Registered*>(list); entry != nullptr;) {
        Registered* const next = entry->next;
        entry->destroy(entry->object);
        delete entry;
        entry = next;
    }
}

} // namespace

void AtExit(void* object, void (*destroy)(void*)) {
    static const DWORD slot = FlsAlloc(DestroyAll);
    // Without a slot the object stays until the process ends, as it would without a destructor.
    if (slot == FLS_OUT_OF_INDEXES) return;
    Registered* const entry = new (std::nothrow) Registered{ object, destroy, static_cast<Registered*>(FlsGetValue(slot)) };
    if (entry != nullptr) FlsSetValue(slot, entry);
}

} // namespace RayoMd::Thread
#endif
