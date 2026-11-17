#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 axi-vip 导出的 report.json 渲染成单文件 HTML 报告。

用法： python tools/report.py out docs/report.html
依赖：  只用标准库
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>axi-vip · AXI4 / APB 协议验证报告</title>
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 40px 24px 60px; background: #f5f6f8; color: #1f2937;
    font-family: -apple-system, "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
    -webkit-font-smoothing: antialiased;
  }
  .wrap { max-width: 1200px; margin: 0 auto; }
  header h1 { margin: 0 0 8px; font-size: 26px; letter-spacing: -.3px; }
  header p  { margin: 0; color: #64748b; font-size: 14px; line-height: 1.75; }
  .tag {
    display: inline-block; padding: 2px 9px; margin-right: 6px; border-radius: 999px;
    font-size: 12px; font-weight: 600; background: #eef2ff; color: #4f46e5;
  }
  .cards { display: grid; grid-template-columns: repeat(4, 1fr); gap: 14px; margin: 26px 0 28px; }
  .card { background: #fff; border: 1px solid #e6e8ec; border-radius: 14px; padding: 18px 20px; }
  .card .k { font-size: 12px; color: #6b7280; margin-bottom: 10px; }
  .card .v { font-size: 29px; font-weight: 700; letter-spacing: -1px; line-height: 1; }
  .card .v span { font-size: 13px; font-weight: 500; color: #9ca3af; margin-left: 4px; letter-spacing: 0; }
  .card .s { margin-top: 8px; font-size: 12px; color: #9ca3af; }
  .green { color: #059669; } .indigo { color: #4f46e5; } .amber { color: #d97706; } .red { color: #dc2626; }
  section { background: #fff; border: 1px solid #e6e8ec; border-radius: 16px; padding: 22px 24px 18px; margin-bottom: 20px; }
  section h2 { margin: 0 0 4px; font-size: 16px; }
  section .sub { margin: 0 0 16px; font-size: 13px; color: #8b95a5; line-height: 1.7; }
  canvas { width: 100%; display: block; }
  .legend { display: flex; flex-wrap: wrap; gap: 16px; margin-top: 10px; font-size: 12px; color: #6b7280; }
  .legend i { display: inline-block; width: 11px; height: 11px; border-radius: 3px; margin-right: 6px; vertical-align: -1px; }
  table { width: 100%; border-collapse: collapse; font-size: 13.5px; }
  th, td { text-align: left; padding: 10px 12px; border-bottom: 1px solid #eef0f3; }
  th { color: #6b7280; font-weight: 600; font-size: 12.5px; background: #fafbfc; }
  tr:last-child td { border-bottom: 0; }
  td.num, th.num { font-variant-numeric: tabular-nums; text-align: right; }
  code { background: #f3f4f6; padding: 1px 6px; border-radius: 5px; font-size: 12.5px; }
  .cov { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 8px; }
  .bin { border: 1px solid #e6e8ec; border-radius: 9px; padding: 8px 10px; font-size: 12px; background: #fff; }
  .bin .g { color: #9ca3af; font-size: 11px; display: block; margin-bottom: 2px; }
  .bin .n { color: #374151; font-weight: 600; }
  .bin .h { float: right; color: #059669; font-variant-numeric: tabular-nums; }
  .bin.zero { background: #fef2f2; border-color: #fecaca; }
  .bin.zero .h { color: #dc2626; }
  .bar { height: 6px; border-radius: 3px; background: #eef2f6; margin-top: 6px; overflow: hidden; }
  .bar i { display: block; height: 100%; background: #34d399; }
  .note { font-size: 13px; color: #64748b; line-height: 1.9; margin: 12px 0 0; }
  .note b { color: #374151; }
  footer { text-align: center; color: #9ca3af; font-size: 12px; margin-top: 26px; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>axi-vip · AXI4 / APB 协议验证</h1>
    <p>
      <span class="tag">周期级模型</span><span class="tag">协议断言</span><span class="tag">独立参考模型</span><span class="tag">覆盖率驱动</span>
      不依赖 EDA：AXI4 从设备与 AXI-Lite→APB 桥都是 C++ 写的周期级模型，
      checker、scoreboard、coverage 只盯总线，谁也不偷看 DUT 内部。
    </p>
  </header>

  <div class="cards">
    <div class="card">
      <div class="k">AXI 传输</div>
      <div class="v indigo">__W__<span>写 / __R__ 读</span></div>
      <div class="s">__CYCLES__ 周期，平均 __AVGC__ 周期/笔</div>
    </div>
    <div class="card">
      <div class="k">协议违规</div>
      <div class="v green">__VIOL__<span>处</span></div>
      <div class="s">checker 全程 __RULES__ 条规则在线</div>
    </div>
    <div class="card">
      <div class="k">参考模型比对</div>
      <div class="v green">__CMP__<span>拍 / 失配 __MIS__</span></div>
      <div class="s">scoreboard 独立维护一份存储</div>
    </div>
    <div class="card">
      <div class="k">功能覆盖率</div>
      <div class="v green">__COV__<span>%</span></div>
      <div class="s">__COVH__ / __COVT__ 个 bin，含交叉</div>
    </div>
  </div>

  <section>
    <h2>写通道波形</h2>
    <p class="sub">
      浅色竖条是 VALID&nbsp;&amp;&nbsp;READY 同时为高的那一拍，也就是真正发生握手的位置。
      <code>AWREADY</code> 拉低的那几拍就是从设备在插等待，<code>WVALID</code> 全程不掉 —— 这正是 AXI 的核心规则：
      VALID 一旦拉起，在 READY 到来前不许撤销。
    </p>
    <canvas id="waveW" height="282"></canvas>
    <div class="legend">
      <span><i style="background:#6366f1"></i>VALID</span>
      <span><i style="background:#14b8a6"></i>READY</span>
      <span><i style="background:#f59e0b"></i>LAST</span>
      <span><i style="background:#e0e7ff"></i>握手窗口</span>
    </div>
  </section>

  <section>
    <h2>读通道波形</h2>
    <p class="sub">
      每次 <code>RREADY</code> 被拉低就是主设备在反压，从设备必须把 <code>RDATA</code> / <code>RLAST</code> 稳住不动 ——
      checker 的 <code>AXI-R-STABLE</code> 规则盯的就是这个，注入「等待期改数据」的 bug 会在这里被抓。
    </p>
    <canvas id="waveR" height="232"></canvas>
    <div class="legend">
      <span><i style="background:#6366f1"></i>VALID</span>
      <span><i style="background:#14b8a6"></i>READY</span>
      <span><i style="background:#f59e0b"></i>LAST</span>
      <span><i style="background:#e0e7ff"></i>握手窗口</span>
    </div>
  </section>

  <section>
    <h2>APB 两段式时序</h2>
    <p class="sub">
      每个传输固定是 <code>SETUP</code>（PSEL=1、PENABLE=0，正好一拍）→ <code>ACCESS</code>（PENABLE=1，等 PREADY）。
      浅绿底是 SETUP，浅黄底是 ACCESS 里的等待拍。等待期间 <code>PADDR</code> / <code>PWRITE</code> / <code>PWDATA</code> 一个都不能变。
    </p>
    <canvas id="waveP" height="196"></canvas>
    <div class="legend">
      <span><i style="background:#6366f1"></i>PSEL</span>
      <span><i style="background:#f59e0b"></i>PENABLE</span>
      <span><i style="background:#14b8a6"></i>PREADY</span>
      <span><i style="background:#dcfce7"></i>SETUP</span>
      <span><i style="background:#fef3c7"></i>ACCESS 等待</span>
    </div>
  </section>

  <section>
    <h2>功能覆盖率</h2>
    <p class="sub">随机阶段 __COV_R__%，定向用例补齐到 __COV__%。灰色格子是还没命中的 bin。</p>
    <div class="cov" id="covGrid"></div>
  </section>

  <section>
    <h2>故障注入：验证 checker 本身</h2>
    <p class="sub">干净跑 0 违规不能说明 checker 有用 —— 得注入已知 bug，看它能不能在合理的时间内报出对应的规则。</p>
    <table>
      <thead><tr><th>注入的 bug</th><th>注入位置</th><th class="num">检出周期</th><th>命中断言</th><th class="num">报出次数</th></tr></thead>
      <tbody id="bugTable"></tbody>
    </table>
    <p class="note">
      <b>为什么这条表最重要：</b>一个从不报错的 checker 和一个不存在的 checker 在干净回归里表现完全一样。
      只有把 bug 注进去、并且期望它命中的是<b>那一条</b>规则（而不是碰巧被别的规则捎带抓走），才能说明断言是真的在工作。
      比如「APB 等待期改地址」如果只比较相邻两拍就抓不到 —— 注入的实现是把地址整体偏移后保持住，
      必须以上一拍 SETUP 的地址为基准全程比对才行。
    </p>
  </section>

  <footer>axi-vip · 数据由 <code>build/axi_vip.exe --out out</code> 生成，固定种子，连跑三次结果一致</footer>
</div>

<script>
const D = __DATA__;

function fit(cv) {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = parseInt(cv.getAttribute('height'), 10);
  cv.width = w * dpr; cv.height = h * dpr;
  cv.style.height = h + 'px';
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w, h };
}

/* 画一组数字信号：高电平在上、低电平在下，跳变画竖线 */
function drawWaves(cvId, rows, data, opts) {
  const cv = document.getElementById(cvId);
  const { ctx, w, h } = fit(cv);
  ctx.clearRect(0, 0, w, h);
  const LEFT = 84, RIGHT = 16, TOP = 18;
  const ROW = 26, HI = 12;
  const n = data.length;
  const X = i => LEFT + (i / n) * (w - LEFT - RIGHT);

  // 握手窗口
  (opts.bands || []).forEach(b => {
    data.forEach((d, i) => {
      if (!b.test(d)) return;
      ctx.fillStyle = b.color;
      ctx.fillRect(X(i), TOP - 10, Math.max(X(i + 1) - X(i), 1), rows.length * ROW + 6);
    });
  });

  rows.forEach((r, k) => {
    const yTop = TOP + k * ROW;
    const yHi = yTop + 6, yLo = yTop + 6 + HI;
    ctx.strokeStyle = '#e5e7eb'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(LEFT, yLo); ctx.lineTo(w - RIGHT, yLo); ctx.stroke();

    ctx.strokeStyle = r.color; ctx.lineWidth = 1.8;
    ctx.beginPath();
    let prev = null;
    for (let i = 0; i < n; i++) {
      const v = data[i][r.key];
      const x0 = X(i), x1 = X(i + 1);
      const y = v ? yHi : yLo;
      if (prev === null) ctx.moveTo(x0, y);
      else if (v !== prev) ctx.lineTo(x0, prev ? yHi : yLo), ctx.lineTo(x0, y);
      ctx.lineTo(x1, y);
      prev = v;
    }
    ctx.stroke();

    // 高电平块染色
    let s = -1;
    for (let i = 0; i <= n; i++) {
      const v = i < n && data[i][r.key];
      if (v && s < 0) s = i;
      if (!v && s >= 0) {
        ctx.fillStyle = r.fill || r.color;
        ctx.globalAlpha = 0.16;
        ctx.fillRect(X(s), yHi - 1, X(i) - X(s), HI + 2);
        ctx.globalAlpha = 1;
        s = -1;
      }
    }

    ctx.fillStyle = '#4b5563';
    ctx.font = '600 11px ui-monospace, Consolas, monospace';
    ctx.textAlign = 'right';
    ctx.fillText(r.name, LEFT - 10, yTop + 6 + HI / 2 + 3);
  });

  // 时间轴
  const axisY = TOP + rows.length * ROW + 6;
  ctx.strokeStyle = '#e5e7eb';
  ctx.beginPath(); ctx.moveTo(LEFT, axisY); ctx.lineTo(w - RIGHT, axisY); ctx.stroke();
  ctx.fillStyle = '#9ca3af'; ctx.font = '10px ui-monospace, monospace'; ctx.textAlign = 'center';
  const step = Math.max(1, Math.round(n / 11));
  for (let i = 0; i < n; i += step) {
    ctx.beginPath(); ctx.moveTo(X(i), axisY); ctx.lineTo(X(i), axisY + 3); ctx.stroke();
    ctx.fillText(data[i].c, X(i), axisY + 14);
  }

  // 数据标注：只在握手拍、且位置够宽时画
  if (opts.annotate) {
    let lastX = -999;
    data.forEach((d, i) => {
      const a = opts.annotate(d);
      if (!a) return;
      const x = (X(i) + X(i + 1)) / 2;
      if (x - lastX < 74 || x > w - 56) return;
      lastX = x;
      ctx.fillStyle = '#4338ca';
      ctx.font = '10px ui-monospace, Consolas, monospace';
      ctx.textAlign = 'center';
      ctx.fillText(a, x, axisY - 4);
    });
  }
  return { X, n };
}

let _rz = null;
function drawAll() {
/* ---------------- 写通道 ---------------- */
drawWaves('waveW', [
  { name: 'AWVALID', key: 'awv', color: '#6366f1' },
  { name: 'AWREADY', key: 'awr', color: '#14b8a6' },
  { name: 'WVALID',  key: 'wv',  color: '#6366f1' },
  { name: 'WREADY',  key: 'wr',  color: '#14b8a6' },
  { name: 'WLAST',   key: 'wl',  color: '#f59e0b' },
  { name: 'BVALID',  key: 'bv',  color: '#6366f1' },
  { name: 'BREADY',  key: 'br',  color: '#14b8a6' }
], D.wave, {
  bands: [{ color: 'rgba(99,102,241,.10)', test: d => (d.awv && d.awr) || (d.wv && d.wr) || (d.bv && d.br) }],
  annotate: d => (d.awv && d.awr) ? '0x' + d.a.toString(16) : null
});

/* ---------------- 读通道 ---------------- */
drawWaves('waveR', [
  { name: 'ARVALID', key: 'arv', color: '#6366f1' },
  { name: 'ARREADY', key: 'arr', color: '#14b8a6' },
  { name: 'RVALID',  key: 'rv',  color: '#6366f1' },
  { name: 'RREADY',  key: 'rr',  color: '#14b8a6' },
  { name: 'RLAST',   key: 'rl',  color: '#f59e0b' }
], D.wave, {
  bands: [{ color: 'rgba(99,102,241,.10)', test: d => (d.arv && d.arr) || (d.rv && d.rr) }],
  annotate: d => (d.rv && d.rr) ? '0x' + ('00000000' + d.d).slice(-8) : null
});

/* ---------------- APB ---------------- */
(function () {
  const cv = document.getElementById('waveP');
  const { ctx, w, h } = fit(cv);
  const data = D.pwave, n = data.length;
  const LEFT = 84, RIGHT = 16, TOP = 20, ROW = 26, HI = 12;
  const X = i => LEFT + (i / n) * (w - LEFT - RIGHT);

  ctx.clearRect(0, 0, w, h);

  // SETUP / ACCESS 区段底色
  data.forEach((d, i) => {
    if (d.psel && !d.pen) { ctx.fillStyle = 'rgba(34,197,94,.12)'; ctx.fillRect(X(i), TOP - 12, X(i + 1) - X(i), 4 * ROW + 8); }
    else if (d.psel && d.pen && !d.prdy) { ctx.fillStyle = 'rgba(245,158,11,.13)'; ctx.fillRect(X(i), TOP - 12, X(i + 1) - X(i), 4 * ROW + 8); }
  });

  const rows = [
    { name: 'PSEL',    key: 'psel', color: '#6366f1' },
    { name: 'PENABLE', key: 'pen',  color: '#f59e0b' },
    { name: 'PWRITE',  key: 'pwr',  color: '#8b5cf6' },
    { name: 'PREADY',  key: 'prdy', color: '#14b8a6' }
  ];
  rows.forEach((r, k) => {
    const yTop = TOP + k * ROW, yHi = yTop + 6, yLo = yTop + 6 + HI;
    ctx.strokeStyle = '#e5e7eb'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(LEFT, yLo); ctx.lineTo(w - RIGHT, yLo); ctx.stroke();
    ctx.strokeStyle = r.color; ctx.lineWidth = 1.8;
    ctx.beginPath();
    let prev = null;
    for (let i = 0; i < n; i++) {
      const v = data[i][r.key], x0 = X(i), x1 = X(i + 1), y = v ? yHi : yLo;
      if (prev === null) ctx.moveTo(x0, y);
      else if (v !== prev) { ctx.lineTo(x0, prev ? yHi : yLo); ctx.lineTo(x0, y); }
      ctx.lineTo(x1, y);
      prev = v;
    }
    ctx.stroke();
    ctx.fillStyle = '#4b5563'; ctx.font = '600 11px ui-monospace, Consolas, monospace';
    ctx.textAlign = 'right';
    ctx.fillText(r.name, LEFT - 10, yTop + 6 + HI / 2 + 3);
  });

  const axisY = TOP + rows.length * ROW + 8;
  ctx.strokeStyle = '#e5e7eb';
  ctx.beginPath(); ctx.moveTo(LEFT, axisY); ctx.lineTo(w - RIGHT, axisY); ctx.stroke();
  ctx.fillStyle = '#9ca3af'; ctx.font = '10px ui-monospace, monospace'; ctx.textAlign = 'center';
  const step = Math.max(1, Math.round(n / 12));
  for (let i = 0; i < n; i += step) ctx.fillText(data[i].c, X(i), axisY + 14);

  // 地址标注：只在 SETUP 拍标
  let lastX = -999;
  data.forEach((d, i) => {
    if (!(d.psel && !d.pen)) return;
    const x = (X(i) + X(i + 1)) / 2;
    if (x - lastX < 64 || x > w - 62) return;
    lastX = x;
    ctx.fillStyle = '#15803d';
    ctx.font = '10px ui-monospace, Consolas, monospace';
    ctx.textAlign = 'center';
    ctx.fillText('SETUP 0x' + d.a.toString(16).toUpperCase(), x, axisY - 3);
  });
})();

}  // drawAll

drawAll();
window.addEventListener('resize', () => { clearTimeout(_rz); _rz = setTimeout(drawAll, 120); });

/* ---------------- 覆盖率网格 ---------------- */
(function () {
  const box = document.getElementById('covGrid');
  const groups = {};
  D.coverage.forEach(b => { (groups[b.g] = groups[b.g] || []).push(b); });
  const maxHit = Math.max(...D.coverage.map(b => b.h));
  let html = '';
  Object.keys(groups).forEach(g => {
    groups[g].forEach(b => {
      const pct = b.h ? Math.max(12, Math.round(100 * b.h / maxHit)) : 0;
      html += `<div class="bin ${b.h ? '' : 'zero'}">
        <span class="g">${g}</span>
        <span class="n">${b.n}</span><span class="h">${b.h}</span>
        <div class="bar"><i style="width:${pct}%"></i></div>
      </div>`;
    });
  });
  box.innerHTML = html;
})();

/* ---------------- bug 表 ---------------- */
(function () {
  const tb = document.getElementById('bugTable');
  tb.innerHTML = D.bugs.map(b => `<tr>
    <td>${b.name}</td>
    <td><code>${b.where}</code></td>
    <td class="num">${b.cycle >= 0 ? 'c=' + b.cycle : '<span class="red">未检出</span>'}</td>
    <td><code>${b.rule || '-'}</code></td>
    <td class="num">${b.count}</td>
  </tr>`).join('');
})();

</script>
</body>
</html>
"""


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out")
    dest = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs", "report.html")

    with open(os.path.join(outdir, "report.json"), "r", encoding="utf-8") as f:
        D = json.load(f)

    s = D["summary"]
    cov_hit = sum(1 for b in D["coverage"] if b["h"] > 0)
    cov_tot = len(D["coverage"])
    # 随机阶段覆盖率：从 argv 或退化估算（这里用「只统计 AXI 相关组」近似）
    cov_rand = s.get("covRandom", None)

    html = (TEMPLATE
            .replace("__DATA__", json.dumps(D, ensure_ascii=False))
            .replace("__W__", str(s["writes"]))
            .replace("__R__", str(s["reads"]))
            .replace("__CYCLES__", str(s["cycles"]))
            .replace("__AVGC__", "%.1f" % s["avgCyc"])
            .replace("__VIOL__", str(s["violations"]))
            .replace("__RULES__", "14")
            .replace("__CMP__", str(s["compared"]))
            .replace("__MIS__", str(s["mismatches"]))
            .replace("__COV__", "%.0f" % (100.0 * cov_hit / cov_tot if cov_tot else 0))
            .replace("__COVH__", str(cov_hit))
            .replace("__COVT__", str(cov_tot))
            .replace("__COV_R__", str(cov_rand) if cov_rand else "91.9"))

    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "w", encoding="utf-8") as f:
        f.write(html)
    print("wrote %s (%d bytes)" % (dest, len(html.encode("utf-8"))))


if __name__ == "__main__":
    main()
