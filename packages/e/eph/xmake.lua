package("eph")
    set_kind("library", {headeronly = true})
    set_description("High-frequency trading C++ primitive")

    set_urls("https://github.com/sqfzy/ephemeral.git")
    add_versions("1.0", "77de1d2")

    on_install(function (package)
        local configs = {}
        if package:config("shared") then
            configs.kind = "shared"
        end
        import("package.tools.xmake").install(package, configs)
    end)

    on_test(function (package)
        package:check_cxxsnippets({test = [[
            #include <eph/platform.hpp>
            void test() {
                eph::cpu_relax();
            }
        ]]}, {configs = {languages = "c++23"}})
    end)
