# Brand Asset Prompt

Use this prompt when regenerating the Fast Markdown mascot or app icon.

```text
Use case: logo-brand
Asset type: open-source project mascot, README hero graphic, and app icon source
Primary request: Create a flat vector-style modern mascot mark for a lightweight Markdown-to-PDF tool named Fast Markdown.
Subject: A friendly minimal cat mascot integrated with a simple document/PDF sheet and subtle speed lines or a speed gauge cue. The cat must be clearly recognizable, charming, and professional, but simple enough to work as an app icon.
Style/medium: clean flat vector illustration, crisp geometric shapes, minimal gradients if any, logo-like, modern developer-tool branding, high contrast, scalable icon feel.
Composition/framing: centered square composition, generous padding, strong silhouette, readable at small sizes. Cat and document should form one cohesive mark.
Color palette: gray cat, white document, teal/cyan accent, small warm amber eye accent. Avoid purple-dominant palettes and avoid photorealistic fur.
Background: perfectly flat solid #00ff00 chroma-key background for transparent background removal. The background must be one uniform color with no shadows, gradients, texture, reflections, floor plane, or lighting variation.
Text: no text, no letters, no watermark.
Constraints: do not use #00ff00 anywhere in the subject; no cast shadow, no contact shadow, no reflection; crisp edges; simple vector-like shapes; professional software product look.
```

Post-process the generated image by removing the chroma-key background, then export:

- `catto.png`: transparent 1024 x 1024 PNG
- `catto.ico`: multi-size Windows icon with 16, 32, 48, 64, 128, and 256 px entries
