const API = require('./utils/api')

App({
  globalData: {
    serverUrl: 'http://192.168.101.35',  // ESP32 IP 地址
    refreshInterval: 5000,  // 默认 5 秒刷新

    /* ── 用户身份 ── */
    openid: '',             // 微信 openid（wx.login 获取）
    userId: 0,              // 售货机分配的用户 ID
    userName: '',           // 昵称

    /* ── 角色 ── */
    isAdmin: false,         // 是否为管理员
    adminToken: '',         // 管理员登录凭证

    /* ── 绑定 ── */
    machineId: '',          // 当前绑定的售货机 ID
    isBound: true           // 默认已绑定（调试阶段跳过扫码绑定）
  },

  onLaunch() {
    // 从本地存储恢复状态
    const savedUrl = wx.getStorageSync('server_url')
    if (savedUrl) {
      this.globalData.serverUrl = savedUrl
    }

    const savedRole = wx.getStorageSync('is_admin')
    if (savedRole) {
      this.globalData.isAdmin = true
    }

    const savedMachine = wx.getStorageSync('machine_id')
    if (savedMachine) {
      this.globalData.machineId = savedMachine
      this.globalData.isBound = true
    }

    // 微信登录（获取 openid）
    this.wxLogin()
  },

  /* ── 微信登录 ── */
  wxLogin() {
    wx.login({
      success: (res) => {
        if (res.code) {
          // 实际项目需要后端用 code 换取 openid
          // 这里用 code 前8位作为临时 openid
          this.globalData.openid = 'wx_' + res.code.substring(0, 8)
          console.log('WeChat login OK, openid:', this.globalData.openid)
        }
      },
      fail: (err) => {
        console.warn('WeChat login failed:', err)
      }
    })
  },

  /* ── 判断当前是否为管理员 ── */
  checkIsAdmin() {
    return this.globalData.isAdmin
  },

  /* ── 管理员登录 ── */
  adminLogin(password) {
    return API.adminLogin(password).then(res => {
      if (res.success) {
        this.globalData.isAdmin = true
        wx.setStorageSync('is_admin', true)
      }
      return res
    })
  },

  /* ── 退出管理员 ── */
  adminLogout() {
    this.globalData.isAdmin = false
    this.globalData.adminToken = ''
    wx.removeStorageSync('is_admin')
  },

  /* ── 保存服务器地址 ── */
  setServerUrl(url) {
    this.globalData.serverUrl = url
    wx.setStorageSync('server_url', url)
  },

  /* ── 绑定售货机 ── */
  bindMachine(machineId) {
    this.globalData.machineId = machineId
    this.globalData.isBound = true
    wx.setStorageSync('machine_id', machineId)
  }
})
