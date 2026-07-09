/**
 * API 请求封装
 * 与 ESP32 REST API 通信
 */
function baseUrl() {
  const app = getApp();
  return (app && app.globalData.serverUrl) || 'http://192.168.101.35'
}

function request(path, options = {}) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: baseUrl() + path,
      method: options.method || 'GET',
      data: options.data || {},
      header: {
        'Content-Type': 'application/json'
      },
      success(res) {
        if (res.statusCode === 200) {
          resolve(res.data)
        } else {
          reject(new Error('HTTP ' + res.statusCode))
        }
      },
      fail(err) {
        reject(err)
      }
    })
  })
}

module.exports = {
  /* ================================================================
   * 系统状态
   * ================================================================ */
  getSystem() {
    return request('/api/system')
  },

  getStatus() {
    return request('/api/status')
  },

  /* ================================================================
   * 商品（小程序端）
   * ================================================================ */
  getProducts() {
    return request('/api/products')
  },

  getInventory() {
    return request('/api/inventory')
  },

  /* ================================================================
   * 推荐
   * ================================================================ */
  getRecommend(userId) {
    return request('/api/recommend/' + userId)
  },

  /* ================================================================
   * 订单
   * ================================================================ */
  createOrder(product, userId, paymentMethod = 'face') {
    return request('/api/order', {
      method: 'POST',
      data: {
        product: product,
        user_id: userId,
        payment_method: paymentMethod
      }
    })
  },

  getOrder(orderId) {
    return request('/api/order/' + orderId)
  },

  cancelOrder(orderId) {
    return request('/api/order/' + orderId + '/cancel', {
      method: 'POST'
    })
  },

  confirmOrder(orderId) {
    return request('/api/order/' + orderId + '/confirm', {
      method: 'POST'
    })
  },

  /* ================================================================
   * 销售数据
   * ================================================================ */
  getSalesSummary() {
    return request('/api/sales/summary')
  },

  getTodaySales() {
    return request('/api/sales/today')
  },

  getRanking() {
    return request('/api/sales/ranking')
  },

  getSalesHistory(limit = 20) {
    return request('/api/sales/history?limit=' + limit)
  },

  /* ================================================================
   * 用户
   * ================================================================ */
  getUsers() {
    return request('/api/users')
  },

  getUserPurchases(userId) {
    return request('/api/users/' + userId + '/purchases')
  },

  /* ================================================================
   * 管理员
   * ================================================================ */
  adminLogin(password) {
    return request('/api/admin/login', {
      method: 'POST',
      data: { password: password }
    })
  },

  changePassword(oldPwd, newPwd) {
    return request('/api/admin/password', {
      method: 'POST',
      data: {
        old_password: oldPwd,
        new_password: newPwd
      }
    })
  },

  /* ================================================================
   * 库存管理（管理员）
   * ================================================================ */
  restock(product, count) {
    return request('/api/inventory/restock', {
      method: 'POST',
      data: { product, count }
    })
  },

  /* ================================================================
   * 人脸注册
   * ================================================================ */
  faceRegister(userId, mode = 'device') {
    return request('/api/face/register', {
      method: 'POST',
      data: {
        user_id: userId,
        mode: mode
      }
    })
  }
}
