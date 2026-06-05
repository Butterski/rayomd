# Raport Testowy: Konwerter Markdown

## 1. Test Polskich Znaków i Typografii
Zażółć gęślą jaźń. To jest test kodowania UTF-8: ą, ć, ę, ł, ń, ó, ś, ź, ż.
Sprawdzamy style: **pogrubienie**, *kursywa*, ***bold italic*** oraz ~~przekreślenie~~.

## 2. Sekcja Techniczna (Code Blocks)
Poniżej test kolorowania składni i czcionki monospaced (kluczowe dla stylu "Tech").

```
package main

import "fmt"

func main() {
    // Sprawdzamy czy wcięcia są zachowane
    fmt.Println("System Ready")
}
```

## 3. Matematyka i LaTeX (Style Elegant)
To jest test renderowania wzorów, ważny do Twojej inżynierki.
Równanie w tekście: $E = mc^2$.

Równanie w bloku (całka Gaussa):
$$
\int_{-\infty}^{\infty} e^{-x^2} dx = \sqrt{\pi}
$$

## 4. Tabela i Listy (Style Modern)

| Usługa | Status | Latency |
| :--- | :---: | ---: |
| API Gateway | ✅ Online | 45ms |
| Database | ⚠️ Warning | 120ms |
| Auth Service | ❌ Offline | Timeout |

* Punkt pierwszy
* Punkt drugi
    * Zagnieżdżenie A
    * Zagnieżdżenie B
1. Lista numerowana
2. Kolejny element

> "To jest cytat (blockquote). Powinien mieć wcięcie i inny styl, np. szary pasek z boku w stylu Modern."
