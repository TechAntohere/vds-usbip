# Ninja is a single-config generator, so the build type MUST be set at configure
# time (--config is ignored). Force Release so shipped binaries link the
# redistributable CRT (msvcp140/vcruntime140), not the non-redistributable debug
# CRT (msvcp140d/vcruntime140d/ucrtbased).
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cd /d C:\Users\Antonio\Documents\vds\build && cmake -S C:\Users\Antonio\Documents\vds -B . -DCMAKE_BUILD_TYPE=Release && cmake --build . --target vdsd vdsctl 2>&1'
