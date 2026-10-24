import { ClusterModel, Matter } from '@matter/main/model';

// This is the bridge's vendor extension for directional steps, not standard
// mechanical pan/tilt: it defines no angles, durations, position or zoom.
export const MIJIA_PTZ_STEP_CLUSTER_ID = 0xfff1fc01;
export const PTZ_STEP_DIRECTIONS = Object.freeze(['up', 'down', 'left', 'right']);

const field = (name, id, type, extra = {}) =>
  ({ tag: 'field', name, id, type, conformance: 'M', ...extra });
const attribute = (name, id, type, value) =>
  ({ tag: 'attribute', name, id, type, access: 'R V', conformance: 'M', default: value });

export const MijiaPtzStep = new ClusterModel({
  name: 'MijiaPtzStep',
  id: MIJIA_PTZ_STEP_CLUSTER_ID,
  classification: 'application',
  children: [
    { tag: 'attribute', name: 'ClusterRevision', id: 0xfffd, type: 'ClusterRevision', default: 1 },
    attribute('Supported', 0, 'bool', false),
    attribute('Available', 1, 'bool', false),
    attribute('Mode', 2, 'ModeEnum', 0),
    attribute('Directions', 3, 'DirectionBitmap', 0),
    { tag: 'datatype', name: 'ModeEnum', type: 'enum8', children: [field('Step', 0)] },
    { tag: 'datatype', name: 'DirectionEnum', type: 'enum8',
      children: PTZ_STEP_DIRECTIONS.map((name, id) => field(name[0].toUpperCase() + name.slice(1), id)) },
    { tag: 'datatype', name: 'DirectionBitmap', type: 'map8',
      children: PTZ_STEP_DIRECTIONS.map((name, id) =>
        ({ tag: 'field', name: name[0].toUpperCase() + name.slice(1), constraint: String(id), conformance: 'M' })) },
    { tag: 'command', name: 'MoveStep', id: 0, access: 'O', conformance: 'M',
      direction: 'request', response: 'status',
      children: [field('Direction', 0, 'DirectionEnum', { constraint: '0 to 3' })] },
  ],
});

export function registerMijiaPtzStep() {
  const existing = Matter.clusters(MIJIA_PTZ_STEP_CLUSTER_ID);
  if (existing && existing !== MijiaPtzStep) {
    throw new Error('Mijia PTZ cluster ID is already registered by a different schema');
  }
  if (!existing) Matter.children.push(MijiaPtzStep);
  return MijiaPtzStep;
}

// ModelMapper snapshots Matter.clusters and freezes ClusterMap at import time.
// This module must finish before the first ws-controller/ModelMapper import.
registerMijiaPtzStep();
