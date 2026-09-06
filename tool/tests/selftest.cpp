// SPDX-License-Identifier: GPL-3.0-or-later
// Executes the real CLI/Gpu planner with the in-memory fake_backend.cpp only.

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fake_backend.hpp"
#include "nvtune/arch.hpp"
#include "nvtune/cli.hpp"

namespace {

using nvtune::test::backend;
using nvtune::test::reset_backend;
using nvtune::test::kSlot;
constexpr const char* kComplete = "dry run complete: no registers written\n";

// Deliberately not assert(): these checks must execute in Release/NDEBUG builds.
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Capture {
    std::ostream& stream;
    std::ostringstream text;
    std::streambuf* original;
    std::ios formatting{nullptr};
    explicit Capture(std::ostream& target) : stream(target), original(target.rdbuf()) {
        formatting.copyfmt(stream);
        stream.rdbuf(text.rdbuf());
    }
    ~Capture() {
        stream.rdbuf(original);
        stream.copyfmt(formatting);
    }
};

struct Run {
    int rc;
    std::string out;
    std::string err;
};

Run run(const std::vector<std::string>& arguments) {
    std::vector<std::string> storage{"nvtune"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : storage) argv.push_back(argument.data());
    const int argc = static_cast<int>(argv.size());
    argv.push_back(nullptr);
    Capture out(std::cout), err(std::cerr);
    const int rc = nvtune::cli::main(argc, argv.data());
    return {rc, out.text.str(), err.text.str()};
}

bool contains(const std::string& text, const std::string& value) {
    return text.find(value) != std::string::npos;
}

void require_no_write_or_backup() {
    require(backend().write_attempts == 0, "preview attempted a BAR0 write");
    require(backend().writes == 0, "preview changed fake BAR0");
    require(backend().config_dir_calls == 0, "preview requested a backup directory");
    require(backend().ensure_dir_calls == 0, "preview attempted to create a backup directory");
    require(backend().file_exists_calls == 0, "preview entered the stock backup path");
    require(backend().words.at(nvtune::test::kConfig3) == nvtune::test::kInitialConfig3,
            "preview changed the seeded register word");
}

void require_preview(const Run& result, bool changed = true) {
    require(result.rc == 0, "preview failed: " + result.err);
    require(result.err.empty(), "successful preview wrote an error");
    require(result.out.size() >= std::string(kComplete).size() &&
            result.out.compare(result.out.size() - std::string(kComplete).size(),
                               std::string(kComplete).size(), kComplete) == 0,
            "preview lacks final explicit completion marker");
    require(!contains(result.out, "[write]"), "preview was labelled as a write");
    require(!contains(result.out, "applied and verified"), "preview claimed a commit");
    require(contains(result.out, changed ? "[would write]" : "unchanged"),
            "preview did not describe the expected register operation");
    require(backend().readonly_opens == 1 && backend().writable_opens == 0,
            "preview did not open exactly one read-only fake GPU");
    require(backend().opened_slots == std::vector<std::string>{kSlot},
            "preview selected the wrong GPU");
    require(backend().closes == 1, "preview did not release the fake GPU");
    require(backend().reads > 1, "preview did not read and plan real registers");
    require_no_write_or_backup();
}

void require_rejected_before_hardware(const std::vector<std::string>& args) {
    reset_backend();
    const Run result = run(args);
    require(result.rc == 2, "invalid mode did not return usage status 2");
    require(contains(result.err, "error:"), "invalid mode did not explain its error");
    require(backend().pci_queries == 0 && backend().privilege_checks == 0,
            "invalid mode reached GPU discovery or privilege checking");
    require(backend().readonly_opens == 0 && backend().writable_opens == 0 &&
            backend().reads == 0, "invalid mode opened or read a GPU");
    require_no_write_or_backup();
}

void require_commit(const std::vector<std::string>& args) {
    reset_backend();
    const Run result = run(args);
    require(result.rc == 0, "fixture commit failed: " + result.err);
    require(backend().writable_opens == 1 && backend().readonly_opens == 0,
            "commit did not open one writable fake GPU");
    require(backend().writes == 1 && backend().write_attempts == 1,
            "commit did not write exactly one fake register");
    require(backend().words.at(nvtune::test::kConfig3) == nvtune::test::kUpdatedConfig3,
            "commit did not apply FAW=13 while preserving other register bits");
    require(contains(result.out, "[write]") && contains(result.out, "applied and verified"),
            "commit did not report writing and verification");
    require(!contains(result.out, kComplete), "commit claimed to be a preview");
    require(backend().config_dir_calls == 1 && backend().ensure_dir_calls == 1 &&
            backend().file_exists_calls == 1, "commit skipped stock-backup protection");
    require(backend().closes == 1, "commit did not release the fake GPU");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "usage: selftest [tests/fixtures directory]\n";
        return 2;
    }
    const std::string fixture_dir = argc == 2 ? argv[1] : "tests/fixtures";
    const std::string profile = fixture_dir + "/preview.json";
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"fixture identifies as GP102", [] {
            const auto arch = nvtune::identify(nvtune::test::kBoot0);
            require(arch.chipset == 0x132 && arch.codename == "GP102" &&
                    arch.layout.broadcast == 0x9A0000, "incorrect fake GPU architecture");
        }},
        {"explicit set preview reads only", [] {
            reset_backend();
            const Run result = run({"set", "-d", kSlot, "--dry-run", "FAW=13"});
            require_preview(result);
            require(contains(result.out, "CONFIG3 @0x9A029C") &&
                    contains(result.out, "0x2200194A -> 0x22001B4A"),
                    "real planner did not compute the expected register update");
        }},
        {"force cannot turn a warning preview into a write", [] {
            reset_backend();
            const Run result = run({"set", "FAW=5", "--force", "--dry-run", "-d", kSlot});
            require_preview(result);
            require(contains(result.out, "outside the typical range") &&
                    contains(result.out, "more than halved"), "preview lost real range warnings");
        }},
        {"warnings remain previewable without force", [] {
            reset_backend();
            require_preview(run({"set", "FAW=5", "--dry-run", "-d", kSlot}));
        }},
        {"unchanged preview still acknowledges completion", [] {
            reset_backend();
            const Run result = run({"set", "FAW=12", "--dry-run", "-d", kSlot});
            require_preview(result, false);
            require(!contains(result.out, "[would write]"), "unchanged register was marked dirty");
        }},
        {"apply profile preview reads only", [&] {
            reset_backend();
            require_preview(run({"apply", profile, "--dry-run", "-d", kSlot}));
        }},
        {"all FBPA preview reads every scope without writes", [] {
            reset_backend();
            const Run result = run({"set", "FAW=13", "--all-fbpa", "--dry-run", "-d", kSlot});
            require_preview(result);
            require(contains(result.out, "[FBPA0]") && contains(result.out, "[FBPA1]"),
                    "preview did not cover both fake partitions");
        }},
        {"later scope failure cannot emit success marker", [] {
            reset_backend();
            backend().fail_read_offset = 0x90429C;
            const Run result = run({"set", "FAW=13", "--all-fbpa", "--dry-run", "-d", kSlot});
            require(result.rc == 1, "read failure was reported as success");
            require(contains(result.out, "[would write]"), "failure did not follow a partial plan");
            require(!contains(result.out, kComplete), "partial plan emitted completion marker");
            require_no_write_or_backup();
        }},
        {"second GPU open failure invalidates the entire preview", [] {
            reset_backend();
            backend().include_second_gpu = true;
            backend().fail_open_slot = nvtune::test::kSecondSlot;
            const Run result = run({"set", "FAW=13", "--dry-run"});
            require(result.rc == 1 && contains(result.err, "injected fixture open failure"),
                    "partial GPU availability was reported as success");
            require(!contains(result.out, kComplete) && !contains(result.out, "[would write]"),
                    "preview planned only the available GPU or emitted success");
            require(backend().open_attempts == 2 && backend().readonly_opens == 1 &&
                    backend().writable_opens == 0 && backend().closes == 1,
                    "partial GPU open did not close the first read-only handle");
            require_no_write_or_backup();
        }},
        {"zero GPUs cannot produce a successful preview", [] {
            reset_backend();
            backend().no_devices = true;
            const Run result = run({"set", "FAW=13", "--dry-run"});
            require(result.rc == 1 && !contains(result.out, kComplete),
                    "zero GPUs was reported as a complete preview");
            require(backend().open_attempts == 0, "zero GPUs still opened a device");
            require_no_write_or_backup();
        }},
        {"conflicting modes reject before hardware", [] {
            require_rejected_before_hardware({"set", "FAW=13", "--dry-run", "--commit"});
            require_rejected_before_hardware({"apply", "not-opened.json", "--commit", "--dry-run"});
        }},
        {"unsupported preview modes reject before hardware", [] {
            for (const char* command : {"get", "save", "restore", "daemon", "poke", "list", "fields"})
                require_rejected_before_hardware({command, "--dry-run"});
        }},
        {"unsupported commit mode rejects before hardware", [] {
            require_rejected_before_hardware({"get", "FAW", "--commit"});
        }},
        {"invalid assignment rejects before hardware", [] {
            require_rejected_before_hardware({"set", "FAW=9999", "--dry-run"});
            require_rejected_before_hardware({"set", "UNKNOWN_FIELD=1", "--dry-run"});
        }},
        {"explicit commit writes fake backend", [] {
            require_commit({"set", "-d", kSlot, "FAW=13", "--commit"});
        }},
        {"legacy default set still writes fake backend", [] {
            require_commit({"set", "-d", kSlot, "FAW=13"});
        }},
        {"warning commit refuses before write or backup", [] {
            reset_backend();
            const Run result = run({"set", "-d", kSlot, "FAW=5", "--commit"});
            require(result.rc == 1 && contains(result.err, "refusing to write with warnings"),
                    "warning commit was not refused");
            require_no_write_or_backup();
        }},
    };

    unsigned failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    }
    std::cout << tests.size() << " CLI contract cases, " << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
