// SPDX-License-Identifier: Apache-2.0
// The command lines' terminate handler, in place of libstdc++'s: the same message, with the
// exception's type as the ABI names it ("St9bad_alloc") instead of demangled. libstdc++'s handler
// links the C++ demangler (54 KB of code and tables) into a program that never throws on purpose;
// defined here, the archive member that holds it is never linked. Built with -fexceptions, as it
// rethrows to read what().
#include <cxxabi.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>

namespace __gnu_cxx {

void __verbose_terminate_handler() {
    static bool terminating = false;
    if (terminating) {
        std::fputs("terminate called recursively\n", stderr);
        std::abort();
    }
    terminating = true;
    if (const std::type_info* type = abi::__cxa_current_exception_type()) {
        std::fprintf(stderr, "terminate called after throwing an instance of '%s'\n", type->name());
        try {
            throw;
        } catch (const std::exception& exception) {
            std::fprintf(stderr, "  what():  %s\n", exception.what());
        } catch (...) {
        }
    } else {
        std::fputs("terminate called without an active exception\n", stderr);
    }
    std::abort();
}

} // namespace __gnu_cxx
