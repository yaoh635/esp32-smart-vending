const app = getApp()

Page({
  data: {
    isBound: false,
    machineId: '',
    serverUrl: ''
  },

  onLoad() {
    this.setData({
      isBound: app.globalData.isBound,
      machineId: app.globalData.machineId,
      serverUrl: app.globalData.serverUrl
    })

    // 如果还没绑定，自动弹出扫码
    if (!app.globalData.isBound) {
      setTimeout(() => this.onScanBind(), 500)
    }
  },

  onScanBind() {
    wx.scanCode({
      onlyFromCamera: true,
      scanType: ['qrCode'],
      success: (res) => {
        const result = res.result
        let machineId = result
        let serverUrl = ''

        // 支持格式: vending:<machine_id>:<ip>
        if (result.startsWith('vending:')) {
          const parts = result.split(':')
          machineId = parts[1] || result
          if (parts.length >= 3) {
            serverUrl = 'http://' + parts[2]
          }
        }

        app.bindMachine(machineId)
        if (serverUrl) app.setServerUrl(serverUrl)

        this.setData({
          isBound: true,
          machineId: machineId,
          serverUrl: serverUrl
        })

        wx.showToast({ title: '绑定成功', icon: 'success' })

        // 延迟跳回首页
        setTimeout(() => {
          wx.switchTab({ url: '/pages/index/index' })
        }, 1500)
      },
      fail: (err) => {
        if (err.errMsg.indexOf('cancel') === -1) {
          wx.showToast({ title: '扫码失败，请重试', icon: 'none' })
        }
      }
    })
  },

  onManualInput() {
    this.setData({ serverUrl: app.globalData.serverUrl || 'http://192.168.1.100' })
  },

  onServerInput(e) {
    this.setData({ serverUrl: e.detail.value })
  },

  onSaveServer() {
    const url = this.data.serverUrl.trim()
    if (!url) return
    if (!url.startsWith('http://')) {
      wx.showToast({ title: '请输入 http:// 开头', icon: 'none' })
      return
    }
    app.setServerUrl(url)
    wx.showToast({ title: '已保存', icon: 'success' })
  },

  returnHome() {
    wx.switchTab({ url: '/pages/index/index' })
  }
})
