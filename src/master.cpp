#include "dv.h"

// =====================================================================
// 主设备：约束随机 + 定向脚本
// 采用"一次一笔事务"的顺序模型（AW → W → B 或 AR → R），
// 这样从设备的实现可以保持简单，同时握手、背压、突发长度这些协议行为
// 全都是真实跑出来的，不是查表查出来的。
// =====================================================================

void AxiMaster::reset(const Cfg& cfg, Rng* rng, uint32_t base, uint32_t size) {
    cfg_ = cfg; rng_ = rng; base_ = base; size_ = size;
    ph_ = IDLE; beat_ = 0; gap_ = 0; stall_ = 0; txnStart_ = -1; cycle_ = 0;
    txns_ = 0; rejected_ = 0; busyCycles_ = 0; stallSum_ = 0;
    scriptPos_ = 0;
    finished.clear();
    cur_ = Txn();
}

void AxiMaster::setScript(const std::vector<Txn>& t) {
    script_ = t;
    scriptPos_ = 0;
    cfg_.useScript = true;
}

bool AxiMaster::done() const {
    if (cfg_.useScript) return ph_ == FIN || (scriptPos_ >= script_.size() && ph_ == IDLE);
    return ph_ == FIN || txns_ >= cfg_.maxTxns;
}

// 一笔合法的 INCR 突发：
//   1) 地址按 size 对齐
//   2) 整笔传输不能跨 4KB 边界（AXI 硬约束，必须由 generator 保证）
// 抽到不合法的就直接丢弃重抽，丢弃次数说明约束真的在拦东西
Txn AxiMaster::genRandom() {
    for (int tries = 0; tries < 64; ++tries) {
        Txn t;
        t.write = rng_->chance(50);
        if (cfg_.lite) {
            // AXI-Lite：单拍、4 字节、地址 4 字节对齐
            t.len  = 0;
            t.size = 2;
            t.addr = (base_ + (uint32_t)rng_->range(0, (int)(size_ - 1))) & ~3u;
            t.strb = 0xF;
            t.data[0] = ((uint64_t)rng_->next() << 32) | rng_->next();
            return t;
        }
        t.size  = rng_->range(0, 3);                    // 1/2/4/8 字节
        static const int lens[] = {0, 0, 0, 1, 3, 7, 15};
        t.len = lens[rng_->range(0, 6)];

        uint32_t bytes = (uint32_t)(t.len + 1) << t.size;
        t.addr = base_ + (uint32_t)rng_->range(0, (int)(size_ - 1));

        // 少量样本打到"故障寄存器"和"未映射"区间，用来覆盖 SLVERR / DECERR。
        // 注意：必须在重映射之后再做对齐和 4KB 检查，否则会生成非对齐地址 —— 我第一版
        // 就是先检查后重映射，结果 checker 报了一条 4KB 越界、scoreboard 还失配了几十拍。
        int roll = rng_->range(0, 99);
        if (roll < cfg_.slverrPct)      t.addr = base_ + 0xF00u + (uint32_t)rng_->range(0, 0xFC);
        else if (roll < cfg_.slverrPct + cfg_.decerrPct) t.addr = base_ + size_ + 0x400u;

        t.addr &= ~((uint32_t)(1u << t.size) - 1u);     // 按 size 对齐

        // 4KB 边界检查
        if ((t.addr & 0xFFFu) + bytes > 0x1000u) { rejected_++; continue; }

        // WSTRB：大多数是全字节，少数只写一部分（模拟寄存器部分写）
        uint32_t nbytes = 1u << t.size;
        t.strb = (uint8_t)((1u << nbytes) - 1u);
        if (rng_->chance(30) && nbytes > 1) {
            uint8_t m = 0;
            for (uint32_t i = 0; i < nbytes; ++i) if (rng_->chance(60)) m |= (uint8_t)(1u << i);
            if (m) t.strb = m;
        }

        for (int b = 0; b <= t.len; ++b) t.data[b] = ((uint64_t)rng_->next() << 32) | rng_->next();
        return t;
    }
    // 极端情况下也要给出一个合法的
    Txn t;
    t.write = true; t.size = 2; t.len = 0; t.addr = base_;
    t.strb = 0xF; t.data[0] = 0x1122334455667788ull;
    return t;
}

Txn AxiMaster::nextTxn() {
    if (cfg_.useScript && scriptPos_ < script_.size()) return script_[scriptPos_++];
    return genRandom();
}

void AxiMaster::driveW(Axi& a) {
    a.wvalid = true;
    a.wdata  = cur_.data[beat_];
    a.wstrb  = cur_.strb;
    a.wlast  = (beat_ == cur_.len);
}

void AxiMaster::eval(Axi& a, const Axi& in, const Hs& hs) {
    ++cycle_;
    if (ph_ == FIN) { a.awvalid = a.wvalid = a.arvalid = false; return; }

    switch (ph_) {
    case IDLE:
        a.awvalid = a.wvalid = a.arvalid = false;
        a.wlast = false;
        if (gap_ > 0) { --gap_; break; }
        if (done()) { ph_ = FIN; break; }
        cur_ = nextTxn();
        beat_ = 0; stall_ = 0;
        txnStart_ = cycle_;
        if (cur_.write) {
            ph_ = AW;
            a.awvalid = true;
            a.awaddr  = cur_.addr;
            a.awlen   = (uint8_t)cur_.len;
            a.awsize  = (uint8_t)cur_.size;
            a.awburst = 1;                       // INCR
        } else {
            ph_ = AR;
            a.arvalid = true;
            a.araddr  = cur_.addr;
            a.arlen   = (uint8_t)cur_.len;
            a.arsize  = (uint8_t)cur_.size;
            a.arburst = 1;
        }
        break;

    case AW:
        // 保持 valid 与载荷不变，直到从设备给出 ready
        if (hs.aw) {
            a.awvalid = false;
            ph_ = W;
            driveW(a);
        } else {
            ++stall_;
        }
        break;

    case W:
        if (hs.w) {
            if (beat_ >= cur_.len) {
                a.wvalid = false; a.wlast = false;
                ph_ = B;
                a.bready = true;
            } else {
                ++beat_;
                driveW(a);
            }
        }
        break;

    case B:
        // 随机撤 BREADY 制造背压（合法行为）
        a.bready = !rng_->chance(cfg_.breadyLowPct);
        if (hs.b) {
            cur_.resp = in.bresp;
            cur_.stall = stall_;
            finished.push_back(cur_);
            ++txns_;
            busyCycles_ += cycle_ - txnStart_;
            stallSum_   += stall_;
            a.bready = true;
            ph_ = IDLE;
            gap_ = rng_->range(0, cfg_.gapMax);
        }
        break;

    case AR:
        if (hs.ar) {
            a.arvalid = false;
            ph_ = R;
            rbeat_ = 0;
            a.rready = true;
        } else {
            ++stall_;
        }
        break;

    case R:
        a.rready = !rng_->chance(cfg_.rreadyLowPct);
        if (hs.r) {
            if (in.rlast) {
                cur_.resp = in.rresp;
                cur_.stall = stall_;
                finished.push_back(cur_);
                ++txns_;
                busyCycles_ += cycle_ - txnStart_;
                stallSum_   += stall_;
                a.rready = true;
                ph_ = IDLE;
                gap_ = rng_->range(0, cfg_.gapMax);
            } else {
                ++rbeat_;
            }
        }
        break;

    default:
        break;
    }
}
