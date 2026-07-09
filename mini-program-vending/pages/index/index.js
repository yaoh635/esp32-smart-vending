const API = require('../../utils/api')
const app = getApp()

Page({
  data: {
    isAdmin: false,
    isBound: false,

    /* 用户：推荐 */
    recommendations: [],
    hotProducts: [],
    loading: true,

    /* 管理员：仪表盘 */
    sales: 0,
    revenue: '0.00',
    todaySales: 0,
    users: 0,
    ranking: [],
    uptime: '--',
    heap: '--',
    vendingActive: false,
    lastUpdate: '-'
  },

  onShow() {
    const admin = app.checkIsAdmin()
    this.setData({
      isAdmin: admin,
      isBound: app.globalData.isBound
    })

    if (admin) {
      this.loadAdminData()
    } else {
      this.loadUserData()
    }
  },

  onPullDownRefresh() {
    const promise = this.data.isAdmin
      ? this.loadAdminData()
      : this.loadUserData()
    promise.then(() => wx.stopPullDownRefresh())
  },

  /* ── 用户：推荐 + 畅销 ── */
  loadUserData() {
    this.setData({ loading: true })
    const userId = app.globalData.userId || 1

    return Promise.all([
      API.getRecommend(userId),
      API.getRanking()
    ]).then(([rec, rank]) => {
      this.setData({
        recommendations: (rec.recommendations || []).slice(0, 5).map(r => ({
          ...r,
          image: '/images/' + (r.product_name || '').toLowerCase() + '.jpg'
        })),
        hotProducts: (rank.ranking || []).slice(0, 3).map(r => ({
          ...r,
          image: '/images/' + (r.name || '').toLowerCase() + '.jpg'
        })),
        loading: false
      })
    }).catch(err => {
      console.error('加载推荐失败:', err)
      this.setData({ loading: false })
    })
  },

  /* ── 管理员：仪表盘 ── */
  loadAdminData() {
    this.setData({ loading: true })

    return Promise.all([
      API.getSalesSummary(),
      API.getInventory(),
      API.getRanking(),
      API.getSystem()
    ]).then(([summary, inv, rank, sys]) => {
      this.setData({
        sales: summary.total_sales || 0,
        revenue: (summary.total_revenue || 0).toFixed(2),
        todaySales: summary.today_sales || 0,
        users: summary.total_users || 0,
        ranking: (rank.ranking || []).slice(0, 5),
        uptime: this.fmtUptime(sys.uptime_s || 0),
        heap: this.fmtBytes(sys.free_heap || 0),
        vendingActive: sys.vending_active || false,
        lastUpdate: new Date().toLocaleTimeString('zh-CN'),
        loading: false
      })
    }).catch(err => {
      console.error('加载仪表盘失败:', err)
      this.setData({ loading: false })
    })
  },

  /* ── 商品点击 ── */
  onProductTap(e) {
    const product = e.currentTarget.dataset.product
    wx.navigateTo({
      url: '/pages/order-confirm/order-confirm?product=' +
           encodeURIComponent(JSON.stringify(product))
    })
  },

  /* ── 扫码绑定入口 ── */
  onGoBind() {
    wx.navigateTo({ url: '/pages/scan-bind/scan-bind' })
  },

  /* ── 工具函数 ── */
  fmtUptime(s) {
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60)
    return h > 0 ? h + 'h ' + m + 'm' : m + 'm ' + (s % 60) + 's'
  },

  fmtBytes(b) {
    if (b > 1048576) return (b / 1048576).toFixed(1) + ' MB'
    if (b > 1024) return (b / 1024).toFixed(1) + ' KB'
    return b + ' B'
  }
})
