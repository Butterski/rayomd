# Fast Markdown — Optimization Roadmap Part 2 (Deep Scan)

Odkryte podczas szczegółowego audytu kodu (linijka po linijce) 2026-06-06.
Rzeczy, których nie było w oryginalnym `todo.md`.

---

## 🔴 A. CRITICAL: Nowo odkryte gorące ścieżki

### A.1 `PaintText` woła `HexText` PODWÓJNIE gdy `fauxBold=true`

**Lokalizacja:** `src/core/tiny_pdf.cpp` ~L1551-1568

```cpp
void PaintText(..., bool fauxBold = false, ...) {
    c += HexText(font, line, usedCids);  // PIERWSZE
    c += " Tj";
    if (fauxBold) {
        // ...przesunięcie x
        c += HexText(font, line, usedCids);  // DRUGIE — ta sama linia!
        c += " Tj";
    }
}
```

`fauxBold` jest włączone dla nagłówków i nagłówków tabeli. Każde wywołanie `HexText`:
- Iteruje wszystkie codepointy
- Woła `usedCids.insert(cid)` (O(log n) per znak)
- Woła `AppendHex4` (zapis do bufora per znak)
- Alokuje i zwraca nowy `std::string`

**Fix:** Oblicz hex string raz:
```cpp
std::string hex = HexText(font, line, usedCids);
c += hex;
c += " Tj";
if (fauxBold) {
    c += " 1 0 ... ";
    c += hex;
    c += " Tj";
}
```

**Szacowany zysk:** ~15-25% szybszy rendering dla dokumentów z nagłówkami.

---

### A.2 `TextWidth` per-znak w fallback ścieżce dzielenia słów

**Lokalizacja:** `src/core/tiny_pdf.cpp` ~L1021-1032 (`PushWrappedWord`) i ~L1063-1072 (`WrapCodeLine`)

Gdy słowo nie mieści się w linii (rzadkie, ale występuje dla długich słów technicznych / URL-i), kod dzieli je znak po znaku:

```cpp
for (wchar_t ch : word) {
    double chWidth = TextWidth(font, std::wstring_view(&ch, 1), size);
    // ...
}
```

Każde wywołanie `TextWidth` przechodzi przez pełną ścieżkę: `ForEachCodepoint` → `CidForCodepoint` → `font.GlyphFor()` (hash lookup w `unordered_map`) → `font.WidthForCid()` (kolejny hash lookup).

**Fix:** Prekomputuj tablicę szerokości znaków ASCII (0-127) przy inicjalizacji fontu:
```cpp
double asciiWidth[128];
for (int i = 0; i < 128; i++) {
    wchar_t ch = (wchar_t)i;
    asciiWidth[i] = TextWidth(font, std::wstring_view(&ch, 1), size);
}
```
Dla znaków spoza ASCII (rzadkie w URL-ach), fallback do `TextWidth`. 99%+ przypadków pokryte przez lookup O(1) w tablicy.

**Alternatywnie (lepiej):** Nie dziel znak po znaku — użyj binary search na prefix słowa:
```cpp
// Znajdź najdłuższy prefix mieszczący się w maxWidth
size_t lo = 1, hi = word.size();
while (lo < hi) {
    size_t mid = (lo + hi + 1) / 2;
    if (TextWidth(font, word.substr(0, mid), size) <= maxWidth)
        lo = mid;
    else
        hi = mid - 1;
}
```
Redukuje N wywołań `TextWidth` do log₂(N) — dla 100-znakowego słowa: ze 100 do ~7.

---

### A.3 `std::to_string` w `MakeWidths` — 2 alokacje per CID

**Lokalizacja:** `src/core/tiny_pdf.cpp` ~L2036-2039

```cpp
for (uint16_t cid : used) {
    out += std::to_string(cid);              // alokacja #1
    out += " [ ";
    out += std::to_string(font.WidthForCid(cid)); // alokacja #2
    out += " ] ";
}
```

Dla 200 unikalnych CIDów to 400 alokacji string. `std::to_string` tworzy tymczasowego `std::string`.

**Fix:** Użyj `AppendInt` na stack buffer:
```cpp
void AppendInt(std::string& out, int v) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", v);
    out.append(buf, (size_t)n);
}
```
Lub jeszcze szybciej: `std::to_chars` (C++17):
```cpp
char buf[16];
auto [ptr, ec] = std::to_chars(buf, buf + 16, cid);
out.append(buf, ptr - buf);
```

**Szacowany zysk:** Eliminuje 200-400 alokacji per dokument Unicode.

---

## 🟠 B. Średnie: Font pipeline

### B.1 `std::set<uint16_t>` dla `usedCids` — wolne inserty

**Lokalizacja:** `src/core/tiny_pdf.cpp` ~L1348

`usedCids.insert(cid)` jest wołane dla KAŻDEGO znaku w dokumencie (przez `HexText` → `ForEachCodepoint`). `std::set` to drzewo RB, każdy insert to O(log n) + alokacja noda. Dla dokumentu z 5000 znaków, przy 200 unikalnych CIDach, to 4800 nadmiarowych tree lookupów (bo duplikaty).

**Fix 1 (prosty):** `std::bitset<65536>` podczas collectu, konwersja do `vector<uint16_t>` posortowanego na końcu:
```cpp
std::bitset<65536> usedBits;
// w HexText:
usedBits.set(cid);
// na końcu:
std::vector<uint16_t> usedCids;
for (uint32_t i = 0; i < 65536; i++)
    if (usedBits[i]) usedCids.push_back((uint16_t)i);
```
Koszt: 8KB bitsetu (mniej niż obecny `std::set` z ~200 nodami = ~8KB i tak). Insert O(1) bez alokacji.

**Fix 2 (hybrida):** Krótki `small_vector<uint16_t, 256>` z `std::sort` + `std::unique` na końcu.

---

### B.2 `MakeCidKey` budowane 4× dla tego samego `usedCids`

**Lokalizacja:** `src/core/tiny_pdf.cpp` ~L1182

`CachedFontFileObject`, `MakeCidToGidMap`, `MakeToUnicodeCMap`, `MakeWidths` — każde niezależnie woła `MakeCidKey(used)`, budując identyczny 400-bajtowy binary string.

**Fix:** Oblicz raz w `BuildUnicodePdfBytes`:
```cpp
std::string cidKey = MakeCidKey(used);
// przekaż jako parametr do każdej funkcji
```
Albo jeszcze lepiej — przekaż referencję do `used` set i użyj `std::set` jako klucza bezpośrednio (hash zestawu) z custom hasherem.

---

### B.3 Font subsetting: przycinanie `cmap`, `hmtx`, `name`

**Lokalizacja:** `src/core/tiny_pdf.cpp` ~L1832 (`BuildSubsetFontBytes`)

Obecnie tylko `glyf` i `loca` są przycinane. Tabele `cmap`, `hmtx`, `name`, `OS/2`, `post` są kopiowane w całości.

**cmap format 4:** ma segmenty — dla każdego segmentu zachowaj tylko te codepointy, których glify są faktycznie użyte. Jeśli segment staje się pusty — usuń go. Przelicz `segCount` i nagłówek.

**hmtx:** Skopiuj tylko metryki dla glifów, które są w `include[]`. To może zmniejszyć tabelę z ~8000 wpisów do ~200.

**name:** Zachowaj tylko nameID 1 (family), 2 (subfamily), 4 (full name). Resztę usuń.

**Szacowany zysk:** Dalsze 100-300KB mniej w PDF. Druga co do wielkości optymalizacja.

---

### B.4 `ParseCmap` ładuje CAŁY BMP zakres

**Lokalizacja:** `src/core/tiny_pdf.cpp` ~L740-790

Dla każdego segmentu format-4 cmap, iteruje PO WSZYSTKICH codepointach w segmencie. Segoe UI ma segmenty pokrywające tysiące kodów. Rezultat: `glyphForBmp` ma tysiące wpisów, z których realnie używane jest <500.

**Fix:** Ładuj lazy — tylko gdy `GlyphFor(cp)` jest wołane po raz pierwszy. Dodaj mały cache miss handler który szuka w cmap table dla konkretnego cp.

**Szacowany zysk:** Szybsze ładowanie fontu (~50ms → ~15ms), mniej pamięci.

---

## 🟡 C. Średnie: Renderer

### C.1 Konwersja UTF-8→UTF-16 rozbita na fragmenty

**Lokalizacja:** `src/core/tiny_pdf.cpp` ~L1390 (`PushSpan`)

`ParseInlineStyled` → `PushSpan` woła `Utf8ToWide(text)` dla każdego fragmentu tekstu (po każdym toggle stylu). Dla paragrafu z 10 przełączeniami bold/italic to 10 małych konwersji zamiast jednej.

**Fix:** Przekonwertuj cały tekst wejściowy na `wstring` raz, potem `wstring_view` substr:
```cpp
std::wstring wide = Utf8ToWide(source);
// parsuj na wide, PushSpan bierze wstring_view
```

---

### C.2 `SplitStyledWords` usuwa spacje, potem `WrapStyled` dodaje je z powrotem

**Lokalizacja:** L1486-1519

Spacje są wycinane w `SplitStyledWords`, potem rekonstruowane jako osobne `StyledSpan{L" ", false, false, false}` w `WrapStyled`. To tworzy dodatkowe spany — jeden per spację. Przy renderingu każdy span to osobne wywołanie `PaintText` + `HexText`.

**Fix:** Zachowaj spacje w słowach:
```cpp
// Zamiast:
if (ch == L' ') { flush word; /* drop space */ }
// Zrób:
if (ch == L' ') { current.push_back(L' '); }
```
Albo jeszcze lepiej: zrezygnuj z podziału na słowa i zrób jedno-przebiegowe mierzenie + wrapping.

---

### C.3 Dzielenie zmiennoprzecinkowe per codepoint w `TextWidth`

**Lokalizacja:** ~L909

```cpp
w += font.WidthForCid(cid) * size / 1000.0;
```

Kompilator NIE MOŻE wynieść dzielenia przed pętlę (bo floating-point nie jest łączne).

**Fix:**
```cpp
double scale = size * 0.001;  // precompute once
for (...) {
    w += font.WidthForCid(cid) * scale;
}
```

---

### C.4 Podwójne rozwiązywanie cp→cid (wrapping + painting)

Każdy znak przechodzi przez `CidForCodepoint` DWA razy:
1. Podczas `TextWidth` (wrapping) — ~L907
2. Podczas `HexText` (painting) — ~L1149

Oba wywołania robią to samo: `cp → font.GlyphFor(cp) → cid`.

**Fix:** Cache'uj per-block: podczas wrappingu zapisz listę (`vector<uint16_t>`) cid dla każdego słowa. Podczas paintowania użyj prekomputowanej listy.

**Alternatywnie (minimalny fix):** Zrób `GlyphFor` inline/hot i dodaj mały LRU cache (8-16 wpisów) na najbardziej używaną ścieżkę.

---

### C.5 `WrapCodeLine` przyjmuje `std::wstring` przez wartość

**Lokalizacja:** ~L1057

```cpp
static std::vector<std::wstring> WrapCodeLine(const TtfFont& font,
    std::wstring line,  // KOPIA! każda linia kodu
    double maxWidth, double size)
```

**Fix:** `std::wstring_view line`

---

## 🟢 D. Niskie

### D.1 `SplitStyledWords` — brak `.reserve()`

**Lokalizacja:** ~L1487

`words.reserve(spans.size() * 3)` — proste, eliminuje realokacje.

### D.2 `SplitTableRow` — `Trim(cell)` alokuje kopię

**Lokalizacja:** ~L286

`Trim()` tworzy nowy `std::string`. Wystarczy `TrimView(cell)` i trzymać `string_view` w `rows`.

### D.3 `PdfObjects::Build` — `std::to_string` w pętli po objects

**Lokalizacja:** ~L1296

```cpp
pdf += std::to_string(i + 1);
```

Dla 50 obiektów PDF to 50 małych alokacji. `AppendInt` jak w A.3.

### D.4 `RenderListItem` — `std::to_string(block.number) + ". "`

**Lokalizacja:** ~L1323

Konkatenacja dwóch stringów per element listy numerowanej.

### D.5 `NormalizeSymbols` kopiuje wejście nawet bez emoji

**Lokalizacja:** ~L298

`StripInlineMarkdown` i `ParseInlineStyled` zawsze wołają `NormalizeSymbols`, która robi 4× `ReplaceAll`. Dla tekstu bez emoji to redundantne.

**Fix:** Szybki check: czy tekst zawiera 3-bajtowe sekwencje UTF-8 (0xE2 prefix)? Jeśli nie — skip `NormalizeSymbols`.

---

## 🔬 E. Research: Techniki z literatury naukowej

### E.1 SIMD UTF-8 validation (Lemire 2018, arXiv:2010.03090)

Daniel Lemire pokazał walidację UTF-8 w **0.7 cykli na bajt** używając SSE2/AVX.
Projekt już używa SSE2 dla `IsAsciiDocument` i `SplitLineViews`.

**Możliwe rozszerzenie:** `IsAsciiDocument` obecnie tylko sprawdza czy dokument
jest ASCII vs Unicode. Można by też wykrywać dokumenty "prawie-ASCII" (≥95% bajtów
<128) i używać ścieżki StandardRenderer z fallbackiem Unicode tylko dla nie-ASCII
fragmentów — tzw. "mixed mode" renderer. Lemire: `_mm_movemask_epi8` daje bitmapę
nie-ASCII bajtów, można szybko policzyć proporcję (POPCNT na masce).

### E.2 cmap Format 4 Binary Search (Apple/OpenType spec)

Format 4 cmap jest zaprojektowany do binary search: segmenty są sortowane po
`endCode[]`, a nagłówek zawiera `searchRange`/`entrySelector`/`rangeShift` do
optymalnego wyszukiwania. Obecny `ParseCmap` iteruje po wszystkich codepointach
w każdym segmencie liniowo.

**Fix dla lazy cmap:** Zaimplementuj binary search per-codepoint zamiast
pre-populacji mapy. Dla 8 segmentów to ~3 porównania zamiast iteracji po
tysiącach kodów.

### E.3 Branchless integer-to-string (np. fmtlib, {fmt})

Biblioteka `{fmt}` (C++20 `std::format`) używa branchless algorytmu
integer-to-string (Jeaiii算法 / Ryū algorithm) który jest 2-10× szybszy niż
`snprintf`. Bez zależności — można zaimplementować uproszczoną wersję dla
`uint16_t` (max 5 cyfr) jako `constexpr` lookup table.

```cpp
// Dla uint16_t: tylko 5 cyfr max
static const char digits[100][2] = { {"00"}, {"01"}, ... {"99"} };
void AppendU16(std::string& out, uint16_t v) {
    if (v >= 10000) { out.append(digits[v / 100], 2); v %= 100; }
    if (v >= 100)    { out.append(digits[v / 100], 2); v %= 100; }
    out.append(digits[v], 2);
    // trim leading zeros
}
```

### E.4 Arena/Pool allocation (C++17 `std::pmr`)

C++17 `std::pmr::monotonic_buffer_resource` jest dostępne w MinGW. Można nim
zastąpić wszystkie alokacje w `ParseMarkdown`:

```cpp
std::pmr::monotonic_buffer_resource arena(256 * 1024);
std::pmr::vector<Block> blocks(&arena);
// blocks, ich text, rows — wszystko w arenie
// JEDEN reset po BuildPdfBytes
```

Zero fragmentacji, zero individual `free()`, lepszy cache locality.

---

## 📊 Zbiorcza tabela prioritetów (Part 2)

| # | Optymalizacja | Impact | Trudność | Szacowany zysk |
|---|---|---|---|---|
| A.1 | `PaintText` double `HexText` na fauxBold | 🔴 CRITICAL | Łatwa | 15-25% renderingu |
| A.2 | Binary search zamiast char-by-char w fallback wrappingu | 🔴 CRITICAL | Średnia | 5-10% dla długich słów |
| A.3 | `std::to_string` → `AppendInt` w `MakeWidths` | 🔴 CRITICAL | Łatwa | Eliminuje ~400 alokacji |
| B.1 | `std::bitset` zamiast `std::set` dla `usedCids` | 🟠 HIGH | Łatwa | Eliminuje O(log n) inserty per znak |
| B.2 | `MakeCidKey` raz, nie 4× | 🟠 HIGH | Łatwa | Eliminuje 3×400 bajtów kopiowania |
| B.3 | Przycinanie `cmap`/`hmtx`/`name` w subsettingu | 🟠 HIGH | Trudna | 100-300KB mniej w PDF |
| B.4 | Lazy `ParseCmap` | 🟡 MEDIUM | Średnia | Szybsze ładowanie fontu |
| C.1 | Jedna konwersja UTF-8→UTF-16 per paragraf | 🟡 MEDIUM | Średnia | Mniej alokacji |
| C.2 | Zachowaj spacje w `SplitStyledWords` | 🟡 MEDIUM | Średnia | Mniej `PaintText` wywołań |
| C.3 | `scale = size * 0.001` w `TextWidth` | 🟡 MEDIUM | Łatwa | Unika dzielenia per znak |
| C.4 | Cache cp→cid między wrappingiem a paintingiem | 🟡 MEDIUM | Trudna | Eliminuje podwójną pracę |
| C.5 | `WrapCodeLine(wstring_view)` | 🟢 LOW | Łatwa | Unika kopii |
| D.1-D.5 | Drobne `.reserve()`, `Trim`→`TrimView`, itp. | 🟢 LOW | Łatwe | Sumarycznie ~5% |
| E.1 | SIMD mixed-mode renderer | 🔮 FUTURE | Trudna | Dla dokumentów 95%+ ASCII |
| E.3 | Branchless `AppendU16` | 🟡 MEDIUM | Średnia | Szybsze formatowanie |
| E.4 | Arena `pmr` dla parsera | 🟠 HIGH | Średnia | Eliminuje >50 alokacji |

---

## 🎯 Rekomendowana kolejność implementacji

1. **A.1** + **A.3** — oba łatwe, razem dają ~25-30% przyspieszenia
2. **B.1** + **C.3** — kolejne łatwe fixy
3. **B.2** — eliminacja redundantnych obliczeń
4. **A.2** — binary search w fallback wrappingu
5. **B.3** — pełny font subsetting (największy zysk ale najtrudniejsze)
6. **E.4** — arena allocator dla parsera
7. **C.1** + **C.2** + **C.5** — czyszczenie renderera
8. **C.4** + **E.1** + **E.3** — zaawansowane optymalizacje