cmake_minimum_required(VERSION 3.16)

# Add microlink-v2 component path
set(EXTRA_COMPONENT_DIRS
    "../../components"
)

# Merge sdkconfig.defaults, then the local git-ignored sdkconfig.credentials
# (if present) on top, so real secrets survive `idf.py fullclean` / a fresh
# checkout without being baked into a tracked file.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.credentials")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.credentials")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(microlink_v2_basic_connect)
