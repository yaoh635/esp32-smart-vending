const API = require('../../utils/api')

Page({
  data: {
    history: [],
    ranking: [],
    loading: true,
    activeTab: 'history'  // 'history' | 'ranking'
  },

  onShow() {
    this.loadData()
  },

  onPullDownRefresh() {
    this.loadData().then(() => wx.stopPullDownRefresh())
  },

  async loadData() {
    this.setData({ loading: true })
    try {
      const [hist, rank] = await Promise.all([
        API.getSalesHistory(30),
        API.getRanking()
      ])
      this.setData({
        history: (hist.history || []).reverse(),
        ranking: (rank.ranking || []).map(r => ({
          ...r,
          revenueFmt: (r.revenue || 0).toFixed(2)
        })),
        loading: false
      })
    } catch (e) {
      this.setData({ loading: false })
      wx.showToast({ title: '加载失败', icon: 'none' })
    }
  },

  switchTab(e) {
    this.setData({ activeTab: e.currentTarget.dataset.tab })
  },

  fmtTime(us) {
    const d = new Date(us / 1000)
    const M = (d.getMonth() + 1).toString().padStart(2, '0')
    const D = d.getDate().toString().padStart(2, '0')
    const h = d.getHours().toString().padStart(2, '0')
    const m = d.getMinutes().toString().padStart(2, '0')
    return M + '/' + D + ' ' + h + ':' + m
  }
})
