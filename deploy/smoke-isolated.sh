#!/usr/bin/env bash
# Exercise the actual controller and gateway without a route to the host or LAN.
# Building the images can contact Docker Hub/npm; containers below use network=none.
set -euo pipefail

deploy_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$deploy_dir/.." && pwd)"
fixture="$(mktemp -d)"
name="matterdesk-smoke-$$"
caddy='caddy:2.11.9-alpine@sha256:003a97808e781aa9f7e75cc2f43d34d461099031faf5e20faefb9f61126836f0'
controller_image='matterdesk-isolated-test:controller'

cleanup() {
    docker rm -f "$name-gateway" "$name-controller" >/dev/null 2>&1 || true
    docker volume rm "$name-fabric" >/dev/null 2>&1 || true
    rm -rf -- "$fixture"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
docker compose --env-file "$deploy_dir/.env.example" -f "$deploy_dir/compose.yaml" config -q
docker compose --env-file "$deploy_dir/.env.example" -f "$deploy_dir/compose.yaml" -f "$deploy_dir/compose.direct-tls.yaml" config -q
docker build -f "$deploy_dir/Dockerfile" -t "$controller_image" "$project_dir"

fixture_password='isolated-fixture-not-a-real-secret'
hash="$(printf '%s' "$fixture_password" | docker run --rm -i --network none "$caddy" caddy hash-password --algorithm bcrypt)"
printf '%s\n' "fixture $hash" >"$fixture/users.caddy"
chmod 0600 "$fixture/users.caddy"
mkdir -p "$fixture/web"
printf '%s\n' '<!doctype html><title>MatterDesk smoke fixture</title>' >"$fixture/web/index.html"
docker volume create "$name-fabric" >/dev/null
start_controller() {
    docker run -d --name "$name-controller" --network none --init --read-only --tmpfs /tmp --cap-drop ALL \
        --security-opt no-new-privileges:true --env DCL_OFFLINE=true \
        --volume "$name-fabric:/var/lib/matterdesk" "$controller_image" >/dev/null
}
start_controller
docker run --rm -d --name "$name-gateway" --network "container:$name-controller" --read-only --tmpfs /tmp \
    --cap-drop ALL --cap-add DAC_READ_SEARCH --security-opt no-new-privileges:true \
    --volume "$deploy_dir/Caddyfile:/etc/caddy/Caddyfile:ro" \
    --volume "$fixture/users.caddy:/run/secrets/matterdesk-users:ro" \
    --volume "$fixture/web:/srv/matterdesk:ro" "$caddy" >/dev/null

if ! docker exec -i "$name-controller" node --input-type=module - "$fixture_password" <<'JS'
import assert from 'node:assert/strict';
import WebSocket from '/opt/matterdesk/controller/node_modules/ws/wrapper.mjs';

const base = 'http://127.0.0.1:18081';
const user = 'fixture';
const password = process.argv[2];
const credential = `Basic ${Buffer.from(`${user}:${password}`).toString('base64')}`;
const origin = 'https://matter.linanw.me';

for (let attempt = 0; ; ++attempt) {
    const ready = await Promise.all([fetch(base), fetch('http://127.0.0.1:5580/health')])
        .then(([front, back]) => front.status === 401 && back.ok, () => false);
    if (ready) break;
    if (attempt >= 40) throw new Error('gateway or controller did not become ready');
    await new Promise(resolve => setTimeout(resolve, 250));
}

const denied = await fetch(base);
assert.equal(denied.status, 401);
assert.match(denied.headers.get('www-authenticate'), /Basic realm="MatterDesk"/);
assert.equal((await fetch(base, {headers: {Authorization: 'Basic Zml4dHVyZTp3cm9uZw=='}})).status, 401);
const accepted = await fetch(base, {headers: {Authorization: credential}});
assert.equal(accepted.status, 200);
assert.match(await accepted.text(), /MatterDesk smoke fixture/);
assert.equal((await fetch(`${base}/health`, {headers: {Authorization: credential}})).status, 404);

const handshake = headers => new Promise((resolve, reject) => {
    // The read-only handshake subscribes to this new, empty Fabric; no pairing or control.
    const ws = new WebSocket('ws://127.0.0.1:18081/ws', {headers, handshakeTimeout: 3000});
    const timer = setTimeout(() => { ws.terminate(); reject(new Error('WebSocket test timed out')); }, 5000);
    ws.on('unexpected-response', (_req, response) => {
        response.resume(); clearTimeout(timer); resolve(response.statusCode); ws.terminate();
    });
    ws.on('error', error => { clearTimeout(timer); reject(error); });
    ws.on('message', message => {
        const data = JSON.parse(message.toString());
        if (data.schema_version !== undefined) {
            assert(data.schema_version >= 11);
            ws.send(JSON.stringify({message_id: 'smoke', command: 'start_listening'}));
        }
        if (data.message_id === 'smoke') {
            assert.deepEqual(data.result, []);
            clearTimeout(timer); ws.close(); resolve(101);
        }
    });
});
assert.equal(await handshake({}), 401);
assert.equal(await handshake({Authorization: credential, Origin: 'https://bad.invalid'}), 403);
assert.equal(await handshake({Authorization: credential, Origin: 'null'}), 403);
assert.equal(await handshake({Authorization: credential, Origin: origin}), 101);
assert.equal(await handshake({Authorization: credential}), 101);
console.log('Isolated smoke passed: HTTP and WebSocket authentication, Origin checks, upstream health isolation, real OHF WebSocket state.');
JS
then
    docker logs "$name-controller" >&2 || true
    docker logs "$name-gateway" >&2 || true
    exit 1
fi

# Verify that the named volume retains the generated credentials across a fresh container.
fabric_id() {
    docker exec -i "$name-controller" node --input-type=module - <<'JS'
import WebSocket from '/opt/matterdesk/controller/node_modules/ws/wrapper.mjs';
for (let attempt = 0; ; ++attempt) {
    if (await fetch('http://127.0.0.1:5580/health').then(r => r.ok, () => false)) break;
    if (attempt >= 40) throw new Error('controller did not become ready');
    await new Promise(resolve => setTimeout(resolve, 250));
}
const socket = new WebSocket('ws://127.0.0.1:5580/ws', {handshakeTimeout: 3000});
const timer = setTimeout(() => { socket.terminate(); process.exitCode = 1; }, 5000);
socket.once('message', message => {
    const info = JSON.parse(message.toString());
    if (!info.compressed_fabric_id) throw new Error('missing Fabric identity');
    console.log(info.compressed_fabric_id);
    clearTimeout(timer);
    socket.close();
});
JS
}
original_fabric="$(fabric_id)"
docker stop --time 20 "$name-gateway" >/dev/null
docker stop --time 20 "$name-controller" >/dev/null
docker rm "$name-controller" >/dev/null
start_controller
[[ -n "$original_fabric" && "$(fabric_id)" == "$original_fabric" ]]
echo 'Isolated restart passed: persisted Fabric identity was reused.'
