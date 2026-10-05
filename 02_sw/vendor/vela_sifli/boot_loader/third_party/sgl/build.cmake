#
# MIT License
#
# Copyright(c) 2023-present All contributors of SGL
# Document reference link: https://sgl-docs.readthedocs.io
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

file(GLOB_RECURSE SGL_HEADER
    ${SGL_SOURCE_PATH}/*.h
)

file(GLOB_RECURSE SGL_INCLUDE_HEADER
    ${SGL_HEADER_PATH}/*.h
)

file(GLOB_RECURSE SGL_GENERATED_HEADER
    ${SGL_GENERATED_PATH}/*.h
)

set(SGL_INCLUDE_DIRS
    ${SGL_GENERATED_PATH}
    ${SGL_SOURCE_PATH}
    ${SGL_HEADER_PATH}
)

include_directories(${SGL_INCLUDE_DIRS})
include(${SGL_SOURCE_PATH}/build.cmake)

configure_file(
    ${SGL_MODULE_PATH}/config.h.in
    ${SGL_GENERATED_PATH}/sgl_config.h
)

set(SGL_LIBRARY_HEADER
    ${SGL_HEADER}
    ${SGL_HEADER_INCLUDE}
)

set(SGL_LIBRARY_SOURCE
    ${SGL_SOURCE}
)

set(SGL_LIBRARY
    ${SGL_LIBRARY_HEADER}
    ${SGL_LIBRARY_SOURCE}
)
