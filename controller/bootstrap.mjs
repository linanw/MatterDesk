// Keep ws-controller (and therefore ModelMapper) behind the dynamic import:
// static imports of the server would snapshot its cluster map too early.
import './vendor-camera-ptz.mjs';
import './vendor-camera-metadata.mjs';

await import('./node_modules/matter-server/dist/esm/bin.js');
