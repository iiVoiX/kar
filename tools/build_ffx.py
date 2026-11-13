"""Builds the FidelityFX SDK 1.1.4 (FSR 3 upscaler, frame interpolation, optical flow, Vulkan backend) with MSYS2
UCRT64 Clang and Ninja, for the Kirby Air Riders frame generation work (RELEASE_PLAN.md 16.7).

The SDK (refs/fidelityfx/sdk, a sparse checkout of GPUOpen-LibrariesAndSDKs/FidelityFX-SDK tag v1.1.4, MIT) builds
with Visual Studio only. This copies it to build/ffx-src, applies the changes Clang and MinGW need, configures
build/ffx and builds the static libraries into build/ffx-src/bin/ffx_sdk. The SDK's own shader compiler
(tools/binary_store/FidelityFX_SC.exe) runs as part of the build.

Changes:
  - toolchain.cmake / CMakeLists.txt: no forced generator platform with Ninja; Ninja counts as x64.
  - include/FidelityFX/gpu/CMakeCompileShaders.txt: the permutation output list no longer starts with its own
    variable name (Ninja rejects that name as a missing input).
  - ffx_message.cpp: <wchar.h> and plain swprintf.
  - FrameInterpolationSwapchainVK.cpp: a function-pointer reinterpret_cast, and named SubmissionSemaphores where a
    temporary was bound to a non-const reference (an MSVC extension).
  - Compiler flags: MINGW_HAS_SECURE_API, forced includes of wchar.h, stdio.h, cmath and new (MSVC pulls them in).

Usage: .venv/Scripts/python tools/build_ffx.py
"""
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'refs' / 'fidelityfx' / 'sdk'
COPY = ROOT / 'build' / 'ffx-src'
BUILD = ROOT / 'build' / 'ffx'
UCRT = Path(r'C:\msys64\ucrt64\bin')


def edit(path, old, new, count=1):
    p = COPY / path
    s = p.read_text(encoding='utf-8', errors='surrogateescape')
    if old not in s:
        raise SystemExit(f'{path}: text to replace not found: {old[:60]!r}')
    p.write_text(s.replace(old, new, count), encoding='utf-8', errors='surrogateescape')


def main():
    if not SRC.is_dir():
        raise SystemExit(f'{SRC} missing: check out FidelityFX-SDK v1.1.4 (sparse, sdk/) into refs/fidelityfx')
    shutil.rmtree(COPY, ignore_errors=True)
    shutil.rmtree(BUILD, ignore_errors=True)
    shutil.copytree(SRC, COPY)
    edit('toolchain.cmake', 'set(CMAKE_GENERATOR_PLATFORM "x64" CACHE STRING "" FORCE)',
         'if(NOT CMAKE_GENERATOR STREQUAL "Ninja")\n\tset(CMAKE_GENERATOR_PLATFORM "x64" CACHE STRING "" FORCE)\nendif()')
    edit('CMakeLists.txt',
         'elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR CMAKE_EXE_LINKER_FLAGS STREQUAL "/machine:x64")',
         'elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR CMAKE_EXE_LINKER_FLAGS STREQUAL "/machine:x64" OR '
         'CMAKE_GENERATOR STREQUAL "Ninja")')
    cs = 'include/FidelityFX/gpu/CMakeCompileShaders.txt'
    edit(cs, 'list(APPEND PERMUTATION_OUTPUTS ', 'list(APPEND _FFX_OUTS ', count=4)
    edit(cs, 'set(${PERMUTATION_OUTPUTS} ${PERMUTATION_OUTPUTS} PARENT_SCOPE)', 'set(${PERMUTATION_OUTPUTS} ${_FFX_OUTS} PARENT_SCOPE)')
    edit(cs, '\tforeach(PASS_SHADER ${SHADER_FILES})', '\tset(_FFX_OUTS )\n\tforeach(PASS_SHADER ${SHADER_FILES})')
    msg = 'src/shared/ffx_message.cpp'
    edit(msg, '#include <FidelityFX/host/ffx_message.h>', '#include <FidelityFX/host/ffx_message.h>\n#include <wchar.h>')
    edit(msg, 'swprintf_s(buffer, 512,', 'swprintf(buffer, 512,', count=2)
    fi = 'src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp'
    edit(fi, 'static_cast<FfxWaitCallbackFunc>(valuePtr)', 'reinterpret_cast<FfxWaitCallbackFunc>(valuePtr)')
    p = COPY / fi
    lines = p.read_text(encoding='utf-8', errors='surrogateescape').split('\n')
    n = 0
    for i, line in enumerate(lines):
        m = re.match(r'(\s*)(.*)\bSubmissionSemaphores\(\)(.*;)\s*$', line)
        if m:
            lines[i] = f'{m.group(1)}{{ SubmissionSemaphores ffxEmptySemaphores; {m.group(2)}ffxEmptySemaphores{m.group(3)} }}'
            n += 1
    if n != 2:
        raise SystemExit(f'{fi}: expected 2 temporary SubmissionSemaphores, found {n}')
    p.write_text('\n'.join(lines), encoding='utf-8', errors='surrogateescape')

    env = dict(os.environ)
    env['PATH'] = str(UCRT) + os.pathsep + env.get('PATH', '')
    env.setdefault('VULKAN_SDK', r'C:\VulkanSDK\1.4.363.0')
    flags = '-DMINGW_HAS_SECURE_API=1 -Wno-everything -include wchar.h -include stdio.h -include cmath -include new'
    subprocess.run([str(UCRT / 'cmake.exe'), '-S', str(COPY), '-B', str(BUILD), '-G', 'Ninja',
                    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
                    f'-DCMAKE_CXX_FLAGS={flags}', '-DFFX_API_BACKEND=VK_X64', '-DFFX_FSR3=ON',
                    '-DFFX_AUTO_COMPILE_SHADERS=1'], check=True, env=env, stdout=subprocess.DEVNULL)
    subprocess.run([str(UCRT / 'cmake.exe'), '--build', str(BUILD), '-j', '16'], check=True, env=env)
    for lib in sorted((COPY / 'bin' / 'ffx_sdk').glob('*.a')):
        print(f'{lib.name}: {lib.stat().st_size} bytes')


if __name__ == '__main__':
    main()
