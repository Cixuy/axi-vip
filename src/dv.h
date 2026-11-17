#pragma once
// 平台组件的对外接口（实现分散在 master.cpp / slave.cpp / apb.cpp / check.cpp）
#include "sim.h"

// =====================================================================
// 主设备（VIP 的 driver）：约束随机产生 AXI4 事务
//   约束：
//     - INCR 突发不能跨 4KB 边界（这是 AXI 的硬约束，违反直接算协议错误）
//     - 地址按 size 对齐
//     - WSTRB 只在该拍有效字节范围内置位
//   不满足约束的样本会被丢弃重抽，丢弃次数单独统计（能看出约束在起作用）
// =====================================================================
class AxiMaster {
public:
    struct Cfg {
        int  maxBeats      = 16;
        int  maxTxns       = 200;
        int  gapMax        = 3;
        int  breadyLowPct  = 25;    // 写响应背压概率
        int  rreadyLowPct  = 25;    // 读数据背压概率
        int  slverrPct     = 8;     // 命中"故障寄存器"区间的比例
        int  decerrPct     = 4;     // 命中未映射区间的比例
        bool useScript     = false; // 用定向脚本（覆盖率收尾时打开）
        bool lite          = false; // AXI-Lite 模式：只发单拍 4 字节，用来驱动 APB 桥
    };

    void reset(const Cfg& cfg, Rng* rng, uint32_t base, uint32_t size);
    void setScript(const std::vector<Txn>& t);
    bool done() const;
    // out=下一拍要驱动的输出；in=本拍总线快照（只读）
    void eval(Axi& out, const Axi& in, const Hs& hs);

    int txns()      const { return txns_; }
    int rejected()  const { return rejected_; }     // 约束丢弃次数
    int phase()     const { return (int)ph_; }       // 调试用
    int busyCycles()const { return busyCycles_; }   // 事务占用周期（含停顿）
    int stallSum()  const { return stallSum_; }
    std::vector<Txn> finished;                      // 已完成事务（给统计/定向判断用）

private:
    enum Ph { IDLE, AW, W, B, AR, R, FIN };
    Txn  genRandom();
    Txn  nextTxn();
    void driveW(Axi& a);

    Cfg      cfg_;
    Rng*     rng_ = nullptr;
    uint32_t base_ = 0, size_ = 0;
    Ph       ph_   = IDLE;
    Txn      cur_;
    int      beat_ = 0, rbeat_ = 0, gap_ = 0;
    int      stall_ = 0, txnStart_ = -1, cycle_ = 0;
    int      txns_ = 0, rejected_ = 0, busyCycles_ = 0, stallSum_ = 0;
    size_t   scriptPos_ = 0;
    std::vector<Txn> script_;
};

// =====================================================================
// 从设备（DUT）：带存储的 AXI4 从机，三个区间的响应不同
//   [base, base+0xF00)        正常存储，OKAY
//   [base+0xF00, base+0x1000) 故障寄存器，SLVERR
//   其余                       未映射，DECERR
// 另外可以注入三类时序 bug，用来验证 checker 本身有效
// =====================================================================
struct SlaveCfg {
    uint32_t base = 0x10000000, size = 0x1000;
    int  awLat = 1, wLat = 0, bLat = 2, arLat = 1, rLat = 1;
    bool randLat = true;
    int  latMax  = 3;
    bool bugBEarly    = false;   // 写响应早于 WLAST
    bool bugRLastEarly= false;   // RLAST 早一拍
    bool bugRDataFlip = false;   // 等待期篡改 RDATA
};

class AxiMemSlave {
public:
    void reset(const SlaveCfg& cfg, Rng* rng);
    void eval(Axi& out, const Axi& in, const Hs& hs);
    const std::map<uint32_t, uint8_t>& mem() const { return mem_; }
    // 调试用
    int dbgRs() const { return (int)rs_; }
    int dbgRbeat() const { return rbeat_; }
    int dbgArLen() const { return ar_len_; }
    int dbgWs() const { return (int)ws_; }
    int dbgBeat() const { return beat_; }

private:
    enum WS { W_IDLE, W_LAT, W_DATA, W_RESP };
    enum RS { R_IDLE, R_LAT, R_DATA };
    int  lat();
    void wr(uint32_t addr, uint64_t data, uint8_t strb, int size);
    uint64_t rd(uint32_t addr, int size);
    uint8_t  respFor(uint32_t addr) const;

    SlaveCfg cfg_;
    Rng*     rng_ = nullptr;
    std::map<uint32_t, uint8_t> mem_;

    WS ws_ = W_IDLE;  RS rs_ = R_IDLE;
    // 五个通道的延迟计数器必须分开：写通道和读通道是并发的
    int awlat_ = 0, wlat_ = 0, blat_ = 0, arlat_ = 0, rlat_ = 0;
    int beat_ = 0, rbeat_ = 0;
    uint32_t aw_addr_ = 0, ar_addr_ = 0;
    int aw_len_ = 0, ar_len_ = 0, aw_size_ = 2, ar_size_ = 2;
    uint64_t rdata_ = 0;
    uint8_t  bresp_ = 0, rresp_ = 0;
    bool     bpend_ = false;
};

// =====================================================================
// APB 寄存器从机：16 个 32 位寄存器
//   0x00..0x3C 普通读写；0x3C 是"等待控制器"，写入的值决定后续访问插入几拍 wait
// =====================================================================
struct ApbCfg {
    int  baseWait = 0;
    bool bugPreadyNoEnable = false;  // 没有 PENABLE 就拉 PREADY
    bool bugPsLverrAlways  = false;
};

class ApbRegSlave {
public:
    void reset(const ApbCfg& cfg, Rng* rng);
    void eval(Apb& out, const Apb& in);
    uint32_t reg(int i) const { return regs_[i]; }
    int waitStates() const { return lastWait_; }
private:
    ApbCfg cfg_;
    Rng*   rng_ = nullptr;
    uint32_t regs_[16] = {0};
    int  wait_ = 0, cnt_ = 0, lastWait_ = 0;
    bool active_ = false;
};

// =====================================================================
// AXI-Lite -> APB 桥（DUT）
//   AXI 侧只接受单拍传输（awlen/arlen 必须为 0），APB 侧走标准的
//   SETUP -> ACCESS（PENABLE 拉高、等 PREADY）两段式
// =====================================================================
struct BridgeCfg {
    bool bugPaddrChange = false;   // ACCESS 等待期改 PADDR
    bool bugPselDrop    = false;   // PREADY 前撤 PSEL
};

class Axi2ApbBridge {
public:
    void reset(const BridgeCfg& cfg);
    void eval(Axi& aout, Apb& pout, const Axi& ain, const Apb& pin, const Hs& hs);
    int xfers() const { return xfers_; }
    int waitSum() const { return waitSum_; }
private:
    enum St { IDLE, SETUP, ACCESS, RESP };
    BridgeCfg cfg_;
    St  st_ = IDLE;
    bool wr_ = true;
    bool hasAw_ = false, hasW_ = false;   // AXI 的 AW 和 W 是两条独立通道，可以不同拍到
    uint32_t addr_ = 0, wdata_ = 0, rdata_ = 0;
    uint8_t bresp_ = 0, rresp_ = 0;
    int  wait_ = 0, lastWait_ = 0;
    int  xfers_ = 0, waitSum_ = 0;
};

// =====================================================================
// 协议断言检查器：只看总线，不依赖主从设备内部状态
// =====================================================================
class Checker {
public:
    void reset();
    void onCycle(const Axi& a, const Apb& p, const Hs& hs, int cycle);
    const std::vector<Violation>& violations() const { return v_; }
    int count() const { return (int)v_.size(); }
    bool quiet() const { return v_.empty(); }
private:
    void violate(int cycle, const char* rule, const std::string& detail);
    Axi  pa_;      // 上一拍的 AXI 信号
    Apb  pp_;      // 上一拍的 APB 信号
    bool first_ = true;
    // APB：以 SETUP 拍的地址/写使能/写数据为基准，PSEL 期间全程比对
    bool     apbOn_ = false;
    uint32_t apbAddr_ = 0, apbWdata_ = 0;
    bool     apbWrite_ = false;
    // 事务级跟踪（用于拍数 / LAST 位置检查）
    bool     wrOpen_ = false, rdOpen_ = false;
    int      wrBeats_ = 0, rdBeats_ = 0, wrLen_ = 0, rdLen_ = 0;
    bool     wrGotLast_ = false, bSeen_ = false;
    int      bWait_ = 0;
    std::vector<Violation> v_;
};

// =====================================================================
// 参考模型 + scoreboard：自己维护一份存储，不偷看 DUT 内部
// =====================================================================
class Scoreboard {
public:
    void reset() { ref_.clear(); mismatches_ = 0; checked_ = 0; beats_ = 0; bad_ = ""; wrOpen_ = false; rdOpen_ = false; }
    void onCycle(const Axi& a, const Hs& hs, int cycle);
    int mismatches() const { return mismatches_; }
    int checked()    const { return checked_; }
    int beats()      const { return beats_; }
    const std::string& firstBad() const { return bad_; }
    const std::map<uint32_t, uint8_t>& ref() const { return ref_; }
private:
    std::map<uint32_t, uint8_t> ref_;
    int mismatches_ = 0, checked_ = 0, beats_ = 0;
    bool wrOpen_ = false, rdOpen_ = false;
    uint32_t wrAddr_ = 0, rdAddr_ = 0;
    int wrSize_ = 2, rdSize_ = 2, wrLen_ = 0, rdLen_ = 0, rb_ = 0;
    uint8_t wrStrb_ = 0xF;
    std::string bad_;
};

// =====================================================================
// 覆盖率：分组的 bin，采样点集中在 check.cpp
// =====================================================================
class Coverage {
public:
    void build(int maxBeats);
    void sampleTxn(bool write, int beats, int sizeBytes, int resp, int stall, uint8_t strb);
    void sampleApbWait(int wait);
    Cov& raw() { return cov_; }
private:
    Cov cov_;
};
