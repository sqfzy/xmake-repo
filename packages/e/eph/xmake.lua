package("eph")
    set_kind("library", {headeronly = true})
    set_description("High-frequency trading C++ primitives (header-only modules)")

    set_urls("https://github.com/sqfzy/ephemeral.git")
    -- Pin to an exact commit for reproducible, machine-portable fetches.
    -- Labels are repo-local (eph's own version.hpp stays 1.0.0); each maps to a commit.
    -- 0.3.0 = ephemeral@8557230: adds eph-sbe (zero-copy SBE decode) AND the
    -- BREAKING non-blocking connect — TcpStream::create() returns a *connecting*
    -- stream driven across poll cycles (was: blocks until Established).
    -- 0.2.0 = ephemeral@1a4e9df: StreamConfig.ws/.kernel + WsCodec::encode_text
    -- (the API market_infra_eph consumes). The older v0.1.0 tag predates it.
    -- 0.4.0 = ephemeral@79cfeb9: eph-sbe Binance WS-API/order/exec/stream SBE
    -- accessors + nested messageData decode; eph-net Ed25519 signer (Binance
    -- session.logon). Additive over 0.3.0.
    -- 0.4.1 = ephemeral@ecf3677: eph-sbe is_supported accepts version >= pinned
    -- (Binance serves "highest compatible" schema — live WS API answers 3:4 to a
    -- 3:2 request; accessors are append-only forward-compatible).
    -- 0.5.0 = ephemeral@7d78512: logging overhaul — single compile-time gate
    -- EPH_ENABLE_LOG (default OFF → eph emits NOTHING, zero hot-path cost, never
    -- touches the host's spdlog default logger nor opens a sink). When enabled,
    -- per-subsystem `eph.*` named loggers via EPH_LOG_*/eph::log::get; removes the
    -- old self-creating stdout loggers. Header/API-compatible for consumers
    -- (SBE/net/codec unchanged) — additive over 0.4.1.
    -- 0.5.1 = ephemeral@a8b001b: eph-sbe exposes cancel_order::transact_time_us
    -- (CancelOrderResponse field id=5 @ block offset +18) — symmetric with
    -- new_order_ack, lets consumers measure cancel uplink latency. Additive over 0.5.0.
    -- 0.5.2 = ephemeral@e0bad78: eph-sbe exposes web_socket_response::for_each_rate_limit
    -- (envelope rateLimits group id=100) → per-IP REQUEST_WEIGHT/ORDERS {limit,current} for
    -- usage-driven throttling. Additive over 0.5.1.
    add_versions("0.5.2", "e0bad78ab91ec9a7275e4859b7e5084158c66439")
    add_versions("0.5.1", "a8b001b28637764cff27a0c357b0f9f13a0fbb91")
    add_versions("0.5.0", "7d7851217d400b431815aa1e1a5fd74a55d9f8df")
    add_versions("0.4.1", "ecf36779f2c8ce0b9393ca768c13bd6b502e63db")
    add_versions("0.4.0", "79cfeb9755c7eb783cc53a134edb0d1f85fb2dda")
    add_versions("0.3.0", "855723015e5bbf9da307ec7a14705d5f9f3ed713")
    add_versions("0.2.0", "1a4e9dfeac4fda7e4756326d1864c2fd80c7d9f0")
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
