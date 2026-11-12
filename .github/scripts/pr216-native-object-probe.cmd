@echo off
setlocal EnableExtensions

if "%~1"=="" (
  echo usage: %~nx0 ^<work-dir^> 1>&2
  exit /b 2
)

set "WORK=%~1"
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%" || exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Visual Studio C++ tools not found 1>&2
  exit /b 1
)
call "%VS%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

pushd "%WORK%" || exit /b 1

where link.exe >where-link.txt 2>&1 || exit /b 1
where lib.exe >where-lib.txt 2>&1 || exit /b 1
where dumpbin.exe >where-dumpbin.txt 2>&1 || exit /b 1
link /? >link-version.txt 2>&1
dumpbin /? >dumpbin-version.txt 2>&1
cl /? >cl-version.txt 2>&1

copy /y "%GITHUB_WORKSPACE%\.github\probe\pr216\public.c" public.c >nul || exit /b 1
copy /y "%GITHUB_WORKSPACE%\.github\probe\pr216\dependency.c" dependency.c >nul || exit /b 1

> base.def echo EXPORTS
>>base.def echo lance_api
>>base.def echo crc_fast_dependency

> narrow.def echo LIBRARY lance_c
>>narrow.def echo EXPORTS
>>narrow.def echo lance_api

cl /nologo /c public.c >cl-public.log 2>&1 || exit /b 1
cl /nologo /c dependency.c >cl-dependency.log 2>&1 || exit /b 1
lib /nologo /out:dependency.lib dependency.obj >lib-dependency.log 2>&1 || exit /b 1
dumpbin /nologo /directives dependency.obj >dependency.directives.txt 2>&1 || exit /b 1

rem Current mechanism: the last DEF excludes rustc's extra symbols, but the
rem ordinary helper reference still loads the archive member with /EXPORT.
link /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 ^
  /OUT:current.dll /IMPLIB:current.lib ^
  /DEF:base.def /DEF:narrow.def public.obj dependency.lib ^
  >current.link.log 2>&1 || exit /b 1
dumpbin /nologo /exports current.dll >current.dll.exports.txt 2>&1 || exit /b 1
dumpbin /nologo /exports current.lib >current.lib.exports.txt 2>&1 || exit /b 1

rem LIB creates the authoritative export file and matching import library.
lib /nologo /machine:x64 /def:narrow.def /out:seed.lib >seed.log 2>&1 || exit /b 1
if not exist seed.exp (
  echo LIB /DEF did not create seed.exp 1>&2
  exit /b 1
)
dumpbin /nologo /exports seed.lib >seed.lib.exports.txt 2>&1 || exit /b 1

rem An export file remains authoritative even when rustc's DEF is still
rem present. The pre-created import library is left untouched by LINK.
link /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 ^
  /OUT:fixed-with-def.dll /IMPLIB:seed.lib ^
  /DEF:narrow.def public.obj dependency.lib seed.exp ^
  >fixed-with-def.link.log 2>&1 || exit /b 1
dumpbin /nologo /exports fixed-with-def.dll >fixed-with-def.dll.exports.txt 2>&1 || exit /b 1
dumpbin /nologo /exports seed.lib >fixed-with-def.lib.exports.txt 2>&1 || exit /b 1

lib /nologo /machine:x64 /def:narrow.def /out:seed-without-def.lib ^
  >seed-without-def.log 2>&1 || exit /b 1
link /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 ^
  /OUT:fixed-without-def.dll /IMPLIB:seed-without-def.lib ^
  public.obj dependency.lib seed-without-def.exp ^
  >fixed-without-def.link.log 2>&1 || exit /b 1
dumpbin /nologo /exports fixed-without-def.dll ^
  >fixed-without-def.dll.exports.txt 2>&1 || exit /b 1
dumpbin /nologo /exports seed-without-def.lib ^
  >fixed-without-def.lib.exports.txt 2>&1 || exit /b 1

rem Supplying only the export file is not enough: LINK assumes the matching
rem import library already exists and intentionally does not create it.
lib /nologo /machine:x64 /def:narrow.def /out:exp-only-seed.lib ^
  >exp-only-seed.log 2>&1 || exit /b 1
del /q exp-only-seed.lib exp-only.lib 2>nul
link /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 ^
  /OUT:exp-only.dll /IMPLIB:exp-only.lib ^
  public.obj dependency.lib exp-only-seed.exp ^
  >exp-only.link.log 2>&1 || exit /b 1
dumpbin /nologo /exports exp-only.dll >exp-only.dll.exports.txt 2>&1 || exit /b 1
if exist exp-only.lib (
  echo LINK unexpectedly created an import library beside an export file 1>&2
  exit /b 1
)

popd
exit /b 0
