export type Attribute = string | number | boolean | Object | null;

export interface MatterNode {
  node_id: number;
  available: boolean;
  attributes: Record<string, Attribute>;
}

export interface Accessory {
  key: string;
  node: number;
  endpoint: number;
  name: string;
  bridge: string;
  online: boolean;
  on: boolean | null;
  power: boolean;
  shade: boolean;
  camera: boolean;
  category: string;
  details: string;
}

function category(clusters: number[], types: Attribute): string {
  if (clusters.includes(1361) || clusters.includes(257)) {
    return '安防';
  }
  if (clusters.includes(258)) {
    return '窗帘';
  }
  if (clusters.includes(513) || clusters.includes(514) || clusters.includes(1026) || clusters.includes(1029)) {
    return '环境';
  }
  const lightTypes = [0x100, 0x101, 0x10c, 0x10d, 0x10e];
  const declaredLight = Array.isArray(types) && (types as Record<string, number>[])
    .some(type => lightTypes.includes(type['0']));
  if (declaredLight || clusters.includes(8) || clusters.includes(768)) {
    return '灯光';
  }
  return clusters.includes(6) ? '开关' : '其他';
}

function text(value: Attribute): string {
  return typeof value === 'string' ? value.trim() : '';
}

export function accessories(nodes: MatterNode[]): Accessory[] {
  const result: Accessory[] = [];
  for (const node of nodes) {
    if (!node.attributes || !Number.isSafeInteger(node.node_id)) {
      continue;
    }
    const attributes = node.attributes;
    const bridge = text(attributes['0/40/5']) || text(attributes['0/40/3']) || `节点 ${node.node_id}`;
    for (const path of Object.keys(attributes)) {
      if (!/^[1-9]\d*\/29\/1$/.test(path)) {
        continue;
      }
      const endpoint = Number(path.split('/')[0]);
      const clusters = attributes[path] as number[];
      if (!Array.isArray(clusters) || !clusters.some(id => id !== 29 && id !== 3)) {
        continue;
      }
      const prefix = `${endpoint}/`;
      const on = attributes[prefix + '6/0'];
      const temperature = attributes[prefix + '1026/0'];
      const humidity = attributes[prefix + '1029/0'];
      const readings: string[] = [];
      if (typeof temperature === 'number') {
        readings.push(`${(temperature / 100).toFixed(1)} °C`);
      }
      if (typeof humidity === 'number') {
        readings.push(`${(humidity / 100).toFixed(1)}%`);
      }
      if (clusters.includes(1361)) {
        readings.push('摄像头');
      }
      result.push({
        key: `${node.node_id}/${endpoint}`,
        node: node.node_id,
        endpoint: endpoint,
        name: text(attributes[prefix + '57/5']) || text(attributes[prefix + '40/5']) ||
          text(attributes[prefix + '57/3']) || (endpoint === 1 ? bridge : `端点 ${endpoint}`),
        bridge: bridge,
        online: node.available === true && attributes[prefix + '57/17'] !== false,
        on: typeof on === 'boolean' ? on : null,
        power: clusters.includes(6),
        shade: clusters.includes(258),
        camera: clusters.includes(1361),
        category: category(clusters, attributes[prefix + '29/0']),
        details: readings.join(' · ')
      });
    }
  }
  return result;
}
