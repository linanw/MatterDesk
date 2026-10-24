import { ClusterModel, Matter } from '@matter/main/model';

// This vendor extension contains cloud metadata for the selected camera. It is
// independent of BasicInformation.NodeLabel and MatterDesk's local aliases.
// Pinned matter.js validates string length as JS code units. Cloud projection
// and Qt must additionally enforce UTF-8 byte limits before using these fields.
export const MIJIA_METADATA_CLUSTER_ID = 0xfff1fc02;
export const MIJIA_METADATA_MAX_REVISION = Number.MAX_SAFE_INTEGER;

const field = (name, id, type, extra = {}) =>
  ({ tag: 'field', name, id, type, conformance: 'M', ...extra });
const text = (name, id, length) => field(name, id, 'string', { constraint: `max ${length}` });
const revision = (name, id) =>
  field(name, id, 'uint64', { constraint: `max ${MIJIA_METADATA_MAX_REVISION}` });
const attribute = (name, id, type, extra = {}) =>
  ({ tag: 'attribute', name, id, type, access: 'R V', conformance: 'M', ...extra });
const guards = () => [
  text('ExpectedIdentity', 0, 64),
  text('ExpectedSourceEpoch', 1, 36),
  revision('ExpectedRevision', 2),
];
const command = (name, id, extra = []) => ({
  tag: 'command', name, id, access: 'O', conformance: 'M',
  direction: 'request', response: 'status', children: [...guards(), ...extra],
});

export const MijiaMetadata = new ClusterModel({
  name: 'MijiaMetadata', id: MIJIA_METADATA_CLUSTER_ID, classification: 'application',
  children: [
    { tag: 'attribute', name: 'ClusterRevision', id: 0xfffd, type: 'ClusterRevision', default: 1 },
    attribute('Snapshot', 0, 'SnapshotStruct'),
    attribute('Rooms', 1, 'list', { constraint: 'max 1000',
      children: [{ tag: 'field', name: 'entry', type: 'RoomStruct' }] }),
    attribute('Capabilities', 2, 'CapabilitiesStruct'),
    attribute('SyncEnabled', 3, 'bool'),
    attribute('Available', 4, 'bool'),
    attribute('Loading', 5, 'bool'),
    attribute('LastError', 6, 'string', { constraint: 'max 40' }),
    { tag: 'datatype', name: 'SnapshotStruct', type: 'struct', children: [
      text('Identity', 0, 64),
      text('SourceEpoch', 1, 36),
      revision('Revision', 2),
      text('Name', 3, 160),
      text('HomeKey', 4, 64),
      text('HomeName', 5, 160),
      text('RoomId', 6, 30),
      text('RoomName', 7, 160),
      text('ObservedAt', 8, 32),
    ] },
    { tag: 'datatype', name: 'RoomStruct', type: 'struct', children: [
      text('Id', 0, 30), text('Name', 1, 160),
    ] },
    { tag: 'datatype', name: 'CapabilitiesStruct', type: 'struct', children: [
      field('Read', 0, 'bool'), field('Rename', 1, 'bool'),
      field('Move', 2, 'bool'), field('SyncConfig', 3, 'bool'),
    ] },
    command('RefreshMetadata', 0),
    command('SetSyncEnabled', 1, [field('Enabled', 3, 'bool')]),
    command('RenameDevice', 2, [text('Name', 3, 160)]),
    command('MoveDevice', 3, [field('RoomId', 3, 'string', { constraint: '1 to 30' })]),
  ],
});

export function registerMijiaMetadata() {
  const existing = Matter.clusters(MIJIA_METADATA_CLUSTER_ID);
  if (existing && existing !== MijiaMetadata) {
    throw new Error('Mijia metadata cluster ID is already registered by a different schema');
  }
  if (!existing) Matter.children.push(MijiaMetadata);
  return MijiaMetadata;
}

// ModelMapper freezes its cluster map at import time. The bootstrap must finish
// importing this module before dynamically importing the server entrypoint.
registerMijiaMetadata();
