import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const controllerRoot = new URL('../', import.meta.url);
const vendorUrl = new URL('vendor-camera-ptz.mjs', controllerRoot).href;
const wsControllerUrl = new URL(import.meta.resolve('@matter-server/ws-controller'));
const mapperUrl = new URL('model/ModelMapper.js', wsControllerUrl).href;
const convertersUrl = new URL('server/Converters.js', wsControllerUrl).href;

function isolated(source) {
  const result = spawnSync(process.execPath, ['--input-type=module', '--eval', source], {
    cwd: fileURLToPath(controllerRoot), encoding: 'utf8', timeout: 30_000,
  });
  assert.ifError(result.error);
  assert.equal(result.signal, null, result.stderr);
  assert.equal(result.status, 0, result.stderr || result.stdout);
}

test('pre-registration resolves the full unsigned vendor ID before the frozen ClusterMap snapshot', () => {
  isolated(`
    import assert from 'node:assert/strict';
    const { MijiaPtzStep, MIJIA_PTZ_STEP_CLUSTER_ID: id, registerMijiaPtzStep } =
      await import(${JSON.stringify(vendorUrl)});
    const { Matter } = await import('@matter/main/model');
    assert.equal(id, 4294048769);
    assert.equal(registerMijiaPtzStep(), MijiaPtzStep);
    assert.equal(Matter.clusters.filter(cluster => cluster.id === id).length, 1);
    const { ClusterMap } = await import(${JSON.stringify(mapperUrl)});
    const cluster = ClusterMap[id];
    assert.equal(Object.isFrozen(ClusterMap), true);
    assert.equal(cluster.model, MijiaPtzStep);
    assert.equal(ClusterMap.mijiaptzstep, cluster);
    assert.equal(cluster.clusterId, id);
    assert.equal(MijiaPtzStep.revision, 1);
    assert.deepEqual([0, 1, 2, 3].map(id => [id, cluster.attributes[id].name]),
      [[0, 'Supported'], [1, 'Available'], [2, 'Mode'], [3, 'Directions']]);
    for (const id of [0, 1, 2, 3]) {
      assert.equal(cluster.attributes[id].effectiveAccess.writable, false);
    }
    assert.equal(cluster.attributes[0].type, 'bool');
    assert.equal(cluster.attributes[1].type, 'bool');
    assert.equal(MijiaPtzStep.datatypes('ModeEnum').type, 'enum8');
    assert.equal(MijiaPtzStep.datatypes('DirectionBitmap').type, 'map8');
    assert.deepEqual(MijiaPtzStep.datatypes('DirectionEnum').children.map(field => [field.name, field.id]),
      [['Up', 0], ['Down', 1], ['Left', 2], ['Right', 3]]);
    const command = cluster.commands.movestep;
    assert.equal(cluster.commands[0], command);
    assert.equal(command.name, 'MoveStep');
    assert.equal(command.isRequest, true);
    assert.equal(command.response, 'status');
    assert.equal(command.children.length, 1);
    assert.equal(command.children[0].id, 0);
    assert.equal(command.children[0].name, 'Direction');
    assert.equal(command.children[0].type, 'DirectionEnum');
    assert.equal(MijiaPtzStep.commands.filter(command => command.isResponse).length, 0);
  `);
});

test('WebSocket conversion and TLV encode exactly one uint8 direction field for every step', () => {
  isolated(`
    import assert from 'node:assert/strict';
    const { MIJIA_PTZ_STEP_CLUSTER_ID: id } = await import(${JSON.stringify(vendorUrl)});
    const { ClusterMap } = await import(${JSON.stringify(mapperUrl)});
    const { convertCommandDataToMatter, convertWebSocketTagBasedToMatter,
      convertMatterToWebSocketTagBased } = await import(${JSON.stringify(convertersUrl)});
    const { TlvOfModel } = await import('@matter/main/types');
    const cluster = ClusterMap[id];
    const command = cluster.commands.movestep;
    const tlv = TlvOfModel(command);
    for (const direction of [0, 1, 2, 3]) {
      const wireRequest = JSON.parse(JSON.stringify({
        command: 'device_command', args: { node_id: '1', endpoint_id: 1, cluster_id: id,
          command_name: 'MoveStep', payload: { direction } }
      }));
      assert.equal(wireRequest.args.cluster_id, 4294048769);
      const fields = convertCommandDataToMatter(wireRequest.args.payload, command, cluster.model);
      assert.deepEqual(fields, { direction });
      assert.deepEqual([...tlv.encode(fields)], [0x15, 0x24, 0, direction, 0x18]);
      assert.deepEqual(tlv.decode(tlv.encode(fields)), { direction });
      assert.deepEqual(convertWebSocketTagBasedToMatter({ 0: direction }, command, cluster.model), { direction });
    }
    assert.throws(() => tlv.validate({}));
    assert.throws(() => tlv.validate({ direction: -1 }));
    // The Matter enum schema reserves future values; the Qt entrypoint and
    // device handler enforce the four permitted direction values.
    const directions = cluster.attributes[3];
    const bitmapTlv = TlvOfModel(directions);
    const all = { up: true, down: true, left: true, right: true };
    assert.deepEqual([...bitmapTlv.encode(all)], [0x04, 15]);
    assert.deepEqual(convertWebSocketTagBasedToMatter(15, directions, cluster.model), all);
    assert.equal(convertMatterToWebSocketTagBased(all, directions, cluster.model), 15);
    assert.equal(convertWebSocketTagBasedToMatter(0, cluster.attributes[2], cluster.model), 0);
    assert.equal(convertWebSocketTagBasedToMatter(true, cluster.attributes[0], cluster.model), true);
    assert.equal(convertWebSocketTagBasedToMatter(false, cluster.attributes[1], cluster.model), false);
  `);
});


test('the real bootstrap registers before loading the server entrypoint without starting a controller', () => {
  const serverEntryUrl = new URL('node_modules/matter-server/dist/esm/bin.js', controllerRoot).href;
  const verificationSource = `
    import assert from 'node:assert/strict';
    const { Matter } = await import('@matter/main/model');
    assert.ok(Matter.clusters(4294048769), 'vendor schema must exist when bin.js begins');
    const { ClusterMap } = await import(${JSON.stringify(mapperUrl)});
    assert.equal(ClusterMap[4294048769].commands.movestep.name, 'MoveStep');
    process.stdout.write('bootstrap_registration_verified\\n');
  `;
  // Substitute only the server entry module, before it is evaluated. The actual
  // bootstrap and vendor module run, but no controller, listener or storage opens.
  const loaderSource = `
    export async function load(url, context, nextLoad) {
      if (url === ${JSON.stringify(serverEntryUrl)}) return {
        format: 'module', shortCircuit: true, source: ${JSON.stringify(verificationSource)}
      };
      return nextLoad(url, context);
    }
  `;
  isolated(`
    import { register } from 'node:module';
    register(${JSON.stringify('data:text/javascript,' + encodeURIComponent(loaderSource))});
    await import(${JSON.stringify(new URL('bootstrap.mjs', controllerRoot).href)});
  `);
});

test('a late registration cannot repair a previously frozen map (cold-process regression)', () => {
  isolated(`
    import assert from 'node:assert/strict';
    const { ClusterMap } = await import(${JSON.stringify(mapperUrl)});
    assert.equal(ClusterMap[4294048769], undefined);
    const { MIJIA_PTZ_STEP_CLUSTER_ID: id } = await import(${JSON.stringify(vendorUrl)});
    const { Matter } = await import('@matter/main/model');
    assert.ok(Matter.clusters(id));
    assert.equal(ClusterMap[id], undefined);
    assert.equal(Object.isFrozen(ClusterMap), true);
  `);
});
