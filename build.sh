#!/bin/sh
# Builds dxgi.dll with mingw-w64:  sudo pacman -S mingw-w64-gcc   then   ./build.sh
set -e
cd "$(dirname "$0")"
CXX=${CXX:-x86_64-w64-mingw32-g++}
FLAGS="-O2 -std=c++17 -fno-exceptions -fno-rtti -Iimgui -include src/tate_assert.h \
  -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS \
  -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS"
rm -rf build && mkdir -p build
for f in imgui/imgui imgui/imgui_draw imgui/imgui_widgets imgui/imgui_tables imgui/imgui_impl_dx11 src/th06nc_tate; do
  echo "compiling $f.cpp"
  $CXX $FLAGS -c "$f.cpp" -o "build/$(basename "$f").o"
done
$CXX -shared -static -static-libgcc -static-libstdc++ -o dxgi.dll build/*.o src/dxgi.def -luser32 -lkernel32
echo "built dxgi.dll"
