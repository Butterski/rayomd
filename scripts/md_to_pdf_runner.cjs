#!/usr/bin/env node

const fs = require("node:fs");
const path = require("node:path");

async function main() {
  const [moduleRoot, input, output] = process.argv.slice(2);
  if (!moduleRoot || !input || !output) {
    throw new Error("usage: md_to_pdf_runner.cjs <module-root> <input.md> <output.pdf>");
  }

  const { mdToPdf } = require(path.resolve(moduleRoot, "md-to-pdf"));
  const result = await mdToPdf(
    { path: path.resolve(input) },
    {
      dest: path.resolve(output),
      pdf_options: { format: "A4", margin: "20mm", printBackground: true },
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
