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
bool print_ops(const std::vector<WriteOp>& ops) {
    bool warned = false;
    for (const WriteOp& op : ops) {
        if (!op.dirty()) {
            std::cout << "  " << op.reg->name << " @" << hex(op.offset, 6)
                      << "  unchanged (" << hex(op.old_word, 8) << ")\n";
            continue;
        }
        std::cout << "  " << op.reg->name << " @" << hex(op.offset, 6) << "  "
                  << hex(op.old_word, 8) << " -> " << hex(op.new_word, 8)
                  << "  [write]\n";
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
    for (auto& g : open_targets(o, true)) {
        const Arch& a = g->arch();
        std::cout << g->dev().slot << "  " << a.codename << " (" << a.family
                  << ")\n";
        if (!a.caveat.empty()) std::cout << "  note: " << a.caveat << "\n";

        for (Scope s : scopes_for(*g, o)) {
            std::cout << "  [" << scope_label(s) << "]\n";
            std::vector<WriteOp> ops = g->plan(assignments, s);
            const bool warned = print_ops(ops);
            if (warned && !o.force) {
                err("refusing to write with warnings outstanding; re-run with "
                    "--force if you mean it");
                rc = 1;
                continue;
            }

            ensure_stock_backup(*g);
            std::vector<std::string> problems = g->commit(ops);
            for (const std::string& p : problems) {
                std::cout << "      ! readback mismatch: " << p << "\n";
                rc = 1;
            }
            if (problems.empty()) std::cout << "      applied and verified\n";
        }
        std::cout << "  reminder: the driver reprograms these on p-state "
                     "changes. Use 'nvtune daemon' to hold them.\n";
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
  set FIELD=VALUE...        write fields
  save                      snapshot all timing registers to JSON
  restore                   write a snapshot back
  apply PROFILE.json        apply a JSON profile
  daemon [FIELD=VALUE...]   hold values against driver reprogramming
  probe                     dump or watch raw FBPA words
  clocks                    read + decode clock domains
  peek OFFSET...            read raw dword(s) at FBPA-relative offset
  poke OFFSET VALUE         write a raw dword (needs --yes)
  vbios                     parse the VBIOS Memory Tweak Table

common options:
  -d, --device SLOT     PCI slot, e.g. 0000:08:00.0. Repeatable.
                        Default: all NVIDIA GPUs.
      --fbpa N          target one partition instead of the broadcast aperture
      --all-fbpa        target every active partition individually
      --force           write even if range checks complain
  -o, --output PATH     save destination
  -i, --input PATH      restore source

dump options:      --raw  --optional
daemon options:    --profile PATH  --interval SECONDS  -v/--verbose
probe options:     --start OFF  --length BYTES  --watch SECONDS
vbios options:     --rom FILE  --prom  --full  --columns A,B,C

Writing to memory-controller registers can hang the machine and corrupt VRAM.
A first write snapshots stock values; use 'restore' to roll back.
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
        if (cmd == "clocks")  return cmd_clocks(o);
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
