const API = require('../../utils/api')
const app = getApp()

Page({
  data: {
    isAdmin: false,
    serverUrl: '',
    isBound: false,
    machineId: '',
    showLoginModal: false,
    loginPassword: ''
  },

  onShow() {
    this.setData({
      isAdmin: app.checkIsAdmin(),
      serverUrl: app.globalData.serverUrl,
      isBound: app.globalData.isBound,
      machineId: app.globalData.machineId
    })
  },

  /* ── 扫码绑定售货机 ── */
  onScanBind() {
    wx.scanCode({
      success: (res) => {
        // 从二维码解析机器 ID（格式：vending:<machine_id>:<ip>）
        const result = res.result
        let machineId = result
        let serverUrl = ''

        if (result.startsWith('vending:')) {
          const parts = result.split(':')
          machineId = parts[1] || result
          serverUrl = parts[2] ? 'http://' + parts[2] : ''
        }

        app.bindMachine(machineId)
        if (serverUrl) app.setServerUrl(serverUrl)

        this.setData({
          isBound: true,
          machineId: machineId,
          serverUrl: serverUrl
        })

        wx.showToast({ title: '绑定成功', icon: 'success' })
      },
      fail: () => {
        wx.showToast({ title: '扫码失败', icon: 'none' })
      }
    })
  },

  /* ── 服务器地址 ── */
  onServerInput(e) {
    this.setData({ serverUrl: e.detail.value })
  },

  onSaveServer() {
    const url = this.data.serverUrl.trim()
    if (!url.startsWith('http://')) {
      wx.showToast({ title: '请输入 http:// 开头的地址', icon: 'none' })
      return
    }
    app.setServerUrl(url)
    wx.showToast({ title: '已保存', icon: 'success' })
  },

  /* ── 人脸注册 ── */
  onFaceRegister() {
    wx.showModal({
      title: '注册人脸',
      content: '请站在售货机摄像头前，然后点击确定',
      success: (res) => {
        if (res.confirm) {
          const userId = app.globalData.userId || 1
          API.faceRegister(userId, 'device').then(() => {
            wx.showToast({ title: '注册请求已发送，请看向摄像头', icon: 'none' })
          }).catch(() => {
            wx.showToast({ title: '请求失败', icon: 'none' })
          })
        }
      }
    })
  },

  /* ── 管理员登录 ── */
  onAdminLogin() {
    if (this.data.isAdmin) {
      // 已登录，显示管理员菜单
      wx.showActionSheet({
        itemList: ['库存管理', '购买记录', '修改密码', '退出管理'],
        success: (res) => {
          switch (res.tapIndex) {
            case 0: wx.switchTab({ url: '/pages/goods/goods' }); break
            case 1: wx.navigateTo({ url: '/pages/sales/sales' }); break
            case 2:
              wx.navigateTo({ url: '/pages/settings/settings' })
              break
            case 3:
              app.adminLogout()
              this.setData({ isAdmin: false })
              wx.showToast({ title: '已退出管理', icon: 'success' })
              break
          }
        }
      })
    } else {
      // 未登录：提供两种方式
      wx.showActionSheet({
        itemList: ['🔐 正常登录（需连接设备）', '🧪 演示模式（直接进入）'],
        success: (res) => {
          if (res.tapIndex === 0) {
            this.setData({ showLoginModal: true })
          } else if (res.tapIndex === 1) {
            this.enterDemoMode()
          }
        }
      })
    }
  },

  /* 演示模式：跳过 API 验证，直接进入管理员界面 */
  enterDemoMode() {
    app.globalData.isAdmin = true
    wx.setStorageSync('is_admin', true)
    this.setData({ isAdmin: true })
    wx.showToast({ title: '已进入演示模式', icon: 'success' })
  },

  onPasswordInput(e) {
    this.setData({ loginPassword: e.detail.value })
  },

  onLoginConfirm() {
    const pwd = this.data.loginPassword
    if (!pwd) {
      wx.showToast({ title: '请输入密码', icon: 'none' })
      return
    }
    app.adminLogin(pwd).then(res => {
      if (res.success) {
        this.setData({ isAdmin: true, showLoginModal: false, loginPassword: '' })
        wx.showToast({ title: '登录成功', icon: 'success' })
      } else {
        wx.showToast({ title: '密码错误', icon: 'none' })
      }
    }).catch(() => {
      wx.showToast({ title: '连接失败', icon: 'none' })
    })
  },

  onLoginCancel() {
    this.setData({ showLoginModal: false, loginPassword: '' })
  }
})
