# ##############################################################################
# vendor/sifli/boards/sf32lb52/drivers/CMakeLists.txt
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

target_compile_definitions(drivers PRIVATE SOC_BF0_HCPU
										   SF32LB52X
										   HAL_TICK_PER_SECOND=1000
										   USE_HAL_DRIVER)

target_include_directories(drivers PRIVATE ${NUTTX_CHIP_ABS_DIR}
										   ${NUTTX_CHIP_ABS_DIR}/include
										   ${NUTTX_CHIP_ABS_DIR}/../drivers/Include
										   ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/Include
										   ${NUTTX_CHIP_ABS_DIR}/../drivers/cmsis/sf32lb52x
										   ${NUTTX_CHIP_ABS_DIR}/../external/CMSIS/Include
										   ${NUTTX_CHIP_ABS_DIR}/../boards/include/config/sf32lb52x
										   ${NUTTX_BOARD_ABS_DIR}/include
										   ${NUTTX_DIR}/arch/arm/src/common
										   ${NUTTX_DIR}/arch/arm/src/armv8-m
										   ${NUTTX_DIR}/arch/arm/src/arm_m)

nuttx_add_subdirectory()
