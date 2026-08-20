# Removes what `cmake --install` put on disk, driven by the manifest that step writes.
# Run through the `uninstall` target from the build directory, never directly.
#
# The manifest records prefix paths with no DESTDIR applied, so a staged install has to be
# uninstalled the same way it was installed: DESTDIR=/tmp/pkg cmake --build <dir> --target
# uninstall. Same contract as the Makefile's uninstall target.
#
# Directories are left alone with one exception. bin and share/man/man1 belong to the system
# and may hold other packages' files; share/doc/rasterminal is ours alone, so it goes once it
# is empty (RASTERMINAL_DOCDIR is passed in by the target, since the manifest lists only
# files). That matches `make uninstall`, which rm -rf's the same directory.

set(manifest "${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt")
if(NOT EXISTS "${manifest}")
  message(FATAL_ERROR "no install_manifest.txt here; nothing was installed from this build directory")
endif()

set(destdir "$ENV{DESTDIR}")
file(STRINGS "${manifest}" files)
foreach(file ${files})
  set(path "${destdir}${file}")
  # IS_SYMLINK first: EXISTS follows the link, so a dangling one would report missing and
  # survive. This removes both.
  if(IS_SYMLINK "${path}" OR EXISTS "${path}")
    message(STATUS "removing ${path}")
    file(REMOVE "${path}")
  else()
    message(STATUS "already gone: ${path}")
  endif()
endforeach()

if(RASTERMINAL_DOCDIR AND IS_DIRECTORY "${destdir}${RASTERMINAL_DOCDIR}")
  file(GLOB leftovers "${destdir}${RASTERMINAL_DOCDIR}/*")
  if(NOT leftovers)
    message(STATUS "removing ${destdir}${RASTERMINAL_DOCDIR}")
    file(REMOVE_RECURSE "${destdir}${RASTERMINAL_DOCDIR}")
  endif()
endif()
