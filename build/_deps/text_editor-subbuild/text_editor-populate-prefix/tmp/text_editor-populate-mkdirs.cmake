# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/malesko/dev/jitgl/build/_deps/text_editor-src")
  file(MAKE_DIRECTORY "/home/malesko/dev/jitgl/build/_deps/text_editor-src")
endif()
file(MAKE_DIRECTORY
  "/home/malesko/dev/jitgl/build/_deps/text_editor-build"
  "/home/malesko/dev/jitgl/build/_deps/text_editor-subbuild/text_editor-populate-prefix"
  "/home/malesko/dev/jitgl/build/_deps/text_editor-subbuild/text_editor-populate-prefix/tmp"
  "/home/malesko/dev/jitgl/build/_deps/text_editor-subbuild/text_editor-populate-prefix/src/text_editor-populate-stamp"
  "/home/malesko/dev/jitgl/build/_deps/text_editor-subbuild/text_editor-populate-prefix/src"
  "/home/malesko/dev/jitgl/build/_deps/text_editor-subbuild/text_editor-populate-prefix/src/text_editor-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/malesko/dev/jitgl/build/_deps/text_editor-subbuild/text_editor-populate-prefix/src/text_editor-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/malesko/dev/jitgl/build/_deps/text_editor-subbuild/text_editor-populate-prefix/src/text_editor-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
