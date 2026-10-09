#include "highlight.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define RAYOMD_NOINLINE __attribute__((noinline))
#else
#define RAYOMD_NOINLINE
#endif

namespace TinyPdf::Internal {
namespace {

enum Language : uint8_t {
    kNoLanguage,
    kShell, kConsole, kPowerShell, kBatch,
    kJavaScript, kTypeScript, kPython, kPycon,
    kC, kCpp, kCSharp, kJava, kKotlin, kGo, kRust, kSwift, kPhp, kRuby, kSql,
    kHtml, kXml, kCss, kScss, kJson, kYaml, kToml, kIni, kDockerfile, kDiff,
    kLanguageCount
};

// How a language is read: by the code lexer, or by the one for terminal sessions, markup, style
// sheets, data or diffs.
enum Mode : uint8_t { kCodeMode, kConsoleMode, kMarkupMode, kCssMode, kJsonMode, kYamlMode, kTomlMode, kIniMode, kDiffMode };

// What the code lexer reads in a language.
enum : uint32_t {
    kSlashLine = 1u << 0,        // "//" comments
    kSlashBlock = 1u << 1,       // "/* */" comments
    kNestedBlocks = 1u << 2,     // ... which nest (Rust, Kotlin, Swift)
    kHashLine = 1u << 3,         // "#" comments
    kHashAtWord = 1u << 4,       // ... only where a word starts (shell)
    kDashLine = 1u << 5,         // "--" comments (SQL)
    kQuoteStrings = 1u << 6,     // '...' is a string, not a character
    kRawQuotes = 1u << 7,        // ... without escapes, '' for a quote (shell, SQL, PowerShell)
    kLongStrings = 1u << 8,      // strings go on over lines
    kTripleQuotes = 1u << 9,     // """...""" (also '''...''' with kQuoteStrings) over lines
    kBacktickStrings = 1u << 10, // `...` is a string over lines (shell, Go, PHP, Ruby)
    kTemplates = 1u << 11,       // `...${code}...` (JavaScript)
    kDollarNames = 1u << 12,     // $name and ${name} are variables, also in "..."
    kDollarLetters = 1u << 13,   // '$' is a letter (JavaScript, Java)
    kHeredocs = 1u << 14,        // <<WORD ... WORD (shell, Ruby), <<<WORD (PHP)
    kDirectives = 1u << 15,      // '#' first on a line starts a preprocessor directive
    kRegexes = 1u << 16,         // /.../ where an operand starts (JavaScript, Ruby)
    kCaseless = 1u << 17,        // words without case (SQL, PowerShell, PHP, batch files)
    kDecorators = 1u << 18,      // @name (Python, Java, TypeScript, Kotlin, Swift)
    kCalls = 1u << 19,           // a name before "(" is a function's
    kInterpolation = 1u << 20,   // "...$name ${code}..." (Kotlin), "...#{code}..." (Ruby)
    kTypeSuffix = 1u << 21,      // a name that ends in "_t" is a type (C, C++)
};

// The words of a language that have a class of their own: space-separated, each group after
// the byte that gives its class. Lists of languages without case are in lower case.
#define KEYWORDS "\001"   // reserved words and built-in types
#define BUILTINS "\002"   // constants and built-in names
#define DEFINERS "\003"   // keywords whose next name is defined: class Name

constexpr char kShellWords[] =
    KEYWORDS "if then else elif fi case esac for select while until do done in time coproc break continue return exit"
    DEFINERS " function"
    BUILTINS " alias bg bind builtin caller cd command compgen complete declare dirs disown echo enable eval exec export"
    " false fc fg getopts hash help history jobs kill let local logout mapfile popd printf pushd pwd read readarray"
    " readonly set shift shopt source suspend test times trap true type typeset ulimit umask unalias unset wait";

constexpr char kDockerWords[] =
    KEYWORDS "ADD ARG AS CMD COPY ENTRYPOINT ENV EXPOSE FROM HEALTHCHECK LABEL MAINTAINER ONBUILD RUN SHELL"
    " STOPSIGNAL USER VOLUME WORKDIR";

constexpr char kPowerShellWords[] =
    KEYWORDS "begin break catch continue data define do dynamicparam else elseif end exit finally for foreach from"
    " hidden if in param process return static switch throw trap try until using var while workflow"
    " -eq -ne -gt -ge -lt -le -like -notlike -match -notmatch -contains -notcontains -in -notin -replace -split -join"
    " -and -or -not -xor -band -bor -bxor -is -isnot -as -f -ceq -cne -clike -cmatch -creplace -ieq -ine -ilike -imatch"
    DEFINERS " function filter class enum";

constexpr char kBatchWords[] =
    KEYWORDS "echo set if else goto call exit for in do not exist defined errorlevel equ neq lss leq gtr geq"
    " setlocal endlocal shift start pause cls cd chdir pushd popd title";

constexpr char kJavaScriptWords[] =
    KEYWORDS "await break case catch const continue debugger default delete do else export finally for if import in"
    " instanceof let new of return static switch throw try typeof var void while with yield async from as"
    DEFINERS " class function extends"
    BUILTINS " true false null undefined NaN Infinity this super console document window globalThis require module"
    " exports process JSON Math Object Array String Number Boolean Promise Symbol Map Set Date RegExp Error";

constexpr char kTypeScriptWords[] =
    KEYWORDS "abstract declare infer is keyof namespace never private protected public readonly satisfies unique"
    " asserts override accessor any bigint boolean number object string symbol unknown module"
    DEFINERS " interface type enum implements";

constexpr char kPythonWords[] =
    KEYWORDS "and as assert async await break continue del elif else except finally for from global if import in is"
    " lambda nonlocal not or pass raise return try while with yield match case"
    DEFINERS " def class"
    BUILTINS " True False None self cls print len range int float str bool bytes list dict set tuple object type"
    " isinstance super open enumerate zip map filter sorted min max sum abs any all repr iter next __name__"
    " Exception ValueError TypeError KeyError";

constexpr char kCWords[] =
    KEYWORDS "auto break case const continue default do else extern for goto if inline register restrict return sizeof"
    " static switch typedef volatile while alignas alignof static_assert thread_local typeof _Alignas _Alignof"
    " _Atomic _Bool _Complex _Generic _Noreturn _Static_assert _Thread_local bool char double float int long short"
    " signed unsigned void"
    DEFINERS " struct union enum"
    BUILTINS " true false NULL nullptr EOF stdin stdout stderr";

constexpr char kCppWords[] =
    KEYWORDS "and and_eq asm bitand bitor catch compl concept consteval constexpr constinit const_cast co_await"
    " co_return co_yield decltype delete dynamic_cast explicit export final friend mutable new noexcept not not_eq"
    " operator or or_eq override private protected public reinterpret_cast requires static_cast template throw try"
    " typeid typename using virtual xor xor_eq import module char8_t char16_t char32_t wchar_t"
    DEFINERS " class namespace"
    BUILTINS " this";

constexpr char kCSharpWords[] =
    KEYWORDS "abstract as break case catch checked const continue default delegate do else event explicit extern"
    " finally fixed for foreach goto if implicit in internal is lock new operator out override params private"
    " protected public readonly ref return sealed sizeof stackalloc static switch throw try typeof unchecked unsafe"
    " using virtual volatile while add alias and async await descending equals from get global group init into"
    " join let nameof not or orderby partial remove required select set unmanaged var when where with yield bool"
    " byte char decimal double float int long object sbyte short string uint ulong ushort void nint nuint dynamic"
    DEFINERS " class struct interface enum record namespace"
    BUILTINS " true false null this base";

constexpr char kJavaWords[] =
    KEYWORDS "abstract assert break case catch const continue default do else final finally for goto if import"
    " instanceof native new package private protected public return static strictfp switch synchronized throw throws"
    " transient try volatile while var yield permits sealed boolean byte char double float int long short void"
    DEFINERS " class interface enum record extends implements"
    BUILTINS " true false null this super";

constexpr char kKotlinWords[] =
    KEYWORDS "as break continue do else for if in is return throw try typealias typeof val var when while by catch"
    " constructor finally get import init set where abstract actual annotation companion const crossinline data"
    " expect external final infix inline inner internal lateinit noinline open operator out override private"
    " protected public reified sealed suspend tailrec vararg package"
    DEFINERS " class fun interface object"
    BUILTINS " true false null this super it";

constexpr char kSwiftWords[] =
    KEYWORDS "associatedtype deinit fileprivate import init inout internal let open operator private precedencegroup"
    " public rethrows static subscript typealias var break case catch continue default defer do else fallthrough for"
    " guard if in repeat return switch throw throws try where while as is async await some any mutating"
    " nonmutating override required convenience final lazy weak unowned dynamic optional indirect get set willSet"
    " didSet"
    DEFINERS " class struct enum protocol extension func actor"
    BUILTINS " true false nil self Self super";

constexpr char kGoWords[] =
    KEYWORDS "break case chan const continue default defer else fallthrough for go goto if import interface map"
    " package range return select struct switch var any bool byte complex64 complex128 error float32 float64 int"
    " int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr"
    DEFINERS " func type"
    BUILTINS " true false iota nil append cap clear close complex copy delete imag len make max min new panic print"
    " println real recover";

constexpr char kRustWords[] =
    KEYWORDS "as async await break const continue crate dyn else extern for if in let loop match move mut pub ref"
    " return static super unsafe use where while bool char str i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize"
    " f32 f64"
    DEFINERS " fn struct enum trait type mod impl union"
    BUILTINS " true false self Self Some None Ok Err";

constexpr char kPhpWords[] =
    KEYWORDS "abstract and array as break callable case catch clone const continue declare default do echo else"
    " elseif empty enddeclare endfor endforeach endif endswitch endwhile final finally fn for foreach global goto if"
    " include include_once instanceof insteadof isset list match namespace new or print private protected public"
    " readonly require require_once return static switch throw try unset use var while xor yield int float bool"
    " string mixed void never iterable"
    DEFINERS " function class interface trait enum extends implements"
    BUILTINS " true false null self parent";

constexpr char kRubyWords[] =
    KEYWORDS "__ENCODING__ __LINE__ __FILE__ BEGIN END alias and begin break case defined? do else elsif end ensure"
    " for if in next not or redo rescue retry return super then undef unless until when while yield"
    DEFINERS " def class module"
    BUILTINS " true false nil self puts print require require_relative attr_accessor attr_reader attr_writer include"
    " extend private protected public raise lambda proc";

constexpr char kSqlWords[] =
    KEYWORDS "select from where and or not in is like ilike between exists as on join inner left right full outer"
    " cross natural using group by having order asc desc nulls limit offset fetch first next rows only union all"
    " intersect except distinct case when then else end insert into values update set delete merge matched create"
    " alter drop truncate table view materialized index unique primary key foreign references constraint check"
    " default if replace temporary temp database schema sequence trigger function procedure returns return begin"
    " commit rollback transaction grant revoke with recursive over partition window cast collate cascade add"
    " column rename to lateral returning conflict do nothing explain analyze declare top show use"
    " int integer smallint bigint decimal numeric real float double precision char character varchar text boolean"
    " bool date time timestamp timestamptz interval uuid json jsonb blob bytea serial bigserial auto_increment"
    BUILTINS " true false null current_date current_time current_timestamp count sum avg min max coalesce now";

#undef KEYWORDS
#undef BUILTINS
#undef DEFINERS

struct LanguageSpec {
    const char* aliases;   // the info strings that name it: GitHub's (Linguist's) and documentation sites'
    const char* words;
    uint8_t base;          // the language whose words count too, or 0
    uint8_t mode;
    uint32_t flags;
};

constexpr uint32_t kShellFlags =
    kHashLine | kHashAtWord | kQuoteStrings | kRawQuotes | kLongStrings | kBacktickStrings | kDollarNames | kHeredocs;
constexpr uint32_t kSlashFlags = kSlashLine | kSlashBlock | kCalls;
constexpr uint32_t kScriptFlags = kSlashFlags | kQuoteStrings | kTemplates | kDollarLetters | kRegexes | kDecorators;

constexpr LanguageSpec kSpecs[kLanguageCount] = {
    { "", nullptr, 0, kCodeMode, 0 },
    { "sh bash shell zsh ksh ash dash shell-script envrc dotnetcli azurecli", kShellWords, 0, kCodeMode, kShellFlags },
    { "console shellsession shell-session sh-session bash-session terminal", nullptr, 0, kConsoleMode, 0 },
    { "powershell ps1 psm1 pwsh posh", kPowerShellWords, 0, kCodeMode,
      kHashLine | kQuoteStrings | kRawQuotes | kLongStrings | kDollarNames | kCaseless },
    { "bat batch cmd dosbatch winbatch", kBatchWords, 0, kCodeMode, kCaseless },
    { "javascript js jsx mjs cjs node javascriptreact js-nolint", kJavaScriptWords, 0, kCodeMode, kScriptFlags },
    { "typescript ts tsx mts cts typescriptreact", kTypeScriptWords, kJavaScript, kCodeMode, kScriptFlags },
    { "python py py3 python3 gyp", kPythonWords, 0, kCodeMode, kHashLine | kQuoteStrings | kTripleQuotes | kDecorators | kCalls },
    { "pycon", nullptr, 0, kConsoleMode, 0 },
    { "c h", kCWords, 0, kCodeMode, kSlashFlags | kDirectives | kTypeSuffix },
    { "cpp c++ cxx cc hpp hxx h++ cuda", kCppWords, kC, kCodeMode, kSlashFlags | kDirectives | kTypeSuffix },
    { "csharp cs c# cake csx", kCSharpWords, 0, kCodeMode, kSlashFlags | kDirectives | kTripleQuotes },
    { "java jsh groovy gradle", kJavaWords, 0, kCodeMode, kSlashFlags | kTripleQuotes | kDecorators | kDollarLetters },
    { "kotlin kt kts", kKotlinWords, 0, kCodeMode, kSlashFlags | kNestedBlocks | kTripleQuotes | kDecorators | kInterpolation },
    { "go golang", kGoWords, 0, kCodeMode, kSlashFlags | kBacktickStrings },
    { "rust rs", kRustWords, 0, kCodeMode, kSlashFlags | kNestedBlocks | kLongStrings },
    { "swift", kSwiftWords, 0, kCodeMode, kSlashFlags | kNestedBlocks | kTripleQuotes | kDecorators },
    { "php php3 php4 php5 phtml", kPhpWords, 0, kCodeMode,
      kSlashFlags | kHashLine | kQuoteStrings | kLongStrings | kBacktickStrings | kDollarNames | kHeredocs | kCaseless },
    { "ruby rb jruby rake gemspec", kRubyWords, 0, kCodeMode,
      kHashLine | kQuoteStrings | kLongStrings | kBacktickStrings | kHeredocs | kRegexes | kCalls | kInterpolation },
    { "sql mysql postgresql postgres psql pgsql plpgsql sqlite tsql plsql", kSqlWords, 0, kCodeMode,
      kDashLine | kSlashBlock | kQuoteStrings | kRawQuotes | kLongStrings | kCaseless },
    { "html htm xhtml vue svelte", nullptr, 0, kMarkupMode, 0 },
    { "xml svg xsd xsl xslt rss wsdl xaml csproj plist", nullptr, 0, kMarkupMode, 0 },
    { "css", nullptr, 0, kCssMode, 0 },
    { "scss sass less", nullptr, 0, kCssMode, 0 },
    { "json geojson jsonl ndjson topojson sarif json5 jsonc", nullptr, 0, kJsonMode, 0 },
    { "yaml yml", nullptr, 0, kYamlMode, 0 },
    { "toml", nullptr, 0, kTomlMode, 0 },
    { "ini cfg conf dosini properties gitconfig editorconfig env dotenv", nullptr, 0, kIniMode, 0 },
    { "dockerfile docker containerfile", kDockerWords, kShell, kCodeMode, kShellFlags },
    { "diff patch udiff", nullptr, 0, kDiffMode, 0 },
};

// ---------------------------------------------------------------------------------------------
// Bytes

constexpr uint8_t kSpaceByte = 1;   // ' ', '\t', '\r', '\f', '\v'; '\n' ends a line instead
constexpr uint8_t kWordByte = 2;    // letters, digits, '_' and bytes from 0x80
constexpr uint8_t kWordStart = 4;   // the same without digits
constexpr uint8_t kDigitByte = 8;

constexpr std::array<uint8_t, 256> ByteKinds() {
    std::array<uint8_t, 256> kinds{};
    for (int c = 0; c < 256; c++) {
        uint8_t kind = 0;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') kind = kSpaceByte;
        else if (c >= '0' && c <= '9') kind = kWordByte | kDigitByte;
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80) kind = kWordByte | kWordStart;
        kinds[(size_t)c] = kind;
    }
    return kinds;
}

constexpr std::array<uint8_t, 256> kBytes = ByteKinds();

inline bool IsSpace(char c) { return (kBytes[(unsigned char)c] & kSpaceByte) != 0; }
inline bool IsWord(char c) { return (kBytes[(unsigned char)c] & kWordByte) != 0; }
inline bool IsWordStart(char c) { return (kBytes[(unsigned char)c] & kWordStart) != 0; }
inline bool IsDigit(char c) { return (kBytes[(unsigned char)c] & kDigitByte) != 0; }
inline bool IsAsciiWord(char c) { return IsWord(c) && (unsigned char)c < 0x80; }
constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c | 0x20) : c; }

// Whether `c` is in `set`; never for '\0'.
inline bool IsOneOf(char c, const char* set) { return c != '\0' && strchr(set, c) != nullptr; }

bool SameCaseless(const char* text, const char* lower, size_t length) {
    for (size_t k = 0; k < length; k++) {
        if (Lower(text[k]) != lower[k]) return false;
    }
    return true;
}

// The bytes of a word against a listed one: keywords are short, and a call to memcmp cost more
// than the comparison.
inline bool SameBytes(const char* text, const char* word, size_t length) {
    for (size_t k = 0; k < length; k++) {
        if (text[k] != word[k]) return false;
    }
    return true;
}

// The text being classified and its classes, which every lexer below writes.
struct Text {
    const char* s;
    uint8_t* out;

    // Tokens are mostly short: up to 16 bytes take two overlapping stores, where a call to
    // memset cost more than the token. One copy out of line: as fast, and 8 KB less code.
    RAYOMD_NOINLINE void Mark(size_t from, size_t to, uint8_t codeClass) const {
        if (to <= from) return;
        const size_t length = to - from;
        uint8_t* at = out + from;
        if (length > 16) {
            memset(at, codeClass, length);
        } else if (length >= 8) {
            const uint64_t bytes = 0x0101010101010101ull * codeClass;
            memcpy(at, &bytes, 8);
            memcpy(at + length - 8, &bytes, 8);
        } else if (length >= 4) {
            const uint32_t bytes = 0x01010101u * codeClass;
            memcpy(at, &bytes, 4);
            memcpy(at + length - 4, &bytes, 4);
        } else {
            at[0] = codeClass;
            at[length - 1] = codeClass;
            at[length / 2] = codeClass;
        }
    }
    // The '\n' that ends the line of `at`, or `end`.
    size_t LineEnd(size_t at, size_t end) const {
        const void* found = at < end ? memchr(s + at, '\n', end - at) : nullptr;
        return found != nullptr ? (size_t)(static_cast<const char*>(found) - s) : end;
    }
    bool At(size_t at, size_t end, const char* literal, size_t length) const {
        return at <= end && end - at >= length && memcmp(s + at, literal, length) == 0;
    }
    // The end of the first `close` (`length` bytes) from `at`, or `end`.
    size_t Through(size_t at, size_t end, const char* close, size_t length) const {
        while (at < end) {
            const void* found = memchr(s + at, close[0], end - at);
            if (found == nullptr) return end;
            at = (size_t)(static_cast<const char*>(found) - s);
            if (end - at >= length && memcmp(s + at, close, length) == 0) return at + length;
            at++;
        }
        return end;
    }
};

// ---------------------------------------------------------------------------------------------
// Words

constexpr uint8_t kDefinerBit = 0x80;   // in a word's kind: the name after it is defined
constexpr uint8_t kClassBits = 0x0F;

// The words of a language, in an open-addressed hash table built on its first use.
class WordTable {
public:
    WordTable(const char* own, const char* base, bool caselessWords) : caseless(caselessWords) {
        const size_t count = Count(own) + Count(base);
        size_t size = 16;
        while (size < count * 2) size <<= 1;
        slots.assign(size, Slot{});
        mask = size - 1;
        Add(own);
        Add(base);
    }

    // The word's kind (a class, kDefinerBit), or 0.
    uint8_t Find(const char* word, size_t length) const {
        if (length < shortest || length > longest) return 0;
        for (size_t at = Hash(word, length) & mask;; at = (at + 1) & mask) {
            const Slot& slot = slots[at];
            if (slot.length == 0) return 0;
            if (slot.length == length &&
                (caseless ? SameCaseless(word, slot.word, length) : SameBytes(word, slot.word, length))) {
                return slot.kind;
            }
        }
    }

private:
    struct Slot {
        const char* word = nullptr;
        uint8_t length = 0;
        uint8_t kind = 0;
    };

    static size_t Count(const char* list) {
        size_t count = 0;
        for (const char* at = list; at != nullptr && *at != '\0'; at++) count += *at > ' ' && (at[1] <= ' ');
        return count;
    }

    // Of the length and four bytes, the first two and the last two: constant time for any
    // word, where a hash of every byte took an eighth of the lexer's time. Words that share
    // them only take another probe.
    uint32_t Hash(const char* word, size_t length) const {
        const size_t second = length > 1 ? 1 : 0;
        const size_t penultimate = length > 1 ? length - 2 : 0;
        uint32_t bytes = (uint32_t)(uint8_t)word[0] | (uint32_t)(uint8_t)word[second] << 8 |
            (uint32_t)(uint8_t)word[penultimate] << 16 | (uint32_t)(uint8_t)word[length - 1] << 24;
        if (caseless) bytes |= 0x20202020u;   // the hash of either case; Find compares exactly
        uint32_t hash = (bytes ^ (uint32_t)length * 0x9E3779B9u) * 0x85EBCA6Bu;
        return hash ^ (hash >> 15);
    }

    void Add(const char* list) {
        uint8_t kind = kCodeKeyword;
        for (const char* at = list; at != nullptr && *at != '\0';) {
            if (*at == '\001' || *at == '\002' || *at == '\003') {
                kind = *at == '\001' ? (uint8_t)kCodeKeyword : *at == '\002' ? (uint8_t)kCodeConstant : (uint8_t)(kCodeKeyword | kDefinerBit);
                at++;
                continue;
            }
            if (*at == ' ') {
                at++;
                continue;
            }
            const char* word = at;
            while (*at > ' ') at++;
            Insert(word, (size_t)(at - word), kind);
        }
    }

    void Insert(const char* word, size_t length, uint8_t kind) {
        size_t at = Hash(word, length) & mask;
        for (; slots[at].length != 0; at = (at + 1) & mask) {
            if (slots[at].length == length && memcmp(slots[at].word, word, length) == 0) return;   // the first counts
        }
        slots[at] = Slot{ word, (uint8_t)length, kind };
        if (length < shortest) shortest = length;
        if (length > longest) longest = length;
    }

    std::vector<Slot> slots;
    size_t mask = 0;
    size_t shortest = 255;
    size_t longest = 0;
    bool caseless;
};

const WordTable& WordsOf(uint8_t language) {
    static std::atomic<const WordTable*> tables[kLanguageCount];
    static std::mutex building;
    const WordTable* table = tables[language].load(std::memory_order_acquire);
    if (table != nullptr) return *table;
    std::lock_guard<std::mutex> lock(building);
    table = tables[language].load(std::memory_order_relaxed);
    if (table == nullptr) {
        const LanguageSpec& spec = kSpecs[language];
        // Kept until the process ends, as the tables of every language used are.
        table = new WordTable(spec.words, spec.base != 0 ? kSpecs[spec.base].words : nullptr, (spec.flags & kCaseless) != 0);
        tables[language].store(table, std::memory_order_release);
    }
    return *table;
}

// ---------------------------------------------------------------------------------------------
// Code

// How String reads a string.
enum : unsigned {
    kEscapes = 1,          // a backslash takes the byte after it
    kTickEscapes = 2,      // a backtick does (PowerShell)
    kMultiline = 4,        // goes on over lines
    kDoubled = 8,          // two quotes stand for one
    kVariables = 16,       // $name and ${name} in variable colour
    kDollarCode = 32,      // ${code} lexed as code
    kHashCode = 64,        // #{code} lexed as code (Ruby)
};

constexpr int kMaxNesting = 8;   // strings in code in strings, beyond which the rest is string

// Where a shell word starts, where '#' starts a comment.
inline bool IsCommandBoundary(char c) { return IsOneOf(c, " \t\n;|&()"); }

// The code lexer: names and keywords, strings, comments, numbers, and what else a language of
// the C, script and shell families has.
class CodeLexer {
public:
    CodeLexer(uint8_t languageId, const Text& text)
        : t(text), s(text.s), language(languageId), flags(kSpecs[languageId].flags), words(WordsOf(languageId)),
          shellWords(languageId == kShell || languageId == kDockerfile || languageId == kPowerShell),
          prefixes(languageId == kPython || languageId == kC || languageId == kCpp || languageId == kRust) {}

    // Classifies [i, end), which ends where the text or the code of an interpolation does.
    void Run(size_t i, size_t end);

private:
    struct Heredoc {
        size_t word = 0;
        size_t length = 0;
        bool indented = false;
    };

    size_t Word(size_t i, size_t end, bool afterDefiner, bool& definer, bool& operand);
    size_t Prefixed(size_t i, size_t quote, size_t end);
    size_t CppRawString(size_t i, size_t quote, size_t end);
    size_t Number(size_t i, size_t end) const;
    size_t Quote(size_t from, size_t quote, size_t end);
    size_t RustQuote(size_t from, size_t quote, size_t end);
    size_t String(size_t from, size_t i, size_t end, char quote, unsigned how);
    size_t Interpolation(size_t i, size_t end);
    size_t Variable(size_t i, size_t end) const;
    size_t Dollar(size_t i, size_t end, char next);
    size_t At(size_t i, size_t end, char next);
    size_t Percent(size_t i, size_t end, char next, bool operand);
    size_t LineComment(size_t i, size_t end) const;
    size_t BlockComment(size_t i, size_t end, char close0, char close1) const;
    size_t HereString(size_t i, size_t end, char quote) const;
    size_t Regex(size_t i, size_t end) const;
    size_t HeredocOpener(size_t i, size_t end, Heredoc& heredoc) const;
    size_t HeredocBody(const Heredoc& heredoc, size_t i, size_t end) const;
    size_t LineStart(size_t i, size_t end, bool& definer) const;

    Text t;
    const char* s;
    uint8_t language;
    uint32_t flags;
    const WordTable& words;
    int nesting = 0;
    // Whether words take dashes (apt-get, Get-ChildItem) and a path's or option's words are no keywords.
    const bool shellWords;
    // Whether a word before a quote may be a string's prefix: r"", u8"", b''.
    const bool prefixes;
};

void CodeLexer::Run(size_t i, size_t end) {
    bool lineStart = true;     // only white space so far on this line
    bool operand = true;       // an operand starts here: '/' opens a regular expression
    bool definer = false;      // the last word defines the name that follows (class Name)
    size_t noRegexUntil = 0;   // a '/' before this opens none: one on its line did not close
    Heredoc heredoc;
    while (i < end) {
        const char c = s[i];
        if (c == '\n') {
            i++;
            lineStart = true;
            if (heredoc.length != 0) {
                i = HeredocBody(heredoc, i, end);
                heredoc.length = 0;
                lineStart = false;
            }
            operand = true;
            definer = false;
            continue;
        }
        if (IsSpace(c)) {
            do i++;
            while (i < end && IsSpace(s[i]));
            continue;
        }
        const bool afterDefiner = definer;
        definer = false;
        if (lineStart) {
            lineStart = false;
            const size_t next = LineStart(i, end, definer);
            if (next != i) {
                i = next;
                continue;
            }
        }
        if (IsWordStart(c) || (c == '$' && (flags & kDollarLetters))) {
            i = Word(i, end, afterDefiner, definer, operand);
            continue;
        }
        const char next = i + 1 < end ? s[i + 1] : '\0';
        if (IsDigit(c) || (c == '.' && IsDigit(next) && (i == 0 || !IsWord(s[i - 1])))) {
            i = Number(i, end);
            operand = false;
            continue;
        }
        size_t to = i;
        switch (c) {
        case '"':
        case '\'':
        case '`':
            to = Quote(i, i, end);
            break;
        case '/':
            if (next == '/' && (flags & kSlashLine)) {
                i = LineComment(i, end);
                continue;
            }
            if (next == '*' && (flags & kSlashBlock)) {
                i = BlockComment(i, end, '*', '/');
                continue;
            }
            if ((flags & kRegexes) && operand && i >= noRegexUntil) {
                to = Regex(i, end);
                if (to == i) noRegexUntil = t.LineEnd(i, end);
            }
            break;
        case '#':
            if ((flags & kHashLine) && (!(flags & kHashAtWord) || i == 0 || IsCommandBoundary(s[i - 1])) &&
                !(language == kPhp && next == '[')) {
                i = LineComment(i, end);
                continue;
            }
            break;
        case '-':
            if (next == '-' && (flags & kDashLine)) {
                i = LineComment(i, end);
                continue;
            }
            if (language == kPowerShell && IsWordStart(next) && (i == 0 || !IsWord(s[i - 1]))) {
                // An operator, -eq, or a parameter, -Path.
                to = i + 1;
                while (to < end && IsWord(s[to])) to++;
                const uint8_t kind = words.Find(s + i, to - i);
                t.Mark(i, to, kind & kClassBits);
            }
            break;
        case '<':
            if (language == kPowerShell && next == '#') {
                i = BlockComment(i, end, '#', '>');
                continue;
            }
            if ((flags & kHeredocs) && next == '<') {
                to = HeredocOpener(i, end, heredoc);
            } else if (language == kPhp && next == '?') {
                to = i + 2;
                while (to < end && IsWord(s[to])) to++;
                t.Mark(i, to, kCodeKeyword);
            }
            break;
        case '?':
            if (language == kPhp && next == '>') {
                to = i + 2;
                t.Mark(i, to, kCodeKeyword);
            }
            break;
        case '@':
            to = At(i, end, next);
            break;
        case '$':
            to = Dollar(i, end, next);
            break;
        case '%':
            to = Percent(i, end, next, operand);
            break;
        case '!':
            // A batch file's !name!, expanded when the line runs.
            if (language == kBatch && IsWordStart(next)) {
                size_t close = i + 1;
                while (close < end && IsWord(s[close])) close++;
                if (close < end && s[close] == '!') {
                    to = close + 1;
                    t.Mark(i, to, kCodeVariable);
                }
            }
            break;
        case ':':
            // A Ruby symbol, :name.
            if (language == kRuby && IsWordStart(next) && (i == 0 || s[i - 1] != ':')) {
                to = i + 1;
                while (to < end && IsWord(s[to])) to++;
                if (to < end && (s[to] == '?' || s[to] == '!')) to++;
                t.Mark(i, to, kCodeConstant);
            }
            break;
        default:
            break;
        }
        if (to != i) {
            i = to;
            operand = false;
            continue;
        }
        // Punctuation, in the plain colour. A '/' after it divides only after ')' and ']'.
        operand = c != ')' && c != ']';
        i++;
    }
}

size_t CodeLexer::Word(size_t i, size_t end, bool afterDefiner, bool& definer, bool& operand) {
    size_t j = i + 1;
    for (;;) {
        while (j < end && IsWord(s[j])) j++;
        if (j >= end) break;
        if (s[j] == '$' && (flags & kDollarLetters)) j++;
        else if (s[j] == '-' && shellWords && j + 1 < end && IsWordStart(s[j + 1])) j += 2;   // apt-get
        else break;
    }
    if (language == kRuby && j < end && (s[j] == '?' || s[j] == '!') && !(j + 1 < end && s[j + 1] == '=')) j++;
    if (prefixes && j < end && (s[j] == '"' || s[j] == '\'' || s[j] == '#')) {
        const size_t to = Prefixed(i, j, end);
        if (to != j) {
            operand = false;
            return to;
        }
    }
    // A member, obj.name and obj->name, is no keyword; nor in a shell is a part of a path or option.
    const char before = i > 0 ? s[i - 1] : '\n';
    const bool member = before == '.' || (before == '>' && i > 1 && s[i - 2] == '-') ||
        (shellWords && (before == '/' || before == '-'));
    const uint8_t kind = member ? 0 : words.Find(s + i, j - i);
    uint8_t codeClass = kCodePlain;
    if (kind != 0) {
        codeClass = kind & kClassBits;
        definer = (kind & kDefinerBit) != 0;
    } else if (afterDefiner) {
        codeClass = kCodeEntity;
    } else if (language == kRuby && s[i] >= 'A' && s[i] <= 'Z') {
        codeClass = kCodeConstant;
    } else if (language == kRuby && j + 1 < end && s[j] == ':' && s[j + 1] != ':') {
        codeClass = kCodeConstant;   // a key of a hash, name:
        j++;
    } else if ((flags & kTypeSuffix) && j - i > 2 && s[j - 2] == '_' && s[j - 1] == 't') {
        codeClass = kCodeConstant;
    } else if (language == kPowerShell && memchr(s + i, '-', j - i) != nullptr) {
        codeClass = kCodeConstant;   // a cmdlet, Verb-Noun
    } else if (language == kRust && j < end && s[j] == '!' && !(j + 1 < end && s[j + 1] == '=')) {
        codeClass = kCodeEntity;     // a macro, name!
        j++;
    } else if (flags & kCalls) {
        size_t k = j;
        while (k < end && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k < end && s[k] == '(') codeClass = kCodeEntity;
    }
    if (codeClass != kCodePlain) t.Mark(i, j, codeClass);   // the classes start plain
    operand = codeClass == kCodeKeyword;
    return j;
}

// A string with a prefix, from s[i]: Python's r"", b"", f"" and their pairs; C and C++'s L"",
// u8"" and R"(raw)"; Rust's b"", c"", r"", br"", r#""# and b''. Returns its end, or `quote` when
// the word s[i, quote) is no prefix of the language.
size_t CodeLexer::Prefixed(size_t i, size_t quote, size_t end) {
    const size_t length = quote - i;
    if (length > 3) return quote;
    const char* p = s + i;
    const char q = s[quote];
    if (language == kPython) {
        if (q == '#' || length > 2) return quote;
        for (size_t k = 0; k < length; k++) {
            if (!IsOneOf(p[k], "rbuftRBUFT")) return quote;
        }
        return Quote(i, quote, end);
    }
    if (language == kC || language == kCpp) {
        if (q == '#') return quote;
        const bool raw = language == kCpp && q == '"' && p[length - 1] == 'R';
        const size_t encoding = raw ? length - 1 : length;
        const bool known = encoding == 0 || (encoding == 1 && IsOneOf(p[0], "uUL")) ||
            (encoding == 2 && p[0] == 'u' && p[1] == '8');
        if (!known || (!raw && encoding == 0)) return quote;
        return raw ? CppRawString(i, quote, end) : Quote(i, quote, end);
    }
    if (language == kRust) {
        if (q == '\'') return length == 1 && p[0] == 'b' ? String(i, quote + 1, end, '\'', kEscapes) : quote;
        const bool raw = p[length - 1] == 'r';
        const size_t encoding = raw ? length - 1 : length;
        if (encoding > 1 || (encoding == 1 && p[0] != 'b' && p[0] != 'c') || (!raw && encoding == 0)) return quote;
        if (!raw) return q == '"' ? String(i, quote + 1, end, '"', kEscapes | kMultiline) : quote;
        size_t j = quote;
        size_t hashes = 0;
        while (j < end && s[j] == '#' && hashes < 255) {
            j++;
            hashes++;
        }
        if (j >= end || s[j] != '"') return quote;   // r#name, a raw identifier
        for (j++; j < end; j++) {
            if (s[j] != '"') continue;
            size_t k = 0;
            while (k < hashes && j + 1 + k < end && s[j + 1 + k] == '#') k++;
            if (k == hashes) {
                j += 1 + hashes;
                break;
            }
        }
        if (j > end) j = end;
        t.Mark(i, j, kCodeString);
        return j;
    }
    return quote;
}

// C++'s R"delimiter( ... )delimiter", from s[i] with its quote at s[quote].
size_t CodeLexer::CppRawString(size_t i, size_t quote, size_t end) {
    const size_t open = quote + 1;
    size_t paren = open;
    while (paren < end && paren - open < 16 && !IsOneOf(s[paren], "()\\\" \t\r\n")) paren++;
    if (paren >= end || s[paren] != '(') return Quote(i, quote, end);
    const size_t delimiter = paren - open;
    size_t j = paren + 1;
    for (; j < end; j++) {
        if (s[j] == ')' && end - j > delimiter + 1 && memcmp(s + j + 1, s + open, delimiter) == 0 && s[j + 1 + delimiter] == '"') {
            j += delimiter + 2;
            break;
        }
    }
    t.Mark(i, j, kCodeString);
    return j;
}

size_t CodeLexer::Number(size_t i, size_t end) const {
    size_t j = i;
    const bool hex = s[j] == '0' && j + 1 < end && (s[j + 1] == 'x' || s[j + 1] == 'X');
    if (hex) j += 2;
    while (j < end) {
        const char c = s[j];
        if (IsAsciiWord(c)) {
            j++;
            const char lower = Lower(c);
            if (((lower == 'e' && !hex) || (lower == 'p' && hex)) && j < end && (s[j] == '+' || s[j] == '-')) j++;
        } else if ((c == '.' && j + 1 < end && IsDigit(s[j + 1])) ||
            (c == '\'' && language == kCpp && j + 1 < end && IsAsciiWord(s[j + 1]))) {
            j++;   // 1.5, and C++'s 1'000'000
        } else {
            break;
        }
    }
    t.Mark(i, j, kCodeNumber);
    return j;
}

// The string whose opening quote is s[quote] and whose colour starts at `from`, where a prefix
// does. Returns its end, or `quote` when the quote opens none here.
size_t CodeLexer::Quote(size_t from, size_t quote, size_t end) {
    const char q = s[quote];
    if (q == '`') {
        if (flags & kTemplates) return String(from, quote + 1, end, '`', kEscapes | kMultiline | kDollarCode);
        if (flags & kBacktickStrings) return String(from, quote + 1, end, '`', (language == kGo ? 0u : (unsigned)kEscapes) | kMultiline);
        return quote;
    }
    if ((flags & kTripleQuotes) && (q == '"' || (flags & kQuoteStrings)) && end - quote >= 3 && s[quote + 1] == q &&
        s[quote + 2] == q) {
        const bool escapes = language != kKotlin && language != kCSharp;
        size_t j = quote + 3;
        while (j < end) {
            if (s[j] == '\\' && escapes) {
                j += 2;
            } else if (s[j] == q && end - j >= 3 && s[j + 1] == q && s[j + 2] == q) {
                j += 3;
                while (j < end && s[j] == q) j++;   // """a"""" closes with the last three
                break;
            } else {
                j++;
            }
        }
        if (j > end) j = end;
        t.Mark(from, j, kCodeString);
        return j;
    }
    const unsigned lines = (flags & kLongStrings) ? (unsigned)kMultiline : 0u;
    if (q == '\'') {
        if (language == kRust) return RustQuote(from, quote, end);
        if (!(flags & kQuoteStrings)) return String(from, quote + 1, end, '\'', kEscapes);   // a character
        if (flags & kRawQuotes) return String(from, quote + 1, end, '\'', kDoubled | lines);
        return String(from, quote + 1, end, '\'', kEscapes | lines);
    }
    unsigned how = kEscapes | lines;
    if (language == kPowerShell) {
        how = kTickEscapes | lines | kVariables;
    } else if (language == kSql) {
        how = kDoubled | lines;
    } else if (language == kBatch) {
        how = 0;
    } else {
        if (flags & kDollarNames) how |= kVariables;
        if (flags & kInterpolation) how |= language == kRuby ? kHashCode : (kVariables | kDollarCode);
    }
    return String(from, quote + 1, end, '"', how);
}

// Rust's 'x' and '\n' are characters; 'a and 'static are lifetimes and labels.
size_t CodeLexer::RustQuote(size_t from, size_t quote, size_t end) {
    const size_t i = quote + 1;
    if (i < end && s[i] == '\\') return String(from, i, end, '\'', kEscapes);
    if (i + 1 < end && s[i + 1] == '\'' && s[i] != '\n') {
        t.Mark(from, i + 2, kCodeString);
        return i + 2;
    }
    if (i < end && (unsigned char)s[i] >= 0xC0) {   // a character of more than one byte
        size_t k = i + 1;
        while (k < end && ((unsigned char)s[k] & 0xC0) == 0x80) k++;
        if (k < end && s[k] == '\'') {
            t.Mark(from, k + 1, kCodeString);
            return k + 1;
        }
    }
    if (i < end && IsWordStart(s[i])) {
        size_t j = i + 1;
        while (j < end && IsWord(s[j])) j++;
        t.Mark(from, j, kCodeKeyword);
        return j;
    }
    return quote;
}

// The string from `from` whose text starts at s[i] and closes with `quote`, read as `how` says;
// one that may not go on over lines ends unclosed at the end of its line.
size_t CodeLexer::String(size_t from, size_t i, size_t end, char quote, unsigned how) {
    size_t segment = from;   // where the part of the string not yet marked starts
    size_t j = i;
    while (j < end) {
        const char c = s[j];
        if (c == quote) {
            if ((how & kDoubled) && j + 1 < end && s[j + 1] == quote) {
                j += 2;
                continue;
            }
            j++;
            break;
        }
        if ((c == '\\' && (how & kEscapes)) || (c == '`' && (how & kTickEscapes))) {
            j += 2;
            continue;
        }
        if (c == '\n' && !(how & kMultiline)) break;
        if (c == '$' && (how & kDollarCode) && j + 1 < end && s[j + 1] == '{') {
            t.Mark(segment, j + 2, kCodeString);
            segment = j = Interpolation(j + 2, end);
            continue;
        }
        if (c == '#' && (how & kHashCode) && j + 1 < end && s[j + 1] == '{') {
            t.Mark(segment, j + 2, kCodeString);
            segment = j = Interpolation(j + 2, end);
            continue;
        }
        if (c == '$' && (how & kVariables)) {
            const size_t to = Variable(j, end);
            if (to != j) {
                t.Mark(segment, j, kCodeString);
                t.Mark(j, to, kCodeVariable);
                segment = j = to;
                continue;
            }
        }
        j++;
    }
    if (j > end) j = end;
    t.Mark(segment, j, kCodeString);
    return j;
}

// The code of ${...} or #{...} in a string, from s[i] after its opening brace: lexed as code.
// Returns the end of its closing brace, which takes the string's colour.
size_t CodeLexer::Interpolation(size_t i, size_t end) {
    size_t close = i;
    for (int open = 1; close < end; close++) {
        if (s[close] == '{') open++;
        else if (s[close] == '}' && --open == 0) break;
    }
    if (nesting < kMaxNesting) {
        nesting++;
        Run(i, close);
        nesting--;
    } else {
        t.Mark(i, close, kCodeString);
    }
    if (close >= end) return end;
    t.Mark(close, close + 1, kCodeString);
    return close + 1;
}

// The end of the variable at s[i], a '$': $name, ${...}, and in shells $1, $? and the like; or i.
size_t CodeLexer::Variable(size_t i, size_t end) const {
    if (i + 1 >= end) return i;
    const char c = s[i + 1];
    if (c == '{') {   // to its closing brace on the line
        size_t j = i + 2;
        for (int open = 1; j < end && s[j] != '\n'; j++) {
            if (s[j] == '{') open++;
            else if (s[j] == '}' && --open == 0) return j + 1;
        }
        return j;
    }
    if (IsWordStart(c)) {
        size_t j = i + 2;
        while (j < end && IsWord(s[j])) j++;
        if (language == kPowerShell && j + 1 < end && s[j] == ':' && IsWordStart(s[j + 1])) {   // $env:Path
            j += 2;
            while (j < end && IsWord(s[j])) j++;
        }
        return j;
    }
    if (shellWords && (IsDigit(c) || IsOneOf(c, "?#@*!$-^"))) return i + 2;
    return i;
}

size_t CodeLexer::Dollar(size_t i, size_t end, char next) {
    if (language == kCSharp && (next == '"' || next == '@')) {   // $"{x}" and $@"..."
        if (next == '"') return String(i, i + 2, end, '"', kEscapes);
        if (i + 2 < end && s[i + 2] == '"') return String(i, i + 3, end, '"', kDoubled | kMultiline);
        return i;
    }
    if ((language == kShell || language == kDockerfile) && next == '\'') return String(i, i + 2, end, '\'', kEscapes);
    if ((flags & kDollarNames) || language == kRuby) {
        const size_t to = Variable(i, end);
        t.Mark(i, to, kCodeVariable);
        return to;
    }
    return i;
}

size_t CodeLexer::At(size_t i, size_t end, char next) {
    if (language == kCSharp) {   // @"verbatim" and @$"..."
        if (next == '"') return String(i, i + 2, end, '"', kDoubled | kMultiline);
        if (next == '$' && i + 2 < end && s[i + 2] == '"') return String(i, i + 3, end, '"', kDoubled | kMultiline);
        return i;
    }
    if (language == kPowerShell && (next == '"' || next == '\'')) {
        const size_t to = HereString(i, end, next);
        if (to != i) return to;
    }
    if ((language == kRuby || language == kPowerShell) && (next == '@' || IsWordStart(next))) {   // @name, @@name
        size_t j = i + 1;
        while (j < end && s[j] == '@') j++;
        while (j < end && IsWord(s[j])) j++;
        t.Mark(i, j, kCodeVariable);
        return j;
    }
    if ((flags & kDecorators) && IsWordStart(next)) {   // @name, @scope.name
        size_t j = i + 1;
        while (j < end && (IsWord(s[j]) || (s[j] == '.' && j + 1 < end && IsWordStart(s[j + 1])))) j++;
        t.Mark(i, j, kCodeEntity);
        return j;
    }
    return i;
}

size_t CodeLexer::Percent(size_t i, size_t end, char next, bool operand) {
    if (language == kBatch) {   // %name%, %%i, %~dp0, %1, %*
        size_t j = i;
        if (next == '%' && i + 2 < end && IsWordStart(s[i + 2])) {
            j = i + 3;
        } else if (IsDigit(next) || next == '*') {
            j = i + 2;
        } else if (next == '~') {
            j = i + 2;
            while (j < end && IsWord(s[j])) j++;
        } else if (IsWordStart(next)) {
            size_t close = i + 1;
            while (close < end && s[close] != '%' && s[close] != '\n' && !IsSpace(s[close])) close++;
            if (close < end && s[close] == '%') j = close + 1;
        }
        t.Mark(i, j, kCodeVariable);
        return j;
    }
    if (language == kRuby && operand) {   // %w[a b], %q(text), %i{a b}, %r{re}
        size_t j = i + 1;
        if (IsOneOf(next, "qQwWiIrsx")) j++;
        if (j >= end) return i;
        const char open = s[j];
        const char* pairs = "()[]{}<>";
        const char* pair = IsOneOf(open, "([{<") ? strchr(pairs, open) : nullptr;
        if (pair == nullptr && !IsOneOf(open, "|!/^")) return i;
        const char close = pair != nullptr ? pair[1] : open;
        int depth = 1;
        for (j++; j < end; j++) {
            if (s[j] == '\\') {
                j++;
            } else if (s[j] == close) {
                if (--depth == 0) break;
            } else if (s[j] == open && pair != nullptr) {
                depth++;
            }
        }
        j = j < end ? j + 1 : end;
        t.Mark(i, j, kCodeString);
        return j;
    }
    return i;
}

size_t CodeLexer::LineComment(size_t i, size_t end) const {
    const size_t to = t.LineEnd(i, end);
    t.Mark(i, to, kCodeComment);
    return to;
}

// A comment from s[i] that `close0 close1` closes; in Rust, Kotlin and Swift they nest. One left
// open runs to the end.
size_t CodeLexer::BlockComment(size_t i, size_t end, char close0, char close1) const {
    const char open0 = s[i];
    const char open1 = s[i + 1];
    const bool nests = (flags & kNestedBlocks) != 0;
    size_t j = i + 2;
    for (int open = 1; j < end;) {
        if (s[j] == close0 && j + 1 < end && s[j + 1] == close1) {
            j += 2;
            if (--open == 0) break;
        } else if (nests && s[j] == open0 && j + 1 < end && s[j + 1] == open1) {
            j += 2;
            open++;
        } else {
            j++;
        }
    }
    t.Mark(i, j, kCodeComment);
    return j;
}

// PowerShell's here-strings: @" or @' ends its line, and the line that starts with "@ or '@
// closes it.
size_t CodeLexer::HereString(size_t i, size_t end, char quote) const {
    size_t j = i + 2;
    while (j < end && IsSpace(s[j])) j++;
    if (j < end && s[j] != '\n') return i;
    while (j < end) {
        const size_t line = j + 1;
        j = t.LineEnd(line, end);
        if (line + 1 < end && s[line] == quote && s[line + 1] == '@') {
            j = line + 2;
            break;
        }
    }
    t.Mark(i, j, kCodeString);
    return j;
}

// A regular expression, /.../flags, from s[i] to a closing slash on its line; or i.
size_t CodeLexer::Regex(size_t i, size_t end) const {
    bool inClass = false;   // in [...], where '/' does not close
    for (size_t j = i + 1; j < end && s[j] != '\n'; j++) {
        const char c = s[j];
        if (c == '\\') {
            if (j + 1 < end && s[j + 1] == '\n') break;
            j++;
        } else if (c == '[') {
            inClass = true;
        } else if (c == ']') {
            inClass = false;
        } else if (c == '/' && !inClass) {
            j++;
            while (j < end && IsAsciiWord(s[j])) j++;
            t.Mark(i, j, kCodeString);
            return j;
        }
    }
    return i;
}

// The opener of a here-document at s[i]: <<WORD, <<-WORD, <<'WORD' (shell), <<~WORD (Ruby),
// <<<WORD (PHP). Its body starts on the next line (HeredocBody).
size_t CodeLexer::HeredocOpener(size_t i, size_t end, Heredoc& heredoc) const {
    size_t j = i + 2;
    if (language == kPhp) {
        if (j >= end || s[j] != '<') return i;
        j++;
    } else if (j < end && s[j] == '<') {
        return i;   // a shell's <<< here-string
    }
    bool indented = language == kPhp;
    if (language != kPhp && j < end && (s[j] == '-' || s[j] == '~')) {
        indented = true;
        j++;
    }
    bool spaced = false;
    if (language != kRuby) {
        while (j < end && (s[j] == ' ' || s[j] == '\t')) {
            j++;
            spaced = true;
        }
    }
    char quote = 0;
    if (j < end && (s[j] == '\'' || s[j] == '"')) quote = s[j++];
    else if (j < end && s[j] == '\\' && language != kPhp) j++;
    const size_t word = j;
    while (j < end && IsWord(s[j])) j++;
    if (j == word || !IsWordStart(s[word])) return i;
    // Without quotes, a word in lower case is a shift, a << b, after a space or in Ruby.
    if (quote == 0 && (spaced || language == kRuby) && !(s[word] >= 'A' && s[word] <= 'Z')) return i;
    const size_t length = j - word;
    if (quote != 0) {
        if (j >= end || s[j] != quote) return i;
        j++;
    }
    heredoc = Heredoc{ word, length, indented };
    t.Mark(i, j, kCodeString);
    return j;
}

// The body of a here-document from s[i], the start of the line after its opener, through the
// word that closes it alone on a line (PHP: before ';', ',' or ')').
size_t CodeLexer::HeredocBody(const Heredoc& heredoc, size_t i, size_t end) const {
    while (i < end) {
        const size_t lineEnd = t.LineEnd(i, end);
        size_t at = i;
        if (heredoc.indented) {
            while (at < lineEnd && IsSpace(s[at])) at++;
        }
        if (lineEnd - at >= heredoc.length && memcmp(s + at, s + heredoc.word, heredoc.length) == 0) {
            const size_t after = at + heredoc.length;
            size_t rest = after;
            while (rest < lineEnd && IsSpace(s[rest])) rest++;
            if (rest == lineEnd || (language == kPhp && !IsWord(s[after]))) {
                t.Mark(i, after, kCodeString);
                return after;
            }
        }
        t.Mark(i, lineEnd, kCodeString);
        i = lineEnd + 1;
    }
    return end;
}

// What starts only a line, at its first byte s[i] after white space: a C preprocessor directive,
// Ruby's =begin comment, a batch file's comment and label. Returns its end, or i.
size_t CodeLexer::LineStart(size_t i, size_t end, bool& definer) const {
    const char c = s[i];
    if (c == '#' && (flags & kDirectives)) {
        size_t j = i + 1;
        while (j < end && IsSpace(s[j])) j++;
        const size_t word = j;
        while (j < end && IsWord(s[j])) j++;
        t.Mark(i, j, kCodeKeyword);
        const size_t length = j - word;
        definer = length == 6 && memcmp(s + word, "define", 6) == 0;
        if ((length == 7 && memcmp(s + word, "include", 7) == 0) || (length == 6 && memcmp(s + word, "import", 6) == 0)) {
            size_t k = j;
            while (k < end && IsSpace(s[k])) k++;
            if (k < end && s[k] == '<') {   // #include <file>
                const size_t lineEnd = t.LineEnd(k, end);
                const void* close = memchr(s + k, '>', lineEnd - k);
                if (close != nullptr) {
                    const size_t to = (size_t)(static_cast<const char*>(close) - s) + 1;
                    t.Mark(k, to, kCodeString);
                    return to;
                }
            }
        }
        return j;
    }
    if (language == kRuby && c == '=' && (i == 0 || s[i - 1] == '\n') && t.At(i + 1, end, "begin", 5) &&
        (end - i == 6 || !IsWord(s[i + 6]))) {
        size_t j = t.LineEnd(i, end);
        while (j < end) {   // through the line that starts with =end
            const size_t line = j + 1;
            j = t.LineEnd(line, end);
            if (t.At(line, end, "=end", 4)) break;
        }
        t.Mark(i, j, kCodeComment);
        return j;
    }
    if (language == kBatch) {
        const size_t j = c == '@' ? i + 1 : i;
        const size_t lineEnd = t.LineEnd(i, end);
        if (j + 1 < lineEnd && s[j] == ':') {   // a ":: comment" or a ":label"
            t.Mark(i, lineEnd, s[j + 1] == ':' ? kCodeComment : kCodeEntity);
            return lineEnd;
        }
        if (lineEnd - j >= 3 && SameCaseless(s + j, "rem", 3) && (lineEnd - j == 3 || IsSpace(s[j + 3]))) {
            t.Mark(i, lineEnd, kCodeComment);
            return lineEnd;
        }
    }
    return i;
}

// ---------------------------------------------------------------------------------------------
// Terminal sessions

// The end of the prompt line [i, end) starts with, the white space after it included, or i. A
// prompt is "$", "#", "%" or ">" alone, or the end of a word with '@', ':' or '~' (user@host:~$),
// after an optional "(venv) "; "PS " starts PowerShell's. Python's console has ">>>" and "...".
size_t PromptEnd(const char* s, size_t i, size_t end, bool python, bool& powershell) {
    if (python) {
        if (end - i >= 3 && (memcmp(s + i, ">>>", 3) == 0 || memcmp(s + i, "...", 3) == 0) && (end - i == 3 || s[i + 3] == ' ')) {
            return end - i == 3 ? end : i + 4;
        }
        return i;
    }
    size_t p = i;
    if (p < end && s[p] == '(') {
        const void* close = memchr(s + p, ')', end - p);
        if (close == nullptr) return i;
        p = (size_t)(static_cast<const char*>(close) - s) + 1;
        while (p < end && s[p] == ' ') p++;
    }
    if (end - p >= 3 && memcmp(s + p, "PS ", 3) == 0) {
        powershell = true;
        p += 3;
    }
    size_t q = p;
    while (q < end && !IsSpace(s[q])) q++;
    if (q == p || !IsOneOf(s[q - 1], "$#%>")) return i;
    if (q - p > 1 && !powershell && memchr(s + p, '@', q - p) == nullptr && memchr(s + p, ':', q - p) == nullptr &&
        memchr(s + p, '~', q - p) == nullptr) {
        return i;
    }
    while (q < end && IsSpace(s[q])) q++;
    return q;
}

// A line that starts with a prompt holds a command, read by the shell's lexer (PowerShell's after
// "PS ", Python's in pycon) with the lines a trailing backslash continues it to; the prompt takes
// the comment colour. Other lines are the commands' output, plain.
void ClassifyConsole(const Text& t, size_t n, bool python) {
    const char* s = t.s;
    CodeLexer commands(python ? kPython : kShell, t);
    std::optional<CodeLexer> powershell;
    for (size_t i = 0; i < n;) {
        size_t lineEnd = t.LineEnd(i, n);
        bool ps = false;
        const size_t command = PromptEnd(s, i, lineEnd, python, ps);
        if (command != i) {
            t.Mark(i, command, kCodeComment);
            for (size_t line = i; !python && lineEnd < n;) {
                size_t last = lineEnd;
                while (last > line && IsSpace(s[last - 1])) last--;
                if (last == line || s[last - 1] != '\\') break;
                line = lineEnd + 1;
                lineEnd = t.LineEnd(line, n);
            }
            if (ps) {
                if (!powershell) powershell.emplace(kPowerShell, t);
                powershell->Run(command, lineEnd);
            } else {
                commands.Run(command, lineEnd);
            }
        }
        i = lineEnd + 1;
    }
}

// ---------------------------------------------------------------------------------------------
// Style sheets: CSS, SCSS, Less

class CssLexer {
public:
    CssLexer(const Text& text, bool scssSyntax) : t(text), s(text.s), scss(scssSyntax) {}

    // A statement, what lies before the next '{', ';' or '}', is a selector or at-rule when '{'
    // ends it, and otherwise a declaration, property: value.
    void Run(size_t i, size_t end) const {
        while (i < end) {
            const char c = s[i];
            if (IsSpace(c) || IsOneOf(c, "\n{};")) {
                i++;
                continue;
            }
            const size_t comment = Comment(i, end);
            if (comment != i) {
                i = comment;
                continue;
            }
            bool block = false;
            const size_t stop = StatementEnd(i, end, block);
            if (c == '@') {
                size_t j = i + 1;
                while (j < stop && IsName(s[j])) j++;
                t.Mark(i, j, kCodeKeyword);
                Values(j, stop);
            } else if (block) {
                Selector(i, stop);
            } else {
                Declaration(i, stop);
            }
            i = stop;
        }
    }

private:
    static bool IsName(char c) { return IsWord(c) || c == '-'; }

    size_t Comment(size_t i, size_t end) const {
        if (s[i] != '/' || i + 1 >= end) return i;
        size_t to = i;
        if (s[i + 1] == '*') to = t.Through(i + 2, end, "*/", 2);
        else if (s[i + 1] == '/' && scss) to = t.LineEnd(i, end);
        t.Mark(i, to, kCodeComment);
        return to;
    }

    // The end of a quoted string from s[i] on its line.
    size_t QuoteEnd(size_t i, size_t end) const {
        size_t j = i + 1;
        while (j < end && s[j] != s[i] && s[j] != '\n') j += s[j] == '\\' ? 2 : 1;
        return j < end && s[j] == s[i] ? j + 1 : (j < end ? j : end);
    }

    // The end of SCSS's #{...} from s[i].
    size_t InterpolationEnd(size_t i, size_t end) const {
        size_t j = i + 2;
        for (int open = 1; j < end; j++) {
            if (s[j] == '{') open++;
            else if (s[j] == '}' && --open == 0) return j + 1;
        }
        return end;
    }

    size_t StatementEnd(size_t i, size_t end, bool& block) const {
        int parens = 0;
        while (i < end) {
            const char c = s[i];
            if (c == '"' || c == '\'') {
                i = QuoteEnd(i, end);
            } else if (c == '/' && i + 1 < end && (s[i + 1] == '*' || (s[i + 1] == '/' && scss && parens == 0))) {
                i = s[i + 1] == '*' ? t.Through(i + 2, end, "*/", 2) : t.LineEnd(i, end);
            } else if (c == '#' && i + 1 < end && s[i + 1] == '{') {
                i = InterpolationEnd(i, end);
            } else {
                if (c == '(') parens++;
                else if (c == ')' && parens > 0) parens--;
                else if (parens == 0 && (c == '{' || c == ';' || c == '}')) {
                    block = c == '{';
                    return i;
                }
                i++;
            }
        }
        return end;
    }

    // Selectors: .class, #id, :pseudo and %placeholder in entity colour, element names in tag
    // colour, attribute names in entity colour.
    void Selector(size_t i, size_t stop) const {
        while (i < stop) {
            const char c = s[i];
            const size_t comment = Comment(i, stop);
            if (comment != i) {
                i = comment;
                continue;
            }
            if (c == '"' || c == '\'') {
                const size_t to = QuoteEnd(i, stop);
                t.Mark(i, to, kCodeString);
                i = to;
                continue;
            }
            if (c == '#' && i + 1 < stop && s[i + 1] == '{') {
                i = InterpolationEnd(i, stop);
                continue;
            }
            if (IsOneOf(c, ".#%:") && i + 1 < stop) {
                size_t j = i + 1;
                if (c == ':' && s[j] == ':') j++;
                const size_t name = j;
                while (j < stop && IsName(s[j])) j++;
                if (j > name) t.Mark(i, j, kCodeEntity);
                i = j > name ? j : i + 1;
                continue;
            }
            if (c == '$' && scss) {
                size_t j = i + 1;
                while (j < stop && IsName(s[j])) j++;
                t.Mark(i, j, kCodeVariable);
                i = j;
                continue;
            }
            if (c == '[') {
                size_t j = i + 1;
                while (j < stop && IsSpace(s[j])) j++;
                const size_t name = j;
                while (j < stop && IsName(s[j])) j++;
                t.Mark(name, j, kCodeEntity);
                // Its value: to the closing bracket, quoted or not.
                const size_t close = t.Through(j, stop, "]", 1);
                for (size_t v = j; v < close; v++) {
                    if (s[v] == '"' || s[v] == '\'') {
                        const size_t to = QuoteEnd(v, close);
                        t.Mark(v, to, kCodeString);
                        v = to - 1;
                    }
                }
                i = close;
                continue;
            }
            if (IsWordStart(c)) {
                size_t j = i + 1;
                while (j < stop && IsName(s[j])) j++;
                t.Mark(i, j, kCodeTag);
                i = j;
                continue;
            }
            i++;
        }
    }

    // property: value, the property in constant colour; SCSS's $name: value in variable colour.
    void Declaration(size_t i, size_t stop) const {
        size_t j = i;
        while (j < stop && (IsName(s[j]) || (j == i && (s[j] == '$' || s[j] == '@')))) j++;
        size_t colon = j;
        while (colon < stop && IsSpace(s[colon])) colon++;
        if (j > i && colon < stop && s[colon] == ':') {
            t.Mark(i, j, s[i] == '$' || s[i] == '@' ? kCodeVariable : kCodeConstant);
            Values(colon + 1, stop);
        } else {
            Values(i, stop);
        }
    }

    // Values: numbers and their units, #colours, strings, !important, functions(), url(...).
    void Values(size_t i, size_t stop) const {
        while (i < stop) {
            const char c = s[i];
            const size_t comment = Comment(i, stop);
            if (comment != i) {
                i = comment;
                continue;
            }
            const char next = i + 1 < stop ? s[i + 1] : '\0';
            size_t j = i + 1;
            if (c == '"' || c == '\'') {
                j = QuoteEnd(i, stop);
                t.Mark(i, j, kCodeString);
            } else if (IsDigit(c) || (IsOneOf(c, ".-+") && (IsDigit(next) || (next == '.' && i + 2 < stop && IsDigit(s[i + 2]))) &&
                (i == 0 || !IsName(s[i - 1])))) {
                while (j < stop && (IsWord(s[j]) || s[j] == '.' || s[j] == '%')) j++;
                t.Mark(i, j, kCodeNumber);
            } else if (c == '#' && next == '{') {
                j = InterpolationEnd(i, stop);
            } else if (c == '#' && IsWord(next)) {
                while (j < stop && IsWord(s[j])) j++;
                t.Mark(i, j, kCodeConstant);
            } else if (c == '!' && IsWordStart(next)) {
                while (j < stop && IsWord(s[j])) j++;
                t.Mark(i, j, kCodeKeyword);
            } else if ((c == '$' || c == '@') && scss && IsWordStart(next)) {
                while (j < stop && IsName(s[j])) j++;
                t.Mark(i, j, kCodeVariable);
            } else if (IsWordStart(c) || (c == '-' && (IsWordStart(next) || next == '-'))) {
                while (j < stop && IsName(s[j])) j++;
                if (j < stop && s[j] == '(') {
                    t.Mark(i, j, kCodeEntity);
                    if (j - i == 3 && memcmp(s + i, "url", 3) == 0) {   // url(...) without quotes is a string
                        size_t k = j + 1;
                        while (k < stop && IsSpace(s[k])) k++;
                        if (k < stop && s[k] != '"' && s[k] != '\'') {
                            const void* close = memchr(s + k, ')', stop - k);
                            j = close != nullptr ? (size_t)(static_cast<const char*>(close) - s) : stop;
                            t.Mark(k, j, kCodeString);
                        }
                    }
                }
            }
            i = j;
        }
    }

    Text t;
    const char* s;
    bool scss;
};

// ---------------------------------------------------------------------------------------------
// Markup: HTML, XML

class MarkupLexer {
public:
    MarkupLexer(const Text& text, bool htmlSyntax) : t(text), s(text.s), html(htmlSyntax) {}

    void Run(size_t i, size_t end) const {
        while (i < end) {
            const char c = s[i];
            if (c == '&') {
                i = Reference(i, end);
                continue;
            }
            if (c != '<') {
                i++;
                continue;
            }
            if (t.At(i, end, "<!--", 4)) {
                const size_t to = t.Through(i + 4, end, "-->", 3);
                t.Mark(i, to, kCodeComment);
                i = to;
                continue;
            }
            if (t.At(i, end, "<![CDATA[", 9)) {
                const size_t to = t.Through(i + 9, end, "]]>", 3);
                t.Mark(i, to, kCodeString);
                i = to;
                continue;
            }
            size_t j = i + 1;
            const bool declaration = j < end && (s[j] == '!' || s[j] == '?');   // <!DOCTYPE html>, <?xml ...?>
            const bool closing = j < end && s[j] == '/';
            if (declaration || closing) j++;
            const size_t name = j;
            while (j < end && (IsWord(s[j]) || IsOneOf(s[j], "-:."))) j++;
            if (j == name || !IsWordStart(s[name])) {
                i++;
                continue;
            }
            t.Mark(declaration ? i : name, j, declaration ? kCodeKeyword : kCodeTag);
            i = Attributes(j, end);
            if (html && !closing && !declaration && s[i - 1] == '>' && s[i - 2] != '/') {
                if (j - name == 6 && SameCaseless(s + name, "script", 6)) i = Embedded(i, end, "</script", false);
                else if (j - name == 5 && SameCaseless(s + name, "style", 5)) i = Embedded(i, end, "</style", true);
            }
        }
    }

private:
    // &name; and &#123;: constant colour.
    size_t Reference(size_t i, size_t end) const {
        size_t j = i + 1;
        if (j < end && s[j] == '#') j++;
        const size_t name = j;
        while (j < end && j - name < 32 && IsAsciiWord(s[j])) j++;
        if (j == name || j >= end || s[j] != ';') return i + 1;
        t.Mark(i, j + 1, kCodeConstant);
        return j + 1;
    }

    // A tag's attributes from s[i] through its '>': names in entity colour, values in string colour.
    size_t Attributes(size_t i, size_t end) const {
        while (i < end) {
            const char c = s[i];
            if (c == '>') return i + 1;
            if (c == '<') return i;   // a tag left open
            size_t j = i + 1;
            if (c == '"' || c == '\'') {
                const void* close = memchr(s + j, c, end - j);
                j = close != nullptr ? (size_t)(static_cast<const char*>(close) - s) + 1 : end;
                t.Mark(i, j, kCodeString);
            } else if (c == '=') {
                while (j < end && (IsSpace(s[j]) || s[j] == '\n')) j++;
                if (j < end && !IsOneOf(s[j], "\"'>")) {   // a value without quotes
                    const size_t value = j;
                    while (j < end && !IsSpace(s[j]) && s[j] != '\n' && s[j] != '>') j++;
                    t.Mark(value, j, kCodeString);
                }
            } else if (!IsSpace(c) && c != '\n' && c != '/' && c != '?') {   // a name: id, v-if, @click, :value
                while (j < end && !IsSpace(s[j]) && !IsOneOf(s[j], "\n=>/\"'<")) j++;
                t.Mark(i, j, kCodeEntity);
            }
            i = j;
        }
        return end;
    }

    // The contents of a script or style element from s[i] to its closing tag, as JavaScript or CSS.
    size_t Embedded(size_t i, size_t end, const char* closeTag, bool css) const {
        const size_t length = strlen(closeTag);
        size_t close = i;
        while (close < end) {
            const void* found = memchr(s + close, '<', end - close);
            if (found == nullptr) {
                close = end;
                break;
            }
            close = (size_t)(static_cast<const char*>(found) - s);
            if (end - close >= length && SameCaseless(s + close, closeTag, length)) break;
            close++;
        }
        if (css) CssLexer(t, false).Run(i, close);
        else CodeLexer(kJavaScript, t).Run(i, close);
        return close;
    }

    Text t;
    const char* s;
    bool html;
};

// ---------------------------------------------------------------------------------------------
// Data: JSON, YAML, TOML, INI; and diffs

// JSON, with JSON5's and JSONC's comments and single quotes: keys in constant colour, string
// values in string colour, numbers, true, false and null.
void ClassifyJson(const Text& t, size_t n) {
    const char* s = t.s;
    for (size_t i = 0; i < n;) {
        const char c = s[i];
        size_t j = i + 1;
        if (c == '"' || c == '\'') {
            while (j < n && s[j] != c && s[j] != '\n') j += s[j] == '\\' ? 2 : 1;
            j = j < n && s[j] == c ? j + 1 : (j < n ? j : n);
            size_t k = j;
            while (k < n && IsSpace(s[k])) k++;
            t.Mark(i, j, k < n && s[k] == ':' ? kCodeConstant : kCodeString);
        } else if (c == '/' && j < n && (s[j] == '/' || s[j] == '*')) {
            j = s[j] == '/' ? t.LineEnd(i, n) : t.Through(i + 2, n, "*/", 2);
            t.Mark(i, j, kCodeComment);
        } else if (IsDigit(c) || (c == '-' && j < n && IsDigit(s[j]))) {
            while (j < n && (IsAsciiWord(s[j]) || s[j] == '.' || ((s[j] == '+' || s[j] == '-') && Lower(s[j - 1]) == 'e'))) j++;
            t.Mark(i, j, kCodeNumber);
        } else if (IsWordStart(c)) {
            while (j < n && IsWord(s[j])) j++;
            const size_t length = j - i;
            size_t k = j;
            while (k < n && IsSpace(s[k])) k++;
            if ((length == 4 && (memcmp(s + i, "true", 4) == 0 || memcmp(s + i, "null", 4) == 0)) ||
                (length == 5 && memcmp(s + i, "false", 5) == 0) || (k < n && s[k] == ':')) {
                t.Mark(i, j, kCodeConstant);
            }
        }
        i = j;
    }
}

inline bool IsFlowIndicator(char c) { return IsOneOf(c, ",[]{}"); }

// The class of a plain YAML scalar: true, null and their kin (YAML 1.1's yes, no, on and off
// too), numbers of the core schema, or a string.
uint8_t YamlScalar(const char* s, size_t length) {
    static const char* const kConstants[] = { "true", "false", "null", "yes", "no", "on", "off", "~" };
    for (const char* constant : kConstants) {
        if (strlen(constant) == length && SameCaseless(s, constant, length)) return kCodeConstant;
    }
    size_t k = length > 0 && (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (length - k == 4 && (SameCaseless(s + k, ".inf", 4) || SameCaseless(s + k, ".nan", 4))) return kCodeNumber;
    if (length - k > 2 && s[k] == '0' && (s[k + 1] == 'x' || s[k + 1] == 'o')) {
        for (k += 2; k < length && (IsDigit(s[k]) || (Lower(s[k]) >= 'a' && Lower(s[k]) <= 'f')); k++) {}
        return k == length ? kCodeNumber : kCodeString;
    }
    bool digits = false;
    while (k < length && (IsDigit(s[k]) || s[k] == '_')) digits |= IsDigit(s[k++]);
    if (k < length && s[k] == '.') {
        for (k++; k < length && (IsDigit(s[k]) || s[k] == '_'); k++) digits = true;
    }
    if (digits && k < length && Lower(s[k]) == 'e') {
        k++;
        if (k < length && (s[k] == '+' || s[k] == '-')) k++;
        const size_t exponent = k;
        while (k < length && IsDigit(s[k])) k++;
        if (k == exponent) return kCodeString;
    }
    return digits && k == length ? kCodeNumber : kCodeString;
}

// YAML: keys in tag colour, values by their kind, comments, anchors and aliases in variable
// colour, tags in keyword colour, and block scalars (| and >) in string colour.
void ClassifyYaml(const Text& t, size_t n) {
    const char* s = t.s;
    size_t literalIndent = SIZE_MAX;   // a block scalar's lines are indented more than this
    int flow = 0;                      // the [ and { open
    for (size_t i = 0; i < n;) {
        const size_t end = t.LineEnd(i, n);
        size_t p = i;
        while (p < end && s[p] == ' ') p++;
        const size_t indent = p - i;
        size_t first = p;
        while (first < end && IsSpace(s[first])) first++;
        if (literalIndent != SIZE_MAX) {
            if (first == end || indent > literalIndent) {
                t.Mark(p, end, kCodeString);
                i = end + 1;
                continue;
            }
            literalIndent = SIZE_MAX;
        }
        if (indent == 0 && (t.At(i, end, "---", 3) || t.At(i, end, "...", 3)) && (end - i == 3 || IsSpace(s[i + 3]))) {
            t.Mark(i, i + 3, kCodeComment);
            p = i + 3;
        }
        while (p < end) {
            const char c = s[p];
            if (IsSpace(c) || c == ',') {
                p++;
                continue;
            }
            if (c == '#' && (p == i || IsSpace(s[p - 1]))) {
                t.Mark(p, end, kCodeComment);
                break;
            }
            if ((c == '-' || c == '?') && flow == 0 && (p + 1 == end || IsSpace(s[p + 1]))) {
                p++;
                continue;
            }
            if (c == '[' || c == '{' || c == ']' || c == '}') {
                flow += c == '[' || c == '{' ? 1 : (flow > 0 ? -1 : 0);
                p++;
                continue;
            }
            if ((c == '|' || c == '>') && flow == 0) {
                size_t q = p + 1;
                while (q < end && (s[q] == '+' || s[q] == '-' || IsDigit(s[q]))) q++;
                if (q == end || IsSpace(s[q])) {
                    literalIndent = indent;
                    t.Mark(p, q, kCodeKeyword);
                    p = q;
                    continue;
                }
            }
            if (c == '&' || c == '*' || c == '!') {
                size_t q = p + 1;
                while (q < end && !IsSpace(s[q]) && !(flow > 0 && IsFlowIndicator(s[q]))) q++;
                t.Mark(p, q, c == '!' ? kCodeKeyword : kCodeVariable);
                p = q;
                continue;
            }
            size_t q = p + 1;
            bool key = false;
            size_t last;
            if (c == '"' || c == '\'') {
                while (q < end) {
                    if (s[q] == '\\' && c == '"') {
                        q += 2;
                    } else if (s[q] == c) {
                        if (c == '\'' && q + 1 < end && s[q + 1] == '\'') {
                            q += 2;
                            continue;
                        }
                        q++;
                        break;
                    } else {
                        q++;
                    }
                }
                if (q > end) q = end;
                last = q;
                while (q < end && IsSpace(s[q])) q++;
                key = q < end && s[q] == ':' && (q + 1 == end || IsSpace(s[q + 1]) || (flow > 0 && IsFlowIndicator(s[q + 1])));
                t.Mark(p, last, key ? kCodeTag : kCodeString);
            } else {
                // A plain scalar, or a key before ": ".
                for (q = p; q < end; q++) {
                    const char d = s[q];
                    if (d == ':' && (q + 1 == end || IsSpace(s[q + 1]) || (flow > 0 && IsFlowIndicator(s[q + 1])))) {
                        key = true;
                        break;
                    }
                    if ((d == '#' && q > p && IsSpace(s[q - 1])) || (flow > 0 && IsFlowIndicator(d))) break;
                }
                last = q;
                while (last > p && IsSpace(s[last - 1])) last--;
                t.Mark(p, last, key ? (uint8_t)kCodeTag : YamlScalar(s + p, last - p));
            }
            p = key ? q + 1 : (c == '"' || c == '\'' ? last : q);
        }
        i = end + 1;
    }
}

// TOML: [tables] in entity colour, keys in constant colour, strings, numbers and dates,
// true, false, inf and nan.
void ClassifyToml(const Text& t, size_t n) {
    const char* s = t.s;
    bool lineStart = true;
    for (size_t i = 0; i < n;) {
        const char c = s[i];
        if (c == '\n' || IsSpace(c)) {
            lineStart = lineStart || c == '\n';
            i++;
            continue;
        }
        const bool first = lineStart;
        lineStart = false;
        size_t j = i + 1;
        uint8_t codeClass = kCodePlain;
        if (c == '#') {
            j = t.LineEnd(i, n);
            t.Mark(i, j, kCodeComment);
            i = j;
            continue;
        }
        if (c == '[' && first) {   // [table] and [[array.of.tables]]
            const size_t end = t.LineEnd(i, n);
            j = t.Through(i, end, "]", 1);
            if (j < end && s[j] == ']') j++;
            t.Mark(i, j, kCodeEntity);
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            if (n - i >= 3 && s[i + 1] == c && s[i + 2] == c) {
                const char close[3] = { c, c, c };
                j = t.Through(i + 3, n, close, 3);
                while (j < n && s[j] == c) j++;
            } else {
                while (j < n && s[j] != c && s[j] != '\n') j += c == '"' && s[j] == '\\' ? 2 : 1;
                j = j < n && s[j] == c ? j + 1 : (j < n ? j : n);
            }
            codeClass = kCodeString;
        } else if (IsDigit(c) || ((c == '+' || c == '-') && j < n && IsDigit(s[j]))) {
            while (j < n && (IsAsciiWord(s[j]) || IsOneOf(s[j], ".:+-"))) j++;
            codeClass = kCodeNumber;
        } else if (IsWord(c) || c == '-') {
            while (j < n && (IsWord(s[j]) || s[j] == '-' || s[j] == '.')) j++;
            const size_t length = j - i;
            if ((length == 4 && memcmp(s + i, "true", 4) == 0) || (length == 5 && memcmp(s + i, "false", 5) == 0) ||
                (length == 3 && (memcmp(s + i, "inf", 3) == 0 || memcmp(s + i, "nan", 3) == 0))) {
                codeClass = kCodeConstant;
            }
        } else {
            i++;
            continue;
        }
        size_t k = j;
        while (k < n && IsSpace(s[k])) k++;
        if (k < n && s[k] == '=') codeClass = kCodeConstant;   // a key
        t.Mark(i, j, codeClass);
        i = j;
    }
}

// INI files, properties and .env: [sections] in entity colour, keys in constant colour, quoted
// values in string colour, comments.
void ClassifyIni(const Text& t, size_t n) {
    const char* s = t.s;
    for (size_t i = 0; i < n;) {
        const size_t end = t.LineEnd(i, n);
        size_t p = i;
        while (p < end && IsSpace(s[p])) p++;
        if (p < end) {
            if (IsOneOf(s[p], ";#!")) {
                t.Mark(p, end, kCodeComment);
            } else if (s[p] == '[') {
                t.Mark(p, t.Through(p, end, "]", 1), kCodeEntity);
            } else {
                size_t separator = p;
                while (separator < end && s[separator] != '=' && s[separator] != ':') separator++;
                if (separator < end) {
                    size_t last = separator;
                    while (last > p && IsSpace(s[last - 1])) last--;
                    t.Mark(p, last, kCodeConstant);
                    size_t value = separator + 1;
                    while (value < end && IsSpace(s[value])) value++;
                    if (value < end && (s[value] == '"' || s[value] == '\'')) {
                        const char close[1] = { s[value] };
                        t.Mark(value, t.Through(value + 1, end, close, 1), kCodeString);
                    }
                }
            }
        }
        i = end + 1;
    }
}

// Diffs, a line at a time by its first byte: added and removed lines, hunk ranges, and the lines
// that tell the files and their versions.
void ClassifyDiff(const Text& t, size_t n) {
    const char* s = t.s;
    for (size_t i = 0; i < n;) {
        const size_t end = t.LineEnd(i, n);
        uint8_t codeClass = kCodePlain;
        if (end > i) {
            const char c = s[i];
            if (c == '+') codeClass = kCodeInserted;
            else if (c == '-') codeClass = kCodeDeleted;
            else if (c == '@' && end - i >= 2 && s[i + 1] == '@') codeClass = kCodeRange;
            else if (c == '\\' || t.At(i, end, "diff ", 5) || t.At(i, end, "index ", 6)) codeClass = kCodeComment;
        }
        t.Mark(i, end, codeClass);
        i = end + 1;
    }
}

// The colours of kCodeColorOfClass's indices: GitHub's light theme (Primer's prettylights).
constexpr const char* kColors[] = {
    "0.12 0.12 0.12",   // plain: the colour of code without highlighting
    ".349 .388 .431",   // comment
    ".039 .188 .412",   // string
    ".02 .314 .682",    // number, constant, tag
    ".812 .133 .18",    // keyword
    ".4 .224 .729",     // entity
    ".584 .22 0",       // variable
    ".067 .388 .161",   // inserted
    ".51 .027 .118",    // deleted
    ".51 .314 .875",    // range
};

// The info strings that name a language, in an open-addressed table made at compile time: a slot
// tells the language, and where in its aliases the name is.
struct AliasSlot {
    uint8_t language = 0;
    uint8_t length = 0;   // 0: an empty slot
    uint16_t offset = 0;
};

constexpr size_t kAliasSlots = 256;   // more than twice the names

constexpr uint32_t AliasHash(const char* name, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t k = 0; k < length; k++) hash = (hash ^ (uint8_t)Lower(name[k])) * 16777619u;
    return hash;
}

struct AliasTable {
    AliasSlot slots[kAliasSlots];
};

constexpr AliasTable MakeAliasTable() {
    AliasTable table{};
    for (uint8_t language = 1; language < kLanguageCount; language++) {
        const char* aliases = kSpecs[language].aliases;
        for (size_t at = 0; aliases[at] != '\0';) {
            size_t end = at;
            while (aliases[end] != '\0' && aliases[end] != ' ') end++;
            size_t slot = AliasHash(aliases + at, end - at) % kAliasSlots;
            while (table.slots[slot].length != 0) slot = (slot + 1) % kAliasSlots;
            table.slots[slot] = AliasSlot{ language, (uint8_t)(end - at), (uint16_t)at };
            at = aliases[end] == ' ' ? end + 1 : end;
        }
    }
    return table;
}

constexpr AliasTable kAliases = MakeAliasTable();

} // namespace

uint8_t CodeLanguage(std::string_view info) {
    size_t at = 0;
    while (at < info.size() && IsOneOf(info[at], " \t{.")) at++;   // Pandoc's {.python}
    size_t end = at;
    while (end < info.size() && !IsOneOf(info[end], " \t\r,{}:")) end++;   // rust,ignore and python title="..."
    const size_t length = end - at;
    if (length == 0 || length > 24) return 0;
    const char* name = info.data() + at;
    for (size_t slot = AliasHash(name, length) % kAliasSlots; kAliases.slots[slot].length != 0; slot = (slot + 1) % kAliasSlots) {
        const AliasSlot& alias = kAliases.slots[slot];
        if (alias.length == length && SameCaseless(name, kSpecs[alias.language].aliases + alias.offset, length)) {
            return alias.language;
        }
    }
    return 0;
}

void ClassifyCode(uint8_t language, std::string_view code, uint8_t* classes) {
    if (code.empty()) return;
    memset(classes, kCodePlain, code.size());
    if (language == 0 || language >= kLanguageCount) return;
    const Text text{ code.data(), classes };
    const size_t n = code.size();
    switch (kSpecs[language].mode) {
    case kCodeMode: CodeLexer(language, text).Run(0, n); break;
    case kConsoleMode: ClassifyConsole(text, n, language == kPycon); break;
    case kMarkupMode: MarkupLexer(text, language == kHtml).Run(0, n); break;
    case kCssMode: CssLexer(text, language == kScss).Run(0, n); break;
    case kJsonMode: ClassifyJson(text, n); break;
    case kYamlMode: ClassifyYaml(text, n); break;
    case kTomlMode: ClassifyToml(text, n); break;
    case kIniMode: ClassifyIni(text, n); break;
    case kDiffMode: ClassifyDiff(text, n); break;
    default: break;
    }
}

const char* CodeColor(uint8_t colorIndex) {
    return colorIndex < sizeof(kColors) / sizeof(kColors[0]) ? kColors[colorIndex] : kColors[0];
}

const char* CodeLineTile(uint8_t codeClass) {
    if (codeClass == kCodeInserted) return ".855 .984 .882";
    if (codeClass == kCodeDeleted) return "1 .922 .914";
    return nullptr;
}

} // namespace TinyPdf::Internal
