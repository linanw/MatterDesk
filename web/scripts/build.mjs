import { cp, mkdir, rm } from "node:fs/promises";
import { resolve, join } from "node:path";
import { fileURLToPath } from "node:url";

const web = fileURLToPath(new URL("..", import.meta.url));
const shared = resolve(web, "../src/camera/player.js");
const dist = join(web, "dist");
await rm(dist, { recursive: true, force: true });
await mkdir(join(dist, "camera"), { recursive: true });
await cp(join(web, "src"), dist, { recursive: true });
await cp(shared, join(dist, "camera/player.js"));
console.log("Built web/dist with the canonical Qt camera player");
