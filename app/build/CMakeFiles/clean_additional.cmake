# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "CMakeFiles\\nextboard_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\nextboard_autogen.dir\\ParseCache.txt"
  "nextboard_autogen"
  )
endif()
