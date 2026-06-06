Przejrzałem cały kod. Oto pełna lista:

---

# Fast Markdown — Optimization Roadmap

## 1. Parser (`ParseMarkdown`)

### 1.1 Eliminacja `std::string` w hot path
`LTrim`, `RTrim`, `Trim`, `StartsWith` — każde wywołanie alokuje nowy `std::string`. Zamień na `std::string_view` przez cały pipeline parsera. `SplitLines` zwraca `vector<string>` — zamień na indeksy do oryginalnego bufora.

### 1.2 `IsBlockStart` wywołuje pełny parse przy każdej linii
```cpp
// Teraz: dla każdej linii paragrafu
while (i < lines.size() && !IsBlockStart(lines[i])) { ... }
// IsBlockStart() wewnętrznie woła ParseHeading, ParseBullet, ParseNumbered
```
`IsBlockStart` robi redundantny parse, który zaraz potem jest powtarzany w głównej pętli. Zamiast tego: jeden pass — parse line → wynik, zapisz i reuse.

### 1.3 SIMD lexer dla `SplitLines`
Skanowanie bajt po bajcie po `\n` jest wolne. SSE2 pozwala skanować 16 bajtów naraz:
```cpp
__m128i nl = _mm_set1_epi8('\n');
__m128i chunk = _mm_loadu_si128((__m128i*)ptr);
int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, nl));
// mask: bitmapa newline'ów w tym chunku
```
`IsAsciiDocument` też skanuje bajt po bajcie — to samo SIMD zastosowanie.

### 1.4 Arena allocator dla `Block`
Każdy `Block` z `text` + `rows` alokuje osobno. Jeden arena reset zamiast `vector<Block>` destruktorów:
```cpp
std::pmr::monotonic_buffer_resource arena(256 * 1024);
std::pmr::vector<Block> blocks(&arena);
```
C++17, zero dodatkowych zależności, MinGW wspiera.

---

## 2. Font pipeline (największy bottleneck dla Unicode)

### 2.1 `CachedFontFileObject` — cache niepełny
```cpp
static const std::string& CachedFontFileObject(const TtfFont& font) {
    static std::string body;
    if (body.empty()) { ... }
    return body;
}
```
Cache działa tylko dla fontu, ale `MakeCidToGidMap` i `MakeToUnicodeCMap` są generowane od nowa przy każdym dokumencie. Cache'uj je osobno, kluczem `set<uint16_t> usedCids`.

### 2.2 Font subsetting — największy ROI
982 KB PDF to głównie cały font embedowany bez subsettingu. Dla typowego polskiego dokumentu używanych jest ~200–400 glifów z 5000+.

Implementacja bez zewnętrznych zależności — z TTF potrzebujesz:
- `glyf` table: wytnij tylko użyte glyph recordy
- `loca` table: przelicz offsety
- `hmtx`: tylko użyte metryki
- `cmap`: zostaw format 4, ale zawęź segmenty

Efekt: PDF 982 KB → ~60–150 KB, czas `CachedFontFileObject` build: z ~50ms → ~2ms.

### 2.3 `widthForBmp` — 128 KB zbędnej tablicy per font instance
`widthForBmp.assign(65536, 500)` alokuje 128 KB przy każdym `Parse()`. Zamień na lazy lookup:
```cpp
uint16_t WidthForCid(uint16_t cid) const {
    uint16_t g = GlyphFor(cid);
    return g < advances.size() ? advances[g] : 500;
}
```
Lookup O(1), zero 128 KB alokacji, cache miss pattern lepszy dla małych dokumentów.

### 2.4 `glyphForBmp` jako `unordered_map` dla BMP gaps
Aktualnie `vector<uint16_t>(65536)` = 128 KB zawsze. Dla typowego dokumentu zajęte jest <1000 codepoints. Zamień na `std::unordered_map<uint16_t, uint16_t>` lub flat sorted array z binary search — mniejszy footprint, lepszy cache behavior.

---

## 3. PDF generation

### 3.1 `std::ostringstream` w hot path
`MakeToUnicodeCMap`, `MakeWidths`, `MakeCidToGidMap` — wszystkie używają `ostringstream` który robi ukryte alokacje. Zamień na `std::string` z `reserve()` i `append()`:
```cpp
std::string out;
out.reserve(used.size() * 16);
// zamiast ss << ...
```

### 3.2 `F()` — `snprintf` przy każdej liczbie
```cpp
static std::string F(double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f", v);
    // ...zwraca std::string — alokacja!
}
```
`F()` jest wołane setki razy przy renderowaniu. Zamień na wersję zapisującą do przekazanego bufora:
```cpp
void AppendF(std::string& out, double v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%.2f", v);
    // trim trailing zeros inline
    out.append(buf, n);
}
```

### 3.3 Jeden `WriteFile` call
`WriteBinaryFile` pisze chunkami po 1 MB, ale dla PDF <5 MB to jeden call. Usuń pętlę dla małych plików:
```cpp
if (content.size() <= 4 * 1024 * 1024) {
    DWORD written;
    WriteFile(hFile, content.data(), (DWORD)content.size(), &written, nullptr);
} else { /* chunked loop */ }
```

### 3.4 PDF content streams — brak kompresji = słuszna decyzja
Nie dodawaj zlib — słusznie go nie ma. Dla dokumentów <5 MB latencja kompresji > zysk z rozmiaru pliku. Zostaw jak jest.

### 3.5 `PdfObjects::Build` — wielokrotne `+=` na dużym stringu
```cpp
pdf += std::to_string(i + 1);
pdf += " 0 obj\n";
// ...
```
`pdf` rośnie do ~1 MB przez N konkatenacji. `reserve()` na początku Build():
```cpp
pdf.reserve(pdfBytes.size() + 64 * 1024); // empiryczne
```

---

## 4. Rendering (`Renderer` / `StandardRenderer`)

### 4.1 `WrapText` alokuje `wstring` per word
Każdy `WrapText` call buduje `std::wstring text = NormalizeSpaces(raw)` plus vector wynikowych linii. Dla długiego paragrafu to dziesiątki alokacji. Zamień na wersję operującą na `wstring_view` z pre-allocated output buffer.

### 4.2 `Utf8ToWide` w hot path renderera
`PushSpan` → `ParseInlineStyled` → `PushSpan` woła `Utf8ToWide` dla każdego spana. Zamień na lazy: trzymaj tekst jako UTF-8 do momentu HexText encoding, konwertuj raz per block.

### 4.3 `HexText` — append char po char
```cpp
static void AppendHex4(std::string& out, uint16_t value) {
    out.push_back(h[(value >> 12) & 0xf]);
    // ...4x push_back
}
```
Zamień na bezpośrednie `memcpy` z pre-computed tabeli 65536 × 4 bajtów:
```cpp
static char hexTable[65536][4]; // init once
// AppendHex4: memcpy(out.data() + out.size(), hexTable[value], 4)
// + manual size bump
```

### 4.4 `pages.back()` — string budowany przez `+=`
Każdy `DrawRect`, `PaintText`, etc. robi `c += "q "`. Ten string rośnie do setek KB przez setki `+=`. Pre-reserve per page:
```cpp
void NewPage() {
    pages.push_back("");
    pages.back().reserve(64 * 1024);
    y = PAGE_H - margin;
}
```

---

## 5. `--serve` mode

### 5.1 Font cache między requestami — już działa
`GetCachedFont()` ze static — dobrze. Rozszerzyć o cache `CidToGidMap` i `ToUnicode` per unikalny `usedCids` set.

### 5.2 `ReadUtf8File` — chunked read zamiast mmap
Aktualnie: `ReadFile` w pętli. Dla `--serve` gdzie pliki MD mają <100 KB, `MapViewOfFile` jest szybszy:
```cpp
HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
const char* data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
content.assign(data, size);
UnmapViewOfFile(data);
```

### 5.3 Output buffer reuse między requestami
Aktualnie `pdfBytes` jest lokalnym `std::string` w każdym `BuildPdfBytes` call. W `--serve` możesz trzymać jeden globalny bufor z `clear()` + `reserve()` zachowanym:
```cpp
static std::string s_pdfBuf;
s_pdfBuf.clear(); // capacity nie resetuje się!
BuildPdfBytes(..., s_pdfBuf);
```

---

## 6. GUI (pomniejsze)

### 6.1 Word/line count przy każdym frame
```cpp
int lines = 1, words = 0, chars = (int)g_markdownText.size();
for (char c : g_markdownText) { ... }
```
To O(N) przy każdym render frame (60 FPS). Inkrementuj counter przy każdej zmianie tekstu, nie przy renderze. ImGui ma `ImGuiInputTextCallbackData` do detekcji zmian.

### 6.2 VSync blokuje CPU
`g_pSwapChain->Present(1, 0)` — vsync on. Przy braku aktywności CPU spin-loops na message pump. Dodaj `WaitMessage()` gdy okno nie jest focusowane.

---

## Szacowane wyniki po optymalizacjach

| Ścieżka | Teraz | Po optimalizacjach | Główna zmiana |
|---|---|---|---|
| ASCII bench | 0.17 ms | 0.08–0.11 ms | `string_view` parser, `AppendF`, reserve |
| Unicode bench | 2.02 ms | 0.35–0.65 ms | Font subsetting + CidMap cache |
| Serve per-file avg | 1.076 ms | 0.25–0.55 ms | Mmap read + output buf reuse + subsetting |
| 20-plik serve total | 67.55 ms | 18–35 ms | Wszystkie powyższe |
| PDF rozmiar Unicode | 982 KB | 55–140 KB | Font subsetting |
| PDF rozmiar ASCII | 4.3 KB | 3.8–4.1 KB | Minor |

Szacunki konserwatywne. Font subsetting to ~60–70% całego zysku dla Unicode path.

---

## Priorytetyzacja (bang for buck)

1. **Font subsetting** — największy efekt, uderza w czas + rozmiar + UX
2. **`string_view` w parserze** — eliminuje ~30–50 alokacji per dokument
3. **Output buffer reuse** w `--serve` — zero-cost w implementacji, duży efekt przy batch
4. **`AppendF` bez alokacji** — łatwe, setki wywołań per dokument
5. **`mmap` dla file read** — 10–20 min roboty, mierzalny efekt
6. **`glyphForBmp` jako mapa** — redukcja footprintu, lepszy cache behavior
7. **Word count poza render loop** — UX, nie performance PDF


---

## Status po audycie kodu (2026-06-06)

Legenda: ✅ zrobione | 🔶 częściowo | ❌ nie zrobione

### 1.1 Eliminacja `std::string` w hot path — 🔶 CZĘŚCIOWO
- `LTrimView`, `RTrimView`, `TrimView`, `StartsWith(string_view)` — ✅ już są
- `SplitLineViews` zwraca `vector<string_view>` — ✅ zrobione
- `LineInfo` używa `string_view` — ✅ zrobione
- `SplitLines` zwraca `vector<string>` jako compat shim — 🔶 nadal istnieje
- `Block::text` to `std::string` — ❌ każdy blok alokuje

### 1.2 `IsBlockStart` — ✅ ZROBIONE
- `ClassifyLine` pre-klasyfikuje wszystkie linie raz do `vector<LineInfo>`

### 1.3 SIMD lexer dla `SplitLines` — ✅ ZROBIONE

### 1.4 Arena allocator dla `Block` — ❌ NIE ZROBIONE

### 2.1 Cache `MakeCidToGidMap`, `MakeToUnicodeCMap`, `MakeWidths` — ✅ ZROBIONE

### 2.2 Font subsetting — 🔶 CZĘŚCIOWO
- `glyf` + `loca` przycinane — ✅
- `cmap`, `hmtx`, `name` kopiowane w całości — ❌

### 2.3 `widthForBmp` — ✅ ZROBIONE (lazy `WidthForCid`)

### 2.4 `glyphForBmp` jako `unordered_map` — ✅ ZROBIONE

### 3.1 `ostringstream` — 🔶 CZĘŚCIOWO
- `MakeToUnicodeCMap`, `MakeWidths` — ✅ `string` z `reserve()`
- `BuildStandardPdfBytes`, `BuildUnicodePdfBytes` — ❌ wciąż `ostringstream`

### 3.2 `F()` — 🔶 CZĘŚCIOWO
- `AppendF()` istnieje i używane w rendererze — ✅
- `F()` wciąż wołane w PDF assembly — ❌

### 3.3 Jeden `WriteFile` call — ✅ ZROBIONE

### 3.4 Brak zlib — ✅ ŚWIADOMA DECYZJA

### 3.5 `PdfObjects::Build` reserve — 🔶 CZĘŚCIOWO
- `pdf.reserve()` jest — ✅
- `std::to_string(i+1)` w pętli — ❌

### 4.1 `WrapText` — 🔶 CZĘŚCIOWO

### 4.2 `Utf8ToWide` w hot path — ❌ NIE ZROBIONE

### 4.3 `AppendHex4` z pre-computed tabelą — ✅ ZROBIONE

### 4.4 `pages.back().reserve(64*1024)` — ✅ ZROBIONE

### 5.1 Font cache — ✅ ZROBIONE

### 5.2 `MapViewOfFile` — ✅ GUI / ❌ CLI (Linux `ifstream`)

### 5.3 Output buffer reuse — ✅ CLI / ❌ GUI

### 6.1 Word/line count — ❌ NIE ZROBIONE (60 FPS scan)

### 6.2 VSync / `WaitMessage()` — ❌ NIE ZROBIONE

