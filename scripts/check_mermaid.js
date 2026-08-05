#!/usr/bin/env node

import fs from "fs/promises";
import path from "path";
import { createRequire } from "module";

const req = createRequire(import.meta.url);

// Resolve mermaid from the global mermaid-cli install (avoids npm install)
const npmGlobalRoot = process.env.NPM_GLOBAL_ROOT || "/home/quintin/.npm-global/lib/node_modules";
const mermaidPath = path.join(npmGlobalRoot, "@mermaid-js/mermaid-cli/node_modules/mermaid/dist/mermaid.core.mjs");
const dompurifyPath = path.join(npmGlobalRoot, "@mermaid-js/mermaid-cli/node_modules/dompurify/dist/purify.cjs.js");

// Polyfill DOM before importing mermaid (DOMPurify 3.x needs a real DOM)
const { JSDOM } = req("/home/quintin/.npm-global/lib/node_modules/jsdom");
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
