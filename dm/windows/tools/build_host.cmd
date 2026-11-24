@echo off
setlocal
rem Run from an external build directory in an ARM64 Native Tools prompt.
if /i not "%VSCMD_ARG_TGT_ARCH%"=="arm64" (echo Requires an ARM64 Native Tools prompt. 1>&2 & exit /b 1)
set "src=%~dp0"
for %%D in ("%src%..\..") do set "dm=%%~fD"
clang --target=aarch64-pc-windows-msvc -DDM_M2_HOST -I "%dm%\gfx\nvn" -c "%src%m1_gate.S" -o m2_gate.obj || exit /b 1
cl /nologo /std:c11 /O2 /Z7 /MP /W4 /WX /Fe:m2_host.exe /I "%dm%\third_party\vulkan-headers\include" /I "%dm%\third_party\dav1d\include" ^
    "%src%m2_host.c" "%src%m2_loader.c" "%src%m3_services.c" "%src%m3_ipc.c" ^
    "%src%m3_audio.c" "%src%m3_opus.c" "%src%m3_save.c" "%src%m3_save_store.c" "%src%m3_socket.c" ^
    "%src%m3_video.c" "%src%m3_av1.c" ^
    "%src%m4_display.c" "%src%m4_window.c" "%src%m4_replay_device.c" "%src%m4_draw.c" "%src%m4_overlay.c" ^
    "%src%m4_shader.c" "%src%m4_pipeline.c" "%src%m4_guest_resources.c" "%src%m4_guest_draw.c" "%src%m4_phase.c" ^
    "%dm%\gfx\nvn\nvn_tex.c" "%dm%\gfx\tex\nvn_deswizzle.c" m2_gate.obj ^
    /link /DEBUG:FULL /INCREMENTAL:NO /OPT:REF,ICF /PDB:m2_host.pdb MinCore.lib User32.lib winsqlite3.lib || exit /b 1
cl /nologo /std:c11 /O2 /W4 /WX /I "%dm%\third_party\vulkan-headers\include" ^
    "%src%m4_overlay_smoke.c" m4_overlay.obj nvn_tex.obj nvn_deswizzle.obj || exit /b 1
cl /nologo /std:c11 /O2 /MT /W4 /WX /Fe:m2_cpu_trace.exe "%src%m2_cpu_trace.c" /link Dbghelp.lib || exit /b 1
for %%F in (m2_guest m2_guest_call_guest m3_guest m3_graphics_guest m3_copy_guest m3_program_guest m3_hid_guest m3_aoc_guest m3_audio_guest m3_opus_guest m3_video_guest m3_save_guest m3_raster_guest m3_kernel_guest m3_socket_guest m4_draw_state_guest m3_counter_guest m4_clear_guest m3_event_guest) do (
    clang --target=aarch64-none-elf -c "%src%%%F.S" -o %%F.o || exit /b 1
    llvm-objcopy -O binary --only-section=.text %%F.o %%F.bin || exit /b 1
)
rem AV1 is loaded on demand. Fixture-only builds do not need the optional DLL.
if not defined DM_DAV1D_DLL if exist "%SystemDrive%\msys64\clangarm64\bin\libdav1d-7.dll" set "DM_DAV1D_DLL=%SystemDrive%\msys64\clangarm64\bin\libdav1d-7.dll"
if defined DM_DAV1D_DLL (
    if not exist "%DM_DAV1D_DLL%" (echo Cannot find "%DM_DAV1D_DLL%". 1>&2 & exit /b 1)
    for %%F in ("%DM_DAV1D_DLL%") do if /i not "%%~fF"=="%CD%\libdav1d-7.dll" (
        copy /y "%%~fF" libdav1d-7.dll >nul || exit /b 1
    )
)
