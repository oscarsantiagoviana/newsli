# MakeInstallerPayload.cmake — generates the installer's resource script
# embedding the deployables as RCDATA. Run in script mode by CMakeLists
# (OUTPUT installer_payload.rc). Binary-safe: the .rc references the
# files by path; rc.exe embeds them verbatim.
set(RC_TEXT "// GENERATED file not for editing
1 RCDATA \"${DXGI}\"
2 RCDATA \"${NVNGX}\"
3 RCDATA \"${ENGINE}\"
4 RCDATA \"${PANEL}\"
5 RCDATA \"${CTL}\"
6 RCDATA \"${INI}\"
")
file(WRITE "${OUT}" "${RC_TEXT}")
