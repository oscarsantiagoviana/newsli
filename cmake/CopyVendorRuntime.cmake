# CopyVendorRuntime.cmake — copies the user-provided vendor NR runtime
# into the package folder WHEN it exists on this machine (searched beside
# the deploy dir and the gamedir pattern). Never part of the repo; never
# required to build. Signed for script mode (-P).
foreach(NAME nvngx_dlssnr.dll nvngx.dll_dlssnr.dll)
  foreach(SRC
      "${PKGSRC}/deploy/${NAME}"
      "${PKGSRC}/../deploy/${NAME}")
    if(EXISTS "${SRC}")
      file(COPY_FILE "${SRC}" "${PKG}/${NAME}")
      message(STATUS "package: vendor runtime ${NAME} copied")
      break()
    endif()
  endforeach()
endforeach()
if(NOT EXISTS "${PKG}/nvngx_dlssnr.dll")
  message(STATUS "package: vendor runtime NOT found — the installer will "
                 "still work; NR needs the runtime beside the installer "
                 "to install it (README documents the sources)")
endif()
