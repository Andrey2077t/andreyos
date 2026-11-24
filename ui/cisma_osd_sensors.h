// Cisma OS - sensores do Display de desempenho (cisma-osd)
//
// So C++ padrao + Linux (/proc, /sys, statvfs). Sem Qt, sem root, sem bibliotecas extras.
// Foi separado do desenho (cisma_osd.cpp) para poder ser testado sozinho:
//
//   printf '#define CISMA_OSD_SENSORS_MAIN\n#include "ui/cisma_osd_sensors.h"\n' > /tmp/s.cpp
//   g++ -std=c++17 -O2 -Wall -Wextra -pthread -I. /tmp/s.cpp -o /tmp/sensores && /tmp/sensores
//   (imprime o que foi lido; -1 = sensor nao existe nesta maquina. `cisma-osd --dump` faz o mesmo.)
//
// Fontes dos dados:
//   CPU    uso: /proc/stat | frequencia: cpufreq (ou /proc/cpuinfo) | temperatura: hwmon coretemp/k10temp/zenpower
//   GPU    AMD: amdgpu em /sys/class/drm (gpu_busy_percent, pp_dpm_sclk, hwmon) | Intel: so frequencia
//          NVIDIA: `nvidia-smi` (driver proprietario), consultado em outra thread
//   RAM    /proc/meminfo (MemTotal - MemAvailable)
//   Disco  statvfs("/") ; tipo (SSD/HDD/NVMe) pelo /sys/class/block/*/queue/rotational
//   Rede   /proc/net/dev (soma das interfaces reais; ignora lo, docker, veth, br-, virbr)
//   FPS    o Linux nao tem um jeito universal e seguro de medir o FPS de outro programa. O cisma-osd le o valor
//          de um arquivo de texto (so um numero) que o jogo/camada grava, e so o considera se tiver sido
//          escrito nos ultimos 3 s:  $CISMA_OSD_FPS_FILE  ou  $XDG_RUNTIME_DIR/cisma-osd-fps
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits.h>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace osd {

// ---------------------------------------------------------------- utilidades
inline std::string readFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::string trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline bool startsWith(const std::string &s, const std::string &p) { return s.rfind(p, 0) == 0; }

inline bool readNum(const std::string &path, double &out) {
    const std::string s = trim(readFile(path));
    if (s.empty()) return false;
    char *end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    out = v;
    return true;
}

inline std::vector<std::string> listDir(const std::string &path) {
    std::vector<std::string> r;
    DIR *d = opendir(path.c_str());
    if (!d) return r;
    while (const dirent *e = readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") r.push_back(n);
    }
    closedir(d);
    std::sort(r.begin(), r.end());
    return r;
}

inline bool exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

inline std::string realPath(const std::string &path) {
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf)) return buf;
    return {};
}

inline std::string lastPart(const std::string &path) {
    const size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

inline std::string parentDir(const std::string &path) {
    const size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

// roda um comando fixo e devolve a saida (nunca recebe texto do usuario)
inline std::string runCmd(const char *cmd) {
    FILE *f = popen(cmd, "r");
    if (!f) return {};
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
    return out;
}

inline void replaceAll(std::string &s, const std::string &a, const std::string &b) {
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) {
        s.replace(p, a.size(), b);
        p += b.size();
    }
}

inline std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> r;
    std::string cur;
    for (const char c : s) {
        if (c == sep) { r.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    r.push_back(trim(cur));
    return r;
}

// "Intel(R) Core(TM) i5-4460  CPU @ 3.20GHz" -> "Intel Core i5-4460"
inline std::string cleanCpuName(std::string s) {
    for (const char *t : {"(R)", "(TM)", "(r)", "(tm)"}) replaceAll(s, t, "");
    const size_t at = s.find(" @");
    if (at != std::string::npos) s.erase(at);
    while (s.find("  ") != std::string::npos) replaceAll(s, "  ", " ");
    s = trim(s);
    for (const char *suf : {" CPU", " Processor"}) {
        const std::string x = suf;
        if (s.size() > x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0) s.erase(s.size() - x.size());
    }
    return s.empty() ? std::string("CPU") : s;
}

// ---------------------------------------------------------------- sensores
struct Sensors {
    // texto fixo (descoberto em init)
    std::string cpuName = "CPU";
    std::string gpuName = "GPU";
    std::string diskLabel = "Disco (/)";

    // valores vivos; -1 = esse sensor nao existe nesta maquina
    double cpuUse = 0;               // 0..1
    double cpuMHz = -1;
    double cpuTemp = -1;             // graus C
    double gpuUse = -1;              // 0..1
    double gpuMHz = -1;
    double gpuTemp = -1;
    double ramUsed = 0, ramTotal = 0;     // bytes
    double diskUsed = 0, diskTotal = 0;   // bytes
    double netDown = 0, netUp = 0;        // bytes/s
    double fps = -1;

    Sensors() = default;
    Sensors(const Sensors &) = delete;
    Sensors &operator=(const Sensors &) = delete;
    ~Sensors() {
        m_nvStop = true;
        if (m_nv.joinable()) m_nv.join();
    }

    void init() {
        initCpu();
        initGpu();
        initDisk();
        readCpuTimes(m_prevTotal, m_prevIdle);
        readNet(m_prevRx, m_prevTx);
        m_prevT = std::chrono::steady_clock::now();
        if (m_gpuKind == Gpu::Nvidia) startNvidia();
    }

    void update() {
        const auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - m_prevT).count();
        if (dt < 0.2) dt = 0.2;
        m_prevT = now;

        // CPU
        unsigned long long total = 0, idle = 0;
        if (readCpuTimes(total, idle) && total > m_prevTotal) {
            const double dTot = double(total - m_prevTotal);
            const double dIdle = double(idle >= m_prevIdle ? idle - m_prevIdle : 0);
            cpuUse = std::clamp(1.0 - dIdle / dTot, 0.0, 1.0);
        }
        m_prevTotal = total;
        m_prevIdle = idle;
        cpuMHz = readCpuMHz();
        cpuTemp = -1;
        double t = 0;
        if (!m_cpuTempPath.empty() && readNum(m_cpuTempPath, t)) cpuTemp = t / 1000.0;

        updateGpu();

        // RAM
        readMem();

        // disco
        struct statvfs sv;
        if (statvfs("/", &sv) == 0 && sv.f_blocks > 0) {
            const double fr = double(sv.f_frsize);
            const double used = double(sv.f_blocks - sv.f_bfree) * fr;
            diskUsed = used;
            diskTotal = used + double(sv.f_bavail) * fr;   // mesma conta do `df`
        }

        // rede
        unsigned long long rx = 0, tx = 0;
        if (readNet(rx, tx)) {
            netDown = double(rx >= m_prevRx ? rx - m_prevRx : 0) / dt;
            netUp = double(tx >= m_prevTx ? tx - m_prevTx : 0) / dt;
            m_prevRx = rx;
            m_prevTx = tx;
        }

        fps = readFps();
    }

    // valores de exemplo (para --grab / testes do desenho)
    void loadDemo() {
        cpuName = "Intel Core i5-4460";
        gpuName = "Radeon RX 550 4GB";
        diskLabel = "SSD (/)";
        cpuUse = 0.35; cpuMHz = 3200; cpuTemp = 57;
        gpuUse = 0.45; gpuMHz = 1071; gpuTemp = 62;
        ramTotal = 16.0 * 1073741824.0; ramUsed = 7.84 * 1073741824.0;
        diskTotal = 93.6 * 1073741824.0; diskUsed = 68.0 * 1073741824.0;
        netDown = 1200.0 * 1024; netUp = 90.0 * 1024;
        fps = 144;
    }

private:
    enum class Gpu { None, Amd, Nvidia, Intel };

    // cpu
    std::string m_cpuTempPath;
    std::vector<std::string> m_freqFiles;
    unsigned long long m_prevTotal = 0, m_prevIdle = 0;
    // gpu
    Gpu m_gpuKind = Gpu::None;
    std::string m_gpuBusy, m_gpuSclk, m_gpuTempPath, m_gpuFreqPath;
    std::thread m_nv;
    std::atomic<bool> m_nvStop{false};
    std::mutex m_nvMu;
    std::string m_nvLine;
    // rede
    unsigned long long m_prevRx = 0, m_prevTx = 0;
    std::chrono::steady_clock::time_point m_prevT = std::chrono::steady_clock::now();

    // ---------- CPU
    void initCpu() {
        std::istringstream in(readFile("/proc/cpuinfo"));
        std::string line;
        while (std::getline(in, line)) {
            if (startsWith(line, "model name")) {
                const size_t c = line.find(':');
                if (c != std::string::npos) cpuName = cleanCpuName(trim(line.substr(c + 1)));
                break;
            }
        }
        for (const std::string &n : listDir("/sys/devices/system/cpu")) {
            if (n.size() < 4 || !startsWith(n, "cpu") || !std::isdigit(static_cast<unsigned char>(n[3]))) continue;
            const std::string f = "/sys/devices/system/cpu/" + n + "/cpufreq/scaling_cur_freq";
            if (exists(f)) m_freqFiles.push_back(f);
        }
        findCpuTemp();
    }

    void findCpuTemp() {
        int best = 99;
        for (const std::string &hw : listDir("/sys/class/hwmon")) {
            const std::string base = "/sys/class/hwmon/" + hw;
            const std::string name = trim(readFile(base + "/name"));
            int prio;
            if (name == "coretemp" || name == "k10temp" || name == "zenpower") prio = 0;
            else if (name == "cpu_thermal" || name == "soc_thermal") prio = 1;
            else if (name == "acpitz") prio = 3;
            else continue;
            std::string chosen;
            for (int i = 1; i <= 16; ++i) {
                const std::string in = base + "/temp" + std::to_string(i) + "_input";
                if (!exists(in)) continue;
                const std::string lab = trim(readFile(base + "/temp" + std::to_string(i) + "_label"));
                if (chosen.empty()) chosen = in;
                if (lab == "Package id 0" || lab == "Tdie") { chosen = in; break; }
                if (lab == "Tctl") chosen = in;
            }
            if (!chosen.empty() && prio < best) { best = prio; m_cpuTempPath = chosen; }
        }
        if (best > 2) {
            for (const std::string &z : listDir("/sys/class/thermal")) {
                if (!startsWith(z, "thermal_zone")) continue;
                if (trim(readFile("/sys/class/thermal/" + z + "/type")) == "x86_pkg_temp") {
                    m_cpuTempPath = "/sys/class/thermal/" + z + "/temp";
                    break;
                }
            }
        }
    }

    static bool readCpuTimes(unsigned long long &total, unsigned long long &idle) {
        std::istringstream in(readFile("/proc/stat"));
        std::string tag;
        in >> tag;
        if (tag != "cpu") return false;
        unsigned long long v[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // user nice system idle iowait irq softirq steal
        for (int i = 0; i < 8; ++i) in >> v[i];
        total = 0;
        for (int i = 0; i < 8; ++i) total += v[i];
        idle = v[3] + v[4];
        return true;
    }

    double readCpuMHz() const {
        double sum = 0;
        int n = 0;
        for (const std::string &f : m_freqFiles) {
            double v = 0;
            if (readNum(f, v)) { sum += v / 1000.0; ++n; }
        }
        if (n > 0) return sum / n;
        std::istringstream in(readFile("/proc/cpuinfo"));   // VMs e maquinas sem cpufreq
        std::string line;
        while (std::getline(in, line)) {
            if (!startsWith(line, "cpu MHz")) continue;
            const size_t c = line.find(':');
            if (c == std::string::npos) continue;
            sum += std::atof(line.c_str() + c + 1);
            ++n;
        }
        return n > 0 ? sum / n : -1;
    }

    // ---------- GPU
    static std::string lspciName(const std::string &card) {
        // "0000:01:00.0" -> "01:00.0"; so passa se tiver apenas [0-9a-f:.] (vai para uma linha de comando)
        std::string slot = lastPart(realPath("/sys/class/drm/" + card + "/device"));
        if (slot.size() > 5 && slot[4] == ':') slot.erase(0, 5);
        if (slot.empty() || slot.find_first_not_of("0123456789abcdefABCDEF:.") != std::string::npos) return {};
        const std::string out = runCmd(("lspci -s " + slot + " -mm 2>/dev/null").c_str());
        // 01:00.0 "VGA compatible controller" "Fabricante" "Modelo [Nome]" ...
        std::vector<std::string> q;
        size_t p = 0;
        while ((p = out.find('"', p)) != std::string::npos) {
            const size_t e = out.find('"', p + 1);
            if (e == std::string::npos) break;
            q.push_back(out.substr(p + 1, e - p - 1));
            p = e + 1;
        }
        if (q.size() < 3) return {};
        std::string model = q[2];
        const size_t a = model.find('['), b = model.rfind(']');
        if (a != std::string::npos && b != std::string::npos && b > a + 1) model = model.substr(a + 1, b - a - 1);
        return trim(model);
    }

    void initGpu() {
        std::string card, vendor;
        int best = 99;
        for (const std::string &c : listDir("/sys/class/drm")) {
            if (!startsWith(c, "card") || c.find('-') != std::string::npos) continue;
            const std::string v = trim(readFile("/sys/class/drm/" + c + "/device/vendor"));
            const int prio = (v == "0x1002" || v == "0x10de") ? 0 : (v == "0x8086" ? 1 : 2);
            if (prio < best) { best = prio; card = c; vendor = v; }
        }
        if (card.empty()) return;
        const std::string dev = "/sys/class/drm/" + card + "/device";
        m_gpuKind = vendor == "0x1002" ? Gpu::Amd : vendor == "0x10de" ? Gpu::Nvidia : vendor == "0x8086" ? Gpu::Intel : Gpu::None;

        const std::string prod = trim(readFile(dev + "/product_name"));
        const std::string nm = prod.empty() ? lspciName(card) : prod;
        if (!nm.empty()) gpuName = nm;

        if (exists(dev + "/gpu_busy_percent")) m_gpuBusy = dev + "/gpu_busy_percent";
        if (exists(dev + "/pp_dpm_sclk")) m_gpuSclk = dev + "/pp_dpm_sclk";
        for (const std::string &h : listDir(dev + "/hwmon")) {
            const std::string t = dev + "/hwmon/" + h + "/temp1_input";
            if (exists(t)) { m_gpuTempPath = t; break; }
        }
        for (const std::string &p : {"/sys/class/drm/" + card + "/gt_cur_freq_mhz", dev + "/gt/gt0/rps_cur_freq_mhz"}) {
            if (exists(p)) { m_gpuFreqPath = p; break; }
        }
    }

    void startNvidia() {
        m_nv = std::thread([this] {
            while (!m_nvStop) {
                const std::string out = runCmd(
                    "nvidia-smi --query-gpu=name,utilization.gpu,temperature.gpu,clocks.sm "
                    "--format=csv,noheader,nounits 2>/dev/null");
                {
                    std::lock_guard<std::mutex> lk(m_nvMu);
                    m_nvLine = trim(out);
                }
                for (int i = 0; i < 20 && !m_nvStop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    void updateGpu() {
        gpuUse = gpuMHz = gpuTemp = -1;
        double v = 0;
        if (m_gpuKind == Gpu::Nvidia) {
            std::string line;
            {
                std::lock_guard<std::mutex> lk(m_nvMu);
                line = m_nvLine;
            }
            const std::vector<std::string> f = split(line, ',');
            if (f.size() >= 4) {
                if (!f[0].empty()) gpuName = f[0];
                if (std::isdigit(static_cast<unsigned char>(f[1].empty() ? 'x' : f[1][0]))) gpuUse = std::atof(f[1].c_str()) / 100.0;
                if (std::isdigit(static_cast<unsigned char>(f[2].empty() ? 'x' : f[2][0]))) gpuTemp = std::atof(f[2].c_str());
                if (std::isdigit(static_cast<unsigned char>(f[3].empty() ? 'x' : f[3][0]))) gpuMHz = std::atof(f[3].c_str());
            }
            return;
        }
        if (!m_gpuBusy.empty() && readNum(m_gpuBusy, v)) gpuUse = std::clamp(v / 100.0, 0.0, 1.0);
        if (!m_gpuTempPath.empty() && readNum(m_gpuTempPath, v)) gpuTemp = v / 1000.0;
        if (!m_gpuSclk.empty()) {                       // linha com '*' = frequencia atual: "1: 1071Mhz *"
            std::istringstream in(readFile(m_gpuSclk));
            std::string line;
            while (std::getline(in, line)) {
                if (line.find('*') == std::string::npos) continue;
                const size_t c = line.find(':');
                if (c != std::string::npos) gpuMHz = std::atof(line.c_str() + c + 1);
                break;
            }
        } else if (!m_gpuFreqPath.empty() && readNum(m_gpuFreqPath, v)) {
            gpuMHz = v;
        }
    }

    // ---------- RAM / disco / rede / FPS
    void readMem() {
        std::istringstream in(readFile("/proc/meminfo"));
        std::string key, unit;
        double val = 0, total = 0, avail = -1, freeKb = 0, buffers = 0, cached = 0;
        while (in >> key >> val >> unit) {
            if (key == "MemTotal:") total = val;
            else if (key == "MemAvailable:") avail = val;
            else if (key == "MemFree:") freeKb = val;
            else if (key == "Buffers:") buffers = val;
            else if (key == "Cached:") cached = val;
        }
        if (avail < 0) avail = freeKb + buffers + cached;   // kernels muito antigos
        ramTotal = total * 1024.0;
        ramUsed = std::max(0.0, total - avail) * 1024.0;
    }

    void initDisk() {
        std::istringstream in(readFile("/proc/mounts"));
        std::string line, root;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string src, mp;
            ls >> src >> mp;
            if (mp == "/") root = src;
        }
        if (!startsWith(root, "/dev/")) return;          // ISO ao vivo (overlay etc.): fica "Disco (/)"
        const std::string name = lastPart(realPath(root));
        const std::string sys = realPath("/sys/class/block/" + name);
        if (name.empty() || sys.empty()) return;
        const std::string disk = exists(sys + "/partition") ? lastPart(parentDir(sys)) : name;
        double rot = 0;
        if (readNum("/sys/class/block/" + disk + "/queue/rotational", rot))
            diskLabel = std::string(rot > 0.5 ? "HDD" : (startsWith(disk, "nvme") ? "NVMe" : "SSD")) + " (/)";
    }

    static bool readNet(unsigned long long &rx, unsigned long long &tx) {
        std::ifstream f("/proc/net/dev");
        if (!f) return false;
        std::string line;
        rx = tx = 0;
        int ln = 0;
        while (std::getline(f, line)) {
            if (++ln <= 2) continue;                      // cabecalho
            const size_t c = line.find(':');
            if (c == std::string::npos) continue;
            const std::string name = trim(line.substr(0, c));
            if (name == "lo" || startsWith(name, "docker") || startsWith(name, "veth") || startsWith(name, "br-") ||
                startsWith(name, "virbr"))
                continue;
            std::istringstream is(line.substr(c + 1));
            unsigned long long v[16] = {0};
            for (int i = 0; i < 16; ++i) is >> v[i];
            rx += v[0];                                   // bytes recebidos
            tx += v[8];                                   // bytes enviados
        }
        return true;
    }

    static double readFps() {
        std::string path;
        const char *e = std::getenv("CISMA_OSD_FPS_FILE");
        if (e && *e) {
            path = e;
        } else {
            const char *x = std::getenv("XDG_RUNTIME_DIR");
            if (!x || !*x) return -1;
            path = std::string(x) + "/cisma-osd-fps";
        }
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return -1;
        if (std::time(nullptr) - st.st_mtime > 3) return -1;   // desatualizado: nenhum jogo informando
        double v = 0;
        if (!readNum(path, v) || v < 0 || v > 10000) return -1;
        return v;
    }
};

inline void dump(const Sensors &s) {
    std::printf("cpu_name=%s\ncpu_use=%.1f%%\ncpu_mhz=%.0f\ncpu_temp=%.1f\n", s.cpuName.c_str(), s.cpuUse * 100,
                s.cpuMHz, s.cpuTemp);
    std::printf("gpu_name=%s\ngpu_use=%.1f%%\ngpu_mhz=%.0f\ngpu_temp=%.1f\n", s.gpuName.c_str(),
                s.gpuUse < 0 ? -1.0 : s.gpuUse * 100, s.gpuMHz, s.gpuTemp);
    std::printf("ram_used_gb=%.2f\nram_total_gb=%.2f\n", s.ramUsed / 1073741824.0, s.ramTotal / 1073741824.0);
    std::printf("disk_label=%s\ndisk_used_gb=%.2f\ndisk_total_gb=%.2f\n", s.diskLabel.c_str(),
                s.diskUsed / 1073741824.0, s.diskTotal / 1073741824.0);
    std::printf("net_down_kbs=%.1f\nnet_up_kbs=%.1f\nfps=%.0f\n", s.netDown / 1024.0, s.netUp / 1024.0, s.fps);
}

}  // namespace osd

#ifdef CISMA_OSD_SENSORS_MAIN
int main() {
    osd::Sensors s;
    s.init();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    s.update();
    osd::dump(s);
    return 0;
}
#endif
