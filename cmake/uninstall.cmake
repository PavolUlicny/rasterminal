# Removes what `cmake --install` put on disk, driven by the manifest that step writes.
# Run through the `uninstall` target from the build directory, never directly.
#
# The manifest records prefix paths with no DESTDIR applied, so a staged install has to be
# uninstalled the same way it was installed: DESTDIR=/tmp/pkg cmake --build <dir> --target
# uninstall.
#
# It also records exactly one install. `cmake --install` overwrites the manifest rather than
# appending, so after installing to two prefixes only the second is removable from this build
# directory. Uninstall each one before installing elsewhere.
#
# Directories are left alone with one exception, share/doc/rasterminal, which is ours alone
# and goes once it is empty. bin and share/man/man1 belong to the system and may hold other
# packages' files. That directory is found by walking the manifest's own entries, NOT from a
# path baked in at configure time: `cmake --install --prefix` overrides the prefix at install
# time, and a configure-time path would then point at a tree this install never touched.

set(manifest "${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt")
if(NOT EXISTS "${manifest}")
  message(FATAL_ERROR "no install_manifest.txt here; nothing was installed from this build directory")
endif()

if(NOT RASTERMINAL_DOCDIR_NAME)
  message(FATAL_ERROR "RASTERMINAL_DOCDIR_NAME not set; run this through the uninstall target")
endif()

set(destdir "$ENV{DESTDIR}")
set(parents "")

file(STRINGS "${manifest}" files)
foreach(file ${files})
  set(path "${destdir}${file}")
  get_filename_component(parent "${path}" DIRECTORY)
  list(APPEND parents "${parent}")
  # IS_SYMLINK first: EXISTS follows the link, so a dangling one would report missing and
  # survive. This removes both.
  if(IS_SYMLINK "${path}" OR EXISTS "${path}")
    message(STATUS "removing ${path}")
    file(REMOVE "${path}")
  else()
    message(STATUS "already gone: ${path}")
  endif()
endforeach()

# Only a directory the manifest actually names, whose own name is the docdir's, and which is
# empty now that its files are gone. Every other installed directory is shared.
list(REMOVE_DUPLICATES parents)
foreach(dir ${parents})
  get_filename_component(name "${dir}" NAME)
  if(name STREQUAL "${RASTERMINAL_DOCDIR_NAME}" AND IS_DIRECTORY "${dir}")
    file(GLOB leftovers "${dir}/*")
    if(NOT leftovers)
      message(STATUS "removing ${dir}")
      file(REMOVE_RECURSE "${dir}")
    endif()
  endif()
endforeach()
