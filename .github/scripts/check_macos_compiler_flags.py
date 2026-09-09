import subprocess
import tempfile
from pathlib import Path


def main():
    repo = Path(__file__).resolve().parents[2]
    helper = repo / "Telegram/cmake/telegram_compilation_cache.cmake"
    tgcalls = repo / "Telegram/ThirdParty/tgcalls/tgcalls"
    openssl = Path(subprocess.check_output([
        "brew", "--prefix", "openssl@3",
    ], text=True).strip())
    with tempfile.TemporaryDirectory(prefix="ayugram compiler flags ") as temporary:
        root = Path(temporary)
        source = root / "source"
        app = source / "app"
        app.mkdir(parents=True)
        (source / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.25)
project(CompilerFlags LANGUAGES C CXX OBJCXX)
add_subdirectory(app)
""")
        (app / "CMakeLists.txt").write_text("""include("${HELPER_FILE}")
add_library(c_probe STATIC probe.c)
target_compile_options(c_probe PRIVATE -DPROBE_OPTION=1)
foreach(language cxx objcxx)
    if(language STREQUAL cxx)
        set(extension cpp)
    else()
        set(extension mm)
    endif()
    add_library(${language}_probe STATIC probe.${extension})
    target_compile_features(${language}_probe PRIVATE cxx_std_20)
    target_compile_options(${language}_probe PRIVATE -DPROBE_OPTION=1)
    target_precompile_headers(${language}_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pch.h")
endforeach()
add_library(tgcalls_crypto_probe STATIC "${TGCALLS_SOURCE_DIR}/CryptoHelper.cpp")
target_compile_features(tgcalls_crypto_probe PRIVATE cxx_std_20)
target_include_directories(tgcalls_crypto_probe PRIVATE "${OPENSSL_INCLUDE_DIR}")
""")
        (app / "probe.c").write_text("""#ifndef PROBE_OPTION
#error C compiler options were overwritten
#endif
int c_probe(void) { return PROBE_OPTION; }
""")
        (app / "pch.h").write_text("#include <cstddef>\n")
        for extension in ("cpp", "mm"):
            (app / f"probe.{extension}").write_text("""#ifndef PROBE_OPTION
#error C++ compiler options were overwritten
#endif
#if __cplusplus < 202002L
#error The build must preserve C++20
#endif
template <typename T> concept HasSize = requires(T value) { sizeof(value); };
static_assert(HasSize<char16_t>);
int cpp_probe() { return PROBE_OPTION; }
""")
        # Test Xcode's generated compiler flags without a remote cache.
        # env forwards compiler arguments unchanged and works as a launcher.
        # The same helper still runs its Xcode target setup, which previously
        # replaced the standard, PCH flags and target-specific options.
        for arch in ("x86_64", "arm64"):
            build = root / f"build-{arch}"
            subprocess.run([
                "cmake", "-S", str(source), "-B", str(build), "-G", "Xcode",
                f"-DHELPER_FILE={helper.as_posix()}",
                f"-DTGCALLS_SOURCE_DIR={tgcalls.as_posix()}",
                f"-DOPENSSL_INCLUDE_DIR={(openssl / 'include').as_posix()}",
                f"-DCMAKE_OSX_ARCHITECTURES={arch}",
                "-DCMAKE_OSX_DEPLOYMENT_TARGET=10.13",
                "-DAYUGRAM_ENABLE_COMPILATION_CACHE=ON",
                "-DAYUGRAM_SCCACHE_EXECUTABLE=/usr/bin/env",
                "-DCMAKE_CONFIGURATION_TYPES=Debug",
                "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO",
            ], check=True)
            subprocess.run([
                "cmake", "--build", str(build), "--config", "Debug",
                "--parallel", "2",
            ], check=True)
            print(f"Xcode preserved {arch} C/C++/Objective-C++ options, C++20 and precompiled headers.")
            print(f"Xcode compiled the tgcalls CryptoHelper for {arch}.")


if __name__ == "__main__":
    main()
