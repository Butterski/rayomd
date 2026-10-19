// SPDX-License-Identifier: Apache-2.0

#include "thread_object.h"

#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace RayoMd::Thread {

unsigned UsableCpus() {
#ifdef _WIN32
    DWORD_PTR process = 0;
    DWORD_PTR system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process != 0) {
        unsigned count = 0;
        for (; process != 0; process &= process - 1) count++;
        return count;
    }
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) > 0) return (unsigned)CPU_COUNT(&set);
#endif
    const unsigned cpus = std::thread::hardware_concurrency();
    return cpus == 0 ? 1 : cpus;
}

} // namespace RayoMd::Thread

#ifdef __MINGW32__
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
