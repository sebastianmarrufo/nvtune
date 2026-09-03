// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

#include "nvtune/cli.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "nvtune/arch.hpp"
#include "nvtune/gpu.hpp"
#include "nvtune/json.hpp"
#include "nvtune/clocks.hpp"
#include "nvtune/pci.hpp"
#include "nvtune/platform.hpp"
#include "nvtune/regs.hpp"
#include "nvtune/vbios.hpp"

namespace nvtune::cli {
namespace {

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop.store(true); }

void err(const std::string& msg) {
    std::cerr << "error: " << msg << "\n";
}

std::string hex(std::uint32_t v, int width) {
    std::ostringstream os;
    os << "0x" << std::uppercase << std::hex << std::setfill('0')
       << std::setw(width) << v;
    return os.str();
}

void need_root() {
    if (!platform::is_elevated()) {
        err(std::string(platform::privilege_name()) +
            " is required to reach BAR0 (backend: " + Bar0::backend_name() +
            ").");
        std::exit(1);
    }
}

// ------------------------------------------------------------ option parsing

// A tiny long-option parser. Deliberately explicit rather than getopt_long, so
// each subcommand can declare exactly what it accepts.
struct Options {
    std::vector<std::string> devices;
    std::vector<std::string> positional;
    std::optional<unsigned>  fbpa;
    bool     all_fbpa = false;
    bool     commit = false;
    bool     force = false;
    bool     raw = false;
    bool     optional_regs = false;
    bool     full = false;
    bool     verbose = false;
    double   interval = 2.0;
    double   watch = 0.0;
    std::uint32_t start = 0x200;
    std::uint32_t length = 0x200;
    std::string output;
    std::string input;
    std::string profile;
    std::string rom;
    std::string columns;
    bool prom = false;
    bool yes = false;
    std::uint32_t len = 0;   // cfg access width; 0 => default 4
    std::string arch;        // 'fermi'|'tesla' override
    std::uint32_t offset = 0;
    bool offset_set = false;
};

[[noreturn]] void die(const std::string& msg) {
    err(msg);
    std::exit(2);
}

std::uint32_t parse_u32(const std::string& s) {
    try {
        std::size_t pos = 0;
        unsigned long v = std::stoul(s, &pos, 0);
        if (pos != s.size() || v > 0xFFFFFFFFul) throw std::exception();
        return static_cast<std::uint32_t>(v);
    } catch (...) {
        die("'" + s + "' is not an integer");
    }
}

double parse_double(const std::string& s) {
    try {
        std::size_t pos = 0;
        double v = std::stod(s, &pos);
        if (pos != s.size()) throw std::exception();
        return v;
    } catch (...) {
        die("'" + s + "' is not a number");
    }
}

Options parse_options(const std::vector<std::string>& args) {
    Options o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) die(std::string(name) + " needs a value");
            return args[++i];
        };
        if (a == "-d" || a == "--device")      o.devices.push_back(value("--device"));
        else if (a == "--fbpa")                o.fbpa = parse_u32(value("--fbpa"));
        else if (a == "--all-fbpa")            o.all_fbpa = true;
        else if (a == "--commit")              o.commit = true;
        else if (a == "--force")               o.force = true;
        else if (a == "--raw")                 o.raw = true;
        else if (a == "--optional")            o.optional_regs = true;
        else if (a == "--full")                o.full = true;
        else if (a == "-v" || a == "--verbose") o.verbose = true;
        else if (a == "--interval")            o.interval = parse_double(value("--interval"));
        else if (a == "--watch")               o.watch = parse_double(value("--watch"));
        else if (a == "--start")               o.start = parse_u32(value("--start"));
        else if (a == "--length")              o.length = parse_u32(value("--length"));
        else if (a == "-o" || a == "--output") o.output = value("--output");
        else if (a == "-i" || a == "--input")  o.input = value("--input");
        else if (a == "--profile")             o.profile = value("--profile");
        else if (a == "--rom")                 o.rom = value("--rom");
        else if (a == "--columns")             o.columns = value("--columns");
        else if (a == "--prom")                o.prom = true;
        else if (a == "--yes" || a == "-y")     o.yes = true;
        else if (a == "--len")                  o.len = parse_u32(value("--len"));
        else if (a == "--arch")                 o.arch = value("--arch");
        else if (a == "--offset")               { o.offset = parse_u32(value("--offset")); o.offset_set = true; }
        else if (a == "--value")                o.len = parse_u32(value("--value"));
        else if (!a.empty() && a[0] == '-' && a != "-") die("unknown option " + a);
        else o.positional.push_back(a);
    }
    return o;
}

// ------------------------------------------------------------ device helpers

std::vector<std::unique_ptr<Gpu>> open_targets(const Options& o, bool writable) {
    std::vector<PciDevice> devs;
    if (o.devices.empty()) {
        devs = enumerate_gpus();
        if (devs.empty()) {
            err("no NVIDIA display devices found "
                "(looked at the PnP display class)");
            std::exit(1);
        }
    } else {
        for (const std::string& s : o.devices) {
            try {
                devs.push_back(find(s));
            } catch (const std::exception& e) {
                err(e.what());
                std::exit(1);
            }
        }
    }

    std::vector<std::unique_ptr<Gpu>> gpus;
    for (PciDevice& d : devs) {
        auto g = std::make_unique<Gpu>(d, writable);
        try {
            g->open();
        } catch (const MmioError& e) {
            err(d.slot + ": " + e.what());
            continue;
        }
        gpus.push_back(std::move(g));
    }
    if (gpus.empty()) std::exit(1);
    return gpus;
}

std::string default_backup_path(const Gpu& g) {
    const std::string dir = platform::config_dir();
    platform::ensure_dir(dir);
    return dir + '\\' + platform::sanitize_filename(g.dev().slot) +
           ".stock.json";
}

bool file_exists(const std::string& p) { return platform::file_exists(p); }

std::string ensure_stock_backup(const Gpu& g) {
    const std::string path = default_backup_path(g);
    if (!file_exists(path)) {
        g.backup(path, true);
        std::cout << "  stock values saved to " << path << "\n";
    }
    return path;
}

Assignments parse_assignments(const std::vector<std::string>& items) {
    Assignments out;
    for (const std::string& item : items) {
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos) {
            die("expected FIELD=VALUE, got '" + item + "'");
        }
        std::string name = item.substr(0, eq);
        const std::uint32_t value = parse_u32(item.substr(eq + 1));

        const std::string canon = resolve_alias(name);
        FieldRef ref{};
        if (!try_lookup(canon, ref)) {
            die("unknown field '" + name +
                "'. Run 'nvtune fields' for the full list.");
        }
        if (value > ref.field->maxval()) {
            die(std::string(ref.field->name) + "=" + std::to_string(value) +
                " does not fit its " + std::to_string(ref.field->width) +
                "-bit field (0.." + std::to_string(ref.field->maxval()) + ")");
        }
        const std::string key = to_upper(ref.field->name);
        auto it = out.find(key);
        if (it != out.end() && it->second != value) {
            die(std::string(ref.field->name) +
                " assigned twice with different values");
        }
        out[key] = value;
    }
    return out;
}

std::vector<Scope> scopes_for(const Gpu& g, const Options& o) {
    if (o.all_fbpa) {
        std::vector<Scope> s;
        for (unsigned i : g.active_fbpas()) s.push_back(i);
        return s;
    }
    return {o.fbpa.has_value() ? Scope(*o.fbpa) : kBroadcast};
}

std::string scope_label(Scope s) {
    return s.has_value() ? ("FBPA" + std::to_string(*s)) : std::string("broadcast");
}

// Returns true if any warning was emitted.
bool print_ops(const std::vector<WriteOp>& ops, bool committed) {
    bool warned = false;
    for (const WriteOp& op : ops) {
        if (!op.dirty()) {
            std::cout << "  " << op.reg->name << " @" << hex(op.offset, 6)
                      << "  unchanged (" << hex(op.old_word, 8) << ")\n";
            continue;
        }
        std::cout << "  " << op.reg->name << " @" << hex(op.offset, 6) << "  "
                  << hex(op.old_word, 8) << " -> " << hex(op.new_word, 8)
                  << "  [" << (committed ? "write" : "would write") << "]\n";
        for (const FieldChange& c : op.changes) {
            std::cout << "      " << std::left << std::setw(12) << c.name
                      << std::right << std::setw(6) << c.old_value << " -> "
                      << std::left << std::setw(6) << c.new_value << "\n";
        }
        for (const std::string& w : op.warnings) {
            warned = true;
            std::cout << "      ! " << w << "\n";
        }
    }
    std::cout << std::right;
    return warned;
}

json::Value load_profile(const std::string& path) {
    json::Value doc = json::parse_file(path);
    if (!doc.contains("fields")) {
        die(path + ": profile has no 'fields' object");
    }
    return doc;
}

Assignments profile_assignments(const json::Value& doc) {
    Assignments a;
    std::vector<std::string> items;
    for (const auto& [k, v] : doc.at("fields").as_object()) {
        items.push_back(k + "=" + std::to_string(json::as_u32(v)));
    }
    return parse_assignments(items);
}

// ------------------------------------------------------------ commands

int cmd_list(const Options&) {
    std::vector<PciDevice> devs = enumerate_gpus();
    if (devs.empty()) {
        std::cout << "no NVIDIA display devices found\n";
        return 1;
    }
    for (const PciDevice& d : devs) {
        std::cout << d.slot << "  [" << hex(d.vendor, 4).substr(2) << ":"
                  << hex(d.device, 4).substr(2) << "]";
        if (d.has_subsystem) {
            std::cout << "  subsys " << hex(d.subsystem_vendor, 4).substr(2)
                      << ":" << hex(d.subsystem_device, 4).substr(2);
        }
        std::cout << "\n";
        std::cout << "    BAR0 0x" << std::uppercase << std::hex
                  << d.bar0_start << "  size 0x" << d.bar0_size << std::dec
                  << std::nouppercase << "\n";

        if (!platform::is_elevated()) {
            std::cout << "    (run as " << platform::privilege_name()
                      << " to identify the chip and read timings)\n";
            continue;
        }
        Gpu g(d, false);
        try {
            g.open();
        } catch (const MmioError& e) {
            std::cout << "    unreadable: " << e.what() << "\n";
            continue;
        }
        const Arch& a = g.arch();
        std::cout << "    " << a.codename << " (" << a.family << ", "
                  << a.series << ")  " << decode_boot0(g.boot0()) << "\n";
        std::cout << "    FBPA broadcast " << hex(a.layout.broadcast, 6)
                  << " (window " << hex(a.layout.window, 4) << "), unicast "
                  << hex(a.layout.unicast, 6) << " stride "
                  << hex(a.layout.unicast_stride, 4) << "\n";
        std::vector<unsigned> active = g.active_fbpas();
        std::cout << "    partitions: " << g.num_fbpa() << " total, active [";
        for (std::size_t i = 0; i < active.size(); ++i) {
            std::cout << (i ? ", " : "") << active[i];
        }
        std::cout << "], floorswept mask " << hex(g.floorswept_mask(), 4)
                  << "\n";
        if (!a.caveat.empty()) std::cout << "    note: " << a.caveat << "\n";
        if (!a.writes_expected) {
            std::cout << "    note: runtime writes are not expected to persist "
                         "on this family\n";
        }
    }
    return 0;
}

int cmd_fields(const Options&) {
    std::cout << std::left << std::setw(14) << "FIELD" << std::setw(10)
              << "REGISTER" << std::setw(10) << "BITS" << std::right
              << std::setw(6) << "MAX" << "  DESCRIPTION\n";
    std::cout << std::string(100, '-') << "\n";
    for (const Register& reg : all_registers()) {
        for (const Field& f : reg.fields) {
            std::cout << std::left << std::setw(14) << f.name << std::setw(10)
                      << reg.name << std::setw(10) << f.bits() << std::right
                      << std::setw(6) << f.maxval() << "  " << f.desc
                      << (f.tunable ? "" : "  [structural]") << "\n";
        }
        if (reg.confidence != Confidence::Documented) {
            std::cout << std::left << std::setw(14) << "" << std::setw(10)
                      << reg.name << to_string(reg.confidence) << ": "
                      << reg.note << "\n";
        }
    }
    std::cout << std::right << "\n";
    std::cout << "Documented by NVIDIA but with no established runtime "
                 "register:\n";
    for (const VbiosOnlyField& v : vbios_only_fields()) {
        std::cout << "  " << std::left << std::setw(16) << v.name << v.desc
                  << "\n";
    }
    std::cout << std::right
              << "  (these are readable per p-state via 'nvtune vbios')\n";
    return 0;
}

int cmd_dump(const Options& o) {
    need_root();
    for (auto& g : open_targets(o, false)) {
        const Arch& a = g->arch();
        std::cout << g->dev().slot << "  " << a.codename << " (" << a.family
                  << ", " << a.series << ")\n";

        std::vector<Scope> scopes;
        if (o.all_fbpa) {
            scopes.push_back(kBroadcast);
            for (unsigned i : g->active_fbpas()) scopes.push_back(i);
        } else {
            scopes = scopes_for(*g, o);
        }

        const std::vector<Register>& which =
            o.optional_regs ? all_registers() : timing_registers();

        for (Scope s : scopes) {
            std::cout << "  [" << scope_label(s) << "] base "
                      << hex(g->aperture(s), 6) << "\n";
            for (const Register& reg : which) {
                const std::uint32_t off = g->reg_offset(reg, s);
                const std::uint32_t word = g->bar().rd32(off);
                std::cout << "    " << reg.name << " @" << hex(off, 6) << " = "
                          << hex(word, 8);
                if (reg.confidence != Confidence::Documented) {
                    std::cout << "  (" << to_string(reg.confidence) << ")";
                }
                std::cout << "\n";
                if (o.raw) continue;
                for (const Field& f : reg.fields) {
                    const std::uint32_t v = f.extract(word);
                    std::string flag;
                    if (f.typical.has_value() &&
                        (v < f.typical->first || v > f.typical->second)) {
                        flag += "  <- outside typical range";
                    }
                    std::cout << "        " << std::left << std::setw(12)
                              << f.name << std::setw(10) << f.bits()
                              << std::right << std::setw(6) << v << flag
                              << "\n";
                }
            }
            std::cout << "\n";
        }
    }
    return 0;
}

int cmd_get(const Options& o) {
    need_root();
    if (o.positional.empty()) die("get needs at least one field name");
    const Scope scope = o.fbpa.has_value() ? Scope(*o.fbpa) : kBroadcast;
    for (auto& g : open_targets(o, false)) {
        std::cout << g->dev().slot << "  ";
        for (const std::string& name : o.positional) {
            const std::string canon = resolve_alias(name);
            FieldRef ref{};
            if (!try_lookup(canon, ref)) {
                die("unknown field '" + name + "'");
            }
            std::cout << ref.field->name << "="
                      << g->read_field(canon, scope) << "  ";
        }
        std::cout << "\n";
    }
    return 0;
}

int do_set(const Options& o, const Assignments& assignments) {
    need_root();
    int rc = 0;
    for (auto& g : open_targets(o, o.commit)) {
        const Arch& a = g->arch();
        std::cout << g->dev().slot << "  " << a.codename << " (" << a.family
                  << ")\n";
        if (!a.caveat.empty()) std::cout << "  note: " << a.caveat << "\n";

        for (Scope s : scopes_for(*g, o)) {
            std::cout << "  [" << scope_label(s) << "]\n";
            std::vector<WriteOp> ops = g->plan(assignments, s);
            const bool warned = print_ops(ops, o.commit);
            if (warned && !o.force && o.commit) {
                err("refusing to write with warnings outstanding; re-run with "
                    "--force if you mean it");
                rc = 1;
                continue;
            }
            if (!o.commit) continue;

            ensure_stock_backup(*g);
            std::vector<std::string> problems = g->commit(ops);
            for (const std::string& p : problems) {
                std::cout << "      ! readback mismatch: " << p << "\n";
                rc = 1;
            }
            if (problems.empty()) std::cout << "      applied and verified\n";
        }
        if (o.commit) {
            std::cout << "  reminder: the driver reprograms these on p-state "
                         "changes. Use 'nvtune daemon' to hold them.\n";
        }
    }
    return rc;
}

int cmd_set(const Options& o) {
    if (o.positional.empty()) die("set needs at least one FIELD=VALUE");
    return do_set(o, parse_assignments(o.positional));
}

int cmd_apply(const Options& o) {
    if (o.positional.empty()) die("apply needs a profile path");
    json::Value doc = load_profile(o.positional[0]);
    std::cout << "profile: " << doc.str_or("name", o.positional[0]) << "\n";
    const std::string d = doc.str_or("description", "");
    if (!d.empty()) std::cout << "  " << d << "\n";
    const std::string ap = doc.str_or("applies_to", "");
    if (!ap.empty()) std::cout << "  intended for: " << ap << "\n";
    Assignments a = profile_assignments(doc);
    if (a.empty()) {
        err("profile has no fields to apply (is it the template?)");
        return 1;
    }
    return do_set(o, a);
}

int cmd_save(const Options& o) {
    need_root();
    for (auto& g : open_targets(o, false)) {
        const std::string path =
            o.output.empty() ? default_backup_path(*g) : o.output;
        g->backup(path, true);
        std::cout << g->dev().slot << "  saved -> " << path << "\n";
    }
    return 0;
}

int cmd_restore(const Options& o) {
    need_root();
    int rc = 0;
    for (auto& g : open_targets(o, true)) {
        const std::string path =
            o.input.empty() ? default_backup_path(*g) : o.input;
        if (!file_exists(path)) {
            err(g->dev().slot + ": no backup at " + path);
            rc = 1;
            continue;
        }
        try {
            std::vector<std::string> problems = g->restore(path);
            for (const std::string& p : problems) {
                std::cout << "  ! " << p << "\n";
                rc = 1;
            }
            std::cout << g->dev().slot << "  restored from " << path
                      << (problems.empty() ? " (verified)" : "") << "\n";
        } catch (const std::exception& e) {
            err(std::string(e.what()));
            rc = 1;
        }
    }
    return rc;
}

int cmd_daemon(const Options& o) {
    need_root();
    Assignments assignments;
    if (!o.profile.empty()) {
        assignments = profile_assignments(load_profile(o.profile));
    } else {
        assignments = parse_assignments(o.positional);
    }
    if (assignments.empty()) {
        err("daemon needs either --profile or FIELD=VALUE arguments");
        return 1;
    }

    platform::install_interrupt_handler(on_signal);

    auto gpus = open_targets(o, true);
    std::map<std::string, std::string> backups;
    for (auto& g : gpus) backups[g->dev().slot] = ensure_stock_backup(*g);

    std::cout << "holding " << assignments.size() << " field(s) on "
              << gpus.size() << " GPU(s), re-applying every " << o.interval
              << "s. Ctrl-C to stop and restore.\n";

    unsigned long cycles = 0, reasserts = 0;
    while (!g_stop.load()) {
        for (auto& g : gpus) {
            std::vector<Scope> scopes;
            if (o.all_fbpa) {
                for (unsigned i : g->active_fbpas()) scopes.push_back(i);
            } else {
                scopes.push_back(kBroadcast);
            }
            for (Scope s : scopes) {
                std::vector<WriteOp> ops = g->plan(assignments, s);
                std::vector<WriteOp> dirty;
                for (WriteOp& op : ops) {
                    if (op.dirty()) dirty.push_back(op);
                }
                if (dirty.empty()) continue;
                ++reasserts;
                std::vector<std::string> problems = g->commit(dirty);
                if (!o.verbose) continue;
                if (!problems.empty()) {
                    for (const std::string& p : problems) {
                        std::cout << "  ! " << p << "\n";
                    }
                } else {
                    std::cout << "  " << g->dev().slot << ": re-asserted";
                    for (const WriteOp& op : dirty) {
                        for (const FieldChange& c : op.changes) {
                            std::cout << " " << c.name;
                        }
                    }
                    std::cout << "\n";
                }
            }
        }
        ++cycles;
        if (o.verbose && cycles % 12 == 0) {
            std::cout << "  ... " << cycles << " cycles, " << reasserts
                      << " re-assertions\n";
        }
        const int ticks = static_cast<int>(o.interval * 10);
        for (int i = 0; i < ticks && !g_stop.load(); ++i) {
            platform::sleep_ms(100);
        }
    }

    std::cout << "\nrestoring stock values...\n";
    for (auto& g : gpus) {
        try {
            g->restore(backups[g->dev().slot]);
            std::cout << "  " << g->dev().slot << ": restored\n";
        } catch (const std::exception& e) {
            err(g->dev().slot + ": restore failed: " + e.what());
        }
    }
    return 0;
}

// Report every PCIe-clamp-related site the patcher can identify in a ROM,
// without modifying it. Helps locate the driver-safe lever on cards whose
// mechanism isn't auto-handled, and to diff a stock vs known-good ROM.
int cmd_vbios_scan(const Options& o) {
    if (o.positional.empty()) die("vbios-scan needs a ROM file");
    std::vector<std::uint8_t> rom;
    try {
        rom = vbios::read_file(o.positional[0]);
    } catch (const vbios::VbiosError& e) { err(e.what()); return 1; }

    std::cout << o.positional[0] << " (" << rom.size() << " bytes)\n";
    const vbios::StrapArch a = vbios::detect_strap_arch(rom);
    std::cout << "device family (by id): "
              << (a == vbios::StrapArch::Fermi ? "Fermi"
                  : a == vbios::StrapArch::Tesla ? "Tesla" : "unknown")
              << "\n";

    // Option-ROM checksum status -- the driver requires this to be 0.
    std::vector<std::uint8_t> tmp = rom;
    const std::uint8_t before_fix = [&] {
        std::size_t il = tmp.size();
        if (tmp.size() >= 0x1A) {
            const std::uint32_t pcir = tmp[0x18] | (tmp[0x19] << 8);
            if (pcir + 18 <= tmp.size()) {
                const std::uint32_t bl = tmp[pcir + 16] | (tmp[pcir + 17] << 8);
                if (bl && bl * 512u <= tmp.size()) il = bl * 512u;
            }
        }
        unsigned s = 0; for (std::size_t i = 0; i < il; ++i) s += tmp[i];
        return static_cast<std::uint8_t>(s & 0xFF);
    }();
    std::cout << "option-ROM checksum: 0x" << std::hex << int(before_fix)
              << std::dec << (before_fix == 0 ? "  (valid; driver will accept)"
                                              : "  (INVALID; driver would reject)")
              << "\n";

    std::cout << "\ncandidate PCIe-Gen clamp sites:\n";
    bool any = false;

    const vbios::StrapBlock b = vbios::find_strap_block(rom);
    if (b.found) {
        any = true;
        std::cout << "  [Fermi-style] strap-override block @0x" << std::hex
                  << b.signature_off << ", control byte @0x" << b.control_off
                  << " = 0x" << int(b.control) << std::dec
                  << "  (clean fix: -> 0xCF)\n";
    }
    const vbios::TeslaPexWrite w = vbios::find_tesla_pex_write(rom);
    if (w.found) {
        any = true;
        std::cout << "  [GT200-style] init-script PEX write reg 0x" << std::hex
                  << w.reg << ", value @0x" << w.value_off << " = 0x" << w.value
                  << std::dec << "  (clean fix: set bits 0+7)\n";
    }
    // Any other reference to the PEX strap register group, in any encoding.
    for (std::uint32_t reg : {0xE180u, 0xE18Cu, 0xE11Cu, 0xE120u}) {
        std::uint8_t pat[4] = {static_cast<std::uint8_t>(reg & 0xFF),
                               static_cast<std::uint8_t>((reg >> 8) & 0xFF),
                               static_cast<std::uint8_t>((reg >> 16) & 0xFF),
                               static_cast<std::uint8_t>((reg >> 24) & 0xFF)};
        for (std::size_t i = 0; i + 4 <= rom.size() && i < 0x20000; ++i) {
            if (rom[i] == pat[0] && rom[i + 1] == pat[1] &&
                rom[i + 2] == pat[2] && rom[i + 3] == pat[3]) {
                any = true;
                std::cout << "  [ref] PEX reg 0x" << std::hex << reg
                          << " referenced @0x" << i << " (opcode byte before: 0x"
                          << int(i ? rom[i - 1] : 0) << ")" << std::dec << "\n";
            }
        }
    }
    if (!any) std::cout << "  none identified structurally\n";

    std::cout << "\nTo compare a stock vs known-good ROM, scan both and diff, "
                 "or:\n  cmp -l stock.rom good.rom   (byte offsets that differ)\n";
    std::cout << "Any clean fix must keep the checksum 0 -- vbios-patch repairs "
                 "it automatically; a raw hex edit does not.\n";
    return 0;
}

// Patch a VBIOS ROM file to force PCIe Gen2 capability. Operates on files, not
// hardware -- the strap is latched at devinit, so this is a ROM edit you then
// flash (nvflash) or use for analysis. Never touches the live card.
int cmd_vbios_patch(const Options& o) {
    if (o.positional.empty()) {
        die("vbios-patch needs an input ROM, e.g. "
            "vbios-patch card.rom -o card.gen2.rom");
    }
    const std::string in = o.positional[0];
    std::string out = o.output;
    if (out.empty()) {
        const std::size_t dot = in.rfind('.');
        out = (dot == std::string::npos ? in : in.substr(0, dot)) +
              ".gen2.rom";
    }

    std::vector<std::uint8_t> rom;
    try {
        rom = vbios::read_file(in);
    } catch (const vbios::VbiosError& e) {
        err(e.what());
        return 1;
    }

    // Arch: explicit override, else auto-detect from the device id.
    vbios::StrapArch arch = vbios::detect_strap_arch(rom);
    if (o.arch == "fermi") arch = vbios::StrapArch::Fermi;
    else if (o.arch == "tesla") arch = vbios::StrapArch::Tesla;

    if (arch == vbios::StrapArch::Unknown && o.arch.empty()) {
        err("could not auto-detect Fermi vs Tesla family from the device id. "
            "Re-run with --arch fermi or --arch tesla (Fermi = GF1xx e.g. GTX "
            "4xx/5xx; Tesla = G8x/G9x/GT2xx e.g. 8800/9800/GTX2xx).");
        return 1;
    }

    // Show what we found first, so the user can sanity-check before writing.
    if (arch == vbios::StrapArch::Tesla) {
        const vbios::TeslaPexWrite w = vbios::find_tesla_pex_write(rom);
        if (!w.found && !o.offset_set) {
            err("no Tesla PEX init-script write found (E0 <reg> E2 <val>). If "
                "you know the exact byte for this card, use --offset/--value.");
            return 1;
        }
        std::cout << "input : " << in << " (" << rom.size() << " bytes)\n";
        std::cout << "family: Tesla (G8x/G9x/GT2xx)"
                  << (o.arch.empty() ? " [auto-detected]" : " [forced]") << "\n";
        if (w.found) {
            std::cout << "target: init-script PEX write reg 0x" << std::hex
                      << w.reg << " value @0x" << w.value_off << " = 0x"
                      << w.value << std::dec << "\n";
        }
    } else {
        const vbios::StrapBlock b = vbios::find_strap_block(rom);
        if (!b.found && !o.offset_set) {
            err("no Fermi strap-override block (0x10de) found; this doesn't "
                "look like a Fermi VBIOS. Use --offset/--value if you know the "
                "exact byte.");
            return 1;
        }
        std::cout << "input : " << in << " (" << rom.size() << " bytes)\n";
        std::cout << "family: Fermi (GF1xx)"
                  << (o.arch.empty() ? " [auto-detected]" : " [forced]") << "\n";
        if (b.found) {
            std::cout << "target: strap block @0x" << std::hex << b.signature_off
                      << ", control byte @0x" << b.control_off << " = 0x"
                      << int(b.control) << std::dec << "\n";
        }
    }

    if (!o.commit) {
        std::cout << "\nThis is a dry run. Re-run with --commit to write "
                  << out << ".\n";
        std::cout << "The card must then be flashed with nvflash; this does "
                     "not touch the live GPU.\n";
        return 0;
    }

    std::string detail;
    const std::uint8_t forced = static_cast<std::uint8_t>(o.len & 0xFF);

    // Explicit offset override: when you already know the exact byte (e.g. a
    // confirmed per-card fix), set it directly and skip the structural guess.
    if (o.offset_set) {
        if (o.offset >= rom.size()) { err("--offset past end of ROM"); return 1; }
        const std::uint8_t val = forced ? forced
                                         : (arch == vbios::StrapArch::Tesla
                                                ? 0xFD : 0xCF);
        const std::uint8_t was = rom[o.offset];
        rom[o.offset] = val;
        const std::uint8_t ck = vbios::fix_rom_checksum(rom);
        char msg[128];
        std::snprintf(msg, sizeof msg,
                      "explicit byte @0x%X: 0x%02X -> 0x%02X; checksum %s",
                      o.offset, was, val, ck == 0 ? "ok" : "NONZERO");
        std::cout << "patch : " << msg << "\n";
    } else {
        if (!vbios::patch_pcie_gen2(rom, arch, forced, detail)) {
            err(detail);
            return 1;
        }
        std::cout << "patch : " << detail << "\n";
    }

    try {
        vbios::write_file(out, rom);
    } catch (const vbios::VbiosError& e) {
        err(e.what());
        return 1;
    }
    std::cout << "wrote : " << out << "\n";
    std::cout << "\nFlash with:  nvflash -6 " << out
              << "   (back up the stock ROM first: nvflash --save stock.rom)\n";
    std::cout << "WARNING: a bad flash bricks the card unless you can recover "
                 "(second GPU / onboard video / a known-good ROM to reflash "
                 "blind). You accept that risk.\n";
    return 0;
}

// Read and decode the clock domains.
int cmd_clocks(const Options& o) {
    need_root();
    for (auto& g : open_targets(o, false)) {
        std::cout << g->dev().slot << "  " << g->arch().codename << " ("
                  << g->arch().family << ")\n";
        std::cout << "  NOTE: clock-tree offsets are INFERRED and vary by "
                     "family; verify a domain against a known clock, and use "
                     "peek/probe to correct offsets in src/clocks.cpp.\n";
        for (const ClockDomain& d : clock_domains()) {
            std::uint32_t coef = 0, ctrl = 0;
            bool ok = true;
            try {
                coef = g->bar().rd32(d.coef_off);
                if (d.ctrl_off) ctrl = g->bar().rd32(d.ctrl_off);
            } catch (const MmioError&) {
                ok = false;
            }
            std::cout << "  " << std::left << std::setw(14) << d.name
                      << std::right;
            if (!ok) {
                std::cout << "  <offset not readable>\n";
                continue;
            }
            std::cout << "  coef@" << hex(d.coef_off, 6) << "=" << hex(coef, 8);
            if (d.ctrl_off) std::cout << " ctrl=" << hex(ctrl, 8);
            if (d.kind == ClockKind::Pll) {
                const PllCoef c = decode_pll(coef);
                if (c.valid) {
                    const std::uint32_t khz = pll_freq_khz(c);
                    std::cout << "  N=" << c.n << " M=" << c.m << " P=" << c.p
                              << "  ~" << (khz / 1000) << " MHz";
                } else {
                    std::cout << "  (M=0: divider or gated, not a live PLL)";
                }
            } else {
                std::cout << "  (" << (d.kind == ClockKind::Divider ? "divider"
                                                                    : "derived")
                          << " off " << d.src << ")";
            }
            std::cout << "\n";
            std::cout << "      " << d.human << "\n";
        }
        std::cout << "  reference assumed 27 MHz; f = ref*N/(M*2^P)\n";
    }
    return 0;
}

// Decode a device's PCIe link status/control. Defaults to the GPU; -d selects.
int cmd_pcie(const Options& o) {
    need_root();
    std::vector<PciDevice> devs;
    if (!o.positional.empty()) {
        std::uint32_t s, b, dv, f;
        if (!parse_slot(o.positional[0], s, b, dv, f)) {
            die("pcie: expected a slot like 0000:01:00.0 or a BDF");
        }
        PciDevice p; p.segment = s; p.bus = b; p.dev = dv; p.func = f;
        p.slot = format_slot(s, b, dv, f);
        devs.push_back(p);
    } else {
        devs = enumerate_gpus();
        if (devs.empty()) { err("no NVIDIA GPU found; pass a slot"); return 1; }
    }

    auto gen_str = [](std::uint32_t sp) -> std::string {
        switch (sp) {
            case 1: return "2.5 GT/s (Gen1)";
            case 2: return "5 GT/s (Gen2)";
            case 3: return "8 GT/s (Gen3)";
            case 4: return "16 GT/s (Gen4)";
            case 5: return "32 GT/s (Gen5)";
            default: return "unknown";
        }
    };

    for (const PciDevice& d : devs) {
        try {
            const std::uint32_t cap = find_pcie_cap(d.bus, d.dev, d.func);
            if (cap == 0) {
                std::cout << d.slot << ": no PCIe capability found\n";
                continue;
            }
            const std::uint32_t linkcap = config_read(d.bus, d.dev, d.func, cap + 0x0C, 4);
            const std::uint32_t linksta = config_read(d.bus, d.dev, d.func, cap + 0x12, 2);
            const std::uint32_t lctl2   = config_read(d.bus, d.dev, d.func, cap + 0x30, 2);

            const std::uint32_t max_speed = linkcap & 0xF;
            const std::uint32_t max_width = (linkcap >> 4) & 0x3F;
            const std::uint32_t cur_speed = linksta & 0xF;
            const std::uint32_t cur_width = (linksta >> 4) & 0x3F;
            const std::uint32_t tgt_speed = lctl2 & 0xF;

            std::cout << d.slot << "  PCIe cap @0x" << std::hex << cap << std::dec << "\n";
            std::cout << "    max  : " << gen_str(max_speed) << "  x" << max_width << "\n";
            std::cout << "    now  : " << gen_str(cur_speed) << "  x" << cur_width;
            if (cur_speed < max_speed) std::cout << "   <- downtrained below max";
            std::cout << "\n";
            std::cout << "    target link speed (LinkCtl2[3:0]) = " << tgt_speed
                      << "  (" << gen_str(tgt_speed) << ")\n";
            std::cout << "    to force a speed on THIS device: cfg-poke " << d.slot
                      << " 0x" << std::hex << (cap + 0x30) << std::dec
                      << " <val> --len 2 --yes, then set retrain bit at 0x"
                      << std::hex << (cap + 0x10) << std::dec << " bit 5.\n";
            std::cout << "    NOTE: link speed is negotiated with the ROOT PORT; "
                         "if it caps Gen1 you must poke the root port too.\n";
        } catch (const ConfigError& e) {
            err(std::string(e.what()));
        }
    }
    return 0;
}

// Raw PCI config-space read, any device.
int cmd_cfg_peek(const Options& o) {
    need_root();
    if (o.positional.size() < 2) {
        die("cfg-peek needs SLOT OFFSET [more offsets], e.g. "
            "cfg-peek 0000:00:01.0 0xa0");
    }
    std::uint32_t s, b, dv, f;
    if (!parse_slot(o.positional[0], s, b, dv, f)) {
        die("cfg-peek: first arg must be a slot like 0000:00:01.0");
    }
    const std::uint32_t len = o.len ? o.len : 4;
    for (std::size_t i = 1; i < o.positional.size(); ++i) {
        const std::uint32_t off = parse_u32(o.positional[i]);
        try {
            const std::uint32_t v = config_read(b, dv, f, off, len);
            std::cout << o.positional[0] << "  cfg+0x" << std::hex << off
                      << " (" << std::dec << len << "B) = 0x" << std::hex << v
                      << std::dec << "\n";
        } catch (const ConfigError& e) {
            err(std::string(e.what()));
        }
    }
    return 0;
}

// Raw PCI config-space write, any device. The instrument for the PCIe mod.
int cmd_cfg_poke(const Options& o) {
    need_root();
    if (o.positional.size() != 3) {
        die("cfg-poke needs SLOT OFFSET VALUE, e.g. "
            "cfg-poke 0000:00:01.0 0xa0 0x42 --len 1 --yes");
    }
    std::uint32_t s, b, dv, f;
    if (!parse_slot(o.positional[0], s, b, dv, f)) {
        die("cfg-poke: first arg must be a slot like 0000:00:01.0");
    }
    const std::uint32_t off = parse_u32(o.positional[1]);
    const std::uint32_t val = parse_u32(o.positional[2]);
    const std::uint32_t len = o.len ? o.len : 4;

    if (!o.yes) {
        err("cfg-poke writes raw PCI config space on an arbitrary device and "
            "can wedge the bus or the machine. Re-run with --yes. Read the "
            "current value with cfg-peek first and write it back to confirm "
            "the path before changing anything.");
        return 1;
    }
    try {
        const std::uint32_t before = config_read(b, dv, f, off, len);
        config_write(b, dv, f, off, len, val);
        const std::uint32_t after = config_read(b, dv, f, off, len);
        std::cout << o.positional[0] << "  cfg+0x" << std::hex << off << " ("
                  << std::dec << len << "B)  0x" << std::hex << before
                  << " -> wrote 0x" << val << " -> reads 0x" << after
                  << std::dec;
        if (after == val) std::cout << "  (took)\n";
        else if (after == before) std::cout << "  (ignored / read-only bits)\n";
        else std::cout << "  (partially took / hardware-modified)\n";
    } catch (const ConfigError& e) {
        err(std::string(e.what()));
        return 1;
    }
    return 0;
}

// Read one raw dword at an arbitrary FBPA-relative offset. For inspecting
// registers found via `probe --watch` that aren't in the named field table.
int cmd_peek(const Options& o) {
    need_root();
    if (o.positional.empty()) die("peek needs an offset, e.g. peek 0x2b0");
    const Scope scope = o.fbpa.has_value() ? Scope(*o.fbpa) : kBroadcast;
    for (auto& g : open_targets(o, false)) {
        for (const std::string& os : o.positional) {
            const std::uint32_t rel = parse_u32(os);
            const std::uint32_t abs = g->aperture(scope) + rel;
            try {
                const std::uint32_t v = g->bar().rd32(abs);
                std::cout << g->dev().slot << "  [" << scope_label(scope)
                          << "] +" << hex(rel, 3) << " @" << hex(abs, 6)
                          << " = " << hex(v, 8) << "\n";
            } catch (const MmioError& e) {
                err(std::string(e.what()));
            }
        }
    }
    return 0;
}

// Write one raw dword at an arbitrary FBPA-relative offset. This is the
// deliberately-unsafe discovery primitive: it targets a register that has no
// named field and no range check of its own, so it demands --yes and snapshots
// stock first. The driver's allowlist still applies underneath -- a poke
// outside the writable FBPA apertures is refused in kernel mode regardless.
int cmd_poke(const Options& o) {
    need_root();
    if (o.positional.size() != 2) {
        die("poke needs OFFSET VALUE, e.g. poke 0x2b0 0x1234");
    }
    const std::uint32_t rel = parse_u32(o.positional[0]);
    const std::uint32_t val = parse_u32(o.positional[1]);
    const Scope scope = o.fbpa.has_value() ? Scope(*o.fbpa) : kBroadcast;

    if (!o.yes) {
        err("poke writes an unnamed register and can wedge the memory "
            "controller. Re-run with --yes if you mean it. Consider 'save' "
            "first so you can 'restore'.");
        return 1;
    }
    int rc = 0;
    for (auto& g : open_targets(o, true)) {
        const std::uint32_t abs = g->aperture(scope) + rel;
        try {
            ensure_stock_backup(*g);
            const std::uint32_t before = g->bar().rd32(abs);
            g->bar().wr32(abs, val);
            const std::uint32_t after = g->bar().rd32(abs);
            std::cout << g->dev().slot << "  [" << scope_label(scope) << "] +"
                      << hex(rel, 3) << " @" << hex(abs, 6) << "  "
                      << hex(before, 8) << " -> wrote " << hex(val, 8)
                      << " -> reads " << hex(after, 8);
            if (after == val) {
                std::cout << "  (took)\n";
            } else if (after == before) {
                std::cout << "  (ignored: write had no effect)\n";
                rc = 1;
            } else {
                std::cout << "  (reasserted: something rewrote it)\n";
                rc = 1;
            }
        } catch (const MmioError& e) {
            err(std::string(e.what()));
            rc = 1;
        }
    }
    return rc;
}

int cmd_probe(const Options& o) {
    need_root();
    const Scope scope = o.fbpa.has_value() ? Scope(*o.fbpa) : kBroadcast;
    for (auto& g : open_targets(o, false)) {
        const std::uint32_t base = g->aperture(scope);
        const std::uint32_t start = base + o.start;
        const std::size_t count = o.length / 4;
        std::cout << g->dev().slot << "  " << g->arch().codename << "  window "
                  << hex(start, 6) << ".." << hex(start + o.length - 1, 6)
                  << "\n";

        if (o.watch <= 0) {
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint32_t off =
                    start + static_cast<std::uint32_t>(4 * i);
                if (i % 4 == 0) std::cout << "\n  " << hex(off, 6) << ":";
                std::cout << " " << hex(g->bar().rd32(off), 8).substr(2);
            }
            std::cout << "\n\n";
            continue;
        }

        std::cout << "  watching for " << o.watch
                  << "s, reporting words that change\n";
        std::vector<std::uint32_t> baseline = g->bar().rd_block(start, count);
        std::map<std::uint32_t, std::vector<std::uint32_t>> seen;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(
                                  static_cast<long long>(o.watch * 1000));
        while (std::chrono::steady_clock::now() < deadline && !g_stop.load()) {
            std::vector<std::uint32_t> cur = g->bar().rd_block(start, count);
            for (std::size_t i = 0; i < count; ++i) {
                if (cur[i] == baseline[i]) continue;
                const std::uint32_t off =
                    start + static_cast<std::uint32_t>(4 * i);
                auto& v = seen[off];
                if (std::find(v.begin(), v.end(), cur[i]) == v.end()) {
                    v.push_back(cur[i]);
                }
                baseline[i] = cur[i];
            }
            platform::sleep_ms(50);
        }
        if (seen.empty()) {
            std::cout << "  nothing changed. Put the card under load and "
                         "retry.\n";
        }
        for (const auto& [off, vals] : seen) {
            const std::uint32_t rel = off - base;
            std::string tag = "  (unmapped)";
            for (const Register& r : all_registers()) {
                if (r.offset == rel) { tag = std::string("  (") + r.name + ")"; break; }
            }
            std::cout << "  " << hex(off, 6) << " rel +" << hex(rel, 3) << tag
                      << ":";
            for (std::size_t i = 0; i < vals.size() && i < 6; ++i) {
                std::cout << " " << hex(vals[i], 8).substr(2);
            }
            std::cout << "\n";
        }
    }
    return 0;
}

void print_table(const vbios::TweakTable& t, const Options& o) {
    std::cout << "  Memory Tweak Table v" << hex(t.version, 2) << " at file "
              << "offset " << hex(static_cast<std::uint32_t>(t.file_offset), 1)
              << ": " << t.entry_count << " entries, " << t.ext_entry_count
              << " extended entries each\n";

    std::vector<std::string> cols;
    if (!o.columns.empty()) {
        std::istringstream is(o.columns);
        std::string c;
        while (std::getline(is, c, ',')) if (!c.empty()) cols.push_back(c);
    } else {
        cols = {"RC", "RFC", "RAS", "RP", "CL", "WL",
                "RD_RCD", "WR_RCD", "FAW", "RRD", "REFRESH", "WR"};
    }

    std::ostringstream head;
    head << "  idx  ";
    for (const std::string& c : cols) head << std::setw(9) << c;
    head << std::setw(7) << "R2P";
    std::cout << head.str() << "\n";
    std::cout << "  " << std::string(head.str().size() - 2, '-') << "\n";

    for (const vbios::TweakEntry& e : t.entries) {
        std::cout << "  " << std::setw(3) << e.index << "  ";
        for (const std::string& c : cols) std::cout << std::setw(9) << e.field(c);
        std::cout << std::setw(7) << e.r2p << "\n";
    }

    if (!o.full) return;
    std::cout << "\n";
    for (const vbios::TweakEntry& e : t.entries) {
        std::cout << "  entry " << e.index << ":\n";
        const auto& tregs = timing_registers();
        for (std::size_t c = 0; c < tregs.size(); ++c) {
            std::cout << "    " << tregs[c].name << " = " << hex(e.config[c], 8)
                      << "\n";
            for (const Field& f : tregs[c].fields) {
                std::cout << "        " << std::left << std::setw(12) << f.name
                          << std::right << std::setw(6)
                          << f.extract(e.config[c]) << "\n";
            }
        }
        std::cout << "    TIMING22 = " << hex(e.timing22, 8)
                  << "  RFCSBA=" << e.field("RFCSBA")
                  << " RFCSBR=" << e.field("RFCSBR") << "\n";
        std::cout << "    R2P=" << e.r2p << "  RDCRC=" << e.rdcrc
                  << "  DriveStrength=" << e.drive_strength << "  Voltages=[";
        for (std::size_t i = 0; i < e.voltages.size(); ++i) {
            std::cout << (i ? ", " : "") << e.voltages[i];
        }
        std::cout << "]\n\n";
    }
}

int cmd_vbios(const Options& o) {
    if (!o.rom.empty()) {
        std::cout << o.rom << "\n";
        print_table(vbios::find_tweak_table(vbios::read_file(o.rom)), o);
        return 0;
    }
    need_root();
    int rc = 0;
    for (auto& g : open_targets(o, false)) {
        std::cout << g->dev().slot << "  " << g->arch().codename << "\n";
        // The BAR0 PROM mirror is the only VBIOS-read route on Windows.
        try {
            std::vector<std::uint8_t> blob = vbios::read_prom(*g);
            std::cout << "  source: BAR0 PROM mirror\n";
            print_table(vbios::find_tweak_table(blob), o);
        } catch (const vbios::VbiosError& e) {
            err(e.what());
            rc = 1;
        } catch (const MmioError& e) {
            err(e.what());
            rc = 1;
        }
    }
    return rc;
}

void usage() {
    std::cout << R"(nvtune - NVIDIA FBPA memory timing tool

usage: nvtune <command> [options]

commands:
  list                      enumerate GPUs, identify the chip, show topology
  fields                    every tunable parameter, register, bit range, limits
  dump                      decode current timings
  get FIELD...              read specific fields
  set FIELD=VALUE...        write fields (dry run unless --commit)
  save                      snapshot all timing registers to JSON
  restore                   write a snapshot back
  apply PROFILE.json        apply a JSON profile
  daemon [FIELD=VALUE...]   hold values against driver reprogramming
  probe                     dump or watch raw FBPA words
  vbios-scan ROM            report PCIe-clamp sites + checksum status
  vbios-patch ROM           patch a ROM to force PCIe Gen2 (file op)
      [--arch fermi|tesla] [--offset 0xNN --value 0xVV] [-o OUT] [--commit]
  clocks                    read + decode clock domains
  pcie [SLOT]               decode PCIe link speed/width (GPU or any slot)
  cfg-peek SLOT OFF...      read raw PCI config space (any device)
  cfg-poke SLOT OFF VAL     write raw PCI config space (needs --yes)
  peek OFFSET...            read raw dword(s) at FBPA-relative offset
  poke OFFSET VALUE         write a raw dword (needs --yes)
  vbios                     parse the VBIOS Memory Tweak Table

common options:
  -d, --device SLOT     PCI slot, e.g. 0000:08:00.0. Repeatable.
                        Default: all NVIDIA GPUs.
      --fbpa N          target one partition instead of the broadcast aperture
      --all-fbpa        target every active partition individually
      --commit          actually write (set/apply)
      --force           write even if range checks complain
  -o, --output PATH     save destination
  -i, --input PATH      restore source

dump options:      --raw  --optional
daemon options:    --profile PATH  --interval SECONDS  -v/--verbose
probe options:     --start OFF  --length BYTES  --watch SECONDS
vbios options:     --rom FILE  --prom  --full  --columns A,B,C

Writing to memory-controller registers can hang the machine and corrupt VRAM.
Everything defaults to a dry run; --commit is required to touch hardware.
)";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage();
        return 0;
    }

    std::vector<std::string> args(argv + 2, argv + argc);
    for (const std::string& a : args) {
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        }
    }

    try {
        const Options o = parse_options(args);
        if (cmd == "list")    return cmd_list(o);
        if (cmd == "fields")  return cmd_fields(o);
        if (cmd == "dump")    return cmd_dump(o);
        if (cmd == "get")     return cmd_get(o);
        if (cmd == "set")     return cmd_set(o);
        if (cmd == "save")    return cmd_save(o);
        if (cmd == "restore") return cmd_restore(o);
        if (cmd == "apply")   return cmd_apply(o);
        if (cmd == "daemon")  return cmd_daemon(o);
        if (cmd == "vbios-scan")  return cmd_vbios_scan(o);
        if (cmd == "vbios-patch") return cmd_vbios_patch(o);
        if (cmd == "clocks")  return cmd_clocks(o);
        if (cmd == "pcie")    return cmd_pcie(o);
        if (cmd == "cfg-peek")return cmd_cfg_peek(o);
        if (cmd == "cfg-poke")return cmd_cfg_poke(o);
        if (cmd == "peek")    return cmd_peek(o);
        if (cmd == "poke")    return cmd_poke(o);
        if (cmd == "probe")   return cmd_probe(o);
        if (cmd == "vbios")   return cmd_vbios(o);
        err("unknown command '" + cmd + "'");
        usage();
        return 2;
    } catch (const MmioError& e) {
        err(e.what());
        return 1;
    } catch (const vbios::VbiosError& e) {
        err(e.what());
        return 1;
    } catch (const json::ParseError& e) {
        err(e.what());
        return 1;
    } catch (const std::exception& e) {
        err(e.what());
        return 1;
    }
}

}  // namespace nvtune::cli
