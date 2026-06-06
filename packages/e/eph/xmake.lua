package("eph")
    set_kind("library", {headeronly = true})
    set_description("High-frequency trading C++ primitives (header-only modules)")

    set_urls("https://github.com/sqfzy/ephemeral.git")
    -- v0.1.0 — post-split layout (eph-core / eph-utils / eph-net / eph-codec /
    -- eph-net-kernel / eph-json / ...). Pin to the exact commit for
    -- reproducible, machine-portable fetches.
    add_versions("0.1.0", "5f2bc6b2040af48756948d4cb6b926bca2502c15")

    -- eph headers log through spdlog (SPDLOG_ACTIVE_LEVEL gated), so every
    -- consumer needs it on the include path. Propagate it transitively.
    add_deps("spdlog")

    on_install(function (package)
        -- eph is consumed HEADER-ONLY. We deliberately do NOT run eph's own
        -- xmake build: the post-split tree carries DPDK-coupled binary targets
        -- (eph_nicd, eph_nicctl, ...) that fail to build on hosts without DPDK,
        -- which would break portable consumption. Instead, merge every module's
        -- public include/ tree into the package include dir.
        --
        -- Copy each header INDIVIDUALLY: modules share parent dirs — e.g.
        -- eph-net owns eph/net/*.hpp while eph-net-kernel owns
        -- eph/net/kernel/*.hpp. A directory-level os.cp (even with {rootdir})
        -- REPLACES the shared eph/net/ subtree, so the second module silently
        -- wipes the first's files. Copying one file at a time to its computed
        -- destination only ever creates/overwrites that single file, so the
        -- module trees merge cleanly.
        local installdir = package:installdir("include")
        local count = 0
        for _, dir in ipairs(os.dirs("eph-*")) do
            local inc = path.join(dir, "include")
            if os.isdir(inc) then
                for _, file in ipairs(os.files(path.join(inc, "**"))) do
                    os.cp(file, path.join(installdir, path.relative(file, inc)))
                end
                count = count + 1
            end
        end
        assert(count > 0, "eph: no eph-*/include directories found — layout changed?")
        print("eph: installed headers from %d module(s)", count)
    end)

    on_test(function (package)
        package:check_cxxsnippets({test = [[
            #include <eph/version.hpp>
            void test() {}
        ]]}, {configs = {languages = "c++23"}})
    end)
