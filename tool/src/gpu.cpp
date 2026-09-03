#include "nvtune/gpu.hpp"

#include <ctime>
#include <cstdio>
#include <utility>

#include "nvtune/json.hpp"

namespace nvtune {
namespace {

std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

std::string hex_off(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%06X", v);
    return buf;
}

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

}  // namespace

Gpu::Gpu(PciDevice dev, bool writable)
    : dev_(std::move(dev)), bar_(dev_, writable) {}

void Gpu::open() {
    bar_.open();
    opened_ = true;
    boot0_ = bar_.rd32(NV_PMC_BOOT_0);
    if (boot0_ == 0x00000000u || boot0_ == 0xFFFFFFFFu) {
        bar_.close();
        opened_ = false;
        throw MmioError(
            dev_.slot + ": BOOT_0 reads " + hex32(boot0_) +
            ". The device is not responding on BAR0 (powered down, in D3, or "
            "the mapping is not actually reaching the device).");
    }
    arch_ = identify(boot0_);

    // Windows learns BAR0's geometry from the driver, so
    // fold it back in once the mapping exists.
    if (bar_.phys_base() != 0) dev_.bar0_start = bar_.phys_base();
    if (bar_.size() != 0) dev_.bar0_size = bar_.size();
}

void Gpu::close() noexcept {
    if (opened_) {
        bar_.close();
        opened_ = false;
    }
}

// ---------------------------------------------------------------- topology

unsigned Gpu::num_fbpa() const {
    const unsigned raw = bar_.rd32(NV_PTOP_SCAL_NUM_FBPAS) & 0x1Fu;
    return (raw > 0 && raw <= arch_.layout.max_fbpa) ? raw
                                                     : arch_.layout.max_fbpa;
}

unsigned Gpu::fbpa_per_fbp() const {
    return bar_.rd32(NV_PTOP_SCAL_NUM_FBPA_PER_FBP) & 0x1Fu;
}

std::uint32_t Gpu::floorswept_mask() const {
    return bar_.rd32(NV_FUSE_STATUS_OPT_FBIO) & 0xFFFFu;
}

std::vector<unsigned> Gpu::active_fbpas() const {
    const std::uint32_t mask = floorswept_mask();
    std::vector<unsigned> out;
    const unsigned n = num_fbpa();
    for (unsigned i = 0; i < n; ++i) {
        if (((mask >> i) & 1u) == 0u) out.push_back(i);
    }
    return out;
}

std::uint32_t Gpu::fbpa_ram_amount(unsigned index) const {
    return bar_.rd32(aperture(index) + NV_PFB_FBPA_CSTATUS_RAMAMOUNT);
}

// ---------------------------------------------------------------- aperture

std::uint32_t Gpu::aperture(Scope scope) const {
    const Layout& L = arch_.layout;
    if (!scope.has_value()) return L.broadcast;
    const unsigned i = *scope;
    if (i >= L.max_fbpa) {
        throw std::out_of_range("FBPA index " + std::to_string(i) +
                                " out of range 0.." +
                                std::to_string(L.max_fbpa - 1));
    }
    return L.unicast + i * L.unicast_stride;
}

std::uint32_t Gpu::reg_offset(const Register& r, Scope scope) const {
    return aperture(scope) + r.offset;
}

// ---------------------------------------------------------------- read

std::uint32_t Gpu::read_reg(const Register& r, Scope scope) const {
    return bar_.rd32(reg_offset(r, scope));
}

std::uint32_t Gpu::read_field(const std::string& name, Scope scope) const {
    FieldRef ref = lookup(resolve_alias(name));
    return ref.field->extract(read_reg(*ref.reg, scope));
}

// ---------------------------------------------------------------- planning

std::vector<std::string> Gpu::field_warnings(const Field& f,
                                             std::uint32_t old_v,
                                             std::uint32_t new_v) {
    std::vector<std::string> w;
    if (!f.tunable && new_v != old_v) {
        w.push_back(std::string(f.name) +
                    " is a training/structural value, not a latency knob. "
                    "Changing it is very likely to corrupt the memory "
                    "interface.");
    }
    if (f.typical.has_value()) {
        const auto [lo, hi] = *f.typical;
        if (new_v < lo || new_v > hi) {
            w.push_back(std::string(f.name) + "=" + std::to_string(new_v) +
                        " is outside the typical range " + std::to_string(lo) +
                        ".." + std::to_string(hi) + ".");
        }
    }
    if (old_v != 0 && new_v * 2 < old_v) {
        w.push_back(std::string(f.name) + " more than halved (" +
                    std::to_string(old_v) + " -> " + std::to_string(new_v) +
                    "); step in small increments instead.");
    }
    return w;
}

std::vector<WriteOp> Gpu::plan(const Assignments& assignments,
                               Scope scope) const {
    // Group by register, preserving the register-table order so output is
    // stable regardless of the order fields were typed on the command line.
    std::vector<WriteOp> ops;
    for (const Register& reg : all_registers()) {
        std::vector<std::pair<const Field*, std::uint32_t>> items;
        for (const auto& [name, value] : assignments) {
            FieldRef ref = lookup(resolve_alias(name));
            if (ref.reg == &reg) items.emplace_back(ref.field, value);
        }
        if (items.empty()) continue;

        WriteOp op;
        op.reg = &reg;
        op.offset = reg_offset(reg, scope);
        op.old_word = bar_.rd32(op.offset);
        op.new_word = op.old_word;

        if (reg.confidence != Confidence::Documented) {
            op.warnings.push_back(std::string(reg.name) + " offset is " +
                                  to_string(reg.confidence) + ": " + reg.note);
        }

        for (const auto& [f, value] : items) {
            const std::uint32_t cur = f->extract(op.new_word);
            op.new_word = f->insert(op.new_word, value);
            if (cur != value) op.changes.push_back({f->name, cur, value});
            for (std::string& w : field_warnings(*f, cur, value)) {
                op.warnings.push_back(std::move(w));
            }
        }
        ops.push_back(std::move(op));
    }
    return ops;
}

// ---------------------------------------------------------------- write

std::vector<std::string> Gpu::commit(const std::vector<WriteOp>& ops,
                                     bool verify) {
    if (!bar_.writable()) {
        throw MmioError("device opened read-only (a write path was reached "
                        "without a writable mapping).");
    }
    std::vector<std::string> problems;
    for (const WriteOp& op : ops) {
        if (!op.dirty()) continue;
        bar_.wr32(op.offset, op.new_word);
        if (!verify) continue;
        const std::uint32_t back = bar_.rd32(op.offset);
        if (back != op.new_word) {
            problems.push_back(std::string(op.reg->name) + " @" +
                               hex_off(op.offset) + ": wrote " +
                               hex32(op.new_word) + ", read back " +
                               hex32(back));
        }
    }
    return problems;
}

// ---------------------------------------------------------------- backup

void Gpu::backup(const std::string& path, bool include_optional) const {
    const std::vector<Register>& which =
        include_optional ? all_registers() : timing_registers();

    json::Object root;
    root["_format"] = json::Value("nvtune-backup-1");
    root["_taken"] = json::Value(now_iso());
    root["slot"] = json::Value(dev_.slot);
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%04x:%04x", dev_.vendor, dev_.device);
        root["pci_id"] = json::Value(std::string(buf));
    }
    root["boot0"] = json::Value(hex32(boot0_));
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%03X", arch_.chipset);
        root["chipset"] = json::Value(std::string(buf));
    }
    root["codename"] = json::Value(arch_.codename);
    root["aperture_broadcast"] = json::Value(hex_off(arch_.layout.broadcast));

    json::Object regs_obj;
    auto snap = [&](Scope scope) {
        json::Object words;
        for (const Register& r : which) {
            words[r.name] = json::Value(hex32(read_reg(r, scope)));
        }
        return json::Value(std::move(words));
    };
    regs_obj["broadcast"] = snap(kBroadcast);
    for (unsigned i : active_fbpas()) {
        try {
            regs_obj["fbpa" + std::to_string(i)] = snap(i);
        } catch (const MmioError&) {
            // Partition aperture unreadable; skip rather than abort the backup.
        }
    }
    root["registers"] = json::Value(std::move(regs_obj));

    json::write_file_atomic(path, json::Value(std::move(root)));
}

std::vector<std::string> Gpu::restore(const std::string& path, bool verify) {
    json::Value doc = json::parse_file(path);
    if (doc.str_or("_format", "") != "nvtune-backup-1") {
        throw std::runtime_error(path + " is not an nvtune backup");
    }
    if (doc.str_or("boot0", "") != hex32(boot0_)) {
        throw std::runtime_error(
            path + " was taken on a different chip (" +
            doc.str_or("codename", "?") + ", boot0 " +
            doc.str_or("boot0", "?") + ")");
    }

    std::map<std::string, const Register*> by_name;
    for (const Register& r : all_registers()) by_name[r.name] = &r;

    std::vector<std::string> problems;
    for (const auto& [scope_name, words] : doc.at("registers").as_object()) {
        Scope scope = kBroadcast;
        if (scope_name != "broadcast") {
            if (scope_name.rfind("fbpa", 0) != 0) continue;
            scope = static_cast<unsigned>(std::stoul(scope_name.substr(4)));
        }
        for (const auto& [reg_name, hexval] : words.as_object()) {
            auto it = by_name.find(reg_name);
            if (it == by_name.end()) continue;
            const std::uint32_t offset = reg_offset(*it->second, scope);
            const std::uint32_t value = json::as_u32(hexval);
            bar_.wr32(offset, value);
            if (verify && bar_.rd32(offset) != value) {
                problems.push_back(scope_name + "/" + reg_name +
                                   " did not take");
            }
        }
    }
    return problems;
}

}  // namespace nvtune
