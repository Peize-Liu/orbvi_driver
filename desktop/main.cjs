const {app, BrowserWindow, Menu, protocol, net, ipcMain, dialog, session} = require('electron');
const fs = require('node:fs/promises');
const path = require('node:path');

protocol.registerSchemesAsPrivileged([
  {scheme: 'orbvi', privileges: {standard: true, secure: true, supportFetchAPI: true, stream: true, corsEnabled: true}}
]);
const backend = new URL(process.env.ORBVI_DASHBOARD_URL || 'http://127.0.0.1:18765');
if (backend.protocol !== 'http:' || !['127.0.0.1', 'localhost', '[::1]'].includes(backend.hostname) ||
    backend.username || backend.password || backend.pathname !== '/' || backend.search || backend.hash) {
  throw new Error('ORBVI_DASHBOARD_URL must be a loopback HTTP origin');
}
let window;
const streams = new Map();
function closeStreams() {
  for (const controller of streams.values()) controller.abort();
  streams.clear();
}
function createWindow() {
  window = new BrowserWindow({
    width: 1460, height: 1040, minWidth: 880, minHeight: 680,
    title: 'ORBVI Viewer', backgroundColor: '#0c1118',
    webPreferences: {preload: path.join(__dirname, 'preload.cjs'), contextIsolation: true,
      nodeIntegration: false, sandbox: true, backgroundThrottling: false}
  });
  window.webContents.setWindowOpenHandler(() => ({action: 'deny'}));
  window.webContents.on('will-navigate', (event, url) => {
    if (url !== 'orbvi://app/') event.preventDefault();
  });
  window.webContents.on('did-start-navigation', (_event, _url, inPlace, mainFrame) => {
    if (mainFrame && !inPlace) closeStreams();
  });
  window.loadURL('orbvi://app/');
  window.on('closed', () => { closeStreams(); window = null; });
}

app.whenReady().then(() => {
  session.defaultSession.setPermissionRequestHandler((_contents, _permission, callback) => callback(false));
  protocol.handle('orbvi', async request => {
    const url = new URL(request.url);
    if (url.host !== 'app' || request.method !== 'GET') return new Response('', {status: 403});
    const route = url.pathname;
    if (route === '/api/status' || /^\/video\/[0-3]\.mp4$/.test(route)) {
      try {
        if (route.startsWith('/video/')) {
          // Explicitly cancel upstream streams on reload/reconnect. A custom
          // protocol request's signal alone does not reliably follow renderer teardown.
          streams.get(route)?.abort();
          const controller = new AbortController();
          streams.set(route, controller);
          return await net.fetch(new URL(route, backend).href, {
            signal: AbortSignal.any([request.signal, controller.signal]), redirect: 'error'
          });
        }
        const response = await net.fetch(new URL(route, backend).href, {
          signal: AbortSignal.any([request.signal, AbortSignal.timeout(2500)]), redirect: 'error'
        });
        return new Response(await response.arrayBuffer(), {status: response.status,
          headers: {'Content-Type': 'application/json', 'Cache-Control': 'no-store'}});
      } catch {
        return new Response('Local capture service unavailable', {status: 503});
      }
    }
    const assets = {'/': ['dashboard.html', 'text/html; charset=utf-8'],
      '/dashboard.css': ['dashboard.css', 'text/css'], '/dashboard.js': ['dashboard.js', 'text/javascript']};
    if (!assets[route]) return new Response('', {status: 404});
    const [file, type] = assets[route];
    return new Response(await fs.readFile(path.join(__dirname, 'ui', file)), {headers: {
      'Content-Type': type, 'Cache-Control': 'no-store',
      'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; media-src 'self' blob:; connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-src 'none'"
    }});
  });
  ipcMain.handle('export-status', async (event, json) => {
    if (event.sender !== window?.webContents || event.senderFrame !== window.webContents.mainFrame ||
        typeof json !== 'string' || Buffer.byteLength(json) > 2 * 1024 * 1024) throw new Error('Invalid export');
    const data = JSON.stringify(JSON.parse(json), null, 2);
    const result = await dialog.showSaveDialog(window, {
      title: '导出采集状态', defaultPath: `orbvi-status-${Date.now()}.json`,
      filters: [{name: 'JSON', extensions: ['json']}]
    });
    if (result.canceled || !result.filePath) return false;
    await fs.writeFile(result.filePath, data, {encoding: 'utf8', mode: 0o600});
    return true;
  });
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    {label: 'ORBVI Viewer', submenu: [{role: 'about'}, {type: 'separator'}, {role: 'quit'}]},
    {label: '编辑', submenu: [{role: 'copy'}, {role: 'selectAll'}]},
    {label: '视图', submenu: [{label: '重新连接', accelerator: 'CmdOrCtrl+R', click: () => window?.reload()},
      {role: 'togglefullscreen'}, {role: 'resetZoom'}, {role: 'zoomIn'}, {role: 'zoomOut'}]}
  ]));
  createWindow();
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});
app.on('window-all-closed', () => app.quit());
