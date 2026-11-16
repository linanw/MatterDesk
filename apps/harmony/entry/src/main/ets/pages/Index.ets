import { webSocket } from '@kit.NetworkKit';
import { webview } from '@kit.ArkWeb';
import { preferences } from '@kit.ArkData';
import { Accessory, Attribute, MatterNode, accessories } from './Devices';

interface CommandArgs {
  node_id?: number;
  endpoint_id?: number;
  cluster_id?: number;
  command_name?: string;
  payload?: Record<string, number>;
  code?: string;
  network_only?: boolean;
}

interface Request {
  message_id: string;
  command: string;
  args: CommandArgs;
}

interface Frame {
  schema_version?: number;
  min_supported_schema_version?: number;
  message_id?: string;
  error_code?: number;
  result?: Object;
  event?: string;
  data?: Attribute;
}

interface Pending {
  command: string;
  timer: number;
  cameraToken?: string;
}

interface CameraConfig {
  node_id: number;
  endpoint_id: number;
  mock: boolean;
}

const CAMERA_COMMANDS = ['camera_get_capabilities', 'camera_start_stream', 'camera_stop_stream',
  'camera_provide_answer', 'camera_provide_ice_candidates', 'camera_release_stream'];

class CameraHost {
  private sendRequest: (token: string, command: string, json: string) => boolean;
  private update: (state: string, detail: string, audio: boolean) => void;
  private close: () => void;

  constructor(
    sendRequest: (token: string, command: string, json: string) => boolean,
    update: (state: string, detail: string, audio: boolean) => void,
    close: () => void
  ) {
    this.sendRequest = sendRequest;
    this.update = update;
    this.close = close;
  }

  send(token: string, command: string, json: string): boolean {
    return this.sendRequest(token, command, json);
  }

  report(state: string, detail: string, audio: boolean): void {
    this.update(state, detail, audio);
  }

  finished(): void {
    this.close();
  }
}

const CONTROLLER = 'ws://127.0.0.1:15580/ws';
const GOLD = '#FFB654';
const INK = '#FBF8F4';
const MUTED = '#C2C2C9';
const PANEL = '#292B35';
const CATEGORIES = ['灯光', '开关', '窗帘', '环境', '安防', '其他'];

@Entry
@Component
struct Index {
  @State address: string = CONTROLLER;
  @State status: string = '未连接';
  @State notice: string = '';
  @State ready: boolean = false;
  @State busy: boolean = false;
  @State code: string = '';
  @State search: string = '';
  @State devices: Accessory[] = [];

  @State pageWidth: number = 760;
  @State section: string = '家庭';
  @State bridge: string = '';
  @State favorites: string[] = [];
  @State sheet: string = '';
  @State selectedKey: string = '';
  @State searchOpen: boolean = false;

  @State cameraTitle: string = '';
  @State cameraState: string = '';
  @State cameraDetail: string = '';
  @State cameraAudio: boolean = false;
  @State cameraSound: boolean = false;
  @State cameraStopping: boolean = false;

  private cameraController: webview.WebviewController = new webview.WebviewController();
  private cameraConfig: CameraConfig | undefined;
  private cameraLoaded: boolean = false;
  private cameraHost: CameraHost = new CameraHost(
    (token, command, json) => this.cameraRequest(token, command, json),
    (state, detail, audio) => {
      if (!this.cameraStopping) {
        this.cameraState = state;
        this.cameraDetail = detail;
        this.cameraAudio = audio;
      }
    },
    () => this.closeCamera()
  );

  private socket?: webSocket.WebSocket;
  private settings?: preferences.Preferences;
  private favoritesKey: string = '';
  private deadline: number = -1;
  private sequence: number = 0;
  private nodes: MatterNode[] = [];
  private pending: Map<string, Pending> = new Map();

  aboutToAppear(): void {
    const context = this.getUIContext().getHostContext();
    try {
      if (context) {
        this.settings = preferences.getPreferencesSync(context, { name: 'matterdesk' });
        this.address = this.settings.getSync('address', CONTROLLER) as string;
      }
    } catch {
      this.notice = '无法读取已保存的连接地址。';
    }
    this.connect();
  }

  onPageHide(): void {
    this.stopCamera();
  }

  onBackPress(): boolean {
    if (this.cameraTitle) {
      this.stopCamera();
      return true;
    }
    if (this.sheet) {
      this.sheet = '';
      this.code = '';
      return true;
    }
    return false;
  }

  aboutToDisappear(): void {
    this.disconnect('未连接');
  }

  private disconnect(status: string): void {
    if (this.cameraConfig && !this.cameraConfig.mock) {
      this.closeCamera();
    }
    const previous = this.socket;
    this.socket = undefined;
    clearTimeout(this.deadline);
    this.pending.forEach(pending => clearTimeout(pending.timer));
    this.pending.clear();
    previous?.close().catch(() => {});
    this.ready = false;
    this.busy = false;
    this.nodes = [];
    this.devices = [];
    this.favorites = [];
    this.favoritesKey = '';
    this.status = status;
    this.code = '';
  }

  private connect(): void {
    const url = this.address.trim();
    if (!/^(ws:\/\/127\.0\.0\.1:\d+|wss:\/\/[^\s\/@]+)\/ws$/.test(url)) {
      this.notice = '请输入本机转发地址或安全的 wss://服务器/ws 地址。';
      return;
    }
    this.disconnect('正在连接控制器');
    this.notice = '';
    const socket = webSocket.createWebSocket();
    this.socket = socket;
    const fail = () => {
      if (this.socket === socket) {
        this.disconnect('连接已断开');
        this.notice = '请确认 Matter Server 和无线调试转发已启动，然后重新连接。';
      }
    };
    this.deadline = setTimeout(fail, 10000);
    socket.on('error', fail);
    socket.on('close', fail);
    socket.on('message', (error, message) => {
      if (this.socket === socket && !error && typeof message === 'string') {
        try {
          this.receive(JSON.parse(message) as Frame, url);
        } catch {
          this.notice = '控制器返回了无效数据。';
        }
      }
    });
    socket.connect(url).catch(fail);
  }

  private request(command: string, args: CommandArgs = {}, timeout: number = 12000, cameraToken?: string): void {
    const socket = this.socket;
    if (!socket) {
      return;
    }
    const id = String(++this.sequence);
    const timer = setTimeout(() => this.failRequest(id, '控制器响应超时。'), timeout);
    this.pending.set(id, { command: command, timer: timer, cameraToken: cameraToken });
    const request: Request = { message_id: id, command: command, args: args };
    socket.send(JSON.stringify(request)).then(sent => {
      if (!sent) {
        this.failRequest(id, '请求发送失败。');
      }
    }).catch(() => this.failRequest(id, '请求发送失败。'));
  }

  private failRequest(id: string, notice: string): void {
    const pending = this.pending.get(id);
    if (!pending) {
      return;
    }
    clearTimeout(pending.timer);
    this.pending.delete(id);
    if (pending.cameraToken) {
      this.cameraResponse(pending, false, '{"error_code":-1}');
      return;
    }
    if (pending.command === 'start_listening') {
      this.disconnect('设备同步失败');
    } else if (pending.command === 'device_command' || pending.command === 'commission_with_code') {
      this.busy = false;
    }
    this.notice = notice;
  }

  private receive(frame: Frame, url: string): void {
    if (frame.schema_version !== undefined && !this.ready && this.pending.size === 0) {
      if (frame.schema_version < 1 || (frame.min_supported_schema_version ?? 0) > 14) {
        this.disconnect('控制器协议版本不兼容');
        return;
      }
      this.request('start_listening');
      return;
    }
    if (frame.message_id) {
      this.receiveResponse(frame.message_id, frame, url);
    } else if (this.ready) {
      this.receiveEvent(frame);
    }
  }

  private receiveResponse(id: string, frame: Frame, url: string): void {
    const pending = this.pending.get(id);
    if (!pending) {
      return;
    }
    if (pending.cameraToken) {
      clearTimeout(pending.timer);
      this.pending.delete(id);
      this.cameraResponse(pending, frame.error_code === undefined && Object.keys(frame).includes('result'),
        JSON.stringify(frame.error_code === undefined ? frame.result ?? null : frame));
      return;
    }
    if (frame.error_code !== undefined || !Object.keys(frame).includes('result')) {
      this.failRequest(id, `控制器未完成操作（${frame.error_code ?? '响应无效'}）。`);
      return;
    }
    if ((pending.command === 'start_listening' || pending.command === 'get_nodes') && !Array.isArray(frame.result)) {
      this.failRequest(id, '控制器未返回设备列表。');
      return;
    }
    clearTimeout(pending.timer);
    this.pending.delete(id);
    if (pending.command === 'start_listening' || pending.command === 'get_nodes') {
      this.nodes = frame.result as MatterNode[];
      this.devices = accessories(this.nodes);
      if (pending.command === 'start_listening') {
        clearTimeout(this.deadline);
        this.ready = true;
        this.status = '已连接 · 实时同步';
        try {
          this.favoritesKey = 'favorites:' + url;
          const savedFavorites = this.settings?.getSync(this.favoritesKey, '[]') ?? '[]';
          this.favorites = JSON.parse(savedFavorites as string) as string[];
          this.settings?.putSync('address', url);
          this.settings?.flushSync();
        } catch {
          this.notice = '无法保存连接地址。';
        }
      }
    } else if (pending.command === 'device_command' || pending.command === 'commission_with_code') {
      this.busy = false;
      this.notice = pending.command === 'device_command' ? '控制器已确认指令。' : '配对成功。';
      this.request('get_nodes');
    }
  }

  private receiveEvent(frame: Frame): void {
    switch (frame.event) {
      case 'webrtc_callback':
      case 'camera_session_ended':
        if (this.cameraConfig && !this.cameraConfig.mock) {
          this.cameraScript(`window.harmonyCamera.event(${JSON.stringify(frame.event)}, ${JSON.stringify(frame.data)})`);
        }
        return;
      case 'node_added':
      case 'node_updated': {
        const node = frame.data as MatterNode;
        this.nodes = this.nodes.filter(item => item.node_id !== node.node_id).concat(node);
        break;
      }
      case 'node_removed':
        this.nodes = this.nodes.filter(node => node.node_id !== frame.data);
        break;
      case 'attribute_updated': {
        const update = frame.data as Attribute[];
        if (!Array.isArray(update) || update.length !== 3 || typeof update[1] !== 'string') {
          return;
        }
        const node = this.nodes.find(item => item.node_id === update[0]);
        if (node) {
          node.attributes[update[1]] = update[2];
        }
        break;
      }
      case 'endpoint_added':
      case 'endpoint_removed':
        this.request('get_nodes');
        return;
      default:
        return;
    }
    this.devices = accessories(this.nodes);
    if (this.cameraConfig && !this.cameraConfig.mock && !this.devices.some(device =>
      device.camera && device.online && device.node === this.cameraConfig?.node_id &&
      device.endpoint === this.cameraConfig?.endpoint_id)) {
      this.stopCamera();
    }
  }

  private cameraScript(script: string): void {
    this.cameraController.runJavaScript(script).catch(() => {
      if (this.cameraStopping) {
        this.closeCamera();
      } else if (this.cameraTitle) {
        this.cameraState = 'error';
        this.cameraDetail = '播放器无法加载。请返回后重试。';
      }
    });
  }

  private cameraRequest(token: string, command: string, json: string): boolean {
    const config = this.cameraConfig;
    if (!config || config.mock || !this.ready || !this.socket || !CAMERA_COMMANDS.includes(command)) {
      return false;
    }
    try {
      const args = JSON.parse(json) as CommandArgs;
      args.node_id = config.node_id;
      args.endpoint_id = config.endpoint_id;
      this.request(command, args, command === 'camera_start_stream' ? 45000 : 15000, token);
      return true;
    } catch {
      return false;
    }
  }

  private cameraResponse(pending: Pending, success: boolean, payload: string): void {
    if (!this.cameraConfig) {
      return;
    }
    this.cameraScript(`window.harmonyCamera.response(${JSON.stringify(pending.cameraToken)}, ` +
      `${JSON.stringify(pending.command)}, ${success}, ${success ? payload : 'null'}, ${success ? 'null' : payload})`);
  }

  private openCamera(device?: Accessory): void {
    if (this.cameraTitle || (device && (!device.camera || !device.online || !this.ready))) {
      return;
    }
    this.cameraConfig = { node_id: device?.node ?? 0, endpoint_id: device?.endpoint ?? 0, mock: !device };
    this.cameraLoaded = false;
    this.cameraController = new webview.WebviewController();
    this.cameraState = 'connecting';
    this.cameraDetail = '正在初始化播放器…';
    this.cameraTitle = device?.name ?? '模拟摄像头';
  }

  private stopCamera(): void {
    if (!this.cameraTitle || this.cameraStopping) {
      return;
    }
    if (!this.cameraLoaded) {
      this.closeCamera();
      return;
    }
    this.cameraStopping = true;
    this.cameraScript('window.harmonyCamera.stop(); void 0');
  }

  private closeCamera(): void {
    this.cameraConfig = undefined;
    this.cameraLoaded = false;
    this.cameraTitle = '';
    this.cameraStopping = false;
    this.cameraSound = false;
    this.cameraAudio = false;
  }

  private control(device: Accessory, cluster: number, command: string): void {
    const current = this.devices.find(item => item.key === device.key);
    if (!this.ready || this.busy || !current?.online || (cluster === 6 ? !current.power : !current.shade)) {
      return;
    }
    this.busy = true;
    this.notice = '正在发送指令…';
    this.request('device_command', {
      node_id: device.node, endpoint_id: device.endpoint, cluster_id: cluster, command_name: command, payload: {}
    });
  }

  private pair(): void {
    const code = this.code.trim();
    if (!/^(\d{11}|\d{21}|MT:[A-Z0-9 .\-]{1,180})$/.test(code)) {
      this.notice = '请输入 11 位或 21 位手动配对码，或 MT: 开头的二维码原文。';
      return;
    }
    this.code = '';
    this.busy = true;
    this.notice = '正在通过控制器配对…';
    this.request('commission_with_code', { code: code, network_only: true }, 300000);
  }

  private select(section: string, bridge: string = ''): void {
    this.section = section;
    this.bridge = bridge;
    this.search = '';
  }

  private toggleFavorite(key: string): void {
    this.favorites = this.favorites.includes(key)
      ? this.favorites.filter(item => item !== key)
      : this.favorites.concat(key);
    try {
      this.settings?.putSync(this.favoritesKey, JSON.stringify(this.favorites));
      this.settings?.flushSync();
    } catch {
      this.notice = '无法保存收藏。';
    }
  }

  private matching(): Accessory[] {
    const query = this.search.trim().toLowerCase();
    return this.devices.filter(device =>
      (this.section === '家庭' ||
        (this.section === '收藏' && this.favorites.includes(device.key)) ||
        (this.section === '网桥' && device.bridge === this.bridge) ||
        device.category === this.section) &&
      `${device.name} ${device.bridge}`.toLowerCase().includes(query));
  }

  private bridges(devices: Accessory[]): string[] {
    return Array.from(new Set(devices.map(device => device.bridge)));
  }

  private inOverview(): boolean {
    return this.section === '家庭' && !this.search.trim();
  }

  private groupedDevices(): Accessory[] {
    return this.matching().filter(device => !(this.inOverview() || (this.section === '安防' && !this.search.trim())) || !device.camera);
  }

  private selected(): Accessory | undefined {
    return this.devices.find(device => device.key === this.selectedKey);
  }

  private tileSpan(): number {
    const width = this.pageWidth - (this.pageWidth >= 740 ? 204 : 0) - 36;
    return width >= 920 ? 3 : width >= 600 ? 4 : width >= 340 ? 6 : 12;
  }

  private icon(name: string): Resource {
    switch (name) {
      case '家庭': return $r('sys.symbol.house_fill');
      case '收藏': return $r('sys.symbol.star_fill');
      case '灯光': return $r('sys.symbol.lightbulb_max_fill');
      case '开关': return $r('sys.symbol.power');
      case '窗帘': return $r('sys.symbol.rectangle_grid_2x1');
      case '环境': return $r('sys.symbol.drop_bottomrighthalf_inset_filled');
      case '安防': return $r('sys.symbol.video_fill');
      default: return $r('sys.symbol.square_grid_2x2');
    }
  }

  private categoryStatus(category: string): string {
    const devices = this.devices.filter(item => item.category === category);
    const on = devices.filter(item => item.online && item.on === true).length;
    return on > 0 ? `${on} 个已开启` : `${devices.length} 个配件`;
  }

  private deviceStatus(device: Accessory): string {
    if (!device.online) {
      return '无响应';
    }
    if (device.power && device.on !== null) {
      return device.on ? '已开启' : '已关闭';
    }
    return device.details || (device.power ? '状态未知' : '在线');
  }

  @Builder
  iconButton(icon: Resource, label: string, action: () => void) {
    Button({ type: ButtonType.Circle }) {
      SymbolGlyph(icon).fontSize(20).fontColor([INK])
    }
    .width(44).height(44).backgroundColor('#45434C')
    .accessibilityText(label)
    .onClick(action)
  }

  @Builder
  navItem(title: string, icon: Resource, chosen: boolean, action: () => void) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 11 }) {
        SymbolGlyph(icon).fontSize(18).fontColor([chosen ? GOLD : MUTED])
        Text(title).fontSize(15).fontColor(chosen ? INK : MUTED).maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis }).layoutWeight(1)
      }.width('100%').padding({ left: 12, right: 10 })
    }
    .width('100%').height(43).borderRadius(12)
    .backgroundColor(chosen ? '#44414A' : Color.Transparent)
    .onClick(action)
  }

  @Builder
  sidebar() {
    Column({ space: 16 }) {
      Row({ space: 10 }) {
        SymbolGlyph($r('sys.symbol.house_fill')).fontSize(20).fontColor([GOLD])
        Text('MatterDesk').fontSize(20).fontWeight(FontWeight.Bold).fontColor(INK)
      }.width('100%').padding({ left: 12, top: 4 })
      Scroll() {
        Column({ space: 4 }) {
          this.navItem('家庭', this.icon('家庭'), this.section === '家庭', () => this.select('家庭'))
          this.navItem('收藏', this.icon('收藏'), this.section === '收藏', () => this.select('收藏'))
          if (this.ready) {
            Text('类别').fontSize(12).fontColor(MUTED).padding({ left: 12, top: 14, bottom: 4 })
            ForEach(CATEGORIES.filter(category => this.devices.some(device => device.category === category)),
              (category: string) => {
                this.navItem(category, this.icon(category), this.section === category, () => this.select(category))
              })
            if (this.bridges(this.devices).length > 0) {
              Text('网桥与节点').fontSize(12).fontColor(MUTED).padding({ left: 12, top: 14, bottom: 4 })
              ForEach(this.bridges(this.devices), (name: string) => {
                this.navItem(name, $r('sys.symbol.square_grid_2x2'), this.bridge === name,
                  () => this.select('网桥', name))
              })
            }
          }
        }.alignItems(HorizontalAlign.Start).width('100%')
      }.layoutWeight(1).width('100%').scrollBar(BarState.Off)
      Text(this.status).fontSize(12).fontColor(MUTED).padding({ left: 12, bottom: 6 }).maxLines(1)
    }
    .width(204).height('100%').padding(14).alignItems(HorizontalAlign.Start)
    .backgroundColor('#252731')
  }

  @Builder
  categoryBar() {
    Scroll() {
      Row({ space: 8 }) {
        if (this.pageWidth < 740) {
          this.categoryChip('家庭')
          this.categoryChip('收藏')
        }
        ForEach(CATEGORIES.filter(category => this.devices.some(device => device.category === category)),
          (category: string) => {
            this.categoryChip(category)
          })
      }.padding({ bottom: 2 })
    }.scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).width('100%').align(Alignment.Start)
  }

  @Builder
  categoryChip(category: string) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 9 }) {
        SymbolGlyph(this.icon(category)).fontSize(19).fontColor([this.section === category ? '#382515' : GOLD])
        Column({ space: 2 }) {
          Text(category).fontSize(14).fontWeight(FontWeight.Medium)
            .fontColor(this.section === category ? '#291C12' : INK)
          if (category !== '家庭' && category !== '收藏') {
            Text(this.categoryStatus(category)).fontSize(11)
              .fontColor(this.section === category ? '#50321E' : MUTED)
          }
        }.alignItems(HorizontalAlign.Start)
      }.padding({ left: 13, right: 15 })
    }
    .height(52).borderRadius(16).backgroundColor(this.section === category ? GOLD : '#42424D')
    .onClick(() => this.select(category))
  }

  @Builder
  tile(device: Accessory) {
    Row({ space: 10 }) {
      Button({ type: ButtonType.Circle }) {
        SymbolGlyph(this.icon(device.category)).fontSize(22)
          .fontColor([device.online && device.on === true ? '#4B2D05' : GOLD])
      }
      .width(44).height(44)
      .backgroundColor(device.online && device.on === true ? GOLD : '#4C4650')
      .enabled(!device.power || (device.online && !this.busy && device.on !== null))
      .accessibilityText(device.power ? `${device.on ? '关闭' : '开启'} ${device.name}` : `查看 ${device.name}`)
      .onClick(() => {
        if (device.power && device.on !== null) {
          this.control(device, 6, device.on ? 'Off' : 'On');
        } else {
          this.selectedKey = device.key;
          this.sheet = '配件';
        }
      })
      Button({ type: ButtonType.Normal }) {
        Column({ space: 4 }) {
          Text(device.name).fontSize(14).fontWeight(FontWeight.Bold).maxLines(2)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .fontColor(device.online && device.on === true ? '#292330' : INK)
          Text(this.deviceStatus(device)).fontSize(12).maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .fontColor(device.online && device.on === true ? '#674A34' : MUTED)
        }.alignItems(HorizontalAlign.Start).width('100%')
      }
      .height('100%').layoutWeight(1).padding(0).backgroundColor(Color.Transparent)
      .accessibilityText(device.name + '，' + this.deviceStatus(device))
      .onClick(() => {
        this.selectedKey = device.key;
        this.sheet = '配件';
      })
    }
    .padding(12).width('100%').height(82).borderRadius(17)
    .backgroundColor(device.online && device.on === true ? '#F9E7CE' : '#454552')
  }

  @Builder
  accessoryGroup(title: string, devices: Accessory[]) {
    Column({ space: 11 }) {
      Text(title).fontSize(21).fontWeight(FontWeight.Bold).fontColor(INK)
        .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
      GridRow({ columns: 12, gutter: { x: 8, y: 8 } }) {
        ForEach(devices, (device: Accessory) => {
          GridCol({ span: this.tileSpan() }) {
            this.tile(device)
          }
        }, (device: Accessory) => JSON.stringify(device))
      }.width('100%')
    }.alignItems(HorizontalAlign.Start).width('100%')
  }

  @Builder
  homeContent() {
    if (!this.ready) {
      Column({ space: 12 }) {
        SymbolGlyph($r('sys.symbol.house')).fontSize(42).fontColor([GOLD])
        Text(this.status).fontColor(INK).fontSize(22).fontWeight(FontWeight.Bold)
        Button('连接控制器').fontColor('#2A2118').backgroundColor(GOLD)
          .onClick(() => this.sheet = '连接设置')
        Button('模拟摄像头').fontColor(INK).backgroundColor('#45434C')
          .onClick(() => this.openCamera())
      }.width('100%').padding({ top: 56, bottom: 56 })
    } else if (this.matching().length === 0 && !this.inOverview() && this.section !== '安防') {
      Text(this.search ? '没有匹配的配件。' : this.section === '收藏' ? '在配件详情中添加收藏。' : '没有配件。')
        .fontColor(MUTED).padding({ top: 28 })
    } else {
      Scroll() {
        Column({ space: 22 }) {
          if (this.inOverview() && this.matching().some(device => this.favorites.includes(device.key))) {
            this.accessoryGroup('收藏', this.matching().filter(device => this.favorites.includes(device.key)))
          }
          if (this.inOverview() || (this.section === '安防' && !this.search.trim())) {
            this.cameraGroup()
          }
          ForEach(this.bridges(this.groupedDevices()), (name: string) => {
            this.accessoryGroup(name, this.groupedDevices().filter(device => device.bridge === name))
          }, (name: string) => name + JSON.stringify(this.groupedDevices().filter(device => device.bridge === name)))
        }.width('100%').alignItems(HorizontalAlign.Start).padding({ top: 4, bottom: 24 })
      }.layoutWeight(1).width('100%').scrollBar(BarState.Auto).align(Alignment.TopStart)
    }
  }

  @Builder
  cameraGroup() {
    Column({ space: 11 }) {
      Text('摄像头').fontSize(21).fontWeight(FontWeight.Bold).fontColor(INK)
      GridRow({ columns: 12, gutter: { x: 8, y: 8 } }) {
        ForEach(this.matching().filter(device => device.camera), (device: Accessory) => {
          GridCol({ span: this.tileSpan() }) {
            this.tile(device)
          }
        }, (device: Accessory) => JSON.stringify(device))
        GridCol({ span: this.tileSpan() }) {
          Button({ type: ButtonType.Normal }) {
            Row({ space: 10 }) {
              SymbolGlyph($r('sys.symbol.video_fill')).fontSize(23).fontColor([GOLD])
              Column({ space: 4 }) {
                Text('模拟摄像头').fontSize(14).fontWeight(FontWeight.Bold).fontColor(INK)
                Text('播放测试画面').fontSize(12).fontColor(MUTED)
              }.alignItems(HorizontalAlign.Start)
            }.width('100%')
          }.height(82).width('100%').padding(14).borderRadius(17).backgroundColor('#454552')
            .onClick(() => this.openCamera())
        }
      }.width('100%')
    }.alignItems(HorizontalAlign.Start).width('100%')
  }

  @Builder
  cameraScreen() {
    Column() {
      Row({ space: 12 }) {
        this.iconButton($r('sys.symbol.xmark'), '退出播放', () => this.stopCamera())
        Column({ space: 4 }) {
          Text(this.cameraTitle).fontSize(22).fontWeight(FontWeight.Bold).fontColor(INK).maxLines(1)
          Text(this.cameraStopping ? '正在关闭播放器…' : this.cameraDetail).fontSize(13).fontColor(MUTED)
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        if (this.cameraAudio && this.cameraState === 'streaming' && !this.cameraStopping) {
          Button(this.cameraSound ? '静音' : '收听').backgroundColor('#45434C').onClick(() => {
            this.cameraSound = !this.cameraSound;
            this.cameraScript(`window.harmonyCamera.sound(${this.cameraSound})`);
          })
        }
        if ((this.cameraState === 'idle' || this.cameraState === 'error') && !this.cameraStopping
          && !this.cameraConfig?.mock) {
          Button('重新播放').backgroundColor('#45434C')
            .onClick(() => this.cameraScript('window.harmonyCamera.restart()'))
        }
      }.width('100%').padding(14)
      Web({ src: $rawfile('camera/index.html'), controller: this.cameraController })
        .width('100%').layoutWeight(1)
        .javaScriptAccess(true)
        .mediaPlayGestureAccess(false)
        .javaScriptProxy({
          object: this.cameraHost,
          name: 'CameraHost',
          methodList: ['send', 'report', 'finished'],
          controller: this.cameraController
        })
        .onPageEnd(() => {
          if (!this.cameraLoaded && this.cameraConfig && !this.cameraStopping) {
            this.cameraLoaded = true;
            this.cameraScript(`window.harmonyCamera.open(${JSON.stringify(this.cameraConfig)}); void 0`);
          }
        })
    }.width('100%').height('100%').backgroundColor('#10141C')
  }

  @Builder
  deviceActions(device: Accessory) {
    SymbolGlyph(this.icon(device.category)).fontSize(44).fontColor([GOLD])
    Text(this.deviceStatus(device)).fontSize(16).fontColor(MUTED)
    if (device.bridge !== device.name) {
      Text(device.bridge).fontSize(13).fontColor(MUTED)
    }
    if (device.camera) {
      Button('观看直播').backgroundColor(GOLD).fontColor('#282016').enabled(device.online)
        .onClick(() => this.openCamera(device))
    }
    if (device.power) {
      Row({ space: 10 }) {
        Button('开启').fontColor('#282016').backgroundColor(GOLD).enabled(device.online && !this.busy)
          .onClick(() => this.control(device, 6, 'On'))
        Button('关闭').fontColor(INK).backgroundColor('#484855').enabled(device.online && !this.busy)
          .onClick(() => this.control(device, 6, 'Off'))
      }
    }
    if (device.shade) {
      Row({ space: 8 }) {
        Button('打开').backgroundColor('#484855').enabled(device.online && !this.busy)
          .onClick(() => this.control(device, 258, 'UpOrOpen'))
        Button('停止').backgroundColor('#484855').enabled(device.online && !this.busy)
          .onClick(() => this.control(device, 258, 'StopMotion'))
        Button('合上').backgroundColor('#484855').enabled(device.online && !this.busy)
          .onClick(() => this.control(device, 258, 'DownOrClose'))
      }
    }
    Button(this.favorites.includes(device.key) ? '取消收藏' : '添加到收藏')
      .fontColor(INK).backgroundColor('#484855').onClick(() => this.toggleFavorite(device.key))
  }

  @Builder
  panel() {
    Column({ space: 16 }) {
      Row({ space: 10 }) {
        Text(this.sheet === '配件' ? this.selected()?.name ?? '配件' : this.sheet)
          .fontSize(22).fontWeight(FontWeight.Bold).fontColor(INK).layoutWeight(1)
        this.iconButton($r('sys.symbol.xmark'), '关闭面板', () => {
          this.sheet = '';
          this.code = '';
        })
      }.width('100%')
      if (this.sheet === '配件' && this.selected()) {
        this.deviceActions(this.selected()!)
      } else if (this.sheet === '连接设置') {
        Text(this.status).fontSize(15).fontColor(MUTED).width('100%')
        TextInput({ text: this.address, placeholder: 'Matter Server 地址' }).onChange(value => this.address = value)
        Button('连接').backgroundColor(GOLD).fontColor('#282016').onClick(() => this.connect())
      } else if (this.sheet === '添加配件') {
        TextInput({ text: this.code, placeholder: 'Matter 配对码' }).type(InputType.Password)
          .onChange(value => this.code = value)
        Button('添加桥接器').backgroundColor(GOLD).fontColor('#282016')
          .enabled(this.ready && !this.busy && this.code.trim().length > 0).onClick(() => this.pair())
      }
      if (this.notice) {
        Text(this.notice).fontSize(13).fontColor(MUTED)
      }
    }
    .padding(22).width('92%').constraintSize({ maxWidth: 448 })
    .borderRadius(24).backgroundColor(PANEL)
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      Row() {
        if (this.pageWidth >= 740) {
          this.sidebar()
        }
        Column({ space: 14 }) {
          Row({ space: 8 }) {
            Column({ space: 3 }) {
              Text(this.section === '网桥' ? this.bridge : this.section === '家庭' ? '我的家' : this.section)
                .fontSize(31).fontWeight(FontWeight.Bold).fontColor(INK).maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
              Text(this.ready ? `${this.devices.filter(device => device.online).length} 个在线 · ${this.devices.length} 个配件` : this.status)
                .fontSize(12).fontColor(MUTED)
            }.alignItems(HorizontalAlign.Start).layoutWeight(1)
            if (this.ready) {
              this.iconButton($r('sys.symbol.magnifyingglass'), '搜索配件', () => {
                this.searchOpen = !this.searchOpen;
                if (!this.searchOpen) {
                  this.search = '';
                }
              })
              if (this.pageWidth >= 460) {
                this.iconButton($r('sys.symbol.arrow_clockwise'), '刷新配件', () => this.request('get_nodes'))
              }
              this.iconButton($r('sys.symbol.plus'), '添加配件', () => {
                this.notice = '';
                this.sheet = '添加配件';
              })
            }
            this.iconButton($r('sys.symbol.gearshape'), '连接设置', () => {
              this.notice = '';
              this.sheet = '连接设置';
            })
          }.width('100%')
          if (this.searchOpen && this.ready) {
            TextInput({ text: this.search, placeholder: '搜索配件' }).width('100%')
              .onChange(value => this.search = value)
          }
          if (this.ready) {
            this.categoryBar()
          }
          if (this.notice && !this.sheet) {
            Text(this.notice).fontSize(13).fontColor(MUTED)
          }
          this.homeContent()
        }
        .alignItems(HorizontalAlign.Start)
        .height('100%').layoutWeight(1)
        .padding({ top: 20, left: 18, right: 18, bottom: 4 })
        .linearGradient({
          direction: GradientDirection.RightBottom,
          colors: [['#543B3D', 0], ['#343443', 0.48], ['#212633', 1]]
        })
      }.width('100%').height('100%').alignItems(VerticalAlign.Top)
      if (this.sheet) {
        Stack({ alignContent: Alignment.Center }) {
          Column().width('100%').height('100%').backgroundColor('#A6000000').onClick(() => {
            this.sheet = '';
            this.code = '';
          })
          this.panel()
        }.width('100%').height('100%')
      }
      if (this.cameraTitle) {
        this.cameraScreen()
      }
    }
    .width('100%').height('100%').backgroundColor(PANEL)
    .onAreaChange((_, area) => {
      this.pageWidth = Number(area.width);
    })
  }
}
