# Run this through the uninstall target. The manifest records only the latest install and omits
# DESTDIR, so use the same DESTDIR that was passed during installation. Shared directories stay;
# the package docdir is removed only when empty.

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
  # EXISTS follows links, so check IS_SYMLINK first to remove dangling links.
  if(IS_SYMLINK "${path}" OR EXISTS "${path}")
    message(STATUS "removing ${path}")
    file(REMOVE "${path}")
  else()
    message(STATUS "already gone: ${path}")
  endif()
endforeach()

# Remove only an empty docdir named by the manifest.
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
