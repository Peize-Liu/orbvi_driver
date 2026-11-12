const {contextBridge, ipcRenderer} = require('electron');
contextBridge.exposeInMainWorld('orbviDesktop', {
  exportStatus: json => ipcRenderer.invoke('export-status', json)
});
