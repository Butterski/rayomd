Co realnie możesz zrobić
Opcja A: Streaming parsera (ma sens, łatwe)
Parser i renderer możesz spiąć w jeden pass:
cpp// Zamiast: blocks = ParseMarkdown(text) → Render(blocks)
// Zrób:
void ParseAndRender(const char* data, size_t size, Renderer& r) {
    LineScanner scanner(data, size);
    while (scanner.HasMore()) {
        Block block = scanner.NextBlock(); // czyta minimum potrzebne
        r.Render(block);                  // od razu renderuje
    }
}
LineScanner czyta linie leniwie, bez SplitLines() które kopiuje wszystko. To eliminuje jeden pełny bufor — zamiast vector<string> lines + vector<Block> blocks, masz tylko bieżący block w pamięci.
Dla 100 KB pliku różnica jest mała. Dla 10 MB pliku — znacząca.
Opcja B: Incremental file read + parse (sens tylko dla gigantycznych plików)
cppHANDLE hFile = ...;
char chunk[65536];
DWORD read;
while (ReadFile(hFile, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
    scanner.Feed(chunk, read);
    while (scanner.HasCompleteBlock()) {
        renderer.Render(scanner.PopBlock());
    }
}
Problem: block może przekraczać granicę chunka (code fence zaczyna się w jednym, kończy w następnym). Potrzebujesz pending bufora na niekompletny block — dokładnie jak masz w RunServeExport dla linii stdin. Dla MD→PDF to overengineering jeśli pliki są <10 MB.


---

Dobra robota z researchu. Znalazłem kilka nieoczywistych rzeczy. Oto pełna lista z naukowymi uzasadnieniami:

---

# Fast Markdown — Research-Backed Optimization Roadmap

## NIEOCZYWISTE — z literatury naukowej

### N1. `simdutf` zamiast `MultiByteToWideChar` — 3–20x szybsze

Twój hot path: `Utf8ToWide()` jest wołane przy **każdym bloku tekstu** w rendererze. Używa `MultiByteToWideChar` — scalar, brak SIMD.

Biblioteka `simdutf` transkoduje miliard znaków na sekundę lub więcej. Podejście jest 3–10x szybsze niż popularna biblioteka ICU na trudnych stringach (non-ASCII), i 20x szybsze dla ASCII.

Zasila Node.js, Bun, WebKit, Chromium, Cloudflare workerd. Zero alokacji, zero wyjątków.

Konkretnie w twoim kodzie:
```cpp
// Zamiast:
std::wstring Utf8ToWide(const std::string& str) {
    int len = MultiByteToWideChar(CP_UTF8, 0, ...);
    // alokacja + scalar conversion
}

// Z simdutf (header-only friendly):
size_t len = simdutf::utf16_length_from_utf8(str.data(), str.size());
std::wstring out(len, 0);
simdutf::convert_utf8_to_utf16le(str.data(), str.size(),
    reinterpret_cast<char16_t*>(out.data()));
```

Biblioteka ma ~200 KB skompilowana, MIT license, MinGW wspiera. Na Windowsie `wchar_t` = UTF-16LE — perfect match.

---

### N2. Arena allocator — do 44% szybszy cały program

Badania identyfikują region allocatory (arena/pool/bump-pointer) jako jedyną technikę custom alokacji dającą **istotną poprawę czasu wykonania — do 44%** względem state-of-the-art general-purpose allocatora. Region allocatory operują przez alokowanie obiektów kontygualnie bez indywidualnego zwalniania, zwracając całą pamięć naraz gdy region jest niszczony.

Podczas gdy tylko 2% kodu jest spędzone na alokacji, **całkowity czas wykonania może się różnić o 72%** przy użyciu różnych alokatorów.

Zastosowanie w twoim kodzie — `std::pmr` jest w C++17, MinGW wspiera:
```cpp
// ParseMarkdown() — jeden arena per dokument
std::pmr::monotonic_buffer_resource arena(128 * 1024); // 128KB upfront
std::pmr::vector<Block> blocks(&arena);
std::pmr::string text(&arena);
// cały free: destruktor areny = jeden munmap
```

---

### N3. SIMD scan dla `IsAsciiDocument` i `SplitLines`

Algorytm SIMD-based do walidacji UTF-8 **konsekwentnie przekracza 10 GiB/s** na procesorach x64. Klasyczne podejście bajt-po-bajcie jest dramatycznie wolniejsze ze względu na branch i cache miss penalties.

Twoja `IsAsciiDocument()` skanuje bajt po bajcie przez cały plik przed każdą konwersją. SIMD SSE2 robi to 16 bajtów naraz:

```cpp
static bool IsAsciiDocumentSIMD(const char* data, size_t size) {
    __m128i mask = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 16 <= size; i += 16) {
        __m128i chunk = _mm_loadu_si128((__m128i*)(data + i));
        // bit 7 set = non-ASCII
        mask = _mm_or_si128(mask, chunk);
    }
    // sprawdź czy jakiś bajt miał bit 7
    if (_mm_movemask_epi8(mask)) return false;
    // tail
    for (; i < size; i++)
        if ((unsigned char)data[i] >= 128) return false;
    return true;
}
```
SSE2 jest dostępne na **każdym** x64 od 2003 — zero runtime detection potrzebne.

---

### N4. Knuth-Plass line breaking zamiast greedy — jakość layoutu

Twój `WrapText` i `WrapAsciiText` używają greedy first-fit — każda linia decydowana osobno.

Algorytm Knuth-Plass używa dynamic programming do minimalizacji globalnej funkcji kosztu przez cały paragraf. Greedy (first-fit) może produkować bardzo nierówne linie bo wybór dla jednej linii wpływa na wszystkie następne. Naiwna implementacja jest O(n²), ale **z optymalizacją prefiksu redukuje się do O(n·w)** gdzie w to maksymalna liczba słów na linii.

To nie przyśpieszy konwersji, ale **dramatycznie poprawi jakość PDF** — mniej rzucających się w oczy dziur między słowami. Przy rynkowym pozycjonowaniu to UX który zauważają użytkownicy.

---

### N5. StringZilla dla wyszukiwania wzorców w parserze

Twój parser robi wiele `s.find()`, `StartsWith()`, `ReplaceAll()` — wszystko scalar STL.

StringZilla osiąga do 100x szybsze operacje na stringach dla C/C++ używając NEON, AVX2, AVX-512. Biblioteka przyspiesza wyszukiwanie, hashing, sortowanie i operacje na pamięci.

Dla twojego parsera: `find('\n')`, `find("](")`, `find("```")` w `StripInlineMarkdown` i `ParseInlineStyled` — każda z tych operacji na długim tekście zyska na SIMD.

---

## ZNANE — potwierdzone przez research

### K1. `std::string_view` przez cały parser

Zamiast `LTrim`/`RTrim`/`Trim` zwracających `std::string` — `string_view`. Eliminuje dziesiątki alokacji per dokument. Bezpośrednie przełożenie z arena allocator research.

### K2. Font subsetting

Segoe UI ma ~5000+ glifów, typowy polski dokument używa ~250–400. Zbuduj subset z tablic `glyf` + `loca` + `hmtx` + `cmap` TTF. Efekt: 982 KB → 60–120 KB PDF, czas embeddingu: dramatyczny spadek.

### K3. Pre-reserve bufory PDF

`pages.back().reserve(64 * 1024)` w `NewPage()`, `pdf_buf.reserve(512 * 1024)` w `Build()`. Eliminuje reallokacje przy rosnących stringach content streamów.

### K4. `AppendF` bez alokacji

`F()` zwraca `std::string` (alokacja) przy każdej liczbie. Setki wywołań per dokument. Zamień na `void AppendF(std::string& out, double v)` z `snprintf` do lokalnego bufora + `out.append()`.

### K5. `mmap` dla file read w `--serve`

`MapViewOfFile` zamiast `ReadFile` loop. OS ładuje strony lazily, zero-copy dla sekwencyjnego odczytu. Kernel prefetcher działa idealnie dla plików MD.

### K6. Output buffer reuse między requestami

W `--serve` mode trzymaj `static std::string s_pdfBuf` — `clear()` zachowuje capacity, zero reallokacji od drugiego dokumentu.


## Priorytet implementacji

1. **`simdutf`** — drop-in za `MultiByteToWideChar`, największy zysk per linia kodu
2. **Font subsetting** — dominujący bottleneck Unicode path, uderza w czas + rozmiar
3. **Arena allocator** (`std::pmr`) — potwierdzone 44% w literaturze, zero nowych zależności
4. **SIMD `IsAsciiDocument`** — 10 linii kodu, natychmiastowy efekt
5. **`AppendF` + reserve** — mechaniczne, bezpieczne, duża liczba wywołań
6. **Knuth-Plass** — nie przyspiesza, ale podnosi jakość PDF do poziom profesjonalny
7. **StringZilla** — tylko jeśli profilujesz i widzisz czas w `find()`

---

### 1. Emoji fallback kosztuje dużo

W `tester.md` masz `✅ ⚠️ ❌` — twój `NormalizeSymbols` zamienia je na `[OK]`, `[!]`, `[X]`. Ale to się dzieje **po** `Utf8ToWide`, czyli te multibyte sekwencje (3–4 bajty każda) idą przez pełny Unicode pipeline zanim zostaną zastąpione ASCII.

Odwróć kolejność — normalize **przed** decyzją ASCII vs Unicode path:

```cpp
static bool IsAsciiDocument(const std::string& s) {
    // najpierw normalize emoji → ASCII substitutes
    // potem sprawdź czy pozostały znaki >127
}
```

Dla `tester.md` po normalizacji emoji — zostają tylko polskie znaki, ale to wystarczy żeby trafić do Unicode path. Jednak HexText encoding dla `[OK]` zamiast `✅` to dramatycznie mniejszy codepoint set.

### 2. `usedCids` jako `std::set` — logarytmiczne inserty

```cpp
std::set<uint16_t> usedCids; // O(log n) insert przy każdym znaku
```

Dla dokumentu z 300+ unikalnymi codepoints to setki `log(n)` insertów w `HexText`. Zamień na:

```cpp
// flat bool array — O(1) insert, O(65536) scan raz na końcu
bool usedCids[65536] = {};
// ...przy renderowaniu:
usedCids[cid] = true;
// ...na końcu zbierz do setu tylko raz:
std::set<uint16_t> finalUsed;
for (int i = 0; i < 65536; i++)
    if (usedCids[i]) finalUsed.insert(i);
```

128 KB na stosie (lub jako pole `Renderer`) — zero alokacji heap, O(1) per znak.

### 3. `TextWidth` w pętli per-znak

```cpp
static double TextWidth(const TtfFont& font, const std::wstring& text, double size) {
    double w = 0.0;
    for (uint32_t cp : Codepoints(text)) { // alokuje vector!
        uint16_t cid = ...;
        w += font.WidthForCid(cid) * size / 1000.0;
    }
    return w;
}
```

`Codepoints()` alokuje `std::vector<uint32_t>` przy **każdym** wywołaniu. `WrapText` woła `TextWidth` dla każdego słowa × każdej próby. Dla paragrafu z 20 słowami to 40+ alokacji.

```cpp
// Inline bez alokacji:
static double TextWidth(const TtfFont& font, 
                         const wchar_t* data, size_t len, double size) {
    double w = 0.0;
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = data[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i+1 < len) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (data[++i] - 0xDC00);
        }
        uint16_t cid = (cp <= 0xffff && font.glyphForBmp[cp]) 
                        ? (uint16_t)cp : (uint16_t)'?';
        w += font.widthForBmp[cid] * size / 1000.0;
    }
    return w;
}
```

### 4. `WrapText` — `NormalizeSpaces` kopiuje cały string

```cpp
static std::vector<std::wstring> WrapText(const TtfFont& font, 
                                           const std::wstring& raw, ...) {
    std::wstring text = NormalizeSpaces(raw); // kopia całego stringa!
```

Dla każdego bloku tekstu — pełna kopia. Zamień na in-place scan bez kopii, traktując wielokrotne spacje jako single space podczas iteracji.

### 5. Pre-computed multiplication dla TextWidth

```cpp
w += font.WidthForCid(cid) * size / 1000.0;
```

`size / 1000.0` jest stałe dla całego wywołania. Wyciągnij:

```cpp
double scale = size / 1000.0;
// w pętli:
w += font.widthForBmp[cid] * scale;
```

Kompilator czasem to robi sam z `-O2`, ale jawne jest bezpieczniejsze.

---

## Szacunek dla tester.md po tych zmianach

| Platforma | Teraz | Po zmianach |
|---|---|---|
| Windows Unicode | 0.50 ms | **0.18–0.25 ms** |
| Linux Unicode | 0.22 ms | **0.08–0.12 ms** |
| Linux ASCII | 0.02 ms | **0.008–0.012 ms** |

Sub-0.1ms na Linuxie dla tego pliku jest realne. Sub-0.2ms na Windowsie też — głównie przez `MultiByteToWideChar` overhead który `simdutf` by wyeliminował.

Masz ETW lub `perf` setup żeby zmierzyć gdzie dokładnie idzie czas?