#pragma once
// =====================================================================
// axi-vip —— AXI4 / APB 协议验证平台（不依赖 EDA）
//
// 时间模型：周期级 + 寄存器化输出。
//   每一拍固定按这个顺序推进：
//     1) 用"当前驱动到总线上的信号"判定握手（valid && ready）
//     2) 观察者（checker / scoreboard / coverage）看这一拍发生了什么
//     3) 主设备、从设备各自根据握手结果算出"下一拍要驱动什么"
//   把输出寄存器化有好处也有代价：
//     好处 —— 握手不会出现同一拍内 valid/ready 互相等待的组合环，仿真确定；
//     代价 —— "READY 组合依赖 VALID" 这类只可能出现在组合逻辑里的 bug 没法注入，
//             所以本项目注入的 bug 都落在状态机/时序层面（B 提前返回、RLAST 位置错、
//             APB 等待态改地址等）。
// =====================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// ---------------- 总线信号 ----------------
struct Axi {
    // 写地址通道 AW
    bool     awvalid = false, awready = false;
    uint32_t awaddr  = 0;
    uint8_t  awlen   = 0, awsize = 2, awburst = 1;   // burst=1 表示 INCR
    // 写数据通道 W
    bool     wvalid = false, wready = false;
    uint64_t wdata  = 0;
    uint8_t  wstrb  = 0xF;
    bool     wlast  = false;
    // 写响应通道 B
    bool     bvalid = false, bready = true;
    uint8_t  bresp  = 0;
    // 读地址通道 AR
    bool     arvalid = false, arready = false;
    uint32_t araddr  = 0;
    uint8_t  arlen   = 0, arsize = 2, arburst = 1;
    // 读数据通道 R
    bool     rvalid = false, rready = true;
    uint64_t rdata  = 0;
    uint8_t  rresp  = 0;
    bool     rlast  = false;
};

struct Apb {
    bool     psel = false, penable = false, pwrite = false;
    uint32_t paddr = 0, pwdata = 0, prdata = 0;
    bool     pready = false, pslverr = false;
};

// 本拍发生的握手（valid && ready）
struct Hs {
    bool aw = false, w = false, b = false, ar = false, r = false;
    int  n() const { return aw + w + b + ar + r; }
};

// ---------------- 事务 ----------------
struct Txn {
    bool     write = true;
    uint32_t addr  = 0;
    int      len   = 0;          // beats - 1
    int      size  = 2;          // 2^size 字节
    uint8_t  strb  = 0xF;        // 每拍沿用同一个 strb 模式
    uint64_t data[16] = {0};
    int      resp  = 0;          // 从设备返回的响应（0 OKAY / 2 SLVERR / 3 DECERR）
    int      stall = 0;          // 观察到的地址通道停顿拍数
};

// ---------------- 随机数（xorshift，固定种子保证可复现）----------------
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 0x2545F4914F6CDD1Dull) : s(seed ? seed : 1) {}
    uint32_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint32_t)(s >> 11);
    }
    int  range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
    bool chance(int pct) { return (int)(next() % 100) < pct; }
    bool oneIn(int n) { return (next() % (uint32_t)n) == 0; }
};

// ---------------- 违规记录 ----------------
struct Violation {
    int         cycle;
    std::string rule;
    std::string detail;
};

// ---------------- 覆盖率 ----------------
struct CovBin {
    std::string group, name;
    int         hit = 0;
};

class Cov {
public:
    void add(const std::string& g, const std::string& n) {
        for (auto& b : bins_) if (b.group == g && b.name == n) return;
        bins_.push_back({g, n, 0});
    }
    void hit(const std::string& g, const std::string& n) {
        for (auto& b : bins_) if (b.group == g && b.name == n) { b.hit++; return; }
    }
    int total() const { return (int)bins_.size(); }
    int hitCount() const {
        int n = 0; for (auto& b : bins_) if (b.hit) n++;
        return n;
    }
    double percent() const { return total() ? 100.0 * hitCount() / total() : 0.0; }
    const std::vector<CovBin>& bins() const { return bins_; }
    std::vector<std::pair<std::string, std::string>> holes() const {
        std::vector<std::pair<std::string, std::string>> h;
        for (auto& b : bins_) if (!b.hit) h.push_back({b.group, b.name});
        return h;
    }
    void merge(const Cov& o) {
        for (auto& b : o.bins_) { add(b.group, b.name); for (int i = 0; i < b.hit; ++i) hit(b.group, b.name); }
    }
private:
    std::vector<CovBin> bins_;
};

// ---------------- 工具 ----------------
inline std::string hexs(uint64_t v, int width = 8) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%0*llx", width, (unsigned long long)v);
    return buf;
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
