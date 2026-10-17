// latte2msl: offline Latte (R600/R700) shader -> MSL translation for the native renderer.
//
//   latte2msl --cemu runtime/cemu --out <dir> <transferable cache .bin>...
//
// Reads every shader in the given Cemu transferable shader caches (inventory.cpp), translates the
// vertex and pixel shaders with our own decoder/emitter (r600.cpp, msl.cpp), and writes
//   <dir>/msl/<vs|ps>_<base>_<aux>.metal   one translation per cache entry
//   <dir>/manifest.tsv                     stage, hashes, status, bindings, baked register state
//   <dir>/raw/...                          the raw programs (for regression tests)
// Compile + report with tools/shaders/compile_all.py.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <fstream>
#include <map>
#include <set>
#include <string>

#include "inventory.h"
#include "msl.h"

static void mkdirs(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
        }
        if (i < p.size()) cur += p[i];
    }
}

static void write_file(const std::string& path, const void* d, size_t n) {
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)d, (std::streamsize)n);
}

int main(int argc, char** argv) {
    std::string cemu = "runtime/cemu", out = "build-native/shaders";
    std::vector<std::string> caches;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--cemu" && i + 1 < argc) cemu = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else caches.push_back(a);
    }
    if (caches.empty()) {
        fprintf(stderr, "usage: latte2msl [--cemu runtime/cemu] [--out dir] cache.bin...\n");
        return 2;
    }
    mkdirs(out + "/msl");
    mkdirs(out + "/raw");
    std::map<std::string, latte_inv::Entry> uniq;  // by stage+hashes
    for (auto& c : caches) {
        latte_inv::Inventory inv;
        std::string err;
        if (!latte_inv::load(cemu, c, inv, err)) {
            fprintf(stderr, "%s: %s\n", c.c_str(), err.c_str());
            return 1;
        }
        printf("%s: %u files, %zu shader entries, %u unreadable\n", c.c_str(), inv.files, inv.entries.size(), inv.bad);
        for (auto& e : inv.entries) {
            char key[64];
            snprintf(key, sizeof key, "%d_%016llx_%016llx", (int)e.stage, (unsigned long long)e.base_hash,
                     (unsigned long long)e.aux_hash);
            uniq.emplace(key, std::move(e));
        }
    }
    std::ofstream man(out + "/manifest.tsv");
    man << "stage\tbase_hash\taux_hash\tstatus\tfile\tbindings\tvariant_regs\terror\n";
    std::map<std::string, int> fail_reasons;
    int counts[3] = {}, ok[3] = {};
    std::set<std::string> programs[3];
    for (auto& [key, e] : uniq) {
        int st = (int)e.stage;
        counts[st]++;
        const char* sn = st == 0 ? "vs" : st == 1 ? "gs" : "ps";
        char name[64];
        snprintf(name, sizeof name, "%s_%016llx_%016llx", sn, (unsigned long long)e.base_hash,
                 (unsigned long long)e.aux_hash);
        // raw programs
        write_file(out + "/raw/" + name + ".bin", e.program.data(), e.program.size());
        if (!e.fetch.empty()) write_file(out + "/raw/" + name + ".fs.bin", e.fetch.data(), e.fetch.size());
        programs[st].insert(std::string(e.program.begin(), e.program.end()));
        msl::Input in;
        in.stage = st == 0 ? msl::Stage::Vertex : st == 1 ? msl::Stage::Geometry : msl::Stage::Pixel;
        in.program = e.program.data();
        in.program_size = (uint32_t)e.program.size();
        in.fetch = e.fetch.empty() ? nullptr : e.fetch.data();
        in.fetch_size = (uint32_t)e.fetch.size();
        in.regs = e.regs->data();
        in.name = name;
        msl::Output o = msl::emit(in);
        std::string file;
        if (o.ok) {
            ok[st]++;
            file = std::string("msl/") + name + ".metal";
            std::ofstream(out + "/" + file) << o.source;
        } else {
            std::string reason = o.error;
            // bucket by message prefix (strip numbers after the first word group)
            size_t p = reason.find_first_of("0123456789");
            fail_reasons[std::string(sn) + ": " + (p == std::string::npos ? reason : reason.substr(0, p))]++;
        }
        std::string b, v;
        for (auto& x : o.bindings) b += x.kind + std::to_string(x.slot) + "@" + std::to_string(x.index) + " ";
        for (auto& x : o.variant_regs) v += x + " ";
        man << sn << "\t" << std::hex << e.base_hash << "\t" << e.aux_hash << std::dec << "\t" << (o.ok ? "ok" : "fail") << "\t"
            << file << "\t" << b << "\t" << v << "\t" << o.error << "\n";
    }
    printf("unique entries: VS %d (distinct programs %zu), GS %d (%zu), PS %d (%zu)\n", counts[0], programs[0].size(),
           counts[1], programs[1].size(), counts[2], programs[2].size());
    printf("translated: VS %d/%d, GS %d/%d, PS %d/%d\n", ok[0], counts[0], ok[1], counts[1], ok[2], counts[2]);
    for (auto& [r, n] : fail_reasons) printf("  fail %5d  %s\n", n, r.c_str());
    return 0;
}
