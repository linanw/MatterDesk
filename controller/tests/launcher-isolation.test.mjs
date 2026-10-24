import assert from 'node:assert/strict';
import { test } from 'node:test';
import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, readdir, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createServer } from 'node:net';
import { once } from 'node:events';

const project = fileURLToPath(new URL('../../', import.meta.url));
const freePort = async () => {
  const server = createServer(); server.listen(0, '127.0.0.1'); await once(server, 'listening');
  const { port } = server.address(); await new Promise(resolve => server.close(resolve));
  assert.notEqual(port, 5580); return port;
};
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function startAndCheck(root, storage, separated) {
  const port = await freePort();
  const args = separated ? ['--port', String(port), '--log-level', 'fatal'] : [`--port=${port}`, '--log-level=fatal'];
  const child = spawn(resolve(project, 'scripts/start-controller.sh'), args, {
    cwd: root, env: { ...process.env, HOME: join(root,'home'), XDG_DATA_HOME: join(root,'xdg-data'),
      XDG_CONFIG_HOME: join(root,'xdg-config'), XDG_STATE_HOME: join(root,'xdg-state'),
      MATTERDESK_DATA_DIR: storage, MATTER_INTERFACE: 'lo' }, stdio:['ignore','pipe','pipe']
  });
  let output = ''; child.stdout.on('data', chunk => output += chunk); child.stderr.on('data', chunk => output += chunk);
  let socket;
  try {
    for (let attempt = 0; attempt < 100; attempt++) {
      if (child.exitCode !== null) throw new Error(`isolated launcher exited ${child.exitCode}: ${output.slice(-1500)}`);
      const candidate = new WebSocket(`ws://127.0.0.1:${port}/ws`);
      const connected = await new Promise(resolve => { candidate.onopen=()=>resolve(true); candidate.onerror=()=>resolve(false); });
      if (connected) { socket=candidate; break; }
      candidate.close(); await pause(50);
    }
    assert.ok(socket, 'isolated launcher did not open its requested non-production port');
    const greeting = await new Promise((resolve,reject) => {
      const timer=setTimeout(()=>reject(new Error('no schema greeting')),3000);
      socket.onmessage=({data})=>{ const message=JSON.parse(data); if(message.schema_version){clearTimeout(timer);resolve(message);} };
    });
    assert.equal(greeting.schema_version,14);
    const nodes = await new Promise((resolve,reject) => {
      const timer=setTimeout(()=>reject(new Error('no empty isolated node list')),3000);
      socket.onmessage=({data})=>{ const message=JSON.parse(data); if(message.message_id==='isolated'){clearTimeout(timer);resolve(message.result);} };
      socket.send(JSON.stringify({message_id:'isolated',command:'start_listening'}));
    });
    assert.deepEqual(nodes, []);
    assert.ok((await readdir(storage)).length > 0, 'custom storage path was not used');
    assert.equal(output.includes('Invalid value for configuration variable log.level'), false);
    return { fabric: greeting.fabric_id, compressed: greeting.compressed_fabric_id };
  } finally {
    socket?.close(); child.kill('SIGINT');
    await Promise.race([once(child,'exit'),pause(4000)]);
    if(child.exitCode===null && child.signalCode===null){child.kill('SIGKILL');await once(child,'exit');}
  }
}
test('launcher normalizes valued flags and preserves isolated custom storage on restart', {timeout:25000}, async () => {
  const root=await mkdtemp(join(tmpdir(),'matterdesk-launcher-qa-'));
  const storage=join(root,'custom storage with spaces');
  try {
    await mkdir(join(root,'home')); const first=await startAndCheck(root,storage,true);
    const second=await startAndCheck(root,storage,false);
    assert.deepEqual(second,first,'restarting with the same storage changed the isolated fabric identity');
    assert.equal((await readdir(join(root,'xdg-data')).catch(error=>error.code==='ENOENT'?[]:Promise.reject(error))).length,0,
      'launcher wrote a default XDG storage directory instead of only the specified storage');
  } finally {await rm(root,{recursive:true,force:true});}
});
