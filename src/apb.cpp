#include "dv.h"

// =====================================================================
// AXI-Lite -> APB 桥（DUT）
//
//   AXI 侧：AW / W / B 与 AR / R，只接受单拍（len=0）
//   APB 侧：严格的 SETUP(1 拍) -> ACCESS(PENABLE 拉高，等 PREADY) 两段式
//
// 这个桥是 APB 协议里最容易被写错的地方，所以专门做成了 DUT：
//   - SETUP 必须正好一拍，不能直接进 ACCESS
//   - ACCESS 等待期间 PADDR/PWRITE/PWDATA/PSEL 都不能动（组合逻辑常犯）
//   - PREADY 只能在 PENABLE 拉高之后出现
// =====================================================================

void Axi2ApbBridge::reset(const BridgeCfg& cfg) {
    cfg_ = cfg;
    st_ = IDLE;
    wr_ = true;
    hasAw_ = hasW_ = false;
    addr_ = wdata_ = rdata_ = 0;
    bresp_ = rresp_ = 0;
    wait_ = lastWait_ = 0;
    xfers_ = waitSum_ = 0;
}

void Axi2ApbBridge::eval(Axi& a, Apb& p, const Axi& ain, const Apb& pin, const Hs& hs) {
    // ---- AXI 侧默认不拉任何输出 ----
    a.awready = false;
    a.wready  = false;
    a.bvalid  = false;
    a.arready = false;
    a.rvalid  = false;
    a.rlast   = false;
    // ---- APB 侧默认空闲 ----
    p.psel = false;
    p.penable = false;
    p.pwrite = false;
    p.paddr = 0;
    p.pwdata = 0;

    switch (st_) {
    case IDLE:
        // 收地址与数据。AW 和 W 是两条通道，允许不同拍到
        if (!hasAw_) a.awready = true;
        if (!hasW_)  a.wready  = true;
        if (hs.aw) { addr_ = ain.awaddr; hasAw_ = true; }
        if (hs.w)  { wdata_ = ain.wdata; hasW_ = true; }

        if (hasAw_ && hasW_) {
            wr_ = true;
            st_ = SETUP;
            break;
        }
        // 没有挂起的写才接读，避免 APB 总线冲突
        if (!hasAw_ && !hasW_) {
            a.arready = true;
            if (hs.ar) { addr_ = ain.araddr; wr_ = false; st_ = SETUP; }
        }
        break;

    case SETUP:
        // APB 的 SETUP 段：PSEL 拉高、PENABLE 保持低，正好一拍
        p.psel    = true;
        p.penable = false;
        p.paddr   = addr_;
        p.pwrite  = wr_;
        p.pwdata  = wr_ ? wdata_ : 0;
        st_       = ACCESS;
        break;

    case ACCESS:
        p.psel    = true;
        p.penable = true;
        p.paddr   = addr_;
        p.pwrite  = wr_;
        p.pwdata  = wr_ ? wdata_ : 0;

        // [bug] 等待期改地址：APB 明确要求 ACCESS 段地址保持不变
        if (cfg_.bugPaddrChange && !pin.pready) p.paddr = addr_ ^ 0x4u;
        // [bug] 还没应答就撤掉 PSEL
        if (cfg_.bugPselDrop && !pin.pready) p.psel = false;

        if (pin.pready) {
            // 本拍完成传输：下一拍必须回到 IDLE（PSEL/PENABLE 立刻拉低），
            // 否则 APB 上会出现"连着两拍都是 ACCESS"，从机还会误以为要再传一次
            p.psel    = false;
            p.penable = false;
            lastWait_ = wait_;
            waitSum_ += wait_;
            ++xfers_;
            wait_ = 0;
            if (wr_) { bresp_ = pin.pslverr ? 0x2 : 0x0; }
            else     { rdata_ = pin.prdata; rresp_ = pin.pslverr ? 0x2 : 0x0; }
            st_ = RESP;
        } else {
            ++wait_;
        }
        break;

    case RESP:
        if (wr_) {
            a.bvalid = true;
            a.bresp  = bresp_;
            if (hs.b) {
                // 握手完成，下一拍必须把 BVALID 拉低，否则主设备会当成第二次响应
                a.bvalid = false;
                hasAw_ = hasW_ = false;
                st_ = IDLE;
            }
        } else {
            a.rvalid = true;
            a.rdata  = rdata_;
            a.rresp  = rresp_;
            a.rlast  = true;
            if (hs.r) {
                // 同上：完成即拉低，避免多送一拍 R（1 拍读会被主设备当成 2 拍）
                a.rvalid = false;
                a.rlast  = false;
                st_ = IDLE;
            }
        }
        break;
    }
}

// =====================================================================
// APB 寄存器从机：16 个寄存器，0x3C 作为"等待发生器"
//   往 0x3C 写 N，后续访问就会被插入 N 拍 wait（经典的自测激励手段）
// =====================================================================

void ApbRegSlave::reset(const ApbCfg& cfg, Rng* rng) {
    cfg_ = cfg;
    rng_ = rng;
    for (int i = 0; i < 16; ++i) regs_[i] = 0x1000u + (uint32_t)i * 0x1111u;
    wait_ = cfg_.baseWait;
    cnt_ = 0;
    lastWait_ = 0;
    active_ = false;
}

void ApbRegSlave::eval(Apb& p, const Apb& in) {
    p.pready  = false;
    p.prdata  = 0;
    p.pslverr = false;

    // [bug] PENABLE 还没拉高就应答：违反 APB 的 setup/access 时序
    if (cfg_.bugPreadyNoEnable) {
        p.pready = true;
    }

    if (!in.psel) { active_ = false; cnt_ = 0; return; }
    if (!in.penable) { active_ = false; cnt_ = 0; return; }  // SETUP 阶段从机不应答

    if (!active_) {
        active_ = true;
        cnt_ = 0;
        wait_ = (int)(regs_[15] & 0x7);                     // 0x3C 决定插几拍 wait
    }
    if (cnt_ < wait_) { ++cnt_; return; }                    // 插等待

    uint32_t idx = (in.paddr >> 2) & 0xF;
    if (in.paddr >= 0x40u) {
        p.pslverr = true;                                    // 越界访问
        lastWait_ = cnt_;
        p.pready  = true;
        return;
    }
    if (in.pwrite) regs_[idx] = in.pwdata;
    else           p.prdata  = regs_[idx];

    if (cfg_.bugPsLverrAlways) p.pslverr = true;
    lastWait_ = cnt_;
    p.pready  = true;
}
