// SPDX-License-Identifier: GPL-3.0-only
// web/orkbank.js against the firmware's own upload code (tests/bank_device.c: app/orkest_bank.c, simulated flash).
// Twice: a clean upload, and one where the FM1's answer to a DATA message is lost.
import { spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import { uploadBank } from "../../web/orkbank.js";

const bank = new Uint8Array(readFileSync("build/orkest-bank.bin"));
async function run(drop) {
  const dev = spawn("build/host/bank_device", [], { env: { ...process.env, DROP: String(drop) } });
  let state = "", buf = "";
  dev.stderr.on("data", (d) => { state += d; });
  const listeners = [];
  dev.stdout.on("data", (d) => {
    buf += d;
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const hex = buf.slice(0, i); buf = buf.slice(i + 1);
      const data = Uint8Array.from(hex.match(/../g).map((h) => parseInt(h, 16)));
      listeners.forEach((l) => l({ data }));
    }
  });
  const input = { name: "FM1", state: "connected", addEventListener: (t, f) => listeners.push(f),
                  removeEventListener: (t, f) => listeners.splice(listeners.indexOf(f), 1) };
  const output = { name: "FM1", state: "connected",
                   send: (m) => dev.stdin.write(Buffer.from(m).toString("hex") + "\n") };
  const midi = { inputs: new Map([["i", input]]), outputs: new Map([["o", output]]) };
  let last = 0;
  const info = await uploadBank(midi, bank, (n) => { last = n; });
  dev.stdin.end();
  await new Promise((r) => dev.on("close", r));
  const flash = new Uint8Array(readFileSync("build/host/flash.bin"));
  const same = flash.subarray(24, bank.length).every((b, i) => b === bank[24 + i]);
  const ok = info && info.state === 0 && info.total === bank.length && last === bank.length && same &&
             /state 0 patches 24/.test(state);
  console.log(`${drop ? `upload, the answer to message ${drop} lost` : "upload"}: ${info && info.name}, ` +
              `${last} B`.padEnd(40) + (ok ? "ok" : "FAIL " + state));
  return ok;
}
const results = [await run(0), await run(37)];
console.log(results.every(Boolean) ? "upload tests passed" : "upload tests FAILED");
process.exit(results.every(Boolean) ? 0 : 1);
