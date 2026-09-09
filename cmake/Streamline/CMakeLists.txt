cmake_minimum_required(VERSION 3.21)

add_library(Streamline INTERFACE)

target_include_directories(
  Streamline INTERFACE ${CMAKE_SOURCE_DIR}/extern/Streamline-DX12/include)

if(MSVC)
  target_compile_options(Streamline INTERFACE /wd5103)
endif()

# Streamline 2.12.0 sl_pcl.h aliases std::to_underlying with `using`, which is ill-formed (an alias
# of a function template) and fails under /std:c++latest with /Zc:__cplusplus. Route it to the
# header's own fallback template. Idempotent; touches the submodule working tree only.
set(_sl_pcl_header "${CMAKE_SOURCE_DIR}/extern/Streamline-DX12/include/sl_pcl.h")
if(EXISTS "${_sl_pcl_header}")
  file(READ "${_sl_pcl_header}" _sl_pcl_content)
  string(FIND "${_sl_pcl_content}" "#if __cplusplus == 202302L" _sl_pcl_alias_index)
  if(NOT _sl_pcl_alias_index EQUAL -1)
    string(REGEX REPLACE
      "#if __cplusplus == 202302L([\r\n]+)using to_underlying = std::to_underlying;"
      "#if 0 // patched by cmake/Streamline/CMakeLists.txt: alias of a function template is ill-formed, use the fallback below\\1using to_underlying = std::to_underlying;"
      _sl_pcl_content "${_sl_pcl_content}")
    file(WRITE "${_sl_pcl_header}" "${_sl_pcl_content}")
    message(STATUS "Patched extern/Streamline-DX12/include/sl_pcl.h to_underlying alias")
  endif()
endif()
