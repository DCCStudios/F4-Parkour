-- Multi-runtime build: one DLL supports OG, NG, and AE.
-- Uses the same CommonLibF4 fork FPGunplayOverhaul builds against
-- (referenced in place — keep the two checkouts side by side).
includes("../FPGunplayOverhaul/lib/commonlibf4")

set_project("F4Parkour")
set_version("1.0.0")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")
set_allowedarchs("windows|x64")
set_allowedmodes("debug", "releasedbg")
set_defaultarchs("windows|x64")
set_defaultmode("releasedbg")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- REL::ID initializer-list slots are always [OG, NG, AE].
add_defines("COMMONLIB_RUNTIMECOUNT=3")

target("F4Parkour")
    add_rules("commonlibf4.plugin", {
        name = "F4Parkour",
        author = "DCC Studios",
        description = "Code-driven vaulting and mantling.",
        plugin_template = path.join(os.projectdir(), "res/commonlibf4-plugin.cpp.in"),
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")

    add_defines(
        "_UNICODE",
        "UNICODE",
        "NOMINMAX",
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING"
    )

    set_pcxxheader("src/PCH.h")
    set_runtimes("MD")
    set_symbols("debug")
    set_optimize("fastest")

    -- Keep the existing staged mod layout used by deployment.
    set_targetdir("Compile/F4SE/Plugins")

    after_build(function(target)
        os.cp("F4Parkour.ini", target:targetdir())
    end)
