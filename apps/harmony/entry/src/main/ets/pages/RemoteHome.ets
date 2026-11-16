import { preferences } from '@kit.ArkData';
import { webview } from '@kit.ArkWeb';
import { url } from '@kit.ArkTS';

const DEFAULT_ORIGIN = 'https://matter.linanw.me';
const AUTH_REALM = 'MatterDesk';

function controllerOrigin(value: string): string {
  try {
    const input = value.trim();
    // Only a site origin: the Web UI serves at / and uses same-origin /ws.
    if (!/^https:\/\/[^/?#@\\\s]+\/?$/i.test(input)) return '';
    const parsed = url.URL.parseURL(input);
    return parsed.protocol === 'https:' && parsed.hostname.length > 0 && !parsed.username && !parsed.password
      ? parsed.origin : '';
  } catch {
    return '';
  }
}

@Entry
@Component
struct RemoteHome {
  @State origin: string = '';
  @State address: string = '';
  @State settingsOpen: boolean = false;
  @State authOpen: boolean = false;
  @State username: string = '';
  @State password: string = '';
  @State status: string = '';
  @State nativeOpen: boolean = false;

  private controller: webview.WebviewController = new webview.WebviewController();
  private controllerReady: boolean = false;
  private initialOrigin: string = DEFAULT_ORIGIN;
  private settings?: preferences.Preferences;
  private authHandler?: HttpAuthHandler;
  private authOrigin: string = '';
  private authAttempted: boolean = false;

  aboutToAppear(): void {
    const context = this.getUIContext().getHostContext();
    try {
      if (context) this.settings = preferences.getPreferencesSync(context, { name: 'matterdesk-remote' });
      const saved = this.settings?.getSync('origin', DEFAULT_ORIGIN) ?? DEFAULT_ORIGIN;
      this.origin = (typeof saved === 'string' ? controllerOrigin(saved) : '') || DEFAULT_ORIGIN;
    } catch {
      this.origin = DEFAULT_ORIGIN;
      this.status = '无法读取已保存的网址。';
    }
    this.address = this.origin;
    this.initialOrigin = this.origin;
  }

  onPageShow(): void {
    if (this.nativeOpen) {
      this.controller = new webview.WebviewController();
      this.initialOrigin = this.origin;
      this.authAttempted = false;
      this.nativeOpen = false;
    }
  }

  onPageHide(): void {
    this.clearAuth();
  }

  onBackPress(): boolean {
    if (this.authOpen) {
      this.cancelLogin();
      return true;
    }
    if (this.settingsOpen) {
      this.settingsOpen = false;
      return true;
    }
    try {
      if (this.controllerReady && this.controller.accessBackward()) {
        this.controller.backward();
        return true;
      }
    } catch { /* The controller may already be detaching. */ }
    return false;
  }

  private openSettings(): void {
    this.clearAuth();
    this.address = this.origin;
    this.settingsOpen = true;
  }

  private saveAddress(): void {
    const next = controllerOrigin(this.address);
    if (!next) {
      this.status = '请输入 HTTPS 站点地址，不要包含子路径、参数、用户名或密码。';
      return;
    }
    this.clearAuth();
    this.origin = next;
    this.address = next;
    this.settingsOpen = false;
    this.navigate();
    try {
      this.settings?.putSync('origin', next);
      this.settings?.flush().catch(() => this.status = '无法保存远端网址，本次仍可连接。');
    } catch {
      this.status = '无法保存远端网址，本次仍可连接。';
    }
  }

  private navigate(): void {
    this.clearAuth();
    this.authAttempted = false;
    this.status = '';
    if (!this.controllerReady) return;
    try {
      this.controller.loadUrl(this.origin + '/');
    } catch {
      this.status = '暂时无法打开远端界面，请重试。';
    }
  }

  private showNative(): void {
    this.clearAuth();
    this.settingsOpen = false;
    this.controllerReady = false;
    this.nativeOpen = true;
    this.getUIContext().getRouter().pushUrl({ url: 'pages/Index' }).catch(() => {
      this.controller = new webview.WebviewController();
      this.initialOrigin = this.origin;
      this.nativeOpen = false;
      this.status = '无法打开直连模式。';
    });
  }

  private sameOrigin(target: string): boolean {
    try {
      const parsed = url.URL.parseURL(target);
      return parsed.protocol === 'https:' && parsed.origin === this.origin && !parsed.username && !parsed.password;
    } catch {
      return false;
    }
  }

  private interceptNavigation(target: string): boolean {
    if (this.sameOrigin(target)) return false;
    this.status = '远端界面只能在当前站点内打开。请在连接设置中更改服务器。';
    return true;
  }

  private interceptResource(target: string): WebResourceResponse {
    if (this.sameOrigin(target) || /^data:image\//i.test(target) || target.startsWith('blob:' + this.origin + '/')) {
      // ArkWeb documents null as "continue loading" but its SDK type omits it.
      return null!;
    }
    const blocked = new WebResourceResponse();
    blocked.setResponseCode(403);
    blocked.setReasonMessage('Forbidden');
    blocked.setResponseMimeType('text/plain');
    blocked.setResponseData('Cross-origin resource blocked');
    return blocked;
  }

  private receiveAuth(event: OnHttpAuthRequestEvent): boolean {
    if (this.settingsOpen || this.nativeOpen) {
      event.handler.cancel();
    } else if (!this.matchesAuthTarget(event.host, event.realm)) {
      event.handler.cancel();
      this.status = '已拒绝不匹配的登录请求。确认网址和服务器认证名称 MatterDesk。';
    } else if (this.authAttempted || this.authHandler) {
      event.handler.cancel();
      if (this.authAttempted) this.status = '验证失败。检查账号密码后点「重试」。';
    } else {
      this.settingsOpen = false;
      this.authHandler = event.handler;
      this.authOrigin = this.origin;
      this.authOpen = true;
    }
    return true;
  }

  private matchesAuthTarget(host: string, realm: string): boolean {
    try {
      const configured = url.URL.parseURL(this.origin);
      const challengeHost = host.toLowerCase();
      return realm === AUTH_REALM && (challengeHost === configured.host.toLowerCase()
        || (!configured.port && challengeHost === configured.hostname.toLowerCase() + ':443'));
    } catch {
      return false;
    }
  }

  private confirmLogin(): void {
    const handler = this.authHandler;
    if (!handler || this.authOrigin !== this.origin || !this.username || !this.password) return;
    this.authHandler = undefined;
    this.authAttempted = true;
    try {
      if (!handler.confirm(this.username, this.password)) this.status = '无法发送登录信息，请重试。';
    } catch {
      this.status = '无法发送登录信息，请重试。';
    } finally {
      this.clearAuth();
    }
  }

  private cancelLogin(): void {
    this.clearAuth();
    this.status = '已取消登录，点「重试」重新连接。';
  }

  private clearAuth(): void {
    const handler = this.authHandler;
    this.authHandler = undefined;
    this.authOrigin = '';
    this.username = '';
    this.password = '';
    this.authOpen = false;
    try { handler?.cancel(); } catch { /* The WebView may have closed the request. */ }
  }

  @Builder
  remoteWeb() {
    Web({ src: this.initialOrigin + '/', controller: this.controller })
      .width('100%').layoutWeight(1)
      .javaScriptAccess(true).domStorageAccess(true)
      .fileAccess(false).multiWindowAccess(false).mixedMode(MixedMode.None)
      .mediaPlayGestureAccess(true)
      .onControllerAttached(() => {
        this.controllerReady = true;
        if (this.initialOrigin !== this.origin) this.navigate();
      })
      .onLoadIntercept(event => this.interceptNavigation(event.data.getRequestUrl()))
      .onOverrideUrlLoading(request => this.interceptNavigation(request.getRequestUrl()))
      .onInterceptRequest(event => this.interceptResource(event.request.getRequestUrl()))
      .onHttpAuthRequest(event => this.receiveAuth(event))
      .onSslErrorEventReceive(event => {
        event.handler.handleCancel();
        this.clearAuth();
        this.status = '服务器 HTTPS 证书验证失败。请检查站点证书和地址。';
      })
      .onErrorReceive(event => {
        if (event.request.isMainFrame() && !this.authOpen && !this.status) {
          this.status = '无法加载远端界面，请检查网络、网址及 HTTPS 证书。';
        }
      })
      .onHttpErrorReceive(event => {
        if (event.request.isMainFrame() && !this.authOpen && !this.status) {
          this.status = `服务器返回 HTTP ${event.response.getResponseCode()}。`;
        }
      })
  }

  @Builder
  settingsPanel() {
    Column({ space: 12 }) {
      Text('连接设置').fontSize(22).fontWeight(FontWeight.Bold)
      Text('填写 MatterDesk Web 的 HTTPS 站点；认证在连接时单独输入。').fontSize(14)
      TextInput({ text: this.address, placeholder: 'https://matter.linanw.me' })
        .type(InputType.URL).enableAutoFill(false).onChange(value => this.address = value)
      Row({ space: 8 }) {
        Button('保存并连接').onClick(() => this.saveAddress())
        Button('取消').onClick(() => this.settingsOpen = false)
      }
      Button('使用 Matter Server 直连').onClick(() => this.showNative())
    }.padding(16).width('100%').constraintSize({ maxWidth: 520 })
  }

  @Builder
  loginPanel() {
    Column({ space: 12 }) {
      Text('登录 MatterDesk').fontSize(22).fontWeight(FontWeight.Bold)
      Text(this.authOrigin).fontSize(14)
      TextInput({ text: this.username, placeholder: '用户名' })
        .enableAutoFill(false).onChange(value => this.username = value)
      TextInput({ text: this.password, placeholder: '密码' })
        .type(InputType.Password).enableAutoFill(false).onChange(value => this.password = value)
      Row({ space: 8 }) {
        Button('登录').enabled(!!this.username && !!this.password).onClick(() => this.confirmLogin())
        Button('取消').onClick(() => this.cancelLogin())
      }
    }.padding(16).width('100%').constraintSize({ maxWidth: 520 })
  }

  build() {
    Column() {
      Row({ space: 8 }) {
        Column({ space: 2 }) {
          Text('MatterDesk').fontWeight(FontWeight.Bold).fontSize(18)
          Text(this.origin).fontSize(11).maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        Button('重试').enabled(!this.authOpen).onClick(() => this.navigate())
        Button('连接设置').onClick(() => this.openSettings())
      }.width('100%').padding({ left: 12, right: 12, top: 8, bottom: 8 })
      if (this.status) Text(this.status).fontSize(13).padding({ left: 12, right: 12, top: 5, bottom: 5 })
      if (this.settingsOpen) this.settingsPanel();
      if (this.authOpen) this.loginPanel();
      if (this.origin && !this.nativeOpen) this.remoteWeb();
    }.width('100%').height('100%')
  }
}
