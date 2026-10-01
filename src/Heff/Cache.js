import { createHash } from "node:crypto";
import { mkdirSync, statSync } from "node:fs";

export const sha256Hex = (s) => createHash("sha256").update(s, "utf8").digest("hex");
export const mtimeMs = (p) => () => { try { return statSync(p).mtimeMs; } catch { return -1; } };
export const ensureDir = (p) => () => { mkdirSync(p, { recursive: true }); };
