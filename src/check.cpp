#include "dv.h"

// =====================================================================
// 协议断言 + 参考模型 scoreboard + 覆盖率
//
// 这三样都只"看总线"，不读 DUT 内部状态 —— 谁抄谁的问题就不存在了。
// =====================================================================

// ---------------------------------------------------------------------
// Checker
// ---------------------------------------------------------------------
void Checker::reset() {
    pa_ = Axi();
    pp_ = Apb();
    first_ = true;
    apbOn_ = false;
    wrOpen_ = rdOpen_ = false;
    wrBeats_ = rdBeats_ = wrLen_ = rdLen_ = 0;
    wrGotLast_ = false;
    bSeen_ = false;
    bWait_ = 0;
    v_.clear();
}

void Checker::violate(int cycle, const char* rule, const std::string& detail) {
    Violation v;
    v.cycle = cycle;
    v.rule = rule;
    v.detail = detail;
    v_.push_back(v);
}

void Checker::onCycle(const Axi& a, const Apb& p, const Hs& hs, int cycle) {
    if (!first_) {
        // ---------- AXI：VALID 一旦拉起，在 READY 到来前不能撤销、载荷不能变 ----------
        if (pa_.awvalid && !pa_.awready) {
            if (!a.awvalid)
                violate(cycle, "AXI-AW-STABLE", "AWVALID 在 AWREADY 之前被撤销");
            else if (a.awaddr != pa_.awaddr || a.awlen != pa_.awlen || a.awsize != pa_.awsize)
                violate(cycle, "AXI-AW-STABLE", "AW 载荷在等待期发生了变化 addr=" + hexs(a.awaddr));
        }
        if (pa_.wvalid && !pa_.wready) {
            if (!a.wvalid)
                violate(cycle, "AXI-W-STABLE", "WVALID 在 WREADY 之前被撤销");
            else if (a.wdata != pa_.wdata || a.wstrb != pa_.wstrb || a.wlast != pa_.wlast)
                violate(cycle, "AXI-W-STABLE", "W 载荷在等待期发生了变化");
        }
        if (pa_.arvalid && !pa_.arready) {
            if (!a.arvalid)
                violate(cycle, "AXI-AR-STABLE", "ARVALID 在 ARREADY 之前被撤销");
            else if (a.araddr != pa_.araddr || a.arlen != pa_.arlen)
                violate(cycle, "AXI-AR-STABLE", "AR 载荷在等待期发生了变化");
        }
        if (pa_.rvalid && !pa_.rready) {
            if (!a.rvalid)
                violate(cycle, "AXI-R-STABLE", "RVALID 在 RREADY 之前被撤销");
            else if (a.rdata != pa_.rdata || a.rlast != pa_.rlast)
                violate(cycle, "AXI-R-STABLE", "RDATA/RLAST 在等待期发生了变化");
        }
        if (pa_.bvalid && !pa_.bready && !a.bvalid)
            violate(cycle, "AXI-B-STABLE", "BVALID 在 BREADY 之前被撤销");

        // ---------- APB 时序 ----------
        // SETUP 段必须正好一拍：上一拍是 PSEL&&!PENABLE，这一拍必须是 PSEL&&PENABLE
        if (pp_.psel && !pp_.penable) {
            if (!(p.psel && p.penable))
                violate(cycle, "APB-SETUP", "SETUP 之后没有进入 ACCESS（PENABLE 未紧跟 PSEL 拉高）");
        }
        // PSEL 拉高期间，PADDR / PWRITE / PWDATA 必须全程保持不变。
        // 基准取 SETUP 那一拍 —— 只比较相邻两拍会漏掉"一直偏移着"的实现
        // （bridge.bugPaddrChange 注入的正是这种，第一版没抓到）。
        if (p.psel && !p.penable) {
            apbOn_    = true;
            apbAddr_  = p.paddr;
            apbWrite_ = p.pwrite;
            apbWdata_ = p.pwdata;
        } else if (p.psel && apbOn_) {
            if (p.paddr != apbAddr_)
                violate(cycle, "APB-STABLE", "ACCESS 期间 PADDR 变了 " +
                                              hexs(apbAddr_, 2) + " -> " + hexs(p.paddr, 2));
            else if (p.pwrite != apbWrite_ || p.pwdata != apbWdata_)
                violate(cycle, "APB-STABLE", "ACCESS 期间 PWRITE/PWDATA 变了");
        }
        if (!p.psel) apbOn_ = false;
        // ACCESS 完成后必须回 SETUP 或 IDLE，不能连着两拍都是 ACCESS
        if (pp_.penable && pp_.pready && p.penable)
            violate(cycle, "APB-ACCESS-STABLE", "ACCESS 结束后没有回到 SETUP");
        // PREADY 只能出现在 PENABLE 拉高之后
        if (p.pready && !p.penable)
            violate(cycle, "APB-PREADY", "PREADY 在 PENABLE 为低时被拉高");
    }

    // ---------- 事务级：突发长度与 LAST 位置 ----------
    if (hs.aw) {
        wrOpen_ = true;
        wrBeats_ = 0;
        wrLen_ = a.awlen;
        wrGotLast_ = false;
        bSeen_ = false;
        uint32_t bytes = ((uint32_t)a.awlen + 1) << a.awsize;
        if ((a.awaddr & 0xFFFu) + bytes > 0x1000u)
            violate(cycle, "AXI-4KB", "写突发跨越 4KB 边界 addr=" + hexs(a.awaddr) +
                                       " bytes=" + std::to_string(bytes));
    }
    if (hs.w) {
        ++wrBeats_;
        bool shouldLast = (wrBeats_ - 1 == wrLen_);
        if (a.wlast != shouldLast)
            violate(cycle, "AXI-WLAST", std::string("WLAST 位置错误：第 ") + std::to_string(wrBeats_) +
                                         " 拍 wlast=" + (a.wlast ? "1" : "0") +
                                         "，AWLEN=" + std::to_string(wrLen_));
        if (a.wlast) wrGotLast_ = true;
        if (wrBeats_ > wrLen_ + 1)
            violate(cycle, "AXI-W-LEN", "W 拍数超过 AWLEN+1");
    }
    if (hs.b) {
        if (wrOpen_ && !wrGotLast_)
            violate(cycle, "AXI-B-EARLY", "写响应在 WLAST 之前返回");
        if (wrOpen_ && wrBeats_ != wrLen_ + 1)
            violate(cycle, "AXI-W-LEN", "W 拍数与 AWLEN 不符：收到 " + std::to_string(wrBeats_) +
                                        " 拍，期望 " + std::to_string(wrLen_ + 1));
        wrOpen_ = false;
        bSeen_ = true;
    }

    if (hs.ar) {
        rdOpen_ = true;
        rdBeats_ = 0;
        rdLen_ = a.arlen;
        uint32_t bytes = ((uint32_t)a.arlen + 1) << a.arsize;
        if ((a.araddr & 0xFFFu) + bytes > 0x1000u)
            violate(cycle, "AXI-4KB", "读突发跨越 4KB 边界 addr=" + hexs(a.araddr) +
                                       " bytes=" + std::to_string(bytes));
    }
    if (hs.r) {
        ++rdBeats_;
        bool shouldLast = (rdBeats_ - 1 == rdLen_);
        if (a.rlast != shouldLast)
            violate(cycle, "AXI-RLAST", std::string("RLAST 位置错误：第 ") + std::to_string(rdBeats_) +
                                         " 拍 rlast=" + (a.rlast ? "1" : "0") +
                                         "，ARLEN=" + std::to_string(rdLen_));
        if (a.rlast) rdOpen_ = false;
    }

    pa_ = a;
    pp_ = p;
    first_ = false;
}

// ---------------------------------------------------------------------
// Scoreboard：独立参考模型
// ---------------------------------------------------------------------
void Scoreboard::onCycle(const Axi& a, const Hs& hs, int cycle) {
    if (hs.aw) {
        wrOpen_ = true;
        wrAddr_ = a.awaddr;
        wrSize_ = a.awsize;
        wrLen_  = a.awlen;
    }
    if (hs.w && wrOpen_) {
        uint32_t nbytes = 1u << wrSize_;
        for (uint32_t i = 0; i < nbytes; ++i)
            if (a.wstrb & (1u << i)) ref_[wrAddr_ + i] = (uint8_t)(a.wdata >> (8 * i));
        wrAddr_ += nbytes;      // INCR：地址逐拍递增
    }
    if (hs.b) wrOpen_ = false;

    if (hs.ar) {
        rdOpen_ = true;
        rdAddr_ = a.araddr;
        rdSize_ = a.arsize;
        rdLen_  = a.arlen;
        rb_     = 0;
    }
    if (hs.r && rdOpen_) {
        uint32_t nbytes = 1u << rdSize_;
        uint64_t exp = 0;
        for (uint32_t i = 0; i < nbytes; ++i) {
            auto it = ref_.find(rdAddr_ + i);
            uint8_t b = (it == ref_.end()) ? 0 : it->second;
            exp |= (uint64_t)b << (8 * i);
        }
        uint64_t mask = (nbytes >= 8) ? ~0ull : ((1ull << (8 * nbytes)) - 1ull);
        ++checked_;
        ++beats_;
        if ((a.rdata & mask) != (exp & mask)) {
            ++mismatches_;
            if (bad_.empty())
                bad_ = "cycle " + std::to_string(cycle) + " addr " + hexs(rdAddr_) +
                       " 期望 " + hexs(exp) + " 实际 " + hexs(a.rdata);
        }
        ++rb_;
        rdAddr_ += nbytes;
        if (a.rlast) rdOpen_ = false;
    }
}

// ---------------------------------------------------------------------
// 覆盖率
// ---------------------------------------------------------------------
void Coverage::build(int /*maxBeats*/) {
    const char* dirs[]  = {"写", "读"};
    const char* lens[]  = {"1 拍", "2 拍", "4 拍", "8 拍", "16 拍"};
    const char* sizes[] = {"1B", "2B", "4B", "8B"};
    const char* resps[] = {"OKAY", "SLVERR", "DECERR"};
    const char* stalls[]= {"0 拍", "1-2 拍", "3-7 拍", "8+ 拍"};
    const char* strbs[] = {"全字节", "部分字节"};
    const char* waits[] = {"0 拍", "1-2 拍", "3+ 拍"};

    for (auto d : dirs)   cov_.add("方向", d);
    for (auto l : lens)   cov_.add("突发长度", l);
    for (auto s : sizes)  cov_.add("传输大小", s);
    for (auto r : resps)  cov_.add("响应", r);
    for (auto s : stalls) cov_.add("地址通道停顿", s);
    for (auto s : strbs)  cov_.add("WSTRB", s);
    for (auto w : waits)  cov_.add("APB 等待态", w);

    for (auto d : dirs) {
        for (auto s : sizes) cov_.add("交叉:方向x大小", std::string(d) + "-" + s);
        for (auto r : resps) cov_.add("交叉:方向x响应", std::string(d) + "-" + r);
    }
}

void Coverage::sampleTxn(bool write, int beats, int sizeBytes, int resp, int stall, uint8_t strb) {
    const char* dir = write ? "写" : "读";
    cov_.hit("方向", dir);

    const char* l = nullptr;
    if (beats == 1) l = "1 拍"; else if (beats == 2) l = "2 拍";
    else if (beats <= 4) l = "4 拍"; else if (beats <= 8) l = "8 拍"; else l = "16 拍";
    cov_.hit("突发长度", l);

    const char* s = (sizeBytes == 1) ? "1B" : (sizeBytes == 2) ? "2B" : (sizeBytes == 4) ? "4B" : "8B";
    cov_.hit("传输大小", s);

    const char* r = (resp == 0) ? "OKAY" : (resp == 2) ? "SLVERR" : "DECERR";
    cov_.hit("响应", r);

    const char* st = (stall == 0) ? "0 拍" : (stall <= 2) ? "1-2 拍" : (stall <= 7) ? "3-7 拍" : "8+ 拍";
    cov_.hit("地址通道停顿", st);

    uint32_t mask = (1u << sizeBytes) - 1u;
    cov_.hit("WSTRB", (strb == mask) ? "全字节" : "部分字节");

    cov_.hit("交叉:方向x大小", std::string(dir) + "-" + s);
    cov_.hit("交叉:方向x响应", std::string(dir) + "-" + r);
}

void Coverage::sampleApbWait(int wait) {
    const char* w = (wait == 0) ? "0 拍" : (wait <= 2) ? "1-2 拍" : "3+ 拍";
    cov_.hit("APB 等待态", w);
}
