// SPDX-License-Identifier: Apache-2.0
// The std::string and std::wstring members the MinGW build (GCC's libstdc++) calls out of line,
// instantiated here instead of linked from libstdc++.a. GNU ld's --gc-sections keeps every
// function that has SEH unwind data on PE (its scripts KEEP .pdata, which refers to the
// function), so the first member referenced linked all of libstdc++'s string-inst.o and
// wstring-inst.o: 49 KB of rayomd.exe, most of it never called; an explicit instantiation of the
// whole classes here was 43 KB for the same reason. Instantiated one by one, only these are
// linked. A member the program calls that is not listed links libstdc++'s whole object again,
// silently: after code changes, list the undefined basic_string members of the objects
// (performance.md, "Regression and size gate") and add them here. Linux needs none of this: its
// gc removes the members nothing calls.
// The library's explicit instantiation declarations are off in this unit, so that members defined
// in the class body are emitted here when their address is taken.
#include <bits/c++config.h>
#undef _GLIBCXX_EXTERN_TEMPLATE
#define _GLIBCXX_EXTERN_TEMPLATE 0
#include <string>

namespace {
using S = std::basic_string<char>;
using W = std::basic_string<wchar_t>;
using Size = std::size_t;
} // namespace

// A member that an explicit instantiation cannot name, as a member template of the same name
// matches as well: emitted through its address, taken by a function this unit defines.
template <typename T, T member> struct EmitStringMember {
    static T Address() { return member; }
};

template S::basic_string(const S&);
template S::basic_string(S&&);
template S& S::operator=(S&&);
template bool S::_M_is_local() const;
template void S::_M_dispose();
template S::pointer S::_M_create(Size&, Size);
template void S::_M_construct(Size, char);
template void S::_S_copy(char*, const char*, Size);
template struct EmitStringMember<void (*)(char*, const char*, const char*), &S::_S_copy_chars>;
template void S::_M_assign(const S&);
template void S::_M_mutate(Size, Size, const char*, Size);
template void S::_M_erase(Size, Size);
template S& S::_M_append(const char*, Size);
template S& S::_M_replace(Size, Size, const char*, Size);
template S& S::_M_replace_aux(Size, Size, Size, char);
template Size S::capacity() const;
template void S::reserve(Size);
template void S::resize(Size);
template void S::resize(Size, char);
template void S::swap(S&);
template void S::push_back(char);
template void S::pop_back();
template S& S::append(const char*);
template S& S::append(const char*, Size);
template S& S::append(const S&);
template S& S::assign(Size, char);
template S::iterator S::insert(S::const_iterator, char);
template S& S::insert(Size, const char*);
template S& S::insert(Size, const S&);
template S::iterator S::erase(S::const_iterator, S::const_iterator);
template S& S::replace(Size, Size, const char*, Size);
template S S::substr(Size, Size) const;
template int S::compare(const S&) const;
template int S::compare(Size, Size, const char*) const;
template Size S::find(const char*, Size) const;
template Size S::find(const char*, Size, Size) const;
template Size S::find(char, Size) const;
template Size S::rfind(const char*, Size) const;
template Size S::rfind(char, Size) const;
template Size S::find_first_of(const char*, Size) const;
template Size S::find_first_not_of(const char*, Size) const;
template Size S::find_first_not_of(char, Size) const;
template Size S::find_last_not_of(const char*, Size) const;
template const Size S::npos;

template W::basic_string(const W&);
template W::basic_string(W&&);
template bool W::_M_is_local() const;
template void W::_M_dispose();
template W::pointer W::_M_create(Size&, Size);
template void W::_M_construct(Size, wchar_t);
template void W::_M_set_length(Size);
template void W::_S_copy(wchar_t*, const wchar_t*, Size);
template struct EmitStringMember<void (*)(wchar_t*, const wchar_t*, const wchar_t*), &W::_S_copy_chars>;
template void W::_M_assign(const W&);
template void W::_M_mutate(Size, Size, const wchar_t*, Size);
template void W::_M_erase(Size, Size);
template W& W::_M_append(const wchar_t*, Size);
template W& W::_M_replace(Size, Size, const wchar_t*, Size);
template W& W::_M_replace_aux(Size, Size, Size, wchar_t);
template Size W::capacity() const;
template void W::reserve(Size);
template void W::resize(Size);
template void W::resize(Size, wchar_t);
template void W::swap(W&);
template void W::push_back(wchar_t);
template void W::pop_back();
template W::iterator W::end();
template W::const_iterator W::end() const;
template wchar_t& W::back();
template const wchar_t& W::back() const;
template W& W::append(const wchar_t*);
template W& W::append(const wchar_t*, Size);
template W& W::append(const W&);
template W& W::insert(Size, const wchar_t*);
template W& W::insert(Size, const W&);
template W::iterator W::erase(W::const_iterator);
template W W::substr(Size, Size) const;
template int W::compare(const W&) const;
template int W::compare(Size, Size, const wchar_t*) const;
template Size W::find(wchar_t, Size) const;
template Size W::rfind(const wchar_t*, Size) const;
template Size W::rfind(wchar_t, Size) const;
template Size W::find_first_of(const wchar_t*, Size, Size) const;
template Size W::find_last_of(const wchar_t*, Size) const;
