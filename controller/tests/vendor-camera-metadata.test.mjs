import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const controllerRoot = new URL('../', import.meta.url);
const vendorUrl = new URL('vendor-camera-metadata.mjs', controllerRoot).href;
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

const setup = `
  import assert from 'node:assert/strict';
  const { MijiaMetadata, MIJIA_METADATA_CLUSTER_ID: id, registerMijiaMetadata } =
    await import(${JSON.stringify(vendorUrl)});
  const { ClusterMap } = await import(${JSON.stringify(mapperUrl)});
  const cluster = ClusterMap[id];
  const { TlvOfModel } = await import('@matter/main/types');
  const { convertCommandDataToMatter, convertWebSocketTagBasedToMatter,
    convertMatterToWebSocketTagBased, convertMatterToWebSocketNameBased, buildAttributePath } =
    await import(${JSON.stringify(convertersUrl)});
  const { toBigIntAwareJson } = await import('@matter-server/ws-client');
  const snapshot = { identity: 'a'.repeat(64),
    sourceEpoch: '00000000-0000-4000-8000-000000000001', revision: 7,
    name: '摄像头', homeKey: 'b'.repeat(64), homeName: '家',
    roomId: '42', roomName: '客厅', observedAt: '2026-10-24T00:00:00.000Z' };
  const guards = { expectedIdentity: snapshot.identity,
    expectedSourceEpoch: snapshot.sourceEpoch, expectedRevision: snapshot.revision };
`;

test('unsigned vendor ID registers once before the frozen mapper and exposes read-only revision-one attributes', () => {
  isolated(setup + `
    const { Matter } = await import('@matter/main/model');
    assert.equal(id, 4294048770);
    assert.equal(registerMijiaMetadata(), MijiaMetadata);
    assert.equal(Matter.clusters.filter(value => value.id === id).length, 1);
    assert.equal(Object.isFrozen(ClusterMap), true);
    assert.equal(cluster.model, MijiaMetadata);
    assert.equal(ClusterMap.mijiametadata, cluster);
    assert.equal(cluster.clusterId, id);
    assert.equal(MijiaMetadata.revision, 1);
    assert.deepEqual(Array.from({ length: 7 }, (_, key) => [key, cluster.attributes[key].name]), [
      [0, 'Snapshot'], [1, 'Rooms'], [2, 'Capabilities'], [3, 'SyncEnabled'],
      [4, 'Available'], [5, 'Loading'], [6, 'LastError']
    ]);
    for (let key = 0; key <= 6; key++) {
      assert.equal(cluster.attributes[key].effectiveAccess.writable, false);
    }
    assert.equal(MijiaMetadata.commands.filter(value => value.isResponse).length, 0);
  `);
});

test('actual OHF attribute wire conversion uses numeric TLV tags for snapshot, rooms and capabilities', () => {
  isolated(setup + `
    const rooms = [{ id: '42', name: '客厅' }, { id: '43', name: '书房' }];
    const capabilities = { read: true, rename: false, move: false, syncConfig: true };
    const values = [snapshot, rooms, capabilities, true, true, false, ''];
    const wire = [
      { 0: snapshot.identity, 1: snapshot.sourceEpoch, 2: 7, 3: snapshot.name,
        4: snapshot.homeKey, 5: snapshot.homeName, 6: '42', 7: '客厅', 8: snapshot.observedAt },
      [{ 0: '42', 1: '客厅' }, { 0: '43', 1: '书房' }],
      { 0: true, 1: false, 2: false, 3: true }, true, true, false, ''
    ];
    for (let attributeId = 0; attributeId <= 6; attributeId++) {
      const model = cluster.attributes[attributeId];
      const tlv = TlvOfModel(model);
      const decoded = tlv.decode(tlv.encode(values[attributeId]));
      assert.deepEqual(convertMatterToWebSocketTagBased(decoded, model, cluster.model), wire[attributeId]);
      assert.deepEqual(convertWebSocketTagBasedToMatter(wire[attributeId], model, cluster.model), values[attributeId]);
      const path = buildAttributePath(1, id, attributeId);
      assert.equal(path, '1/4294048770/' + attributeId);
      const readResult = JSON.parse(toBigIntAwareJson({ [path]: wire[attributeId] }));
      assert.deepEqual(readResult[path], wire[attributeId]);
    }
    // NameBased is only a separate conversion utility, not the read_attribute
    // or cache format. Its RoomId alias must not be mistaken for numeric tags.
    const named = convertMatterToWebSocketNameBased(snapshot, cluster.attributes[0], cluster.model);
    assert.equal(named.roomID, '42');
    assert.equal(named.roomId, '42');
  `);
});

test('all four commands convert camelCase payloads into exact required TLV guards plus their fourth field', () => {
  isolated(setup + `
    const cases = [
      [0, 'RefreshMetadata', {}], [1, 'SetSyncEnabled', { enabled: true }],
      [2, 'RenameDevice', { name: '门口摄像头' }], [3, 'MoveDevice', { roomId: '42' }]
    ];
    for (const [commandId, commandName, extra] of cases) {
      const model = cluster.commands[commandId];
      assert.equal(cluster.commands[commandName.toLowerCase()], model);
      assert.equal(model.name, commandName);
      assert.equal(model.isRequest, true);
      assert.equal(model.response, 'status');
      assert.deepEqual(model.children.map(value => [value.id, value.propertyName]), [
        [0, 'expectedIdentity'], [1, 'expectedSourceEpoch'], [2, 'expectedRevision'],
        ...Object.keys(extra).map(name => [3, name])
      ]);
      const json = JSON.parse(JSON.stringify({ command: 'device_command', args: {
        node_id: '1', endpoint_id: 1, cluster_id: id,
        command_name: commandName, payload: { ...guards, ...extra }
      } }));
      assert.equal(json.args.cluster_id, 4294048770);
      const fields = convertCommandDataToMatter(json.args.payload, model, cluster.model);
      assert.deepEqual(fields, { ...guards, ...extra });
      const tlv = TlvOfModel(model);
      assert.deepEqual(tlv.decode(tlv.encode(fields)), fields);
      const tagBased = convertMatterToWebSocketTagBased(fields, model, cluster.model);
      assert.equal(tagBased[0], guards.expectedIdentity);
      assert.equal(tagBased[1], guards.expectedSourceEpoch);
      assert.equal(tagBased[2], 7);
      if (commandId === 3) assert.equal(tagBased[3], '42');
      for (const key of Object.keys(guards)) {
        const missing = { ...fields };
        delete missing[key];
        assert.throws(() => tlv.validate(missing));
      }
      // device_command is name based. Numeric tags must not silently become
      // valid guard fields in this converter.
      const wrongShape = convertCommandDataToMatter(tagBased, model, cluster.model);
      assert.throws(() => tlv.validate(wrongShape));
    }
  `);
});

test('uint64 revision remains an exact JSON number throughout the safe integer range', () => {
  isolated(setup + `
    const snapshotTlv = TlvOfModel(cluster.attributes[0]);
    const commandTlv = TlvOfModel(cluster.commands[0]);
    for (const revision of [0, 7, 4294967296, Number.MAX_SAFE_INTEGER]) {
      const decoded = snapshotTlv.decode(snapshotTlv.encode({ ...snapshot, revision }));
      const wire = convertMatterToWebSocketTagBased(decoded, cluster.attributes[0], cluster.model);
      const json = JSON.parse(toBigIntAwareJson(wire));
      assert.equal(json[2], revision);
      assert.equal(typeof json[2], 'number');
      assert.equal(Number.isSafeInteger(json[2]), true);
      const commandValue = { ...guards, expectedRevision: revision };
      const decodedCommand = commandTlv.decode(commandTlv.encode(commandValue));
      assert.equal(JSON.parse(toBigIntAwareJson(decodedCommand)).expectedRevision, revision);
    }
    for (const revision of [-1, Number.MAX_SAFE_INTEGER + 1]) {
      assert.throws(() => snapshotTlv.validate({ ...snapshot, revision }));
      assert.throws(() => commandTlv.validate({ ...guards, expectedRevision: revision }));
    }
  `);
});

test('schema enforces string, room list and required member bounds while accepting an unassigned room', () => {
  isolated(setup + `
    const snapshotTlv = TlvOfModel(cluster.attributes[0]);
    const limits = { identity: 64, sourceEpoch: 36, name: 160, homeKey: 64,
      homeName: 160, roomId: 30, roomName: 160, observedAt: 32 };
    for (const [key, maximum] of Object.entries(limits)) {
      assert.doesNotThrow(() => snapshotTlv.validate({ ...snapshot, [key]: 'x'.repeat(maximum) }));
      assert.throws(() => snapshotTlv.validate({ ...snapshot, [key]: 'x'.repeat(maximum + 1) }));
      const missing = { ...snapshot };
      delete missing[key];
      assert.throws(() => snapshotTlv.validate(missing));
    }
    assert.doesNotThrow(() => snapshotTlv.validate({ ...snapshot, roomId: '', roomName: '' }));
    // Projection errors keep the guards and clear all cloud data. Empty
    // ObservedAt is only interpreted as this unavailable sentinel by Qt.
    const incomplete = { ...snapshot, name: '', homeName: '', roomId: '', roomName: '', observedAt: '' };
    assert.deepEqual(snapshotTlv.decode(snapshotTlv.encode(incomplete)), incomplete);
    assert.doesNotThrow(() => snapshotTlv.validate({ ...snapshot, name: '汉'.repeat(160) }));
    const roomsTlv = TlvOfModel(cluster.attributes[1]);
    assert.doesNotThrow(() => roomsTlv.validate([]));
    assert.doesNotThrow(() => roomsTlv.validate(Array.from({ length: 1000 }, () => ({ id: '1', name: '房间' }))));
    assert.throws(() => roomsTlv.validate(Array.from({ length: 1001 }, () => ({ id: '1', name: '房间' }))));
    assert.throws(() => roomsTlv.validate([{ id: 'x'.repeat(31), name: '房间' }]));
    assert.throws(() => roomsTlv.validate([{ id: '1', name: 'x'.repeat(161) }]));
    const lastErrorTlv = TlvOfModel(cluster.attributes[6]);
    assert.doesNotThrow(() => lastErrorTlv.validate('x'.repeat(40)));
    assert.throws(() => lastErrorTlv.validate('x'.repeat(41)));
    assert.throws(() => TlvOfModel(cluster.commands[2]).validate({ ...guards, name: 'x'.repeat(161) }));
    assert.throws(() => TlvOfModel(cluster.commands[3]).validate({ ...guards, roomId: 'x'.repeat(31) }));
    assert.throws(() => TlvOfModel(cluster.commands[3]).validate({ ...guards, roomId: '' }));
  `);
});

test('pinned TLV string validation needs an additional UTF-8 byte gate for metadata projection and Qt', () => {
  isolated(setup + `
    const nameModel = [...cluster.attributes[0].members].find(value => value.propertyName === 'name');
    const nameTlv = TlvOfModel(nameModel);
    const atLimit = '汉'.repeat(53) + 'a';
    const overLimit = atLimit + 'a';
    assert.equal(Buffer.byteLength(atLimit, 'utf8'), 160);
    assert.equal(Buffer.byteLength(overLimit, 'utf8'), 161);
    assert.doesNotThrow(() => nameTlv.validate(atLimit));
    assert.equal(nameTlv.decode(nameTlv.encode(atLimit)), atLimit);
    // The model max alone accepts >160 bytes: the cloud projection and Qt's
    // parser/rename entry point enforce the protocol's byte limit separately.
    assert.doesNotThrow(() => nameTlv.validate(overLimit));
    assert.equal(Buffer.byteLength(overLimit, 'utf8') <= 160, false);
    const badPrefix = '汉'.repeat(86);
    assert.equal(Buffer.byteLength(badPrefix, 'utf8'), 258);
    assert.doesNotThrow(() => nameTlv.validate(badPrefix));
    // Characterize the pinned encoder defect rather than claiming that this
    // model repairs third-party TLV encoding. Inputs are rejected before use.
    assert.notEqual(nameTlv.decode(nameTlv.encode(badPrefix)), badPrefix);
    assert.equal(Buffer.byteLength(badPrefix, 'utf8') <= 160, false);
  `);
});

test('real bootstrap registers both vendor models before server loading without starting a listener', () => {
  const serverEntryUrl = new URL('node_modules/matter-server/dist/esm/bin.js', controllerRoot).href;
  const verificationSource = `
    import assert from 'node:assert/strict';
    const { Matter } = await import('@matter/main/model');
    assert.ok(Matter.clusters(4294048769));
    assert.ok(Matter.clusters(4294048770));
    const { ClusterMap } = await import(${JSON.stringify(mapperUrl)});
    assert.equal(ClusterMap[4294048769].commands.movestep.name, 'MoveStep');
    assert.equal(ClusterMap[4294048770].commands.renamedevice.name, 'RenameDevice');
    process.stdout.write('bootstrap_metadata_registration_verified\\n');
  `;
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

test('late metadata registration cannot mutate a previously frozen ModelMapper map', () => {
  isolated(`
    import assert from 'node:assert/strict';
    const { ClusterMap } = await import(${JSON.stringify(mapperUrl)});
    assert.equal(ClusterMap[4294048770], undefined);
    await import(${JSON.stringify(vendorUrl)});
    const { Matter } = await import('@matter/main/model');
    assert.ok(Matter.clusters(4294048770));
    assert.equal(ClusterMap[4294048770], undefined);
    assert.equal(Object.isFrozen(ClusterMap), true);
  `);
});
