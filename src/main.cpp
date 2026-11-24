#include "dv.h"
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#endif

// =====================================================================
// 回归入口：
//   [1] AXI4 内存从设备，约束随机
//   [2] 定向用例把覆盖率补到 100%
//   [3] AXI-Lite -> APB 桥
//   [4] 故障注入，反过来验证 checker 真的能抓
// =====================================================================

// 按"显示列宽"补空格：中文占两列，用 printf 的 %-Ns 会错位
static int disp_w(const char* s) {
    int w = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p < 0x80)                { w += 1; p += 1; }
        else if ((*p & 0xE0) == 0xC0) { w += 1; p += 2; }
        else if ((*p & 0xF0) == 0xE0) { w += 2; p += 3; }
        else                          { w += 2; p += 4; }
    }
    return w;
}
static void pad(const char* s, int w) {
    int d = disp_w(s), i;
    printf("%s", s);
    for (i = d; i < w; ++i) putchar(' ');
}

static void ensure_dir(const char* d) {
#ifdef _WIN32
    _mkdir(d);
#else
    mkdir(d, 0755);
#endif
}

// ---------------- 波形窗口 ----------------
struct Row {
    int      c = 0;
    bool     awv = false, awr = false, wv = false, wr = false, wl = false;
    bool     bv = false, br = false;
    bool     arv = false, arr = false, rv = false, rr = false, rl = false;
    uint32_t addr = 0;
    uint64_t data = 0;
};
struct PRow {
    int      c = 0;
    bool     psel = false, pen = false, pwr = false, prdy = false;
    uint32_t paddr = 0;
};

static int g_dirViol = 0, g_dirMis = 0, g_dirTxns = 0;
// 种子偏移：--seed 会在这里叠加，让同一套激励在不同种子下重新随机，
// 但定向段和 bug 注入段保持固定 —— 那两段要的是可复现，不是随机。
static uint64_t g_seedOff = 0;
static uint64_t seeded(uint64_t base) { return base ^ (g_seedOff * 0x9E3779B97F4A7C15ull); }

static std::vector<Row>  g_wave;
static std::vector<PRow> g_pwave;

static void capAxi(const Axi& a, int cyc) {
    Row r;
    r.c = cyc;
    r.awv = a.awvalid; r.awr = a.awready;
    r.wv = a.wvalid;   r.wr = a.wready;  r.wl = a.wlast;
    r.bv = a.bvalid;   r.br = a.bready;
    r.arv = a.arvalid; r.arr = a.arready;
    r.rv = a.rvalid;   r.rr = a.rready;  r.rl = a.rlast;
    r.addr = a.awvalid ? a.awaddr : a.araddr;
    r.data = a.wvalid ? a.wdata : a.rdata;
    g_wave.push_back(r);
}
static void capApb(const Apb& p, int cyc) {
    PRow r;
    r.c = cyc; r.psel = p.psel; r.pen = p.penable;
    r.pwr = p.pwrite; r.prdy = p.pready; r.paddr = p.paddr;
    g_pwave.push_back(r);
}

// ---------------- 一次 AXI 回归 ----------------
struct AxiResult {
    int cycles = 0, writes = 0, reads = 0;
    int violations = 0;
    int masterTxns = 0, masterPhase = -1;
    int mismatches = 0, compared = 0;
    int rejected = 0;
    double avgCyc = 0, avgStall = 0;
    std::string firstRule, firstDetail;
    int firstCycle = -1;
    std::string sbBad;
};

static AxiResult runAxi(const AxiMaster::Cfg& mcfg, const SlaveCfg& scfg,
                        int maxCycles, Coverage* cov, int waveStart = -1, int waveLen = 0) {
    Rng rng(seeded(0xA5A5F00Dull));
    AxiMaster mst;
    AxiMemSlave slv;
    Checker chk;
    Scoreboard sb;

    mst.reset(mcfg, &rng, scfg.base, scfg.size);
    slv.reset(scfg, &rng);
    chk.reset();
    sb.reset();

    Axi a;
    Hs  hs;
    int seen = 0;
    AxiResult res;

    for (int cyc = 0; cyc < maxCycles; ++cyc) {
        // 本拍总线快照：所有观察者和两个设备都只看这一份，
        // 谁也不许读对方"刚为下一拍写好"的输出
        const Axi bus = a;
        hs.aw = bus.awvalid && bus.awready;
        hs.w  = bus.wvalid  && bus.wready;
        hs.b  = bus.bvalid  && bus.bready;
        hs.ar = bus.arvalid && bus.arready;
        hs.r  = bus.rvalid  && bus.rready;

        chk.onCycle(bus, Apb(), hs, cyc);
        sb.onCycle(bus, hs, cyc);

        if (cov) {
            for (size_t i = seen; i < mst.finished.size(); ++i) {
                const Txn& t = mst.finished[i];
                cov->sampleTxn(t.write, t.len + 1, 1 << t.size, t.resp, t.stall, t.strb);
            }
            seen = (int)mst.finished.size();
        }

        // waveStart < 0 表示"从第一次握手开始截"，避免波形前面一大段全空闲
        if (waveStart < 0 && hs.n() && g_wave.empty()) waveStart = cyc;
        if (waveStart >= 0 && cyc >= waveStart && (int)g_wave.size() < waveLen) capAxi(bus, cyc);

        Axi nxt = a;
        slv.eval(nxt, bus, hs);
        mst.eval(nxt, bus, hs);
        a = nxt;

        res.cycles = cyc + 1;
        if (mst.done() && !hs.n() && !a.awvalid && !a.arvalid && !a.wvalid) break;
    }

    res.masterTxns = mst.txns();
    res.masterPhase = mst.phase();
    res.violations = chk.count();
    if (res.violations) {
        res.firstRule = chk.violations()[0].rule;
        res.firstDetail = chk.violations()[0].detail;
        res.firstCycle = chk.violations()[0].cycle;
    }
    res.mismatches = sb.mismatches();
    res.sbBad = sb.firstBad();
    res.compared = sb.checked();
    res.rejected = mst.rejected();
    for (const Txn& t : mst.finished) { if (t.write) ++res.writes; else ++res.reads; }
    if (!mst.finished.empty()) {
        res.avgCyc = (double)mst.busyCycles() / (double)mst.finished.size();
        res.avgStall = (double)mst.stallSum() / (double)mst.finished.size();
    }
    return res;
}

static inline bool bus_p_ok(const Apb& p) { return p.psel && p.penable && p.pready; }

// ---------------- 一次 APB 回归 ----------------
struct ApbResult {
    int cycles = 0, violations = 0, xfers = 0, waitSum = 0;
    double avgWait = 0;
    std::string firstRule, firstDetail;
    int firstCycle = -1;
};

static ApbResult runApb(const AxiMaster::Cfg& mcfg, const BridgeCfg& bcfg, const ApbCfg& acfg,
                        int maxCycles, Coverage* cov, int waveStart = -1, int waveLen = 0) {
    Rng rng(seeded(0x5EED1234ull));
    AxiMaster mst;
    Axi2ApbBridge br;
    ApbRegSlave  reg;
    Checker chk;

    mst.reset(mcfg, &rng, 0, 0x40);
    br.reset(bcfg);
    reg.reset(acfg, &rng);
    chk.reset();

    Axi a;
    Apb p;
    Hs  hs;
    ApbResult res;
    int seen = 0;

    for (int cyc = 0; cyc < maxCycles; ++cyc) {
        const Axi bus = a;
        const Apb pbus = p;
        hs.aw = bus.awvalid && bus.awready;
        hs.w  = bus.wvalid  && bus.wready;
        hs.b  = bus.bvalid  && bus.bready;
        hs.ar = bus.arvalid && bus.arready;
        hs.r  = bus.rvalid  && bus.rready;

        chk.onCycle(bus, pbus, hs, cyc);

        if (cov) {
            for (size_t i = seen; i < mst.finished.size(); ++i) {
                const Txn& t = mst.finished[i];
                cov->sampleTxn(t.write, t.len + 1, 1 << t.size, t.resp, t.stall, t.strb);
            }
            seen = (int)mst.finished.size();
        }
        if (waveStart < 0 && (pbus.psel || hs.n()) && g_pwave.empty()) waveStart = cyc;
        if (waveStart >= 0 && cyc >= waveStart && (int)g_pwave.size() < waveLen) capApb(pbus, cyc);

        Axi anext = a; Apb pnext = p;
        br.eval(anext, pnext, bus, pbus, hs);
        reg.eval(pnext, pnext);   // PREADY 是组合应答，必须看桥这一拍刚驱动的 PSEL/PENABLE
        mst.eval(anext, bus, hs);
        a = anext; p = pnext;

        // APB 传输完成（PSEL & PENABLE & PREADY）时，记录从设备插入的等待拍数。
        // 注意统计的是"从设备视角"的 wait state，不含桥自身取样 PREADY 的那一拍。
        if (bus_p_ok(pbus)) {
            int w = reg.waitStates();
            ++res.xfers;
            res.waitSum += w;
            if (cov) cov->sampleApbWait(w);
        }

        res.cycles = cyc + 1;
        if (mst.done() && !hs.n()) break;
    }

    res.violations = chk.count();
    if (res.violations) {
        res.firstRule = chk.violations()[0].rule;
        res.firstDetail = chk.violations()[0].detail;
        res.firstCycle = chk.violations()[0].cycle;
    }
    res.avgWait = res.xfers ? (double)res.waitSum / res.xfers : 0.0;
    return res;
}

// ---------------- 定向用例 ----------------
static std::vector<Txn> directedScript(uint32_t base) {
    std::vector<Txn> s;
    auto mk = [](bool w, uint32_t a, int len, int size, uint8_t strb) {
        Txn t; t.write = w; t.addr = a; t.len = len; t.size = size; t.strb = strb;
        for (int i = 0; i <= len; ++i) t.data[i] = 0xD00D0000ull + (uint64_t)i * 0x0101 + a;
        return t;
    };
    s.push_back(mk(true,  base + 0x100, 15, 3, 0xFF));   // 16 拍 x 8B 写
    s.push_back(mk(false, base + 0x100, 15, 3, 0xFF));   // 16 拍 x 8B 读
    s.push_back(mk(true,  base + 0x200, 7,  2, 0x5));    // 8 拍 x 4B 部分字节写
    s.push_back(mk(false, base + 0x200, 7,  2, 0xF));    // 8 拍 x 4B 读
    s.push_back(mk(true,  base + 0x300, 1,  1, 0x1));    // 2 拍 x 2B 写
    s.push_back(mk(false, base + 0x300, 1,  1, 0xF));    // 2 拍 x 2B 读
    s.push_back(mk(true,  base + 0x400, 3,  0, 0xF));    // 4 拍 x 1B 写
    s.push_back(mk(false, base + 0x400, 3,  0, 0xF));    // 4 拍 x 1B 读
    s.push_back(mk(true,  base + 0xF00, 0,  2, 0xF));    // SLVERR 写
    s.push_back(mk(false, base + 0xF00, 0,  2, 0xF));    // SLVERR 读
    s.push_back(mk(true,  base + 0x2000, 0, 2, 0xF));    // DECERR 写
    s.push_back(mk(false, base + 0x2000, 0, 2, 0xF));    // DECERR 读
    return s;
}

// ---------------- 主流程 ----------------
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const char* outdir = "out";
    bool jsonOnly = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--json") == 0) { jsonOnly = true; continue; }
        // --dbg / --verbose 在后面单独扫一遍，这里只放行
        if (strcmp(a, "--dbg") == 0 || strcmp(a, "--verbose") == 0) continue;
        if (strcmp(a, "--out") == 0) {
            // 缺参数时不能静默落回默认 out：脚本里 `--out $DIR` 变量为空会写进当前目录还不报错
            if (i + 1 >= argc || argv[i + 1][0] == '-' || argv[i + 1][0] == '\0') {
                fprintf(stderr, "error: --out 需要一个目录路径\n");
                fprintf(stderr, "usage: axi_vip [--out dir] [--json] [--seed N] [--dbg] [--verbose]\n");
                return 2;
            }
            outdir = argv[++i];
            continue;
        }
        if (strcmp(a, "--seed") == 0) {
            // --seed 必须真的改变种子：以前缺参数或写错（abс/空变量）会被静默忽略，
            // 于是"4 个种子各跑一遍都过"其实是同一个种子跑了 4 次，等于假回归
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --seed 需要一个非负整数\n");
                fprintf(stderr, "usage: axi_vip [--out dir] [--json] [--seed N]\n");
                return 2;
            }
            const char* s = argv[++i];
            char* end = nullptr;
            errno = 0;
            unsigned long long v = strtoull(s, &end, 10);
            if (end == s || *end != '\0' || s[0] == '-' || errno == ERANGE) {
                fprintf(stderr, "error: --seed 需要一个非负整数（当前 \"%s\"）\n", s);
                return 2;
            }
            g_seedOff = (uint64_t)v;
            continue;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printf("usage: axi_vip [--out dir] [--json] [--seed N] [--dbg] [--verbose]\n"
                   "  --out dir  波形/汇总输出目录（默认 out）\n"
                   "  --json     只输出一行 JSON 汇总，便于 CI 断言\n"
                   "  --seed N   叠加种子偏移，换种子重新随机（不影响定向段与 bug 注入段）\n");
            return 0;
        }
        // 拼错的选项（--jsno）以前被静默忽略，等于跑了一遍默认回归还退 0，CI 看着是绿的
        fprintf(stderr, "error: %s \"%s\"\n",
                a[0] == '-' ? "未知选项" : "多余的参数", a);
        fprintf(stderr, "usage: axi_vip [--out dir] [--json] [--seed N] [--dbg] [--verbose]\n");
        return 2;
    }
    ensure_dir(outdir);

    // 调试：--dbg 打印前 N 拍的信号变化
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dbg") == 0) {
            Rng rng(0xA5A5F00Dull);
            AxiMaster mst; AxiMemSlave slv;
            AxiMaster::Cfg c; c.maxTxns = 5; c.gapMax = 3;
            SlaveCfg s2; s2.latMax = 8; s2.awLat = 0; s2.arLat = 0;
            mst.reset(c, &rng, s2.base, s2.size);
            slv.reset(s2, &rng);
            Axi a; Hs hs;
            for (int cyc = 0; cyc < 140; ++cyc) {
                const Axi bus = a;
                hs.aw = bus.awvalid && bus.awready; hs.w = bus.wvalid && bus.wready;
                hs.b = bus.bvalid && bus.bready;    hs.ar = bus.arvalid && bus.arready;
                hs.r = bus.rvalid && bus.rready;
                printf("%3d AW %d%d len=%d ws=%d b=%d | W %d%d last=%d | B %d%d | AR %d%d | R %d%d last=%d rs=%d rb=%d alen=%d\n",
                       cyc, a.awvalid, a.awready, a.awlen, slv.dbgWs(), slv.dbgBeat(),
                       a.wvalid, a.wready, a.wlast, a.bvalid, a.bready,
                       a.arvalid, a.arready, a.rvalid, a.rready, a.rlast,
                       slv.dbgRs(), slv.dbgRbeat(), slv.dbgArLen());
                Axi nxt = a;
                slv.eval(nxt, bus, hs);
                mst.eval(nxt, bus, hs);
                a = nxt;
            }
            return 0;
        }
    }

    Coverage cov;
    cov.build(16);

    AxiMaster::Cfg mcfg;
    mcfg.maxTxns = 220;
    SlaveCfg scfg;
    scfg.base = 0x10000000;
    scfg.size = 0x1000;
    scfg.awLat = 0; scfg.arLat = 0;
    scfg.bLat = 2; scfg.rLat = 1;
    scfg.latMax = 8;

    // ---- [1] 随机 ----
    AxiResult r1 = runAxi(mcfg, scfg, 20000, &cov, -1, 120);
    char cov1[32];
    snprintf(cov1, sizeof(cov1), "%d/%d (%.1f%%)", cov.raw().hitCount(), cov.raw().total(), cov.raw().percent());

    // ---- [2] 定向补齐 ----
    AxiMaster::Cfg mcfg2 = mcfg;
    mcfg2.useScript = true;
    mcfg2.maxTxns = 0;
    // 定向跑：脚本直接塞进 master
    {
        Rng rng(0x11ull);
        AxiMaster mst; AxiMemSlave slv; Checker chk; Scoreboard sb;
        mst.reset(mcfg2, &rng, scfg.base, scfg.size);
        mst.setScript(directedScript(scfg.base));
        slv.reset(scfg, &rng); chk.reset(); sb.reset();
        Axi a; Hs hs; int seen = 0;
        int dirViol = 0, dirMis = 0;
        for (int cyc = 0; cyc < 20000; ++cyc) {
            const Axi bus = a;
            hs.aw = bus.awvalid && bus.awready; hs.w = bus.wvalid && bus.wready;
            hs.b = bus.bvalid && bus.bready;    hs.ar = bus.arvalid && bus.arready;
            hs.r = bus.rvalid && bus.rready;
            chk.onCycle(bus, Apb(), hs, cyc);
            sb.onCycle(bus, hs, cyc);
            for (size_t i = seen; i < mst.finished.size(); ++i) {
                const Txn& t = mst.finished[i];
                cov.sampleTxn(t.write, t.len + 1, 1 << t.size, t.resp, t.stall, t.strb);
            }
            seen = (int)mst.finished.size();
            Axi nxt = a; slv.eval(nxt, bus, hs); mst.eval(nxt, bus, hs); a = nxt;
            if (mst.done() && !hs.n()) break;
        }
        g_dirViol = chk.count();
        g_dirMis  = sb.mismatches();
        g_dirTxns = (int)mst.finished.size();
        (void)dirViol; (void)dirMis;
    }
    char cov2[32];
    snprintf(cov2, sizeof(cov2), "%d/%d (%.1f%%)", cov.raw().hitCount(), cov.raw().total(), cov.raw().percent());

    // ---- [3] APB 桥 ----
    AxiMaster::Cfg lcfg;
    lcfg.lite = true;
    lcfg.maxTxns = 120;
    lcfg.gapMax = 2;
    BridgeCfg bcfg;
    ApbCfg acfg;
    ApbResult r3 = runApb(lcfg, bcfg, acfg, 20000, &cov, -1, 100);

    // APB 定向：先写等待寄存器，再连续访问，制造 3+ 拍 wait
    ApbResult r3d;
    {
        Rng rng(0x22ull);
        AxiMaster mst; Axi2ApbBridge br; ApbRegSlave reg; Checker chk;
        AxiMaster::Cfg c = lcfg; c.useScript = true;
        mst.reset(c, &rng, 0, 0x40);
        // 定向：先写"等待发生器" 0x3C，再访问，把 0 拍 / 1-2 拍 / 3+ 拍三种等待态都覆盖到
        std::vector<Txn> sc;
        auto mkT = [](bool w, uint32_t a, uint32_t d) {
            Txn t; t.write = w; t.addr = a; t.len = 0; t.size = 2; t.strb = 0xF; t.data[0] = d;
            return t;
        };
        sc.push_back(mkT(true, 0x3C, 0));      // 0 拍 wait
        sc.push_back(mkT(true, 0x00, 0x1111));
        sc.push_back(mkT(false, 0x00, 0));
        sc.push_back(mkT(true, 0x3C, 4));      // 3+ 拍 wait
        sc.push_back(mkT(true, 0x04, 0xBEEF));
        sc.push_back(mkT(false, 0x04, 0));
        sc.push_back(mkT(true, 0x3C, 1));      // 1-2 拍 wait
        sc.push_back(mkT(false, 0x08, 0));
        sc.push_back(mkT(true, 0x3C, 0));      // 再回 0 拍
        sc.push_back(mkT(false, 0x0C, 0));
        mst.setScript(sc);
        br.reset(bcfg); reg.reset(acfg, &rng); chk.reset();
        Axi a; Apb p; Hs hs; int seen = 0;
        for (int cyc = 0; cyc < 20000; ++cyc) {
            const Axi bus = a; const Apb pbus = p;
            hs.aw = bus.awvalid && bus.awready; hs.w = bus.wvalid && bus.wready;
            hs.b = bus.bvalid && bus.bready;    hs.ar = bus.arvalid && bus.arready;
            hs.r = bus.rvalid && bus.rready;
            chk.onCycle(bus, pbus, hs, cyc);
            for (size_t i = seen; i < mst.finished.size(); ++i) {
                const Txn& tt = mst.finished[i];
                cov.sampleTxn(tt.write, tt.len + 1, 1 << tt.size, tt.resp, tt.stall, tt.strb);
            }
            seen = (int)mst.finished.size();
            Axi anext = a; Apb pnext = p;
            br.eval(anext, pnext, bus, pbus, hs); reg.eval(pnext, pnext); mst.eval(anext, bus, hs);
            a = anext; p = pnext;
            if (bus_p_ok(pbus)) { ++r3d.xfers; r3d.waitSum += reg.waitStates(); cov.sampleApbWait(reg.waitStates()); }
            if (mst.done() && !hs.n()) break;
        }
        r3d.violations = chk.count();
        r3d.avgWait = r3d.xfers ? (double)r3d.waitSum / r3d.xfers : 0.0;
        r3d.cycles = 0;
        if (chk.count()) { r3d.firstRule = chk.violations()[0].rule; r3d.firstDetail = chk.violations()[0].detail; r3d.firstCycle = chk.violations()[0].cycle; }
    }

    // ---- [4] 故障注入 ----
    struct BugCase {
        const char* name; const char* where; const char* expect;
        int caughtCycle; std::string rule; int count;
    };
    std::vector<BugCase> bugs;

    auto tryBug = [&](const char* name, const char* where, const char* expect,
                      SlaveCfg s, BridgeCfg b, ApbCfg ac) {
        BugCase bc; bc.name = name; bc.where = where; bc.expect = expect;
        bc.caughtCycle = -1; bc.count = 0;
        if (b.bugPaddrChange || b.bugPselDrop || ac.bugPreadyNoEnable) {
            ApbResult r = runApb(lcfg, b, ac, 3000, nullptr);
            bc.count = r.violations; bc.caughtCycle = r.firstCycle; bc.rule = r.firstRule;
        } else {
            AxiResult r = runAxi(mcfg, s, 3000, nullptr);
            bc.count = r.violations; bc.caughtCycle = r.firstCycle; bc.rule = r.firstRule;
        }
        bugs.push_back(bc);
    };

    SlaveCfg sBE = scfg;   sBE.bugBEarly = true;
    SlaveCfg sRL = scfg;   sRL.bugRLastEarly = true;
    SlaveCfg sRD = scfg;   sRD.bugRDataFlip = true;
    BridgeCfg bPA = bcfg;  bPA.bugPaddrChange = true;
    BridgeCfg bPS = bcfg;  bPS.bugPselDrop = true;
    ApbCfg    aPN = acfg;  aPN.bugPreadyNoEnable = true;

    tryBug("写响应早于 WLAST",      "slave.bugBEarly",       "AXI-B-EARLY",      sBE, BridgeCfg(), ApbCfg());
    tryBug("RLAST 早一拍",          "slave.bugRLastEarly",   "AXI-RLAST",        sRL, BridgeCfg(), ApbCfg());
    tryBug("R 数据等待期翻转",      "slave.bugRDataFlip",    "AXI-R-STABLE",     sRD, BridgeCfg(), ApbCfg());
    tryBug("APB 等待期改地址",      "bridge.bugPaddrChange", "APB-STABLE",       scfg, bPA, ApbCfg());
    tryBug("APB 未应答就撤 PSEL",   "bridge.bugPselDrop",    "APB-SETUP",        scfg, bPS, ApbCfg());
    tryBug("APB 未 PENABLE 就 PREADY","slave.bugPreadyNoEnable","APB-PREADY",    scfg, BridgeCfg(), aPN);

    // ---- 输出 ----
    if (jsonOnly) {
        printf("{\"writes\":%d,\"reads\":%d,\"cycles\":%d,\"violations\":%d,\"mismatches\":%d,"
               "\"rejected\":%d,\"apbXfers\":%d,\"coverage\":\"%d/%d\"}\n",
               r1.writes, r1.reads, r1.cycles, r1.violations, r1.mismatches,
               r1.rejected, r3.xfers, cov.raw().hitCount(), cov.raw().total());
    }

    char covNow[48];
    snprintf(covNow, sizeof(covNow), "%d/%d (%.1f%%)",
             cov.raw().hitCount(), cov.raw().total(), cov.raw().percent());

    if (!jsonOnly) {
        printf("axi-vip - AXI4 / APB 协议验证平台\n");
        printf("随机种子偏移：%llu%s\n", (unsigned long long)g_seedOff,
               g_seedOff ? "" : "（默认）");
        printf("周期级模型，无 EDA 依赖；checker / scoreboard / coverage 全部只看总线\n\n");

        printf("[1] AXI4 内存从设备 · 约束随机\n");
        printf("    完成 %d 笔（写 %d / 读 %d），仿真 %d 周期\n",
               r1.writes + r1.reads, r1.writes, r1.reads, r1.cycles);
        printf("    平均每笔 %0.2f 周期，地址通道平均停顿 %0.2f 拍\n", r1.avgCyc, r1.avgStall);
        printf("    生成器约束丢弃 %d 个样本（4KB 越界）\n", r1.rejected);
        printf("    协议违规 %d    参考模型比对 %d 拍，失配 %d\n", r1.violations, r1.compared, r1.mismatches);
        printf("    覆盖率 %s（随机阶段）\n", cov1);

        bool verbose = false;
        for (int i = 1; i < argc; ++i) if (strcmp(argv[i], "--verbose") == 0) verbose = true;
        if (verbose && !r1.firstRule.empty())
            printf("    首条违规: c=%d %s | %s\n", r1.firstCycle, r1.firstRule.c_str(), r1.firstDetail.c_str());
        if (verbose && !r1.sbBad.empty())
            printf("    首次失配: %s\n", r1.sbBad.c_str());

        printf("\n[2] 定向用例（覆盖率收尾）\n");
        printf("    %d 笔定向：16 拍 x 8B、16 拍 x 1B、部分 WSTRB、SLVERR / DECERR 读写\n", g_dirTxns);
        printf("    协议违规 %d，失配 %d\n", g_dirViol, g_dirMis);

        printf("\n[3] AXI-Lite -> APB 桥\n");
        printf("    随机段 %d 次传输，平均 APB 等待 %0.2f 拍，协议违规 %d\n",
               r3.xfers, r3.avgWait, r3.violations);
        printf("    定向段 %d 次传输（写 0x3C 制造 wait state），平均等待 %0.2f 拍，违规 %d\n",
               r3d.xfers, r3d.avgWait, r3d.violations);
        if (verbose && !r3.firstRule.empty())
            printf("    首条违规: c=%d %s | %s\n", r3.firstCycle, r3.firstRule.c_str(), r3.firstDetail.c_str());

        auto holes = cov.raw().holes();
        printf("\n[4] 功能覆盖率\n");
        printf("    随机阶段 %s  →  全流程 %s", cov1, covNow);
        if (holes.empty()) printf("   —— 无缺口\n");
        else {
            printf("   剩余缺口 %d 个：\n", (int)holes.size());
            for (auto& h : holes) printf("      - %s / %s\n", h.first.c_str(), h.second.c_str());
        }

        printf("\n[5] 故障注入 —— 反过来验证 checker 是否真的有效\n");
        printf("    "); pad("注入的 bug", 26); pad("注入位置", 26); pad("检出周期", 10);
        pad("命中断言", 18); printf("报出次数\n");
        printf("    %s\n", std::string(96, '-').c_str());
        for (auto& b : bugs) {
            char cyc[16];
            if (b.caughtCycle >= 0) snprintf(cyc, sizeof(cyc), "c=%d", b.caughtCycle);
            else snprintf(cyc, sizeof(cyc), "未检出");
            printf("    "); pad(b.name, 26); pad(b.where, 26); pad(cyc, 10);
            pad(b.rule.c_str(), 18); printf("%d 处\n", b.count);
        }
        printf("\n");
    }

    // ---- JSON ----
    char path[512];
    snprintf(path, sizeof(path), "%s/report.json", outdir);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return 1; }

    fprintf(f, "{\n");
    fprintf(f, " \"summary\":{\"cycles\":%d,\"writes\":%d,\"reads\":%d,\"violations\":%d,"
               "\"mismatches\":%d,\"compared\":%d,\"rejected\":%d,\"avgCyc\":%0.3f,\"avgStall\":%0.3f,\n",
            r1.cycles, r1.writes, r1.reads, r1.violations, r1.mismatches, r1.compared,
            r1.rejected, r1.avgCyc, r1.avgStall);
    fprintf(f, "   \"apb\":{\"xfers\":%d,\"avgWait\":%0.3f,\"violations\":%d,"
               "\"directedXfers\":%d,\"directedAvgWait\":%0.3f}},\n",
            r3.xfers, r3.avgWait, r3.violations, r3d.xfers, r3d.avgWait);

    fprintf(f, " \"coverage\":[");
    {
        const auto& bins = cov.raw().bins();
        for (size_t i = 0; i < bins.size(); ++i)
            fprintf(f, "%s{\"g\":\"%s\",\"n\":\"%s\",\"h\":%d}", i ? "," : "",
                    bins[i].group.c_str(), bins[i].name.c_str(), bins[i].hit);
    }
    fprintf(f, "],\n");

    fprintf(f, " \"bugs\":[");
    for (size_t i = 0; i < bugs.size(); ++i)
        fprintf(f, "%s{\"name\":\"%s\",\"where\":\"%s\",\"cycle\":%d,\"rule\":\"%s\",\"count\":%d}",
                i ? "," : "", bugs[i].name, bugs[i].where, bugs[i].caughtCycle,
                bugs[i].rule.c_str(), bugs[i].count);
    fprintf(f, "],\n");

    fprintf(f, " \"wave\":[");
    for (size_t i = 0; i < g_wave.size(); ++i) {
        const Row& r = g_wave[i];
        fprintf(f, "%s{\"c\":%d,\"awv\":%d,\"awr\":%d,\"wv\":%d,\"wr\":%d,\"wl\":%d,"
                   "\"bv\":%d,\"br\":%d,\"arv\":%d,\"arr\":%d,\"rv\":%d,\"rr\":%d,\"rl\":%d,"
                   "\"a\":%u,\"d\":\"%llx\"}",
                i ? "," : "", r.c, (int)r.awv, (int)r.awr, (int)r.wv, (int)r.wr, (int)r.wl,
                (int)r.bv, (int)r.br, (int)r.arv, (int)r.arr, (int)r.rv, (int)r.rr, (int)r.rl,
                r.addr, (unsigned long long)r.data);
    }
    fprintf(f, "],\n");

    fprintf(f, " \"pwave\":[");
    for (size_t i = 0; i < g_pwave.size(); ++i) {
        const PRow& r = g_pwave[i];
        fprintf(f, "%s{\"c\":%d,\"psel\":%d,\"pen\":%d,\"pwr\":%d,\"prdy\":%d,\"a\":%u}",
                i ? "," : "", r.c, (int)r.psel, (int)r.pen, (int)r.pwr, (int)r.prdy, r.paddr);
    }
    fprintf(f, "]\n}\n");
    fclose(f);
    if (!jsonOnly) printf("trace 已写入 %s/report.json\n", outdir);
    return 0;
}
