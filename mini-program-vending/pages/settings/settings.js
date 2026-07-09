const app = getApp()

Page({
  data: {
    serverUrl: '',
    connected: false,
    systemInfo: null,
    testing: false
  },

  onShow() {
    this.setData({ serverUrl: app.globalData.serverUrl || '' })
  },

  onUrlInput(e) {
    this.setData({ serverUrl: e.detail.value })
  },

  saveUrl() {
    const url = this.data.serverUrl.trim()
    if (!url) {
      wx.showToast({ title: '请输入地址', icon: 'none' })
      return
    }
    app.globalData.serverUrl = url
    wx.setStorageSync('server_url', url)
    wx.showToast({ title: '已保存', icon: 'success' })
  },

  async testConnection() {
    const url = this.data.serverUrl.trim()
    if (!url) {
      wx.showToast({ title: '请先输入服务器地址', icon: 'none' })
      return
    }

    this.setData({ testing: true })
    try {
      const res = await new Promise((resolve, reject) => {
        wx.request({
          url: url + '/api/system',
          method: 'GET',
          timeout: 5000,
          success: resolve,
          fail: reject
        })
      })

      if (res.statusCode === 200) {
        const info = res.data
        info.heapKB = (info.free_heap / 1024).toFixed(1)
        info.uptimeStr = this.fmtUptime(info.uptime_s || 0)
        this.setData({ connected: true, systemInfo: info })
        wx.showToast({ title: '连接成功!', icon: 'success' })
      } else {
        this.setData({ connected: false })
        wx.showToast({ title: '连接失败 HTTP ' + res.statusCode, icon: 'none' })
      }
    } catch (e) {
      this.setData({ connected: false })
      wx.showToast({ title: '无法连接', icon: 'none' })
    }
    this.setData({ testing: false })
  },

  fmtUptime(s) {
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60)
    return h > 0 ? h + 'h ' + m + 'm' : m + 'm ' + (s % 60) + 's'
  }
})
