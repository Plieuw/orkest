// SPDX-License-Identifier: GPL-3.0-only
// web/fm1ota.js's command frames against M-VAVE's own (sdk/WebUpdate.js: pack7, frame, parse), byte for byte.
import { readFileSync } from "node:fs";
const src = readFileSync(new URL("../sdk/WebUpdate.js", import.meta.url), "utf8");
const grab = (name) => { const i = src.indexOf(`function ${name}(`); let d = 0, j = src.indexOf("{", i);
  for (let k = j; k < src.length; k++) { if (src[k] === "{") d++; else if (src[k] === "}" && !--d) return src.slice(i, k + 1); } };
const check = (c, m) => { if (!c) throw new Error(m); };
const le = (b, o, n) => { let v = 0; for (let i = n - 1; i >= 0; i--) v = v * 256 + b[o + i]; return v; };
const M = new Function("check", "le", grab("pack7") + grab("frame") + grab("parse") + "; return {frame, parse};")(check, le);
const ota = readFileSync(new URL("../../web/fm1ota.js", import.meta.url), "utf8").replace(/^export /gm, "");
const O = new Function(ota + "; return {frame, parseFrame, pack7};")();
let ok = true;
for (const [cmd, payload] of [[17, []], [49, []], [50, [1, 7, 1, 2, 3, 4, ...Array(32).fill(0xab)]], [34, [5, 0, 2, 0, 0, 0, 2, 0, ...Array(512).fill(0).map((_, i) => i & 255)]]]) {
  const a = Array.from(M.frame(cmd, payload)), b = O.frame(cmd, payload);
  const same = a.length === b.length && a.every((x, i) => x === b[i]);
  const back = O.parseFrame(a), back2 = M.parse(Uint8Array.from(b));
  const rt = back && back.cmd === cmd && back.data.length === payload.length && back2 && back2.command === cmd;
  console.log(`cmd ${cmd} (${payload.length} B): frame ${same ? "identical" : "DIFFERENT"}, parse both ways ${rt ? "ok" : "FAIL"}`);
  ok = ok && same && rt;
}
console.log(ok ? "frame tests passed" : "frame tests FAILED"); process.exit(ok ? 0 : 1);
