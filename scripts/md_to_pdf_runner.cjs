#!/usr/bin/env node

const fs = require("node:fs");
const path = require("node:path");

async function main() {
  const [moduleRoot, input, output] = process.argv.slice(2);
  if (!moduleRoot || !input || !output) {
    throw new Error("usage: md_to_pdf_runner.cjs <module-root> <input.md> <output.pdf>");
  }

  // md-to-pdf 5 loads ESM-only dependencies: Node 22 or later can require them.
  const { mdToPdf } = require(path.resolve(moduleRoot, "md-to-pdf"));
  // MD_TO_PDF_CHROME_ARGS, e.g. "--no-sandbox" for Chromium inside a container.
  const chromeArgs = (process.env.MD_TO_PDF_CHROME_ARGS || "").split(" ").filter(Boolean);
  const result = await mdToPdf(
    { path: path.resolve(input) },
    {
      dest: path.resolve(output),
      pdf_options: { format: "A4", margin: "20mm", printBackground: true },
      launch_options: { args: chromeArgs },
    },
  );
  if (!result || !fs.existsSync(output)) {
    throw new Error("md-to-pdf did not create the requested output");
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
