const API = require('../../utils/api')
const app = getApp()

Page({
  data: {
    products: [],
    loading: true,
    isAdmin: false,
    /* 管理员补货 */
    showRestockModal: false,
    restockProduct: '',
    restockCount: 0
  },

  onShow() {
    this.setData({ isAdmin: app.checkIsAdmin() })
    this.loadProducts()
  },

  onPullDownRefresh() {
    this.loadProducts().then(() => wx.stopPullDownRefresh())
  },

  /* ── 加载商品/库存 ── */
  loadProducts() {
    this.setData({ loading: true })
    const api = app.checkIsAdmin() ? API.getInventory() : API.getProducts()

    return api.then(res => {
      const items = res.products || []
      const products = items.map(p => ({
        name: p.name || '',
        price: p.price || 0,
        stock: p.stock !== undefined ? p.stock : (p.remaining !== undefined ? p.remaining : 0),
        sold: p.sold || 0,
        total: p.total || 0,
        image: '/images/' + (p.name || '').toLowerCase() + '.jpg',
        stockStatus: this.getStockStatus(p),
        stockClass: this.getStockClass(p)
      }))
      this.setData({ products, loading: false })
    }).catch(err => {
      console.error('加载失败:', err)
      this.setData({ loading: false })
      wx.showToast({ title: '连接失败', icon: 'none' })
    })
  },

  getStockStatus(p) {
    const stock = p.stock !== undefined ? p.stock : (p.remaining !== undefined ? p.remaining : 0)
    if (stock > 10) return '充裕'
    if (stock > 0) return '紧张'
    return '售罄'
  },

  getStockClass(p) {
    const stock = p.stock !== undefined ? p.stock : (p.remaining !== undefined ? p.remaining : 0)
    if (stock > 10) return 'good'
    if (stock > 0) return 'warn'
    return 'out'
  },

  /* ── 用户：选商品下单 ── */
  onProductTap(e) {
    if (app.checkIsAdmin()) return // 管理员不跳转下单
    const product = e.currentTarget.dataset.product
    if (product.stock <= 0) {
      wx.showToast({ title: '商品已售罄', icon: 'none' })
      return
    }
    wx.navigateTo({
      url: '/pages/order-confirm/order-confirm?product=' +
           encodeURIComponent(JSON.stringify(product))
    })
  },

  /* ── 管理员：补货 ── */
  onRestockTap(e) {
    const name = e.currentTarget.dataset.name
    this.setData({
      showRestockModal: true,
      restockProduct: name,
      restockCount: 10
    })
  },

  onRestockCountInput(e) {
    this.setData({ restockCount: parseInt(e.detail.value) || 0 })
  },

  onRestockConfirm() {
    const { restockProduct, restockCount } = this.data
    if (!restockProduct || restockCount <= 0) {
      wx.showToast({ title: '请输入数量', icon: 'none' })
      return
    }
    API.restock(restockProduct, restockCount).then(() => {
      wx.showToast({ title: '补货成功', icon: 'success' })
      this.setData({ showRestockModal: false })
      this.loadProducts()
    }).catch(() => {
      wx.showToast({ title: '补货失败', icon: 'none' })
    })
  },

  onRestockCancel() {
    this.setData({ showRestockModal: false })
  }
})
