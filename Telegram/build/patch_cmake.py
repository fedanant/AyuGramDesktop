import os, sys

def patch_file(path, old, new):
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    if old in content:
        content = content.replace(old, new, 1)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"Successfully patched {path}")
    elif new in content:
        print(f"Already patched {path}")
    else:
        print(f"Warning: target pattern not found in {path}")

def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    options_win = os.path.join(root, 'cmake', 'options_win.cmake')
    variables = os.path.join(root, 'cmake', 'variables.cmake')
    
    if os.path.exists(options_win):
        patch_file(options_win, '/MP     # Enable multi process build.', '/MP     # Enable multi process build.\n        /FS')
        patch_file(options_win, '$<IF:$<STREQUAL:$<GENEX_EVAL:$<TARGET_PROPERTY:MSVC_DEBUG_INFORMATION_FORMAT>>,ProgramDatabase>,/DEBUG,/DEBUG:NONE>', '$<IF:$<BOOL:$<GENEX_EVAL:$<TARGET_PROPERTY:MSVC_DEBUG_INFORMATION_FORMAT>>>,/DEBUG,/DEBUG:NONE>')
    
    if os.path.exists(variables):
        patch_file(variables, 'set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase" CACHE STRING "")', 'set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT\n    "$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>$<$<CONFIG:Release,MinSizeRel>:Embedded>"\n    CACHE STRING "" FORCE)')

    run_cmake = os.path.join(root, 'cmake', 'run_cmake.py')
    if os.path.exists(run_cmake):
        patch_file(run_cmake, "'-T v143'", "'-T v143,host=x64'")

if __name__ == '__main__':
    main()
