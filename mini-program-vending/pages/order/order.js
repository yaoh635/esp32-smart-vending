const API = require('../../utils/api')
const app = getApp()

Page({
  data: {
    orders: [],
    loading: true,
    isAdmin: false,
    salesSummary: null,
    tabIndex: 0  // 0=我的订单, 1=销售数据(admin)
  },

  onShow() {
    this.setData({ isAdmin: app.checkIsAdmin() })
    if (app.checkIsAdmin()) {
      this.loadSalesData()
    } else {
      this.loadMyOrders()
    }
  },

  onPullDownRefresh() {
    if (app.checkIsAdmin()) {
      this.loadSalesData().then(() => wx.stopPullDownRefresh())
    } else {
      this.loadMyOrders().then(() => wx.stopPullDownRefresh())
    }
  },

  /* ── 用户：我的订单 ── */
  loadMyOrders() {
    this.setData({ loading: true })
    const userId = app.globalData.userId || 1

    return Promise.all([
      API.getSalesHistory(50),
    ]).then(([history]) => {
      // 从购买记录筛选当前用户的
      const myPurchases = (history.history || [])
        .filter(r => r.user === userId)
        .map(r => ({
          order_id: r.time,
          product: r.product,
          price: r.price,
          time: this.formatTime(r.time),
          status: 'shipped'
        }))
      this.setData({ orders: myPurchases, loading: false })
    }).catch(err => {
      console.error('加载订单失败:', err)
      this.setData({ loading: false })
    })
  },

  /* ── 管理员：销售数据 ── */
  loadSalesData() {
    this.setData({ loading: true })
    return API.getSalesSummary().then(summary => {
      this.setData({ salesSummary: summary, loading: false })
    }).catch(err => {
      console.error('加载销售数据失败:', err)
      this.setData({ loading: false })
    })
  },

  /* 取消订单 */
  cancelOrder(e) {
    const orderId = e.currentTarget.dataset.id
    wx.showModal({
      title: '取消订单',
      content: '确定要取消这个订单吗？',
      success: (res) => {
        if (res.confirm) {
          API.cancelOrder(orderId).then(() => {
            wx.showToast({ title: '已取消', icon: 'success' })
            this.loadMyOrders()
          }).catch(() => {
            wx.showToast({ title: '取消失败', icon: 'none' })
          })
        }
      }
    })
  },

  formatTime(us) {
    const d = new Date(us / 1000)
    const m = (d.getMonth() + 1).toString().padStart(2, '0')
    const day = d.getDate().toString().padStart(2, '0')
    const h = d.getHours().toString().padStart(2, '0')
    const min = d.getMinutes().toString().padStart(2, '0')
    return `${m}-${day} ${h}:${min}`
  }
})
