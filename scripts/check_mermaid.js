#!/usr/bin/env node

import fs from "fs/promises";
import path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

// Resolve npm global modules root. Priority:
// 1. NODE_PATH env var (set by CI: export NODE_PATH=$(npm root -g))
// 2. require.resolve("jsdom") to find the installed package location
// 3. Common system paths as fallback
async function resolveNpmGlobalRoot() {
  // Check NODE_PATH first (set by CI workflow)
  const nodePath = process.env.NODE_PATH;
  if (nodePath) {
    const candidate = Array.isArray(nodePath) ? nodePath[0] : nodePath;
    if (candidate && candidate !== ".") return candidate;
  }

  // Try require.resolve to find jsdom's location
  try {
    const { createRequire } = await import("module");
    const req = createRequire(import.meta.url);
    const jsdomMain = req.resolve("jsdom/package.json");
    return path.dirname(path.dirname(jsdomMain));
  } catch {
    // jsdom not in module search path, try common locations
  }

  // Common npm global prefixes in order of likelihood
  const candidates = [
    "/usr/local/lib/node_modules",
    "/usr/lib/node_modules",
    "/opt/homebrew/lib/node_modules",
    "/home/quintin/.npm-global/lib/node_modules",
  ];
  for (const candidate of candidates) {
    try {
      if (await fs.access(path.join(candidate, "jsdom", "package.json")).then(() => true).catch(() => false)) {
        return candidate;
      }
    } catch { /* try next */ }
  }

  return candidates[0];
}

const npmGlobalRoot = await resolveNpmGlobalRoot();
const mermaidPath = path.join(npmGlobalRoot, "@mermaid-js/mermaid-cli/node_modules/mermaid/dist/mermaid.core.mjs");
const dompurifyPath = path.join(npmGlobalRoot, "@mermaid-js/mermaid-cli/node_modules/dompurify/dist/purify.cjs.js");
const jsdomPath = path.join(npmGlobalRoot, "jsdom");

// Verify jsdom exists at the resolved path
try {
  await fs.access(path.join(jsdomPath, "package.json"));
} catch {
  console.error(`jsdom not found at ${jsdomPath}`);
  console.error("NODE_PATH:", process.env.NODE_PATH || "(not set)");
  process.exit(1);
}

// Polyfill DOM before importing mermaid (DOMPurify 3.x needs a real DOM)
const { createRequire } = await import("module");
const req = createRequire(import.meta.url);
const { JSDOM } = req(jsdomPath);
const dom = new JSDOM("<!DOCTYPE html><html><body></body></html>", { url: "http://localhost" });
globalThis.window = dom.window;
globalThis.document = dom.window.document;
globalThis.Node = dom.window.Node;
globalThis.Element = dom.window.Element;
globalThis.HTMLTemplateElement = dom.window.HTMLTemplateElement;
globalThis.DocumentFragment = dom.window.DocumentFragment;
globalThis.NodeFilter = dom.window.NodeFilter;

const DOMPurifyModule = req(dompurifyPath);
// DOMPurify 3.x exports a factory function; call it with the jsdom window
globalThis.DOMPurify = typeof DOMPurifyModule === "function" ? DOMPurifyModule(dom.window) : DOMPurifyModule.default || DOMPurifyModule;

const mermaid = await import(mermaidPath);

mermaid.default.initialize({
    startOnLoad: false,
    securityLevel: "strict",
});

const root = process.argv[2] || ".";

// Simple glob: find all .md files recursively, excluding build artifacts
const { execSync } = await import("child_process");
let files;
try {
    const out = execSync(
        `find "${root}" -name "*.md" ` +
        `-not -path "*/node_modules/*" -not -path "*/.git/*" ` +
        `-not -path "*/dist/*" -not -path "*/build/*" 2>/dev/null`,
        { encoding: "utf8" }
    );
    files = out.trim().split("\n").filter(Boolean);
} catch {
    files = [];
}

let hasError = false;
const mermaidRegex = /```mermaid\s*([\s\S]*?)```/g;

for (const file of files) {
    const content = await fs.readFile(file, "utf8");
    let match;
    let index = 1;

    while ((match = mermaidRegex.exec(content)) !== null) {
        const code = match[1];
        try {
            await mermaid.default.parse(code);
        } catch (err) {
            hasError = true;
            console.error("========================================");
            console.error("File :", path.relative(root, file));
            console.error("Block:", index);
            console.error("Error:", err.message);
            console.error("========================================");
        }
        index++;
    }
}

if (hasError) {
    console.error("\n❌ Mermaid syntax check failed.");
    process.exit(1);
}

console.log("✅ All Mermaid diagrams are valid.");
