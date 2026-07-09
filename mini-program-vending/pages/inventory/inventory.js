const API = require('../../utils/api')

Page({
  data: {
    products: [],
    loading: true,
    showModal: false,
    restockProduct: '',
    restockCount: 10
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
      const inv = await API.getInventory()
      const products = (inv.products || []).map(p => ({
        ...p,
        pct: p.total > 0 ? Math.round(p.remaining / p.total * 100) : 0,
        barColor: this.getBarColor(p.total > 0 ? p.remaining / p.total : 0)
      }))
      this.setData({ products, loading: false })
    } catch (e) {
      this.setData({ loading: false })
      wx.showToast({ title: '加载失败', icon: 'none' })
    }
  },

  getBarColor(ratio) {
    if (ratio > 0.5) return '#22c55e'
    if (ratio > 0.2) return '#f59e0b'
    if (ratio > 0) return '#ef4444'
    return '#374151'
  },

  openRestock(e) {
    const name = e.currentTarget.dataset.name || ''
    this.setData({ showModal: true, restockProduct: name, restockCount: 10 })
  },

  closeModal() {
    this.setData({ showModal: false })
  },

  onProductChange(e) {
    this.setData({ restockProduct: e.detail.value })
  },

  onCountInput(e) {
    this.setData({ restockCount: parseInt(e.detail.value) || 10 })
  },

  async doRestock() {
    const { restockProduct, restockCount } = this.data
    if (!restockProduct || restockCount <= 0) {
      wx.showToast({ title: '请输入有效信息', icon: 'none' })
      return
    }
    try {
      await API.restock(restockProduct, restockCount)
      wx.showToast({ title: '补货成功!', icon: 'success' })
      this.closeModal()
      this.loadData()
    } catch (e) {
      wx.showToast({ title: '补货失败', icon: 'none' })
    }
  }
})
