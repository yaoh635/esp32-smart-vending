/*
 * Smart Vending Machine — Monitoring Dashboard
 *
 * Embedded web page served by ESP32 HTTP server.
 * Auto-refreshes every 5 seconds.
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

static const char WEB_PAGE_HTML[] = R"rawhtml(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>智能售货机 - 监控面板</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh}
.header{background:linear-gradient(135deg,#1e293b,#334155);padding:16px 20px;border-bottom:1px solid #475569;display:flex;align-items:center;justify-content:space-between}
.header h1{font-size:1.2em;font-weight:600}
.header .badge{background:#22c55e;color:#000;padding:3px 12px;border-radius:12px;font-size:0.75em;font-weight:600}
.header .badge.offline{background:#ef4444;color:#fff}
.container{max-width:1100px;margin:0 auto;padding:16px}
/* KPI Cards */
.kpi-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:12px;margin-bottom:16px}
.kpi-card{background:#1e293b;border:1px solid#334155;border-radius:12px;padding:16px;text-align:center}
.kpi-card .icon{font-size:1.8em;margin-bottom:6px}
.kpi-card .value{font-size:2em;font-weight:700;color:#3b82f6}
.kpi-card .label{color:#94a3b8;font-size:0.85em;margin-top:4px}
/* Grid layout */
.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}
@media(max-width:768px){.grid{grid-template-columns:1fr}}
.grid .full{grid-column:1/-1}
/* Card */
.card{background:#1e293b;border:1px solid#334155;border-radius:12px;padding:18px}
.card h2{font-size:0.85em;text-transform:uppercase;letter-spacing:.05em;color:#94a3b8;margin-bottom:12px;display:flex;align-items:center;gap:8px}
.card h2 .dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.dot.green{background:#22c55e;box-shadow:0 0 6px #22c55e}
.dot.red{background:#ef4444;box-shadow:0 0 6px #ef4444}
/* Inventory Progress Bar */
.inv-item{margin-bottom:14px}
.inv-item:last-child{margin-bottom:0}
.inv-header{display:flex;justify-content:space-between;margin-bottom:4px}
.inv-name{font-weight:600;font-size:0.95em}
.inv-stats{color:#94a3b8;font-size:0.85em}
.inv-bar{height:20px;background:#334155;border-radius:10px;overflow:hidden;position:relative}
.inv-fill{height:100%;border-radius:10px;transition:width .5s ease}
.inv-fill.high{background:linear-gradient(90deg,#22c55e,#4ade80)}
.inv-fill.mid{background:linear-gradient(90deg,#f59e0b,#fbbf24)}
.inv-fill.low{background:linear-gradient(90deg,#ef4444,#f87171)}
.inv-fill.out{background:#374151}
.inv-pct{position:absolute;right:8px;top:0;line-height:20px;font-size:0.75em;font-weight:600;color:#fff;text-shadow:0 0 4px #000}
/* Ranking Bar Chart */
.rank-item{display:flex;align-items:center;gap:8px;margin-bottom:10px}
.rank-num{width:24px;text-align:center;font-weight:700;color:#94a3b8}
.rank-num.top1{color:#fbbf24}
.rank-num.top2{color:#94a3b8}
.rank-num.top3{color:#cd853f}
.rank-name{width:70px;font-size:0.9em;flex-shrink:0}
.rank-bar-wrap{flex:1;height:22px;background:#334155;border-radius:6px;overflow:hidden}
.rank-bar{height:100%;background:linear-gradient(90deg,#3b82f6,#60a5fa);border-radius:6px;transition:width 0.5s;display:flex;align-items:center;padding-left:8px;font-size:0.75em;min-width:30px}
.rank-bar span{white-space:nowrap}
/* Table */
table{width:100%;border-collapse:collapse;font-size:0.88em}
thead th{padding:8px 10px;text-align:left;color:#94a3b8;font-weight:500;border-bottom:1px solid#334155}
tbody td{padding:8px 10px;border-bottom:1px solid#1f2937}
tbody tr:hover{background:#1e3a5f}
/* Footer */
.footer{text-align:center;padding:16px;color:#475569;font-size:0.78em}
/* Buttons */
.btn-row{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
.btn{padding:6px 16px;border:1px solid#475569;border-radius:6px;background:#334155;color:#e2e8f0;cursor:pointer;font-size:0.85em;transition:.2s}
.btn:hover{background:#475569}
.btn.primary{background:#3b82f6;border-color:#3b82f6}
.btn.primary:hover{background:#2563eb}
.btn.danger{background:#ef4444;border-color:#ef4444}
.btn.danger:hover{background:#dc2626}
/* Restock modal */
.modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.6);z-index:100;justify-content:center;align-items:center}
.modal.show{display:flex}
.modal-box{background:#1e293b;border:1px solid#475569;border-radius:12px;padding:24px;width:90%;max-width:360px}
.modal-box h3{margin-bottom:16px;font-size:1.1em}
.modal-box label{display:block;color:#94a3b8;font-size:0.85em;margin-bottom:4px}
.modal-box input,.modal-box select{width:100%;padding:8px 12px;border:1px solid#475569;border-radius:6px;background:#0f172a;color:#e2e8f0;font-size:0.95em;margin-bottom:12px}
.modal-box .btn-row{justify-content:flex-end}
.toast{position:fixed;top:16px;right:16px;padding:10px 20px;border-radius:8px;font-size:0.9em;z-index:200;opacity:0;transition:opacity .3s}
.toast.show{opacity:1}
.toast.ok{background:#22c55e;color:#000}
.toast.err{background:#ef4444;color:#fff}
</style>
</head>
<body>

<div class="header">
  <h1>&#129372; 智能售货机监控面板</h1>
  <span id="conn-badge" class="badge">&#9679; 在线</span>
</div>

<div class="container">
  <!-- KPI 卡片 -->
  <div class="kpi-grid" id="kpi-grid">
    <div class="kpi-card"><div class="icon">&#128230;</div><div class="value" id="kpi-sales">--</div><div class="label">总销量（件）</div></div>
    <div class="kpi-card"><div class="icon">&#128176;</div><div class="value" id="kpi-revenue">--</div><div class="label">总营收</div></div>
    <div class="kpi-card"><div class="icon">&#128200;</div><div class="value" id="kpi-today">--</div><div class="label">今日销量</div></div>
    <div class="kpi-card"><div class="icon">&#128101;</div><div class="value" id="kpi-users">--</div><div class="label">注册用户</div></div>
  </div>

  <div class="grid">
    <!-- 库存面板 -->
    <div class="card">
      <h2><span class="dot green"></span> 库存状态</h2>
      <div id="inventory-list"><p style="color:#94a3b8">加载中...</p></div>
      <div class="btn-row">
        <button class="btn primary" onclick="showRestock()">&#10133; 补货</button>
      </div>
    </div>

    <!-- 畅销排行 -->
    <div class="card">
      <h2>&#127942; 畅销排行</h2>
      <div id="ranking-list"><p style="color:#94a3b8">加载中...</p></div>
    </div>

    <!-- 最近交易 -->
    <div class="card full">
      <h2>&#128196; 最近交易记录</h2>
      <div style="overflow-x:auto">
        <table>
          <thead><tr><th>时间</th><th>用户ID</th><th>商品</th><th>价格</th></tr></thead>
          <tbody id="history-tbody"><tr><td colspan="4" style="color:#94a3b8;text-align:center">加载中...</td></tr></tbody>
        </table>
      </div>
    </div>

    <!-- 系统状态 -->
    <div class="card full">
      <h2>&#9881; 系统状态</h2>
      <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px" id="sys-grid">
        <div><span style="color:#94a3b8">运行时间 </span><span id="sys-uptime">--</span></div>
        <div><span style="color:#94a3b8">可用内存 </span><span id="sys-heap">--</span></div>
        <div><span style="color:#94a3b8">售卖状态 </span><span id="sys-vending">--</span></div>
        <div><span style="color:#94a3b8">注册人数 </span><span id="sys-faces">--</span></div>
      </div>
    </div>
  </div>
</div>

<div class="footer">Smart Vending Machine &middot; ESP32-P4 &middot; <span id="refresh-time">--</span></div>

<!-- 补货弹窗 -->
<div class="modal" id="restock-modal">
  <div class="modal-box">
    <h3>&#128230; 商品补货</h3>
    <label>商品名称</label>
    <select id="restock-product">
      <option value="Cola">Cola</option>
      <option value="Water">Water</option>
      <option value="Chips">Chips</option>
      <option value="Candy">Candy</option>
    </select>
    <label>补货数量</label>
    <input type="number" id="restock-count" value="10" min="1" max="200">
    <div class="btn-row">
      <button class="btn" onclick="hideRestock()">取消</button>
      <button class="btn primary" onclick="doRestock()">确认补货</button>
    </div>
  </div>
</div>
<div class="toast" id="toast"></div>

<script>
/* ── 工具函数 ── */
function $(id){return document.getElementById(id)}
function formatTime(us){
  var d=new Date(us/1000); /* us -> ms */
  var now=new Date();
  var diff=now-d;
  if(diff<60000) return Math.floor(diff/1000)+'秒前';
  if(diff<3600000) return Math.floor(diff/60000)+'分钟前';
  var h=d.getHours().toString().padStart(2,'0');
  var m=d.getMinutes().toString().padStart(2,'0');
  if(diff<86400000) return h+':'+m;
  return (d.getMonth()+1)+'/'+d.getDate()+' '+h+':'+m;
}
function formatBytes(b){
  if(b>1048576) return (b/1048576).toFixed(1)+' MB';
  if(b>1024) return (b/1024).toFixed(1)+' KB';
  return b+' B';
}
function formatUptime(s){
  var h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;
  if(h>0) return h+'h '+m+'m';
  if(m>0) return m+'m '+sec+'s';
  return sec+'s';
}
function showToast(msg,ok){
  var t=$('toast');
  t.textContent=msg;t.className='toast '+(ok?'ok':'err')+' show';
  setTimeout(function(){t.classList.remove('show')},2500);
}

/* ── 数据加载 ── */
var lastUpdate=0;
function loadAll(){
  Promise.all([
    fetch('/api/sales/summary').then(function(r){return r.json()}),
    fetch('/api/inventory').then(function(r){return r.json()}),
    fetch('/api/sales/ranking').then(function(r){return r.json()}),
    fetch('/api/sales/history?limit=8').then(function(r){return r.json()}),
    fetch('/api/system').then(function(r){return r.json()})
  ]).then(function(d){
    var summary=d[0], inv=d[1], rank=d[2], hist=d[3], sys=d[4];
    renderKPI(summary);
    renderInventory(inv);
    renderRanking(rank);
    renderHistory(hist);
    renderSystem(sys);
    lastUpdate=Date.now();
    $('refresh-time').textContent='更新于 '+new Date().toLocaleTimeString('zh-CN');
    $('conn-badge').className='badge';
    $('conn-badge').innerHTML='&#9679; 在线';
  }).catch(function(){
    $('conn-badge').className='badge offline';
    $('conn-badge').innerHTML='&#9679; 离线';
  });
}

/* ── 渲染函数 ── */
function renderKPI(s){
  $('kpi-sales').textContent=s.total_sales||0;
  $('kpi-revenue').textContent='$'+(s.total_revenue||0).toFixed(2);
  $('kpi-today').textContent=s.today_sales||0;
  $('kpi-users').textContent=s.total_users||0;
}

function renderInventory(inv){
  var products=inv.products||[];
  if(products.length===0){
    $('inventory-list').innerHTML='<p style="color:#94a3b8">暂无数据</p>';
    return;
  }
  /* 更新补货下拉框 */
  var sel=$('restock-product');
  sel.innerHTML='';
  products.forEach(function(p){
    sel.innerHTML+='<option value="'+p.name+'">'+p.name+'</option>';
  });

  var maxTotal=0;
  products.forEach(function(p){if(p.total>maxTotal) maxTotal=p.total});

  var html='';
  products.forEach(function(p){
    var pct=p.total>0?Math.round(p.remaining/p.total*100):0;
    var cls=pct>50?'high':(pct>20?'mid':(pct>0?'low':'out'));
    var barPct=Math.max(pct,4);
    html+='<div class="inv-item">';
    html+='<div class="inv-header"><span class="inv-name">'+p.name+'</span>';
    html+='<span class="inv-stats">'+p.remaining+' / '+p.total+' (售 '+p.sold+') ¥'+parseFloat(p.price).toFixed(2)+'</span></div>';
    html+='<div class="inv-bar"><div class="inv-fill '+cls+'" style="width:'+barPct+'%"></div>';
    html+='<span class="inv-pct">'+pct+'%</span></div></div>';
  });
  $('inventory-list').innerHTML=html;
}

function renderRanking(rank){
  var list=rank.ranking||[];
  if(list.length===0){
    $('ranking-list').innerHTML='<p style="color:#94a3b8">暂无数据</p>';
    return;
  }
  var maxSold=list[0]?list[0].sold:1;
  var html='';
  list.forEach(function(p,i){
    var top='';
    if(i===0) top=' top1';
    else if(i===1) top=' top2';
    else if(i===2) top=' top3';
    var barPct=maxSold>0?Math.round(p.sold/maxSold*100):0;
    var name=p.name.length>6?p.name.substring(0,6)+'..':p.name;
    html+='<div class="rank-item">';
    html+='<span class="rank-num'+top+'">'+(i+1)+'</span>';
    html+='<span class="rank-name">'+name+'</span>';
    html+='<div class="rank-bar-wrap"><div class="rank-bar" style="width:'+Math.max(barPct,10)+'%">';
    html+='<span>'+p.sold+'件</span></div></div>';
    html+='</div>';
  });
  $('ranking-list').innerHTML=html;
}

function renderHistory(hist){
  var list=hist.history||[];
  if(list.length===0){
    $('history-tbody').innerHTML='<tr><td colspan="4" style="color:#94a3b8;text-align:center">暂无交易记录</td></tr>';
    return;
  }
  var html='';
  list.reverse().forEach(function(r){
    html+='<tr><td>'+formatTime(r.time)+'</td><td>#'+r.user+'</td><td>'+r.product+'</td><td>'+r.price+'</td></tr>';
  });
  $('history-tbody').innerHTML=html;
}

function renderSystem(sys){
  $('sys-uptime').textContent=formatUptime(sys.uptime_s||0);
  $('sys-heap').textContent=formatBytes(sys.free_heap||0);
  $('sys-vending').innerHTML=sys.vending_active?
    '<span style="color:#f59e0b">售卖中</span>':
    '<span style="color:#22c55e">空闲</span>';
  $('sys-faces').textContent=(sys.faces_registered||0)+'人';
}

/* ── 补货功能 ── */
function showRestock(){$('restock-modal').classList.add('show')}
function hideRestock(){$('restock-modal').classList.remove('show')}
function doRestock(){
  var product=$('restock-product').value;
  var count=parseInt($('restock-count').value)||10;
  fetch('/api/inventory/restock',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({product:product,count:count})
  }).then(function(r){return r.json()}).then(function(d){
    if(d.success){
      showToast(product+' 补货 +'+count+' 成功!',true);
      hideRestock();
      loadAll();
    }else{
      showToast('补货失败: '+(d.error||'未知'),false);
    }
  }).catch(function(){
    showToast('网络错误',false);
  });
}

/* ── 初始化 ── */
loadAll();
setInterval(loadAll, 5000);
</script>
</body>
</html>
)rawhtml";
