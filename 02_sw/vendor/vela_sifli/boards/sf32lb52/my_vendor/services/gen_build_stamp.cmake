# SPDX-License-Identifier: Apache-2.0
# Generate myvendor_build_stamp.h with local time to the second.

if(NOT DEFINED OUT)
  message(FATAL_ERROR "gen_build_stamp.cmake: OUT not set")
endif()

string(TIMESTAMP STAMP "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUT}"
"#ifndef MYVENDOR_BUILD_STAMP_H
#define MYVENDOR_BUILD_STAMP_H

#define MYVENDOR_BUILD_DATE_STR \"${STAMP}\"

#endif /* MYVENDOR_BUILD_STAMP_H */
")
