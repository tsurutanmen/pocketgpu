@echo off
rem Test fake of netsh: prints the file in FAKE_NETSH_OUT (the excluded port ranges, in the same form as the real netsh).
if defined FAKE_NETSH_OUT type "%FAKE_NETSH_OUT%"
exit /b 0
