@echo off
REM build.bat - build tiny_script with VS2022 (MSVC)
REM   build.bat            : build main (script.exe)
REM   build.bat test1..13  : build each phase test
REM   build.bat tests      : build all tests
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )
if not exist build mkdir build

set "CORE=src\core\vm.c src\core\script.c src\core\tokenizer.c src\core\compiler.c src\core\scheduler.c src\core\strutil.c src\core\mathutil.c"
set "CFLAGS=/nologo /W3 /TC /utf-8 /D_CRT_SECURE_NO_WARNINGS /Isrc\core /Isrc\host\common /Isrc\host\pc"
set "PC_CFLAGS=%CFLAGS% /DYJ_CONFIG_HEADER=\"yajir_config_pc.h\" /DYJ_PORT_HEADER=\"yajir_port_pc.h\""

REM PC host sources (common diagnostics + pc glue + entry point)
set "PCHOST=src\host\common\host_diag.c src\host\pc\host_mock.c src\host\pc\netutil.c src\host\pc\fileutil.c src\host\pc\main.c"

set "TARGET=%1"
if "%TARGET%"=="" set "TARGET=main"

if "%TARGET%"=="test1" goto :t1
if "%TARGET%"=="test2" goto :t2
if "%TARGET%"=="test3" goto :t3
if "%TARGET%"=="test4" goto :t4
if "%TARGET%"=="test5" goto :t5
if "%TARGET%"=="test6" goto :t6
if "%TARGET%"=="test7" goto :t7
if "%TARGET%"=="test8" goto :t8
if "%TARGET%"=="test9" goto :t9
if "%TARGET%"=="test10" goto :t10
if "%TARGET%"=="test11" goto :t11
if "%TARGET%"=="test12" goto :t12
if "%TARGET%"=="test13" goto :t13
if "%TARGET%"=="tests" goto :tall
if "%TARGET%"=="main"  goto :main
echo [build] unknown target: %TARGET%
exit /b 1

:t1
cl %CFLAGS% %CORE% tests\test_phase1_vm.c /Fobuild\ /Fe:build\test_phase1.exe || exit /b 1
goto :done
:t2
cl %CFLAGS% %CORE% tests\test_phase2_compile.c /Fobuild\ /Fe:build\test_phase2.exe || exit /b 1
goto :done
:t3
cl %CFLAGS% %CORE% tests\test_phase3_sched.c /Fobuild\ /Fe:build\test_phase3.exe || exit /b 1
goto :done
:t4
cl %CFLAGS% %CORE% tests\test_phase4_events.c /Fobuild\ /Fe:build\test_phase4.exe || exit /b 1
goto :done
:t5
cl %CFLAGS% %CORE% tests\test_phase5_errors.c /Fobuild\ /Fe:build\test_phase5.exe || exit /b 1
goto :done
:t6
cl %CFLAGS% %CORE% tests\test_phase6_ops.c /Fobuild\ /Fe:build\test_phase6.exe || exit /b 1
goto :done
:t7
cl %CFLAGS% %CORE% tests\test_phase7_strings.c /Fobuild\ /Fe:build\test_phase7.exe || exit /b 1
goto :done
:t8
cl %CFLAGS% %CORE% tests\test_phase8_multiarg.c /Fobuild\ /Fe:build\test_phase8.exe || exit /b 1
goto :done
:t9
cl %CFLAGS% %CORE% tests\test_phase9_lifecycle.c /Fobuild\ /Fe:build\test_phase9.exe || exit /b 1
goto :done
:t10
cl %CFLAGS% %CORE% tests\test_phase10_math.c /Fobuild\ /Fe:build\test_phase10.exe || exit /b 1
goto :done
:t11
cl %CFLAGS% %CORE% tests\test_phase11_loops.c /Fobuild\ /Fe:build\test_phase11.exe || exit /b 1
goto :done
:t12
cl %CFLAGS% %CORE% tests\test_phase12_ports.c /Fobuild\ /Fe:build\test_phase12.exe || exit /b 1
goto :done
:t13
cl %CFLAGS% %CORE% tests\test_phase13_local.c /Fobuild\ /Fe:build\test_phase13.exe || exit /b 1
goto :done
:tall
call "%~f0" test1 && call "%~f0" test2 && call "%~f0" test3 && call "%~f0" test4 && call "%~f0" test5 && call "%~f0" test6 && call "%~f0" test7 && call "%~f0" test8 && call "%~f0" test9 && call "%~f0" test10 && call "%~f0" test11 && call "%~f0" test12 && call "%~f0" test13
goto :done
:main
cl %PC_CFLAGS% %CORE% %PCHOST% /Fobuild\ /Fe:build\script.exe || exit /b 1
goto :done

:done
echo [build] OK: %TARGET%
exit /b 0
