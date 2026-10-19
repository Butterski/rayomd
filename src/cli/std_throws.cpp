// SPDX-License-Identifier: Apache-2.0
// The command lines' std::__throw_* functions, in place of libstdc++'s: the error the library
// would throw is reported as its terminate handler reported it (the type as the ABI names it and
// what()) and the process aborts. RayoMD is built without exceptions and catches nothing, so such
// a throw ended the process before as well. libstdc++ defines all of them in one archive member,
// which also links the exception classes, the copy-on-write std::string of their what() and
// std::system_error's categories: on MinGW, whose libstdc++.a has no per-function sections, that
// was 45 KB of rayomd.exe (and the rest of the exception runtime 12 KB more), on Linux 6 KB. Every
// one the program refers to must be here, or the archive member is linked and its definitions
// clash with these: the link fails rather than linking the member silently.
// Release builds only (Debug builds keep exceptions); not part of the engine library.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

[[noreturn]] void Abort(const char* type, const char* what) {
    std::fputs("terminate called after throwing an instance of '", stderr);
    std::fputs(type, stderr);
    std::fputs("'\n  what():  ", stderr);
    std::fputs(what, stderr);
    std::fputs("\n", stderr);
    std::abort();
}

} // namespace

namespace std {

void __throw_bad_alloc() {
    Abort("St9bad_alloc", "std::bad_alloc");
}

void __throw_bad_array_new_length() {
    Abort("St20bad_array_new_length", "std::bad_array_new_length");
}

void __throw_logic_error(const char* what) {
    Abort("St11logic_error", what);
}

void __throw_length_error(const char* what) {
    Abort("St12length_error", what);
}

void __throw_out_of_range(const char* what) {
    Abort("St12out_of_range", what);
}

// libstdc++'s format: "%s", "%zu" and "%%" only (__snprintf_lite).
void __throw_out_of_range_fmt(const char* format, ...) {
    char what[512];
    size_t length = 0;
    const auto put = [&](char c) {
        if (length + 1 < sizeof(what)) what[length++] = c;
    };
    va_list args;
    va_start(args, format);
    for (const char* p = format; *p != '\0'; ++p) {
        if (p[0] == '%' && p[1] == 's') {
            for (const char* s = va_arg(args, const char*); *s != '\0'; ++s) put(*s);
            ++p;
        } else if (p[0] == '%' && p[1] == 'z' && p[2] == 'u') {
            char digits[20];
            int count = 0;
            for (size_t value = va_arg(args, size_t); count == 0 || value != 0; value /= 10) {
                digits[count++] = static_cast<char>('0' + value % 10);
            }
            while (count > 0) put(digits[--count]);
            p += 2;
        } else if (p[0] == '%' && p[1] == '%') {
            put('%');
            ++p;
        } else {
            put(*p);
        }
    }
    va_end(args);
    what[length] = '\0';
    Abort("St12out_of_range", what);
}

void __throw_system_error(int error) {
    Abort("St12system_error", std::strerror(error));
}

} // namespace std
