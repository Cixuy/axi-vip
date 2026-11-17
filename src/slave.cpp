#include "dv.h"

// =====================================================================
// AXI4 从设备（DUT 模型）
//   写通道：AW 收下地址 → 等若干拍 → 逐拍收 W（写进内部存储）→ 回 B
//   读通道：AR 收下地址 → 等若干拍 → 逐拍送 R（从内部存储取数）
// 地址三区间：正常存储 OKAY / 故障寄存器 SLVERR / 未映射 DECERR
//
// 信号所有权约定（重要，模型靠这个避免两边抢同一根线）：
//   master 驱动 awvalid/awaddr/wvalid/wdata/wstrb/wlast/bready/arvalid/rready
//   slave  驱动 awready/wready/bvalid/bresp/arready/rvalid/rdata/rresp/rlast
// =====================================================================

void AxiMemSlave::reset(const SlaveCfg& cfg, Rng* rng) {
    cfg_ = cfg; rng_ = rng;
    mem_.clear();
    ws_ = W_IDLE; rs_ = R_IDLE;
    awlat_ = wlat_ = blat_ = arlat_ = rlat_ = beat_ = rbeat_ = 0;
    aw_addr_ = ar_addr_ = 0;
    aw_len_ = ar_len_ = 0;
    aw_size_ = ar_size_ = 2;
    rdata_ = 0;
    bresp_ = rresp_ = 0;
    bpend_ = false;
}

int AxiMemSlave::lat() {
    if (!cfg_.randLat) return 0;
    return rng_->range(0, cfg_.latMax);
}

uint8_t AxiMemSlave::respFor(uint32_t addr) const {
    if (addr >= cfg_.base + 0xF00u && addr < cfg_.base + cfg_.size) return 0x2;  // SLVERR
    if (addr < cfg_.base || addr >= cfg_.base + cfg_.size)          return 0x3;  // DECERR
    return 0x0;                                                                  // OKAY
}

void AxiMemSlave::wr(uint32_t addr, uint64_t data, uint8_t strb, int size) {
    uint32_t nbytes = 1u << size;
    for (uint32_t i = 0; i < nbytes; ++i)
        if (strb & (1u << i)) mem_[addr + i] = (uint8_t)(data >> (8 * i));
}

uint64_t AxiMemSlave::rd(uint32_t addr, int size) {
    uint32_t nbytes = 1u << size;
    uint64_t v = 0;
    for (uint32_t i = 0; i < nbytes; ++i) {
        auto it = mem_.find(addr + i);
        uint8_t b = (it == mem_.end()) ? 0 : it->second;
        v |= (uint64_t)b << (8 * i);
    }
    return v;
}

void AxiMemSlave::eval(Axi& a, const Axi& in, const Hs& hs) {
    // ---- 本拍默认不拉任何 ready/valid，再按状态决定 ----
    a.awready = false;
    a.wready  = false;
    a.bvalid  = false;
    a.arready = false;
    a.rvalid  = false;

    // ================= 写通道 =================
    switch (ws_) {
    case W_IDLE:
        // 地址通道：可以立刻收，也可以先忙几拍（制造 AW 停顿）
        if (awlat_ > 0) { --awlat_; break; }
        a.awready = true;
        if (hs.aw) {
            aw_addr_ = in.awaddr;
            aw_len_  = in.awlen;
            aw_size_ = in.awsize;
            bresp_   = respFor(aw_addr_);
            awlat_   = cfg_.awLat + lat();
            wlat_    = cfg_.wLat + lat();
            beat_    = 0;
            bpend_   = false;
            ws_      = W_LAT;
        }
        break;

    case W_LAT:
        if (--wlat_ <= 0) ws_ = W_DATA;
        break;

    case W_DATA:
        a.wready = true;
        // [bug] 写响应早于 WLAST：应等数据收完再回 B
        if (cfg_.bugBEarly && beat_ == 0 && !bpend_) {
            a.bvalid = true;
            a.bresp  = 0;
            bpend_   = true;
        }
        if (hs.w) {
            wr(aw_addr_ + ((uint32_t)beat_ << aw_size_), in.wdata, in.wstrb, aw_size_);
            if (beat_ >= aw_len_) {
                a.wready = false;
                ws_      = W_RESP;
                blat_    = cfg_.bLat + lat();
                bpend_   = false;
            } else {
                ++beat_;
            }
        }
        break;

    case W_RESP:
        if (blat_ > 0) { --blat_; break; }
        a.bvalid = true;
        a.bresp  = bresp_;
        bpend_   = true;
        if (hs.b) {
            a.bvalid = false;
            bpend_   = false;
            ws_      = W_IDLE;
            awlat_   = cfg_.awLat + lat();
        }
        break;
    }

    // ================= 读通道 =================
    switch (rs_) {
    case R_IDLE:
        if (arlat_ > 0) { --arlat_; break; }
        a.arready = true;
        if (hs.ar) {
            ar_addr_ = in.araddr;
            ar_len_  = in.arlen;
            ar_size_ = in.arsize;
            rresp_   = respFor(ar_addr_);
            rbeat_   = 0;
            rlat_    = cfg_.rLat + lat();
            arlat_   = cfg_.arLat + lat();
            rs_      = R_LAT;
        }
        break;

    case R_LAT:
        if (--rlat_ <= 0) {
            rdata_ = rd(ar_addr_, ar_size_);
            rs_    = R_DATA;
        }
        break;

    case R_DATA: {
        a.rvalid = true;
        a.rdata  = rdata_;
        a.rresp  = rresp_;
        // [bug] RLAST 早一拍
        a.rlast  = cfg_.bugRLastEarly ? (rbeat_ == ar_len_ - 1) : (rbeat_ == ar_len_);
        if (hs.r) {
            if (rbeat_ >= ar_len_) {
                // 刚才那拍就是最后一拍
                a.rvalid = false;
                a.rlast  = false;
                rs_      = R_IDLE;
                arlat_   = cfg_.arLat + lat();
            } else {
                // 推进到下一拍：rdata 和 rlast 必须一起更新！
                // 只改 rdata 不改 rlast 的话，最后一拍会带着上一拍的 LAST=0 出去，
                // 主设备永远等不到 rlast，直接死锁（这个 bug 我第一版就写了进去）
                ++rbeat_;
                rdata_ = rd(ar_addr_ + ((uint32_t)rbeat_ << ar_size_), ar_size_);
                a.rdata = rdata_;
                a.rlast = cfg_.bugRLastEarly ? (rbeat_ == ar_len_ - 1) : (rbeat_ == ar_len_);
            }
        } else if (cfg_.bugRDataFlip) {
            // [bug] 从设备在等待期改了数据（握手没完成就变），checker 应该抓到
            rdata_ ^= 0x1ull;
        }
        break;
    }
    }
}
