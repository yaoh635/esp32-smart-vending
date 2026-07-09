const API = require('../../utils/api')
const app = getApp()

Page({
  data: {
    product: null,
    quantity: 1,
    paymentMethod: 'face',  // 'face' | 'scan'
    totalPrice: 0,
    submitting: false
  },

  onLoad(options) {
    if (options.product) {
      const product = JSON.parse(decodeURIComponent(options.product))
      this.setData({
        product: product,
        totalPrice: product.price
      })
    }
  },

  /* 数量变化 */
  onMinus() {
    if (this.data.quantity > 1) {
      const qty = this.data.quantity - 1
      this.setData({
        quantity: qty,
        totalPrice: (this.data.product.price * qty).toFixed(2)
      })
    }
  },

  onPlus() {
    const qty = this.data.quantity + 1
    this.setData({
      quantity: qty,
      totalPrice: (this.data.product.price * qty).toFixed(2)
    })
  },

  /* 支付方式 */
  onPaymentChange(e) {
    this.setData({ paymentMethod: e.currentTarget.dataset.method })
  },

  /* 提交订单 */
  onSubmit() {
    if (this.data.submitting) return
    if (!this.data.product) {
      wx.showToast({ title: '商品信息异常', icon: 'none' })
      return
    }

    this.setData({ submitting: true })
    const userId = app.globalData.userId || 1

    API.createOrder(this.data.product.name, userId, this.data.paymentMethod)
      .then(res => {
        if (res.success) {
          wx.showToast({ title: '下单成功', icon: 'success' })
          setTimeout(() => {
            wx.switchTab({ url: '/pages/order/order' })
          }, 1000)
        } else {
          wx.showToast({ title: res.error || '下单失败', icon: 'none' })
        }
      })
      .catch(() => {
        wx.showToast({ title: '网络错误', icon: 'none' })
      })
      .finally(() => {
        this.setData({ submitting: false })
      })
  }
})
