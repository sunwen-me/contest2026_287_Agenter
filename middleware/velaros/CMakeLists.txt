# SPDX-License-Identifier: Apache-2.0

if(CONFIG_VELAROS_CORE)
  set(VELAROS_ROS_SOURCE_ROOT
      "${NUTTX_DIR}/../external/velaros_ros2_sources")
  set(VELAROS_ROS_GENERATED_ROOT
      "${NUTTX_DIR}/../external/velaros_ros2_generated")
  set(RCUTILS_SOURCE_DIR "${VELAROS_ROS_SOURCE_ROOT}/rcutils")
  set(ROSIDL_SOURCE_DIR "${VELAROS_ROS_SOURCE_ROOT}/rosidl")
  set(ROSIDL_DYNAMIC_SOURCE_DIR
      "${VELAROS_ROS_SOURCE_ROOT}/rosidl_dynamic_typesupport")
  set(RMW_SOURCE_DIR "${VELAROS_ROS_SOURCE_ROOT}/rmw")
  set(RCPPUTILS_SOURCE_DIR "${VELAROS_ROS_SOURCE_ROOT}/rcpputils")
  set(ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR
      "${VELAROS_ROS_SOURCE_ROOT}/rosidl_dynamic_typesupport_fastrtps")

  foreach(required_source
          "${RCUTILS_SOURCE_DIR}/package.xml"
          "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/package.xml"
          "${ROSIDL_DYNAMIC_SOURCE_DIR}/package.xml"
          "${RMW_SOURCE_DIR}/rmw/package.xml")
    if(NOT EXISTS "${required_source}")
      message(
        FATAL_ERROR
          "VelaROS ROS 2 source is missing: ${required_source}. "
          "Run tools/restore_velaros_ros2_sources.sh.")
    endif()
  endforeach()

  set(VELAROS_C_FLAGS
      -Wall
      -Wextra
      -Wno-shadow
      -Wno-strict-prototypes
      -Wno-undef
      -Wno-unused-parameter
      -Wno-sign-conversion)

  set(VELAROS_CXX_FLAGS
      -Wall
      -Wextra
      -Wno-shadow
      -Wno-undef
      -Wno-unused-parameter
      -Wno-sign-conversion
      -fexceptions
      -frtti)

  set(RCUTILS_SOURCES
      "${RCUTILS_SOURCE_DIR}/src/allocator.c"
      "${RCUTILS_SOURCE_DIR}/src/array_list.c"
      "${RCUTILS_SOURCE_DIR}/src/base64.c"
      "${RCUTILS_SOURCE_DIR}/src/char_array.c"
      "${RCUTILS_SOURCE_DIR}/src/cmdline_parser.c"
      "${RCUTILS_SOURCE_DIR}/src/env.c"
      "${RCUTILS_SOURCE_DIR}/src/error_handling.c"
      "${RCUTILS_SOURCE_DIR}/src/filesystem.c"
      "${RCUTILS_SOURCE_DIR}/src/find.c"
      "${RCUTILS_SOURCE_DIR}/src/format_string.c"
      "${RCUTILS_SOURCE_DIR}/src/hash_map.c"
      "${RCUTILS_SOURCE_DIR}/src/join.c"
      "${RCUTILS_SOURCE_DIR}/src/logging.c"
      "${RCUTILS_SOURCE_DIR}/src/process.c"
      "${RCUTILS_SOURCE_DIR}/src/qsort.c"
      "${RCUTILS_SOURCE_DIR}/src/repl_str.c"
      "${RCUTILS_SOURCE_DIR}/src/sha256.c"
      "${RCUTILS_SOURCE_DIR}/src/shared_library.c"
      "${RCUTILS_SOURCE_DIR}/src/snprintf.c"
      "${RCUTILS_SOURCE_DIR}/src/split.c"
      "${RCUTILS_SOURCE_DIR}/src/strcasecmp.c"
      "${RCUTILS_SOURCE_DIR}/src/strdup.c"
      "${RCUTILS_SOURCE_DIR}/src/strerror.c"
      "${RCUTILS_SOURCE_DIR}/src/string_array.c"
      "${RCUTILS_SOURCE_DIR}/src/string_map.c"
      "${RCUTILS_SOURCE_DIR}/src/strnlen.c"
      "${RCUTILS_SOURCE_DIR}/src/testing/fault_injection.c"
      "${RCUTILS_SOURCE_DIR}/src/time.c"
      "${RCUTILS_SOURCE_DIR}/src/time_unix.c"
      "${RCUTILS_SOURCE_DIR}/src/uint8_array.c")

  add_library(rcutils STATIC ${RCUTILS_SOURCES})
  target_include_directories(rcutils PUBLIC "${RCUTILS_SOURCE_DIR}/include")
  target_compile_definitions(rcutils PRIVATE RCUTILS_BUILDING_DLL)
  target_compile_options(rcutils PRIVATE ${VELAROS_C_FLAGS})
  set_target_properties(rcutils PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  nuttx_add_external_library(rcutils)

  add_library(rosidl_typesupport_interface INTERFACE)
  target_include_directories(
    rosidl_typesupport_interface
    INTERFACE
      "${ROSIDL_SOURCE_DIR}/rosidl_typesupport_interface/include")

  add_library(
    rosidl_buffer STATIC
    "${ROSIDL_SOURCE_DIR}/rosidl_buffer/src/c_helpers.cpp")
  target_include_directories(
    rosidl_buffer PUBLIC "${ROSIDL_SOURCE_DIR}/rosidl_buffer/include")
  target_compile_definitions(
    rosidl_buffer PRIVATE ROSIDL_BUFFER_BUILDING_DLL)
  set_target_properties(
    rosidl_buffer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  nuttx_add_external_library(rosidl_buffer)

  file(
    GLOB ROSIDL_TYPE_DESCRIPTION_SOURCES
    CONFIGURE_DEPENDS
    "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/src/type_description/*.c")
  set(ROSIDL_RUNTIME_C_SOURCES
      "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/src/message_type_support.c"
      "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/src/primitives_sequence_functions.c"
      "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/src/sequence_bound.c"
      "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/src/service_type_support.c"
      "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/src/string_functions.c"
      "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/src/type_hash.c"
      "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/src/u16string_functions.c"
      "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/src/type_description_utils.c"
      ${ROSIDL_TYPE_DESCRIPTION_SOURCES})

  add_library(rosidl_runtime_c STATIC ${ROSIDL_RUNTIME_C_SOURCES})
  target_include_directories(
    rosidl_runtime_c PUBLIC "${ROSIDL_SOURCE_DIR}/rosidl_runtime_c/include")
  target_link_libraries(
    rosidl_runtime_c PUBLIC rcutils rosidl_buffer
                            rosidl_typesupport_interface)
  target_compile_options(
    rosidl_runtime_c PRIVATE ${VELAROS_C_FLAGS})
  set_target_properties(
    rosidl_runtime_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  nuttx_add_external_library(rosidl_runtime_c)

  add_library(rosidl_runtime_cpp INTERFACE)
  target_include_directories(
    rosidl_runtime_cpp
    INTERFACE "${ROSIDL_SOURCE_DIR}/rosidl_runtime_cpp/include")
  target_link_libraries(
    rosidl_runtime_cpp INTERFACE rosidl_runtime_c rosidl_buffer)

  set(ROSIDL_DYNAMIC_TYPESUPPORT_SOURCES
      "${ROSIDL_DYNAMIC_SOURCE_DIR}/src/api/serialization_support.c"
      "${ROSIDL_DYNAMIC_SOURCE_DIR}/src/api/dynamic_data.c"
      "${ROSIDL_DYNAMIC_SOURCE_DIR}/src/api/dynamic_type.c"
      "${ROSIDL_DYNAMIC_SOURCE_DIR}/src/dynamic_message_type_support_struct.c"
      "${ROSIDL_DYNAMIC_SOURCE_DIR}/src/identifier.c")

  add_library(
    rosidl_dynamic_typesupport STATIC
    ${ROSIDL_DYNAMIC_TYPESUPPORT_SOURCES})
  target_include_directories(
    rosidl_dynamic_typesupport
    PRIVATE "${ROSIDL_DYNAMIC_SOURCE_DIR}/src"
    PUBLIC "${ROSIDL_DYNAMIC_SOURCE_DIR}/include")
  target_link_libraries(
    rosidl_dynamic_typesupport PUBLIC rcutils rosidl_runtime_c)
  target_compile_options(
    rosidl_dynamic_typesupport PRIVATE ${VELAROS_C_FLAGS})
  set_target_properties(
    rosidl_dynamic_typesupport
    PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  nuttx_add_external_library(rosidl_dynamic_typesupport)

  set(RMW_SOURCES
      "${RMW_SOURCE_DIR}/rmw/src/allocators.c"
      "${RMW_SOURCE_DIR}/rmw/src/convert_rcutils_ret_to_rmw_ret.c"
      "${RMW_SOURCE_DIR}/rmw/src/discovery_options.c"
      "${RMW_SOURCE_DIR}/rmw/src/enclave.c"
      "${RMW_SOURCE_DIR}/rmw/src/event.c"
      "${RMW_SOURCE_DIR}/rmw/src/init.c"
      "${RMW_SOURCE_DIR}/rmw/src/init_options.c"
      "${RMW_SOURCE_DIR}/rmw/src/message_sequence.c"
      "${RMW_SOURCE_DIR}/rmw/src/names_and_types.c"
      "${RMW_SOURCE_DIR}/rmw/src/network_flow_endpoint_array.c"
      "${RMW_SOURCE_DIR}/rmw/src/network_flow_endpoint.c"
      "${RMW_SOURCE_DIR}/rmw/src/publisher_options.c"
      "${RMW_SOURCE_DIR}/rmw/src/qos_string_conversions.c"
      "${RMW_SOURCE_DIR}/rmw/src/sanity_checks.c"
      "${RMW_SOURCE_DIR}/rmw/src/security_options.c"
      "${RMW_SOURCE_DIR}/rmw/src/service_endpoint_info_array.c"
      "${RMW_SOURCE_DIR}/rmw/src/service_endpoint_info.c"
      "${RMW_SOURCE_DIR}/rmw/src/subscription_content_filter_options.c"
      "${RMW_SOURCE_DIR}/rmw/src/subscription_options.c"
      "${RMW_SOURCE_DIR}/rmw/src/time.c"
      "${RMW_SOURCE_DIR}/rmw/src/topic_endpoint_info_array.c"
      "${RMW_SOURCE_DIR}/rmw/src/topic_endpoint_info.c"
      "${RMW_SOURCE_DIR}/rmw/src/types.c"
      "${RMW_SOURCE_DIR}/rmw/src/validate_full_topic_name.c"
      "${RMW_SOURCE_DIR}/rmw/src/validate_namespace.c"
      "${RMW_SOURCE_DIR}/rmw/src/validate_node_name.c")

  add_library(rmw STATIC ${RMW_SOURCES})
  target_include_directories(rmw PUBLIC "${RMW_SOURCE_DIR}/rmw/include")
  target_link_libraries(
    rmw PUBLIC rcutils rosidl_dynamic_typesupport rosidl_runtime_c)
  target_compile_options(rmw PRIVATE ${VELAROS_C_FLAGS})
  set_target_properties(rmw PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  nuttx_add_external_library(rmw)

  if(CONFIG_VELAROS_FASTRTPS_TYPESUPPORT)
    foreach(required_source
            "${RCPPUTILS_SOURCE_DIR}/package.xml"
            "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/package.xml")
      if(NOT EXISTS "${required_source}")
        message(
          FATAL_ERROR
            "VelaROS ROS 2 source is missing: ${required_source}. "
            "Run tools/restore_velaros_ros2_sources.sh.")
      endif()
    endforeach()

    set(RCPPUTILS_SOURCES
        "${RCPPUTILS_SOURCE_DIR}/src/asserts.cpp"
        "${RCPPUTILS_SOURCE_DIR}/src/env.cpp"
        "${RCPPUTILS_SOURCE_DIR}/src/filesystem_helper.cpp"
        "${RCPPUTILS_SOURCE_DIR}/src/find_library.cpp"
        "${RCPPUTILS_SOURCE_DIR}/src/process.cpp"
        "${RCPPUTILS_SOURCE_DIR}/src/shared_library.cpp"
        "${RCPPUTILS_SOURCE_DIR}/src/thread_name.cpp")

    add_library(rcpputils STATIC ${RCPPUTILS_SOURCES})
    target_include_directories(
      rcpputils PUBLIC "${RCPPUTILS_SOURCE_DIR}/include")
    target_link_libraries(rcpputils PUBLIC rcutils)
    target_compile_options(
      rcpputils PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rcpputils PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rcpputils)

    set(ROSIDL_DYNAMIC_FASTRTPS_SOURCES
        "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/src/detail/fastrtps_dynamic_data.cpp"
        "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/src/detail/fastrtps_dynamic_type.cpp"
        "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/src/detail/fastrtps_serialization_support.cpp"
        "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/src/detail/utils.cpp"
        "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/src/identifier.cpp"
        "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/src/serialization_support.cpp")

    add_library(
      rosidl_dynamic_typesupport_fastrtps STATIC
      ${ROSIDL_DYNAMIC_FASTRTPS_SOURCES})
    target_include_directories(
      rosidl_dynamic_typesupport_fastrtps
      PRIVATE "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/src"
      PUBLIC "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/include")
    target_link_libraries(
      rosidl_dynamic_typesupport_fastrtps
      PUBLIC fastcdr fastdds rcpputils rcutils
             rosidl_dynamic_typesupport rosidl_runtime_c)
    target_compile_options(
      rosidl_dynamic_typesupport_fastrtps
      PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rosidl_dynamic_typesupport_fastrtps
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rosidl_dynamic_typesupport_fastrtps)
  endif()

  if(CONFIG_VELAROS_RMW_DDS_COMMON)
    set(RMW_DDS_COMMON_SOURCE_DIR
        "${VELAROS_ROS_SOURCE_ROOT}/rmw_dds_common/rmw_dds_common")
    set(ROSIDL_FASTRTPS_SOURCE_DIR
        "${VELAROS_ROS_SOURCE_ROOT}/rosidl_typesupport_fastrtps")
    set(RMW_DDS_COMMON_GENERATED_DIR
        "${VELAROS_ROS_GENERATED_ROOT}/rmw_dds_common")
    set(RMW_DDS_COMMON_GENERATOR_C_DIR
        "${RMW_DDS_COMMON_GENERATED_DIR}/rosidl_generator_c")
    set(RMW_DDS_COMMON_GENERATOR_CPP_DIR
        "${RMW_DDS_COMMON_GENERATED_DIR}/rosidl_generator_cpp")
    set(RMW_DDS_COMMON_FASTRTPS_CPP_DIR
        "${RMW_DDS_COMMON_GENERATED_DIR}/rosidl_typesupport_fastrtps_cpp")

    foreach(required_source
            "${RMW_DDS_COMMON_SOURCE_DIR}/package.xml"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/package.xml"
            "${RMW_DDS_COMMON_GENERATED_DIR}/generation.manifest"
            "${RMW_DDS_COMMON_GENERATOR_C_DIR}/rmw_dds_common/msg/gid.h"
            "${RMW_DDS_COMMON_GENERATOR_CPP_DIR}/rmw_dds_common/msg/gid.hpp"
            "${RMW_DDS_COMMON_FASTRTPS_CPP_DIR}/rmw_dds_common/msg/detail/gid__rosidl_typesupport_fastrtps_cpp.hpp")
      if(NOT EXISTS "${required_source}")
        message(
          FATAL_ERROR
            "VelaROS generated ROS 2 source is missing: ${required_source}. "
            "Run tools/generate_velaros_ros2_interfaces.sh.")
      endif()
    endforeach()

    file(
      GLOB RMW_DDS_COMMON_GENERATOR_C_SOURCES
      CONFIGURE_DEPENDS
      "${RMW_DDS_COMMON_GENERATOR_C_DIR}/rmw_dds_common/msg/detail/*__description.c"
      "${RMW_DDS_COMMON_GENERATOR_C_DIR}/rmw_dds_common/msg/detail/*__functions.c")

    add_library(
      rmw_dds_common_generator_c STATIC
      ${RMW_DDS_COMMON_GENERATOR_C_SOURCES})
    target_include_directories(
      rmw_dds_common_generator_c
      PUBLIC "${RMW_DDS_COMMON_GENERATOR_C_DIR}")
    target_link_libraries(
      rmw_dds_common_generator_c
      PUBLIC rosidl_runtime_c rosidl_typesupport_interface)
    target_compile_options(
      rmw_dds_common_generator_c PRIVATE ${VELAROS_C_FLAGS})
    set_target_properties(
      rmw_dds_common_generator_c
      PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rmw_dds_common_generator_c)

    file(
      GLOB RMW_DDS_COMMON_FASTRTPS_CPP_SOURCES
      CONFIGURE_DEPENDS
      "${RMW_DDS_COMMON_FASTRTPS_CPP_DIR}/rmw_dds_common/msg/detail/dds_fastrtps/*__type_support.cpp")

    add_library(
      rmw_dds_common_typesupport_fastrtps_cpp STATIC
      ${RMW_DDS_COMMON_FASTRTPS_CPP_SOURCES}
      "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/src/identifier.cpp")
    target_include_directories(
      rmw_dds_common_typesupport_fastrtps_cpp
      PUBLIC
        "${RMW_DDS_COMMON_GENERATOR_C_DIR}"
        "${RMW_DDS_COMMON_GENERATOR_CPP_DIR}"
        "${RMW_DDS_COMMON_FASTRTPS_CPP_DIR}"
        "${ROSIDL_SOURCE_DIR}/rosidl_runtime_cpp/include"
        "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
    target_link_libraries(
      rmw_dds_common_typesupport_fastrtps_cpp
      PUBLIC fastcdr rmw rmw_dds_common_generator_c rosidl_runtime_c
             rosidl_runtime_cpp rosidl_typesupport_interface)
    target_compile_definitions(
      rmw_dds_common_typesupport_fastrtps_cpp
      PRIVATE ROSIDL_TYPESUPPORT_FASTRTPS_CPP_BUILDING_DLL)
    target_compile_options(
      rmw_dds_common_typesupport_fastrtps_cpp
      PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rmw_dds_common_typesupport_fastrtps_cpp
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(
      rmw_dds_common_typesupport_fastrtps_cpp)

    set(RMW_DDS_COMMON_SOURCES
        "${RMW_DDS_COMMON_SOURCE_DIR}/src/context.cpp"
        "${RMW_DDS_COMMON_SOURCE_DIR}/src/gid_utils.cpp"
        "${RMW_DDS_COMMON_SOURCE_DIR}/src/graph_cache.cpp"
        "${RMW_DDS_COMMON_SOURCE_DIR}/src/qos.cpp"
        "${RMW_DDS_COMMON_SOURCE_DIR}/src/time_utils.cpp")

    add_library(rmw_dds_common STATIC ${RMW_DDS_COMMON_SOURCES})
    target_include_directories(
      rmw_dds_common
      PUBLIC
        "${RMW_DDS_COMMON_SOURCE_DIR}/include"
        "${RMW_DDS_COMMON_GENERATOR_C_DIR}"
        "${RMW_DDS_COMMON_GENERATOR_CPP_DIR}")
    target_link_libraries(
      rmw_dds_common
      PUBLIC rcpputils rcutils rmw rmw_dds_common_generator_c
             rosidl_runtime_c rosidl_runtime_cpp)
    target_compile_definitions(
      rmw_dds_common PRIVATE RMW_DDS_COMMON_BUILDING_LIBRARY)
    target_compile_options(
      rmw_dds_common PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rmw_dds_common
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rmw_dds_common)

    if(CONFIG_VELAROS_RMW_DDS_COMMON_SMOKE)
      nuttx_add_application(
        NAME
        velaros_rmw_dds_smoke
        COMPILE_FLAGS
        ${VELAROS_CXX_FLAGS}
        SRCS
        "${CMAKE_CURRENT_LIST_DIR}/velaros_rmw_dds_smoke.cpp"
        INCLUDE_DIRECTORIES
        "${RMW_DDS_COMMON_SOURCE_DIR}/include"
        "${RMW_DDS_COMMON_GENERATOR_C_DIR}"
        "${RMW_DDS_COMMON_GENERATOR_CPP_DIR}"
        "${RMW_DDS_COMMON_FASTRTPS_CPP_DIR}"
        "${ROSIDL_SOURCE_DIR}/rosidl_runtime_cpp/include"
        "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include"
        DEPENDS
        rmw_dds_common
        rmw_dds_common_typesupport_fastrtps_cpp
        STACKSIZE
        32768
        PRIORITY
        100)
    endif()
  endif()

  if(CONFIG_VELAROS_RMW_FASTRTPS_CPP)
    set(RMW_FASTRTPS_SOURCE_DIR
        "${VELAROS_ROS_SOURCE_ROOT}/rmw_fastrtps")
    set(RMW_FASTRTPS_SHARED_DIR
        "${RMW_FASTRTPS_SOURCE_DIR}/rmw_fastrtps_shared_cpp")
    set(RMW_FASTRTPS_CPP_DIR
        "${RMW_FASTRTPS_SOURCE_DIR}/rmw_fastrtps_cpp")
    set(RMW_SECURITY_COMMON_DIR
        "${RMW_SOURCE_DIR}/rmw_security_common")
    set(ROSIDL_BUFFER_BACKEND_DIR
        "${ROSIDL_SOURCE_DIR}/rosidl_buffer_backend")
    set(ROSIDL_BUFFER_REGISTRY_DIR
        "${ROSIDL_SOURCE_DIR}/rosidl_buffer_backend_registry")
    set(ROSIDL_INTROSPECTION_C_DIR
        "${ROSIDL_SOURCE_DIR}/rosidl_typesupport_introspection_c")
    set(ROSIDL_INTROSPECTION_CPP_DIR
        "${ROSIDL_SOURCE_DIR}/rosidl_typesupport_introspection_cpp")
    set(ROSIDL_TYPESUPPORT_SOURCE_DIR
        "${VELAROS_ROS_SOURCE_ROOT}/rosidl_typesupport")
    set(TRACETOOLS_SOURCE_DIR
        "${VELAROS_ROS_SOURCE_ROOT}/ros2_tracing/tracetools")

    foreach(required_source
            "${RMW_FASTRTPS_SHARED_DIR}/package.xml"
            "${RMW_FASTRTPS_CPP_DIR}/package.xml"
            "${RMW_SECURITY_COMMON_DIR}/package.xml"
            "${ROSIDL_BUFFER_BACKEND_DIR}/package.xml"
            "${ROSIDL_BUFFER_REGISTRY_DIR}/package.xml"
            "${ROSIDL_INTROSPECTION_C_DIR}/package.xml"
            "${ROSIDL_INTROSPECTION_CPP_DIR}/package.xml"
            "${ROSIDL_TYPESUPPORT_SOURCE_DIR}/rosidl_typesupport_cpp/package.xml"
            "${TRACETOOLS_SOURCE_DIR}/package.xml")
      if(NOT EXISTS "${required_source}")
        message(
          FATAL_ERROR
            "VelaROS Fast DDS RMW source is missing: ${required_source}. "
            "Run tools/restore_velaros_ros2_sources.sh.")
      endif()
    endforeach()

    add_library(
      rmw_security_common STATIC
      "${RMW_SECURITY_COMMON_DIR}/src/security.cpp")
    target_include_directories(
      rmw_security_common PUBLIC "${RMW_SECURITY_COMMON_DIR}/include")
    target_link_libraries(rmw_security_common PUBLIC rcutils rmw)
    target_compile_definitions(
      rmw_security_common PRIVATE RMW_SECURITY_COMMON_BUILDING_LIBRARY)
    target_compile_options(
      rmw_security_common PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rmw_security_common
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rmw_security_common)

    add_library(
      rosidl_typesupport_introspection_c STATIC
      "${ROSIDL_INTROSPECTION_C_DIR}/src/identifier.c")
    target_include_directories(
      rosidl_typesupport_introspection_c
      PUBLIC "${ROSIDL_INTROSPECTION_C_DIR}/include")
    target_link_libraries(
      rosidl_typesupport_introspection_c PUBLIC rosidl_runtime_c)
    target_compile_options(
      rosidl_typesupport_introspection_c PRIVATE ${VELAROS_C_FLAGS})
    nuttx_add_external_library(rosidl_typesupport_introspection_c)

    add_library(
      rosidl_typesupport_introspection_cpp STATIC
      "${ROSIDL_INTROSPECTION_CPP_DIR}/src/identifier.cpp")
    target_include_directories(
      rosidl_typesupport_introspection_cpp
      PUBLIC "${ROSIDL_INTROSPECTION_CPP_DIR}/include")
    target_link_libraries(
      rosidl_typesupport_introspection_cpp PUBLIC rosidl_runtime_c)
    target_compile_options(
      rosidl_typesupport_introspection_cpp PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rosidl_typesupport_introspection_cpp
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rosidl_typesupport_introspection_cpp)

    add_library(rosidl_buffer_backend INTERFACE)
    target_include_directories(
      rosidl_buffer_backend
      INTERFACE "${ROSIDL_BUFFER_BACKEND_DIR}/include")
    target_link_libraries(
      rosidl_buffer_backend INTERFACE rmw rosidl_runtime_c)

    if(CONFIG_VELAROS_BUFFER_BACKEND)
      add_library(
        velaros_buffer_pool STATIC
        "${CMAKE_CURRENT_LIST_DIR}/velaros_buffer_pool.c")
      target_include_directories(
        velaros_buffer_pool
        PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
      target_compile_options(
        velaros_buffer_pool PRIVATE ${VELAROS_C_FLAGS})
      set_target_properties(
        velaros_buffer_pool PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
      nuttx_add_external_library(velaros_buffer_pool)

      add_library(
        velaros_buffer_backend STATIC
        "${CMAKE_CURRENT_LIST_DIR}/velaros_buffer_backend.cpp")
      target_include_directories(
        velaros_buffer_backend
        PUBLIC
          "${CMAKE_CURRENT_LIST_DIR}/include"
          "${ROSIDL_SOURCE_DIR}/rosidl_buffer/include"
          "${ROSIDL_BUFFER_BACKEND_DIR}/include"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
      target_link_libraries(
        velaros_buffer_backend
        PUBLIC
          fastcdr
          rmw
          rmw_dds_common_typesupport_fastrtps_cpp
          rosidl_buffer
          rosidl_buffer_backend
          rosidl_runtime_cpp
          velaros_buffer_pool)
      target_compile_options(
        velaros_buffer_backend PRIVATE ${VELAROS_CXX_FLAGS})
      set_target_properties(
        velaros_buffer_backend
        PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
      nuttx_add_external_library(velaros_buffer_backend)
    endif()

    add_library(
      rosidl_buffer_backend_registry STATIC
      "${ROSIDL_BUFFER_REGISTRY_DIR}/src/buffer_backend_registry.cpp")
    target_include_directories(
      rosidl_buffer_backend_registry
      PUBLIC "${ROSIDL_BUFFER_REGISTRY_DIR}/include")
    target_link_libraries(
      rosidl_buffer_backend_registry
      PUBLIC rcutils rmw rosidl_buffer_backend rosidl_runtime_cpp)
    target_compile_definitions(
      rosidl_buffer_backend_registry
      PRIVATE ROSIDL_BUFFER_BACKEND_REGISTRY_BUILDING_DLL)
    target_compile_options(
      rosidl_buffer_backend_registry PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rosidl_buffer_backend_registry
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rosidl_buffer_backend_registry)

    add_library(tracetools INTERFACE)
    target_include_directories(
      tracetools
      INTERFACE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${TRACETOOLS_SOURCE_DIR}/include")

    add_library(
      rosidl_typesupport_fastrtps_c STATIC
      "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/src/identifier.cpp")
    target_include_directories(
      rosidl_typesupport_fastrtps_c
      PUBLIC
        "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include")
    target_link_libraries(
      rosidl_typesupport_fastrtps_c
      PUBLIC fastcdr rosidl_runtime_c rosidl_typesupport_interface)
    target_compile_options(
      rosidl_typesupport_fastrtps_c PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rosidl_typesupport_fastrtps_c
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rosidl_typesupport_fastrtps_c)

    set(VELAROS_RMW_FASTRTPS_INCLUDE_DIRS
        "${RMW_FASTRTPS_SHARED_DIR}/include"
        "${RMW_FASTRTPS_SHARED_DIR}/src"
        "${RMW_FASTRTPS_CPP_DIR}/include"
        "${RMW_FASTRTPS_CPP_DIR}/src"
        "${RMW_SECURITY_COMMON_DIR}/include"
        "${ROSIDL_BUFFER_BACKEND_DIR}/include"
        "${ROSIDL_BUFFER_REGISTRY_DIR}/include"
        "${ROSIDL_INTROSPECTION_C_DIR}/include"
        "${ROSIDL_INTROSPECTION_CPP_DIR}/include"
        "${ROSIDL_TYPESUPPORT_SOURCE_DIR}/rosidl_typesupport_c/include"
        "${ROSIDL_TYPESUPPORT_SOURCE_DIR}/rosidl_typesupport_cpp/include"
        "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
        "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include"
        "${RMW_DDS_COMMON_GENERATOR_C_DIR}"
        "${RMW_DDS_COMMON_GENERATOR_CPP_DIR}"
        "${RMW_DDS_COMMON_FASTRTPS_CPP_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${TRACETOOLS_SOURCE_DIR}/include")

    file(
      GLOB RMW_FASTRTPS_SHARED_SOURCES
      CONFIGURE_DEPENDS "${RMW_FASTRTPS_SHARED_DIR}/src/*.cpp")
    add_library(
      rmw_fastrtps_shared_cpp STATIC ${RMW_FASTRTPS_SHARED_SOURCES})
    target_include_directories(
      rmw_fastrtps_shared_cpp
      PUBLIC "${RMW_FASTRTPS_SHARED_DIR}/include"
      PRIVATE ${VELAROS_RMW_FASTRTPS_INCLUDE_DIRS})
    target_link_libraries(
      rmw_fastrtps_shared_cpp
      PUBLIC
        fastcdr
        fastdds
        rcpputils
        rcutils
        rmw
        rmw_dds_common
        rmw_security_common
        rosidl_dynamic_typesupport
        rosidl_runtime_c
        rosidl_runtime_cpp
        rosidl_typesupport_introspection_c
        rosidl_typesupport_introspection_cpp
        rosidl_buffer_backend_registry
        tracetools)
    target_compile_definitions(
      rmw_fastrtps_shared_cpp
      PRIVATE RMW_FASTRTPS_SHARED_CPP_BUILDING_LIBRARY)
    target_compile_options(
      rmw_fastrtps_shared_cpp PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rmw_fastrtps_shared_cpp
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rmw_fastrtps_shared_cpp)

    file(
      GLOB RMW_FASTRTPS_CPP_SOURCES
      CONFIGURE_DEPENDS "${RMW_FASTRTPS_CPP_DIR}/src/*.cpp")
    add_library(rmw_fastrtps_cpp STATIC ${RMW_FASTRTPS_CPP_SOURCES})
    target_include_directories(
      rmw_fastrtps_cpp
      PUBLIC "${RMW_FASTRTPS_CPP_DIR}/include"
      PRIVATE ${VELAROS_RMW_FASTRTPS_INCLUDE_DIRS})
    target_link_libraries(
      rmw_fastrtps_cpp
      PUBLIC
        fastcdr
        fastdds
        rcpputils
        rcutils
        rmw
        rmw_dds_common
        rmw_fastrtps_shared_cpp
        rosidl_buffer_backend_registry
        rosidl_dynamic_typesupport
        rosidl_dynamic_typesupport_fastrtps
        rosidl_runtime_c
        rosidl_runtime_cpp
        rosidl_typesupport_fastrtps_c
        rmw_dds_common_typesupport_fastrtps_cpp
        tracetools)
    if(CONFIG_VELAROS_BUFFER_BACKEND)
      target_link_libraries(
        rmw_fastrtps_cpp PUBLIC velaros_buffer_backend)
    endif()
    target_compile_definitions(
      rmw_fastrtps_cpp PRIVATE RMW_FASTRTPS_CPP_BUILDING_LIBRARY)
    target_compile_options(
      rmw_fastrtps_cpp PRIVATE ${VELAROS_CXX_FLAGS})
    set_target_properties(
      rmw_fastrtps_cpp
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rmw_fastrtps_cpp)

    if(CONFIG_VELAROS_RMW_FASTRTPS_SMOKE)
      nuttx_add_application(
        NAME
        velaros_rmw_fastrtps_smoke
        COMPILE_FLAGS
        ${VELAROS_CXX_FLAGS}
        SRCS
        "${CMAKE_CURRENT_LIST_DIR}/velaros_rmw_fastrtps_smoke.cpp"
        INCLUDE_DIRECTORIES
        "${RCUTILS_SOURCE_DIR}/include"
        "${RMW_SOURCE_DIR}/rmw/include"
        DEPENDS
        rmw_fastrtps_cpp
        STACKSIZE
        524288
        PRIORITY
        100)
    endif()

    if(CONFIG_VELAROS_RCL)
      set(RCL_SOURCE_DIR "${VELAROS_ROS_SOURCE_ROOT}/rcl/rcl")
      set(RCL_YAML_SOURCE_DIR
          "${VELAROS_ROS_SOURCE_ROOT}/rcl/rcl_yaml_param_parser")
      set(STD_MSGS_GENERATED_DIR
          "${VELAROS_ROS_GENERATED_ROOT}/std_msgs")
      set(STD_MSGS_GENERATOR_C_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_generator_c")
      set(STD_MSGS_GENERATOR_CPP_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_generator_cpp")
      set(STD_MSGS_FASTRTPS_C_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_typesupport_fastrtps_c")
      set(STD_MSGS_FASTRTPS_CPP_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_typesupport_fastrtps_cpp")
      set(GEOMETRY_MSGS_GENERATOR_C_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_generator_c")
      set(GEOMETRY_MSGS_GENERATOR_CPP_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_generator_cpp")
      set(GEOMETRY_MSGS_FASTRTPS_C_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_typesupport_fastrtps_c")
      set(GEOMETRY_MSGS_FASTRTPS_CPP_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_typesupport_fastrtps_cpp")
      set(STD_SRVS_GENERATED_DIR
          "${VELAROS_ROS_GENERATED_ROOT}/std_srvs")
      set(STD_SRVS_GENERATOR_C_DIR
          "${STD_SRVS_GENERATED_DIR}/rosidl_generator_c")
      set(STD_SRVS_GENERATOR_CPP_DIR
          "${STD_SRVS_GENERATED_DIR}/rosidl_generator_cpp")
      set(STD_SRVS_FASTRTPS_C_DIR
          "${STD_SRVS_GENERATED_DIR}/rosidl_typesupport_fastrtps_c")
      set(STD_SRVS_FASTRTPS_CPP_DIR
          "${STD_SRVS_GENERATED_DIR}/rosidl_typesupport_fastrtps_cpp")
      set(ACTION_GENERATED_DIR
          "${VELAROS_ROS_GENERATED_ROOT}/action")
      set(ACTION_GENERATOR_C_DIR
          "${ACTION_GENERATED_DIR}/rosidl_generator_c")
      set(ACTION_FASTRTPS_C_DIR
          "${ACTION_GENERATED_DIR}/rosidl_typesupport_fastrtps_c")
      set(RCL_ACTION_SOURCE_DIR
          "${VELAROS_ROS_SOURCE_ROOT}/rcl/rcl_action")

      foreach(required_source
              "${RCL_SOURCE_DIR}/package.xml"
              "${RCL_SOURCE_DIR}/src/rcl/init.c"
              "${RCL_SOURCE_DIR}/src/rcl/node.c"
              "${RCL_YAML_SOURCE_DIR}/include/rcl_yaml_param_parser/types.h"
              "${STD_MSGS_GENERATED_DIR}/generation.manifest"
              "${STD_MSGS_GENERATOR_C_DIR}/std_msgs/msg/string.h"
              "${STD_MSGS_FASTRTPS_C_DIR}/std_msgs/msg/detail/string__type_support_c.cpp")
        if(NOT EXISTS "${required_source}")
          message(
            FATAL_ERROR
              "VelaROS rcl source is missing: ${required_source}. "
              "Run tools/restore_velaros_ros2_sources.sh.")
        endif()
      endforeach()

      if(CONFIG_VELAROS_RCLCPP)
        foreach(required_source
                "${VELAROS_ROS_SOURCE_ROOT}/rclcpp/rclcpp/package.xml"
                "${STD_MSGS_GENERATOR_CPP_DIR}/std_msgs/msg/string.hpp"
                "${STD_MSGS_FASTRTPS_CPP_DIR}/std_msgs/msg/detail/dds_fastrtps/string__type_support.cpp"
                "${GEOMETRY_MSGS_GENERATOR_CPP_DIR}/geometry_msgs/msg/twist.hpp"
                "${GEOMETRY_MSGS_FASTRTPS_CPP_DIR}/geometry_msgs/msg/detail/dds_fastrtps/twist__type_support.cpp"
                "${STD_SRVS_GENERATOR_CPP_DIR}/std_srvs/srv/set_bool.hpp"
                "${STD_SRVS_FASTRTPS_CPP_DIR}/std_srvs/srv/detail/dds_fastrtps/set_bool__type_support.cpp")
          if(NOT EXISTS "${required_source}")
            message(
              FATAL_ERROR
                "VelaROS rclcpp source is missing: ${required_source}. "
                "Run tools/restore_velaros_ros2_sources.sh and "
                "tools/generate_velaros_std_msgs.sh plus "
                "tools/generate_velaros_std_srvs.sh.")
          endif()
        endforeach()
      endif()

      set(
        VELAROS_RCL_MINIMAL_SOURCES
        "${RCL_SOURCE_DIR}/src/rcl/common.c"
        "${RCL_SOURCE_DIR}/src/rcl/context.c"
        "${RCL_SOURCE_DIR}/src/rcl/discovery_options.c"
        "${RCL_SOURCE_DIR}/src/rcl/domain_id.c"
        "${RCL_SOURCE_DIR}/src/rcl/guard_condition.c"
        "${RCL_SOURCE_DIR}/src/rcl/init.c"
        "${RCL_SOURCE_DIR}/src/rcl/init_options.c"
        "${RCL_SOURCE_DIR}/src/rcl/node.c"
        "${RCL_SOURCE_DIR}/src/rcl/node_options.c"
        "${RCL_SOURCE_DIR}/src/rcl/publisher.c"
        "${RCL_SOURCE_DIR}/src/rcl/security.c"
        "${RCL_SOURCE_DIR}/src/rcl/subscription.c"
        "${RCL_SOURCE_DIR}/src/rcl/time.c"
        "${RCL_SOURCE_DIR}/src/rcl/timer.c"
        "${RCL_SOURCE_DIR}/src/rcl/validate_enclave_name.c"
        "${RCL_SOURCE_DIR}/src/rcl/wait.c"
        "${CMAKE_CURRENT_LIST_DIR}/rcl_nuttx_minimal_arguments.c")

      if(CONFIG_VELAROS_SERVICES)
        foreach(required_source
                "${STD_SRVS_GENERATED_DIR}/generation.manifest"
                "${STD_SRVS_GENERATOR_C_DIR}/std_srvs/srv/set_bool.h"
                "${STD_SRVS_FASTRTPS_C_DIR}/std_srvs/srv/detail/set_bool__type_support_c.cpp")
          if(NOT EXISTS "${required_source}")
            message(
              FATAL_ERROR
                "VelaROS service source is missing: ${required_source}. "
                "Run tools/generate_velaros_std_srvs.sh.")
          endif()
        endforeach()
        list(
          APPEND VELAROS_RCL_MINIMAL_SOURCES
          "${RCL_SOURCE_DIR}/src/rcl/client.c"
          "${RCL_SOURCE_DIR}/src/rcl/graph.c"
          "${RCL_SOURCE_DIR}/src/rcl/service.c")
      endif()

      if(CONFIG_VELAROS_ACTIONS)
        foreach(required_source
                "${RCL_ACTION_SOURCE_DIR}/package.xml"
                "${RCL_ACTION_SOURCE_DIR}/src/rcl_action/action_client.c"
                "${RCL_ACTION_SOURCE_DIR}/src/rcl_action/action_server.c"
                "${ACTION_GENERATED_DIR}/generation.manifest"
                "${ACTION_GENERATOR_C_DIR}/example_interfaces/action/fibonacci.h"
                "${ACTION_FASTRTPS_C_DIR}/example_interfaces/action/detail/fibonacci__type_support_c.cpp"
                "${ACTION_GENERATOR_C_DIR}/velaros_interfaces/action/move_relative.h"
                "${ACTION_FASTRTPS_C_DIR}/velaros_interfaces/action/detail/move_relative__type_support_c.cpp")
          if(NOT EXISTS "${required_source}")
            message(
              FATAL_ERROR
                "VelaROS Action source is missing: ${required_source}. "
                "Run tools/restore_velaros_ros2_sources.sh and "
                "tools/generate_velaros_action.sh.")
          endif()
        endforeach()
        list(
          APPEND VELAROS_RCL_MINIMAL_SOURCES
          "${RCL_SOURCE_DIR}/src/rcl/event_callback.c")
      endif()

      add_library(rcl STATIC ${VELAROS_RCL_MINIMAL_SOURCES})
      target_include_directories(
        rcl
        PUBLIC
          "${RCL_SOURCE_DIR}/include"
          "${RCL_YAML_SOURCE_DIR}/include"
        PRIVATE
          "${RCL_SOURCE_DIR}/src/rcl")
      target_link_libraries(
        rcl
        PUBLIC
          rcutils
          rmw
          rmw_fastrtps_cpp
          rosidl_runtime_c
          tracetools)
      target_compile_definitions(
        rcl PRIVATE RCL_BUILDING_DLL ROS_PACKAGE_NAME=\"rcl\")
      target_compile_options(rcl PRIVATE ${VELAROS_C_FLAGS})
      set_target_properties(
        rcl PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
      nuttx_add_external_library(rcl)

      if(CONFIG_VELAROS_ACTIONS)
        add_library(
          rcl_action STATIC
          "${RCL_ACTION_SOURCE_DIR}/src/rcl_action/action_client.c"
          "${RCL_ACTION_SOURCE_DIR}/src/rcl_action/action_server.c"
          "${RCL_ACTION_SOURCE_DIR}/src/rcl_action/goal_handle.c"
          "${RCL_ACTION_SOURCE_DIR}/src/rcl_action/goal_state_machine.c"
          "${RCL_ACTION_SOURCE_DIR}/src/rcl_action/names.c"
          "${RCL_ACTION_SOURCE_DIR}/src/rcl_action/types.c")
        target_include_directories(
          rcl_action
          PUBLIC "${RCL_ACTION_SOURCE_DIR}/include"
          PRIVATE
            "${RCL_ACTION_SOURCE_DIR}/src"
            "${ACTION_GENERATOR_C_DIR}")
        target_link_libraries(
          rcl_action
          PUBLIC rcl rmw rosidl_runtime_c
          PRIVATE rcutils)
        target_compile_definitions(
          rcl_action
          PRIVATE
            RCL_ACTION_BUILDING_DLL
            ROS_PACKAGE_NAME=\"rcl_action\"
            VELAROS_ACTION_MAX_GOALS=${CONFIG_VELAROS_ACTION_MAX_GOALS})
        target_compile_options(rcl_action PRIVATE ${VELAROS_C_FLAGS})
        set_target_properties(
          rcl_action PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        nuttx_add_external_library(rcl_action)
      endif()

      if(CONFIG_VELAROS_EXECUTOR)
        add_library(
          velaros_executor STATIC
          "${CMAKE_CURRENT_LIST_DIR}/velaros_executor.c")
        target_include_directories(
          velaros_executor
          PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
        target_link_libraries(velaros_executor PUBLIC rcl)
        if(CONFIG_VELAROS_ACTIONS)
          target_include_directories(
            velaros_executor PRIVATE "${ACTION_GENERATOR_C_DIR}")
          target_link_libraries(velaros_executor PUBLIC rcl_action)
        endif()
        target_compile_options(
          velaros_executor PRIVATE ${VELAROS_C_FLAGS})
        set_target_properties(
          velaros_executor PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_executor)
      endif()

      if(CONFIG_VELAROS_RCLCPP)
        add_library(
          velaros_rclcpp STATIC
          "${CMAKE_CURRENT_LIST_DIR}/velaros_rclcpp.cpp")
        target_include_directories(
          velaros_rclcpp
          PUBLIC
            "${CMAKE_CURRENT_LIST_DIR}/include"
            "${STD_MSGS_GENERATOR_CPP_DIR}"
            "${STD_MSGS_FASTRTPS_CPP_DIR}"
            "${STD_SRVS_GENERATOR_CPP_DIR}"
            "${STD_SRVS_FASTRTPS_CPP_DIR}"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
        target_link_libraries(
          velaros_rclcpp
          PUBLIC
            rcl
            rosidl_runtime_cpp
            rosidl_typesupport_interface
            velaros_executor)
        if(CONFIG_VELAROS_PLATFORM_CONFIG)
          target_link_libraries(velaros_rclcpp PUBLIC velaros_config_kvdb)
        endif()
        if(CONFIG_VELAROS_ACTIONS)
          target_include_directories(
            velaros_rclcpp
            PUBLIC
              "${RCL_ACTION_SOURCE_DIR}/include"
              "${ACTION_GENERATOR_C_DIR}"
              "${ACTION_FASTRTPS_C_DIR}"
              "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include")
          target_link_libraries(
            velaros_rclcpp
            PUBLIC
              rcl_action
              velaros_action_generator_c
              velaros_action_typesupport_fastrtps_c
              velaros_action_typesupport)
        endif()
        target_compile_definitions(
          velaros_rclcpp
          PUBLIC VELAROS_RCLCPP_VERSION_MAJOR=32)
        target_compile_options(
          velaros_rclcpp PRIVATE ${VELAROS_CXX_FLAGS})
        set_target_properties(
          velaros_rclcpp
          PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_rclcpp)
      endif()

      if(CONFIG_VELAROS_SYSLOG_ADAPTER)
        add_library(
          velaros_logging_syslog STATIC
          "${CMAKE_CURRENT_LIST_DIR}/velaros_logging_syslog.c")
        target_include_directories(
          velaros_logging_syslog
          PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
        target_link_libraries(velaros_logging_syslog PUBLIC rcutils)
        target_compile_options(
          velaros_logging_syslog PRIVATE ${VELAROS_C_FLAGS})
        set_target_properties(
          velaros_logging_syslog
          PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_logging_syslog)
      endif()

      if(CONFIG_VELAROS_PLATFORM_CONFIG)
        add_library(
          velaros_config_kvdb STATIC
          "${CMAKE_CURRENT_LIST_DIR}/velaros_config_kvdb.c")
        target_include_directories(
          velaros_config_kvdb
          PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
        target_link_libraries(velaros_config_kvdb PUBLIC framework_utils)
        target_compile_options(
          velaros_config_kvdb PRIVATE ${VELAROS_C_FLAGS})
        set_target_properties(
          velaros_config_kvdb
          PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_config_kvdb)
      endif()

      if(CONFIG_VELAROS_UORB_BRIDGE)
        add_library(
          velaros_uorb_bridge STATIC
          "${CMAKE_CURRENT_LIST_DIR}/velaros_uorb_bridge.c"
          "${CMAKE_CURRENT_LIST_DIR}/velaros_uorb_topics.c")
        target_include_directories(
          velaros_uorb_bridge
          PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
        target_link_libraries(velaros_uorb_bridge PUBLIC rcl uorb)
        if(CONFIG_VELAROS_BUFFER_BACKEND)
          target_link_libraries(
            velaros_uorb_bridge PUBLIC velaros_buffer_pool)
        endif()
        target_compile_options(
          velaros_uorb_bridge PRIVATE ${VELAROS_C_FLAGS})
        set_target_properties(
          velaros_uorb_bridge
          PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_uorb_bridge)
      endif()

      if(CONFIG_VELAROS_AMP)
        add_library(
          velaros_amp STATIC
          "${CMAKE_CURRENT_LIST_DIR}/velaros_amp_protocol.c"
          "${CMAKE_CURRENT_LIST_DIR}/velaros_amp_service.c")
        target_include_directories(
          velaros_amp
          PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
        target_link_libraries(velaros_amp PUBLIC velaros_uorb_bridge uorb)
        target_compile_options(velaros_amp PRIVATE ${VELAROS_C_FLAGS})
        set_target_properties(
          velaros_amp PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_amp)

        if(CONFIG_VELAROS_AMP_SMOKE)
          nuttx_add_application(
            NAME
            velaros_amp_smoke
            COMPILE_FLAGS
            ${VELAROS_C_FLAGS}
            SRCS
            "${CMAKE_CURRENT_LIST_DIR}/velaros_amp_smoke.c"
            INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_LIST_DIR}/include"
            DEPENDS
            velaros_amp
            velaros_uorb_bridge
            uorb
            STACKSIZE
            32768
            PRIORITY
            100)
        endif()
      endif()

      if(CONFIG_VELAROS_NAVIGATION)
        set(VELAROS_NAVIGATION_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/navigation/nav_costmap.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/navigation/astar_planner.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/navigation/trajectory_controller.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/navigation/snapshot_publisher.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/navigation/navigation_pipeline.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/navigation/velaros_navigation_adapter.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/navigation/uorb_sink.cpp")

        add_library(velaros_navigation STATIC ${VELAROS_NAVIGATION_SOURCES})
        target_include_directories(
          velaros_navigation
          PUBLIC
            "${CMAKE_CURRENT_LIST_DIR}/include")
        target_link_libraries(velaros_navigation PUBLIC uorb)
        target_compile_definitions(
          velaros_navigation
          PUBLIC
            VELAROS_NAV_MAX_MAP_CELLS=${CONFIG_VELAROS_NAV_MAX_MAP_CELLS}
            VELAROS_NAV_MAX_PATH_NODES=${CONFIG_VELAROS_NAV_MAX_PATH_NODES}
            VELAROS_NAVIGATION_SCALAR_MATH=1)
        target_compile_options(velaros_navigation PRIVATE ${VELAROS_CXX_FLAGS})
        set_target_properties(
          velaros_navigation
          PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_navigation)

        if(CONFIG_VELAROS_NAVIGATION_SMOKE)
          nuttx_add_application(
            NAME
            velaros_navigation_smoke
            COMPILE_FLAGS
            ${VELAROS_CXX_FLAGS}
            SRCS
            "${CMAKE_CURRENT_LIST_DIR}/velaros_navigation_smoke.cpp"
            INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_LIST_DIR}/include"
            DEPENDS
            velaros_navigation
            uorb
            STACKSIZE
            131072
            PRIORITY
            100)
        endif()
      endif()

      if(CONFIG_VELAROS_LIO)
        set(VELAROS_LIO_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/navigation/lio_runtime.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/common/geometry.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/kernels/point_kernel.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/map/small_ivox_map.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/map/super_lio_octvox_map.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/map/global_map.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/frontend/small_point_lio_frontend.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/keyframe/keyframe_manager.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/keyframe/keyframe_manager_update.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/backend/pose_graph_backend.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/backend/pgo_fusion.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/localization/lidar_localizer.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/localization/imu_dead_reckoner.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/loop/loop_closure.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/system/map_odom.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/src/system/slam_system.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/lio/adapters/miao/miao_pose_graph_backend.cpp")

        add_library(velaros_lio STATIC ${VELAROS_LIO_SOURCES})
        target_include_directories(
          velaros_lio
          PUBLIC
            "${CMAKE_CURRENT_LIST_DIR}/include"
            "${CMAKE_CURRENT_LIST_DIR}/lio/include"
            "${NUTTX_DIR}/../external/eigen/eigen")
        target_link_libraries(velaros_lio PUBLIC velaros_navigation libeigen)
        target_compile_definitions(
          velaros_lio
          PUBLIC
            VELAROS_LIO_TARGET=1
            VELAROS_LIO_MAX_LOCAL_VOXELS=${CONFIG_VELAROS_LIO_MAX_LOCAL_VOXELS}
            VELAROS_LIO_MAX_GLOBAL_POINTS=${CONFIG_VELAROS_LIO_MAX_GLOBAL_POINTS}
            VELAROS_LIO_MAX_CLOUD_POINTS=${CONFIG_VELAROS_LIO_MAX_CLOUD_POINTS}
            EIGEN_NO_IO=1)
        target_compile_options(
          velaros_lio PRIVATE ${VELAROS_CXX_FLAGS} -U__GLIBCXX__)
        set_target_properties(
          velaros_lio PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_lio)

        if(CONFIG_VELAROS_LIO_SMOKE)
          nuttx_add_application(
            NAME
            velaros_lio_smoke
            COMPILE_FLAGS
            ${VELAROS_CXX_FLAGS}
            SRCS
            "${CMAKE_CURRENT_LIST_DIR}/velaros_lio_smoke.cpp"
            INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_LIST_DIR}/include"
            "${CMAKE_CURRENT_LIST_DIR}/lio/include"
            "${NUTTX_DIR}/../external/eigen/eigen"
            DEPENDS
            velaros_lio
            velaros_navigation
            libeigen
            STACKSIZE
            262144
            PRIORITY
            100)
        endif()
      endif()

      if(CONFIG_VELAROS_RUNTIME_SERVICE)
        set(VELAROS_RUNTIME_AIDL_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/velaros_runtime_aidl")
        file(MAKE_DIRECTORY "${VELAROS_RUNTIME_AIDL_DIR}")
        nuttx_add_library(velaros_runtime_aidl STATIC)
        nuttx_add_aidl(
          TARGET
          velaros_runtime_aidl
          AIDL_BASE_DIR
          ${CMAKE_CURRENT_LIST_DIR}
          AIDL_INCLUDE_DIR
          ${CMAKE_CURRENT_LIST_DIR}
          AIDL_HEADER_DIR
          ${VELAROS_RUNTIME_AIDL_DIR}
          AIDL_OUT_DIR
          ${VELAROS_RUNTIME_AIDL_DIR}
          AIDL_FLAGS
          --lang=cpp
          AIDLS
          "${CMAKE_CURRENT_LIST_DIR}/IVelaRosRuntime.aidl")
        target_include_directories(
          velaros_runtime_aidl PUBLIC "${VELAROS_RUNTIME_AIDL_DIR}")
        target_link_libraries(velaros_runtime_aidl PUBLIC android_binder)
        target_compile_options(
          velaros_runtime_aidl
          PRIVATE
            ${VELAROS_CXX_FLAGS}
            -Wno-maybe-uninitialized
            -Wno-return-type
            -Wno-unknown-pragmas)
        set_target_properties(
          velaros_runtime_aidl
          PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

        foreach(velaros_runtime_app velarosd velarosctl)
          if(velaros_runtime_app STREQUAL "velarosd")
            set(VELAROS_RUNTIME_APP_SOURCE
                "${CMAKE_CURRENT_LIST_DIR}/velaros_runtime_service.cpp")
            set(VELAROS_RUNTIME_APP_STACK 65536)
          else()
            set(VELAROS_RUNTIME_APP_SOURCE
                "${CMAKE_CURRENT_LIST_DIR}/velaros_runtime_ctl.cpp")
            set(VELAROS_RUNTIME_APP_STACK 32768)
          endif()
          nuttx_add_application(
            NAME
            ${velaros_runtime_app}
            COMPILE_FLAGS
            ${VELAROS_CXX_FLAGS}
            -Wno-maybe-uninitialized
            -Wno-return-type
            -Wno-unknown-pragmas
            SRCS
            "${VELAROS_RUNTIME_APP_SOURCE}"
            INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_LIST_DIR}/include"
            "${VELAROS_RUNTIME_AIDL_DIR}"
            DEPENDS
            android_binder
            framework_utils
            velaros_config_kvdb
            velaros_runtime_aidl
            STACKSIZE
            ${VELAROS_RUNTIME_APP_STACK}
            PRIORITY
            100)
        endforeach()
      endif()

      set(
        STD_MSGS_GENERATOR_C_SOURCES
        "${STD_MSGS_GENERATOR_C_DIR}/std_msgs/msg/detail/float64__description.c"
        "${STD_MSGS_GENERATOR_C_DIR}/std_msgs/msg/detail/float64__functions.c"
        "${STD_MSGS_GENERATOR_C_DIR}/std_msgs/msg/detail/float64__type_support.c"
        "${STD_MSGS_GENERATOR_C_DIR}/std_msgs/msg/detail/string__description.c"
        "${STD_MSGS_GENERATOR_C_DIR}/std_msgs/msg/detail/string__functions.c"
        "${STD_MSGS_GENERATOR_C_DIR}/std_msgs/msg/detail/string__type_support.c")
      add_library(std_msgs_generator_c STATIC ${STD_MSGS_GENERATOR_C_SOURCES})
      target_include_directories(
        std_msgs_generator_c PUBLIC "${STD_MSGS_GENERATOR_C_DIR}")
      target_link_libraries(
        std_msgs_generator_c
        PUBLIC rosidl_runtime_c rosidl_typesupport_interface)
      target_compile_options(
        std_msgs_generator_c PRIVATE ${VELAROS_C_FLAGS})
      set_target_properties(
        std_msgs_generator_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
      nuttx_add_external_library(std_msgs_generator_c)

      add_library(
        std_msgs_typesupport_fastrtps_c STATIC
        "${STD_MSGS_FASTRTPS_C_DIR}/std_msgs/msg/detail/float64__type_support_c.cpp"
        "${STD_MSGS_FASTRTPS_C_DIR}/std_msgs/msg/detail/string__type_support_c.cpp")
      target_include_directories(
        std_msgs_typesupport_fastrtps_c
        PUBLIC
          "${STD_MSGS_GENERATOR_C_DIR}"
          "${STD_MSGS_FASTRTPS_C_DIR}"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
      target_link_libraries(
        std_msgs_typesupport_fastrtps_c
        PUBLIC
          fastcdr
          rmw
          rosidl_runtime_c
          rosidl_typesupport_fastrtps_c
          rosidl_typesupport_interface
          std_msgs_generator_c)
      target_compile_options(
        std_msgs_typesupport_fastrtps_c PRIVATE ${VELAROS_CXX_FLAGS})
      set_target_properties(
        std_msgs_typesupport_fastrtps_c
        PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
      nuttx_add_external_library(std_msgs_typesupport_fastrtps_c)

      add_library(
        geometry_msgs_generator_c STATIC
        "${GEOMETRY_MSGS_GENERATOR_C_DIR}/geometry_msgs/msg/detail/vector3__description.c"
        "${GEOMETRY_MSGS_GENERATOR_C_DIR}/geometry_msgs/msg/detail/vector3__functions.c"
        "${GEOMETRY_MSGS_GENERATOR_C_DIR}/geometry_msgs/msg/detail/vector3__type_support.c"
        "${GEOMETRY_MSGS_GENERATOR_C_DIR}/geometry_msgs/msg/detail/twist__description.c"
        "${GEOMETRY_MSGS_GENERATOR_C_DIR}/geometry_msgs/msg/detail/twist__functions.c"
        "${GEOMETRY_MSGS_GENERATOR_C_DIR}/geometry_msgs/msg/detail/twist__type_support.c")
      target_include_directories(
        geometry_msgs_generator_c PUBLIC "${GEOMETRY_MSGS_GENERATOR_C_DIR}")
      target_link_libraries(
        geometry_msgs_generator_c
        PUBLIC rosidl_runtime_c rosidl_typesupport_interface)
      target_compile_options(
        geometry_msgs_generator_c PRIVATE ${VELAROS_C_FLAGS})
      set_target_properties(
        geometry_msgs_generator_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
      nuttx_add_external_library(geometry_msgs_generator_c)

      add_library(
        geometry_msgs_typesupport_fastrtps_c STATIC
        "${GEOMETRY_MSGS_FASTRTPS_C_DIR}/geometry_msgs/msg/detail/vector3__type_support_c.cpp"
        "${GEOMETRY_MSGS_FASTRTPS_C_DIR}/geometry_msgs/msg/detail/twist__type_support_c.cpp")
      target_include_directories(
        geometry_msgs_typesupport_fastrtps_c
        PUBLIC
          "${GEOMETRY_MSGS_GENERATOR_C_DIR}"
          "${GEOMETRY_MSGS_FASTRTPS_C_DIR}"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
      target_link_libraries(
        geometry_msgs_typesupport_fastrtps_c
        PUBLIC
          fastcdr
          rmw
          rosidl_runtime_c
          rosidl_typesupport_fastrtps_c
          rosidl_typesupport_interface
          geometry_msgs_generator_c)
      target_compile_options(
        geometry_msgs_typesupport_fastrtps_c PRIVATE ${VELAROS_CXX_FLAGS})
      set_target_properties(
        geometry_msgs_typesupport_fastrtps_c
        PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
      nuttx_add_external_library(geometry_msgs_typesupport_fastrtps_c)

      if(CONFIG_VELAROS_RCLCPP)
        add_library(
          std_msgs_typesupport_fastrtps_cpp STATIC
          "${STD_MSGS_FASTRTPS_CPP_DIR}/std_msgs/msg/detail/dds_fastrtps/float64__type_support.cpp"
          "${STD_MSGS_FASTRTPS_CPP_DIR}/std_msgs/msg/detail/dds_fastrtps/string__type_support.cpp")
        target_include_directories(
          std_msgs_typesupport_fastrtps_cpp
          PUBLIC
            "${STD_MSGS_GENERATOR_C_DIR}"
            "${STD_MSGS_GENERATOR_CPP_DIR}"
            "${STD_MSGS_FASTRTPS_CPP_DIR}"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
        target_link_libraries(
          std_msgs_typesupport_fastrtps_cpp
          PUBLIC
            fastcdr
            rmw
            rosidl_runtime_c
            rosidl_runtime_cpp
            rosidl_typesupport_interface
            std_msgs_generator_c)
        target_compile_definitions(
          std_msgs_typesupport_fastrtps_cpp
          PRIVATE ROSIDL_TYPESUPPORT_FASTRTPS_CPP_BUILDING_DLL)
        target_compile_options(
          std_msgs_typesupport_fastrtps_cpp
          PRIVATE ${VELAROS_CXX_FLAGS})
        set_target_properties(
          std_msgs_typesupport_fastrtps_cpp
          PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        nuttx_add_external_library(std_msgs_typesupport_fastrtps_cpp)

        add_library(
          geometry_msgs_typesupport_fastrtps_cpp STATIC
          "${GEOMETRY_MSGS_FASTRTPS_CPP_DIR}/geometry_msgs/msg/detail/dds_fastrtps/vector3__type_support.cpp"
          "${GEOMETRY_MSGS_FASTRTPS_CPP_DIR}/geometry_msgs/msg/detail/dds_fastrtps/twist__type_support.cpp")
        target_include_directories(
          geometry_msgs_typesupport_fastrtps_cpp
          PUBLIC
            "${GEOMETRY_MSGS_GENERATOR_C_DIR}"
            "${GEOMETRY_MSGS_GENERATOR_CPP_DIR}"
            "${GEOMETRY_MSGS_FASTRTPS_CPP_DIR}"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
        target_link_libraries(
          geometry_msgs_typesupport_fastrtps_cpp
          PUBLIC
            fastcdr
            rmw
            rosidl_runtime_c
            rosidl_runtime_cpp
            rosidl_typesupport_interface
            geometry_msgs_generator_c)
        target_compile_definitions(
          geometry_msgs_typesupport_fastrtps_cpp
          PRIVATE ROSIDL_TYPESUPPORT_FASTRTPS_CPP_BUILDING_DLL)
        target_compile_options(
          geometry_msgs_typesupport_fastrtps_cpp PRIVATE ${VELAROS_CXX_FLAGS})
        set_target_properties(
          geometry_msgs_typesupport_fastrtps_cpp
          PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        nuttx_add_external_library(geometry_msgs_typesupport_fastrtps_cpp)
      endif()

      if(CONFIG_VELAROS_SERVICES)
        add_library(
          std_srvs_generator_c STATIC
          "${STD_SRVS_GENERATOR_C_DIR}/std_srvs/srv/detail/set_bool__functions.c"
          "${CMAKE_CURRENT_LIST_DIR}/velaros_std_srvs_set_bool_hashes.c")
        target_include_directories(
          std_srvs_generator_c PUBLIC "${STD_SRVS_GENERATOR_C_DIR}")
        target_link_libraries(
          std_srvs_generator_c
          PUBLIC rosidl_runtime_c rosidl_typesupport_interface)
        target_compile_options(
          std_srvs_generator_c PRIVATE ${VELAROS_C_FLAGS})
        set_target_properties(
          std_srvs_generator_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        nuttx_add_external_library(std_srvs_generator_c)

        add_library(
          std_srvs_typesupport_fastrtps_c STATIC
          "${STD_SRVS_FASTRTPS_C_DIR}/std_srvs/srv/detail/set_bool__type_support_c.cpp")
        target_include_directories(
          std_srvs_typesupport_fastrtps_c
          PUBLIC
            "${STD_SRVS_GENERATOR_C_DIR}"
            "${STD_SRVS_FASTRTPS_C_DIR}"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
        target_link_libraries(
          std_srvs_typesupport_fastrtps_c
          PUBLIC
            fastcdr
            rmw
            rosidl_runtime_c
            rosidl_runtime_cpp
            rosidl_typesupport_fastrtps_c
            rosidl_typesupport_interface
            std_srvs_generator_c)
        target_compile_options(
          std_srvs_typesupport_fastrtps_c PRIVATE ${VELAROS_CXX_FLAGS})
        set_target_properties(
          std_srvs_typesupport_fastrtps_c
          PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        nuttx_add_external_library(std_srvs_typesupport_fastrtps_c)

        if(CONFIG_VELAROS_RCLCPP)
          add_library(
            std_srvs_typesupport_fastrtps_cpp STATIC
            "${STD_SRVS_FASTRTPS_CPP_DIR}/std_srvs/srv/detail/dds_fastrtps/set_bool__type_support.cpp")
          target_include_directories(
            std_srvs_typesupport_fastrtps_cpp
            PUBLIC
              "${STD_SRVS_GENERATOR_C_DIR}"
              "${STD_SRVS_GENERATOR_CPP_DIR}"
              "${STD_SRVS_FASTRTPS_CPP_DIR}"
              "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
          target_link_libraries(
            std_srvs_typesupport_fastrtps_cpp
            PUBLIC
              fastcdr
              rmw
              rosidl_runtime_c
              rosidl_runtime_cpp
              rosidl_typesupport_interface
              std_srvs_generator_c)
          target_compile_definitions(
            std_srvs_typesupport_fastrtps_cpp
            PRIVATE ROSIDL_TYPESUPPORT_FASTRTPS_CPP_BUILDING_DLL)
          target_compile_options(
            std_srvs_typesupport_fastrtps_cpp
            PRIVATE ${VELAROS_CXX_FLAGS})
          set_target_properties(
            std_srvs_typesupport_fastrtps_cpp
            PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
          nuttx_add_external_library(std_srvs_typesupport_fastrtps_cpp)
        endif()
      endif()

      if(CONFIG_VELAROS_ACTIONS)
        add_library(
          velaros_action_generator_c STATIC
          "${ACTION_GENERATOR_C_DIR}/builtin_interfaces/msg/detail/time__functions.c"
          "${ACTION_GENERATOR_C_DIR}/unique_identifier_msgs/msg/detail/uuid__functions.c"
          "${ACTION_GENERATOR_C_DIR}/action_msgs/msg/detail/goal_info__functions.c"
          "${ACTION_GENERATOR_C_DIR}/action_msgs/msg/detail/goal_status__functions.c"
          "${ACTION_GENERATOR_C_DIR}/action_msgs/msg/detail/goal_status_array__functions.c"
          "${ACTION_GENERATOR_C_DIR}/action_msgs/srv/detail/cancel_goal__functions.c"
          "${ACTION_GENERATOR_C_DIR}/velaros_interfaces/action/detail/move_relative__functions.c"
          "${ACTION_GENERATED_DIR}/velaros_action_hashes.c")
        if(CONFIG_VELAROS_ACTION_FIBONACCI)
          target_sources(
            velaros_action_generator_c
            PRIVATE
              "${ACTION_GENERATOR_C_DIR}/example_interfaces/action/detail/fibonacci__functions.c")
        endif()
        target_include_directories(
          velaros_action_generator_c PUBLIC "${ACTION_GENERATOR_C_DIR}")
        target_link_libraries(
          velaros_action_generator_c
          PUBLIC rosidl_runtime_c rosidl_typesupport_interface)
        target_compile_definitions(
          velaros_action_generator_c
          PRIVATE
            VELAROS_ACTION_MAX_GOALS=${CONFIG_VELAROS_ACTION_MAX_GOALS})
        if(CONFIG_VELAROS_ACTION_FIBONACCI)
          target_compile_definitions(
            velaros_action_generator_c
            PRIVATE
              VELAROS_ACTION_SEQUENCE_CAPACITY=${CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY})
        endif()
        target_compile_options(
          velaros_action_generator_c PRIVATE ${VELAROS_C_FLAGS})
        set_target_properties(
          velaros_action_generator_c
          PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_action_generator_c)

        add_library(
          velaros_action_typesupport_fastrtps_c STATIC
          "${ACTION_FASTRTPS_C_DIR}/builtin_interfaces/msg/detail/time__type_support_c.cpp"
          "${ACTION_FASTRTPS_C_DIR}/unique_identifier_msgs/msg/detail/uuid__type_support_c.cpp"
          "${ACTION_FASTRTPS_C_DIR}/action_msgs/msg/detail/goal_info__type_support_c.cpp"
          "${ACTION_FASTRTPS_C_DIR}/action_msgs/msg/detail/goal_status__type_support_c.cpp"
          "${ACTION_FASTRTPS_C_DIR}/action_msgs/msg/detail/goal_status_array__type_support_c.cpp"
          "${ACTION_FASTRTPS_C_DIR}/action_msgs/srv/detail/cancel_goal__type_support_c.cpp"
          "${ACTION_FASTRTPS_C_DIR}/velaros_interfaces/action/detail/move_relative__type_support_c.cpp")
        if(CONFIG_VELAROS_ACTION_FIBONACCI)
          target_sources(
            velaros_action_typesupport_fastrtps_c
            PRIVATE
              "${ACTION_FASTRTPS_C_DIR}/example_interfaces/action/detail/fibonacci__type_support_c.cpp")
        endif()
        target_include_directories(
          velaros_action_typesupport_fastrtps_c
          PUBLIC
            "${ACTION_GENERATOR_C_DIR}"
            "${ACTION_FASTRTPS_C_DIR}"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include")
        target_link_libraries(
          velaros_action_typesupport_fastrtps_c
          PUBLIC
            fastcdr
            rmw
            rosidl_runtime_c
            rosidl_runtime_cpp
            rosidl_typesupport_fastrtps_c
            rosidl_typesupport_interface
            velaros_action_generator_c)
        target_compile_definitions(
          velaros_action_typesupport_fastrtps_c
          PRIVATE
            VELAROS_ACTION_MAX_GOALS=${CONFIG_VELAROS_ACTION_MAX_GOALS})
        if(CONFIG_VELAROS_ACTION_FIBONACCI)
          target_compile_definitions(
            velaros_action_typesupport_fastrtps_c
            PRIVATE
              VELAROS_ACTION_SEQUENCE_CAPACITY=${CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY})
        endif()
        target_compile_options(
          velaros_action_typesupport_fastrtps_c PRIVATE ${VELAROS_CXX_FLAGS})
        set_target_properties(
          velaros_action_typesupport_fastrtps_c
          PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_action_typesupport_fastrtps_c)

        add_library(
          velaros_action_typesupport STATIC
          "${CMAKE_CURRENT_LIST_DIR}/velaros_action_typesupport.c")
        target_include_directories(
          velaros_action_typesupport
          PUBLIC
            "${CMAKE_CURRENT_LIST_DIR}/include"
            "${ACTION_GENERATOR_C_DIR}")
        target_link_libraries(
          velaros_action_typesupport
          PUBLIC
            rosidl_runtime_c
            rosidl_typesupport_interface
            velaros_action_generator_c
            velaros_action_typesupport_fastrtps_c)
        target_compile_options(
          velaros_action_typesupport PRIVATE ${VELAROS_C_FLAGS})
        set_target_properties(
          velaros_action_typesupport
          PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        nuttx_add_external_library(velaros_action_typesupport)
      endif()

      if(CONFIG_VELAROS_RCL_SMOKE)
        nuttx_add_application(
          NAME
          velaros_rcl_smoke
          COMPILE_FLAGS
          ${VELAROS_C_FLAGS}
          SRCS
          "${CMAKE_CURRENT_LIST_DIR}/velaros_rcl_smoke.c"
          INCLUDE_DIRECTORIES
          "${RCL_SOURCE_DIR}/include"
          "${RCL_YAML_SOURCE_DIR}/include"
          DEPENDS
          rcl
          STACKSIZE
          524288
          PRIORITY
          100)
      endif()

      if(CONFIG_VELAROS_INTEROP)
        set(VELAROS_INTEROP_PLATFORM_DEPENDS)
        if(CONFIG_VELAROS_PLATFORM_CONFIG)
          list(APPEND VELAROS_INTEROP_PLATFORM_DEPENDS velaros_config_kvdb)
        endif()
        foreach(velaros_app velaros_talker velaros_listener)
          nuttx_add_application(
            NAME
            ${velaros_app}
            COMPILE_FLAGS
            ${VELAROS_C_FLAGS}
            SRCS
            "${CMAKE_CURRENT_LIST_DIR}/${velaros_app}.c"
            INCLUDE_DIRECTORIES
            "${RCL_SOURCE_DIR}/include"
            "${RCL_YAML_SOURCE_DIR}/include"
            "${STD_MSGS_GENERATOR_C_DIR}"
            "${STD_MSGS_FASTRTPS_C_DIR}"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
            DEPENDS
            rcl
            std_msgs_generator_c
            std_msgs_typesupport_fastrtps_c
            ${VELAROS_INTEROP_PLATFORM_DEPENDS}
            STACKSIZE
            524288
            PRIORITY
            100)
        endforeach()
      endif()

      if(CONFIG_VELAROS_ACTION_INTEROP)
        foreach(velaros_action_app velaros_action_server velaros_action_client)
          nuttx_add_application(
            NAME
            ${velaros_action_app}
            COMPILE_FLAGS
            ${VELAROS_C_FLAGS}
            SRCS
            "${CMAKE_CURRENT_LIST_DIR}/${velaros_action_app}.c"
            INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_LIST_DIR}/include"
            "${RCL_SOURCE_DIR}/include"
            "${RCL_YAML_SOURCE_DIR}/include"
            "${RCL_ACTION_SOURCE_DIR}/include"
            "${ACTION_GENERATOR_C_DIR}"
            "${ACTION_FASTRTPS_C_DIR}"
            "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
            DEPENDS
            rcl
            rcl_action
            velaros_action_generator_c
            velaros_action_typesupport_fastrtps_c
            velaros_action_typesupport
            velaros_executor
            STACKSIZE
            524288
            PRIORITY
            100)
        endforeach()
      endif()

      if(CONFIG_VELAROS_SERVICE_GATEWAY)
        nuttx_add_application(
          NAME
          velaros_bridge_service
          COMPILE_FLAGS
          ${VELAROS_C_FLAGS}
          SRCS
          "${CMAKE_CURRENT_LIST_DIR}/velaros_bridge_service.c"
          INCLUDE_DIRECTORIES
          "${CMAKE_CURRENT_LIST_DIR}/include"
          "${RCL_SOURCE_DIR}/include"
          "${RCL_YAML_SOURCE_DIR}/include"
          "${STD_SRVS_GENERATOR_C_DIR}"
          "${STD_SRVS_FASTRTPS_C_DIR}"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
          DEPENDS
          rcl
          std_srvs_generator_c
          std_srvs_typesupport_fastrtps_c
          velaros_config_kvdb
          velaros_executor
          STACKSIZE
          524288
          PRIORITY
          100)
      endif()

      if(CONFIG_VELAROS_EXECUTOR_SMOKE)
        nuttx_add_application(
          NAME
          velaros_executor_smoke
          COMPILE_FLAGS
          ${VELAROS_C_FLAGS}
          SRCS
          "${CMAKE_CURRENT_LIST_DIR}/velaros_executor_smoke.c"
          INCLUDE_DIRECTORIES
          "${CMAKE_CURRENT_LIST_DIR}/include"
          "${RCL_SOURCE_DIR}/include"
          "${RCL_YAML_SOURCE_DIR}/include"
          "${STD_MSGS_GENERATOR_C_DIR}"
          "${STD_MSGS_FASTRTPS_C_DIR}"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
          DEPENDS
          rcl
          std_msgs_generator_c
          std_msgs_typesupport_fastrtps_c
          velaros_executor
          STACKSIZE
          524288
          PRIORITY
          100)
      endif()

      if(CONFIG_VELAROS_RCLCPP_SMOKE)
        nuttx_add_application(
          NAME
          velaros_rclcpp_smoke
          COMPILE_FLAGS
          ${VELAROS_CXX_FLAGS}
          SRCS
          "${CMAKE_CURRENT_LIST_DIR}/velaros_rclcpp_smoke.cpp"
          INCLUDE_DIRECTORIES
          "${CMAKE_CURRENT_LIST_DIR}/include"
          "${RCL_SOURCE_DIR}/include"
          "${RCL_YAML_SOURCE_DIR}/include"
          "${STD_MSGS_GENERATOR_CPP_DIR}"
          "${STD_MSGS_FASTRTPS_CPP_DIR}"
          "${STD_SRVS_GENERATOR_CPP_DIR}"
          "${STD_SRVS_FASTRTPS_CPP_DIR}"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include"
          DEPENDS
          rcl
          std_msgs_generator_c
          std_msgs_typesupport_fastrtps_cpp
          std_srvs_generator_c
          std_srvs_typesupport_fastrtps_cpp
          velaros_executor
          velaros_rclcpp
          STACKSIZE
          524288
          PRIORITY
          100)
      endif()

      if(CONFIG_VELAROS_RCLCPP_ACTION_SMOKE)
        nuttx_add_application(
          NAME
          velaros_rclcpp_action_smoke
          COMPILE_FLAGS
          ${VELAROS_CXX_FLAGS}
          SRCS
          "${CMAKE_CURRENT_LIST_DIR}/velaros_rclcpp_action_smoke.cpp"
          INCLUDE_DIRECTORIES
          "${CMAKE_CURRENT_LIST_DIR}/include"
          "${RCL_SOURCE_DIR}/include"
          "${RCL_YAML_SOURCE_DIR}/include"
          "${RCL_ACTION_SOURCE_DIR}/include"
          "${ACTION_GENERATOR_C_DIR}"
          "${ACTION_FASTRTPS_C_DIR}"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
          DEPENDS
          rcl
          rcl_action
          velaros_action_generator_c
          velaros_action_typesupport_fastrtps_c
          velaros_action_typesupport
          velaros_executor
          velaros_rclcpp
          STACKSIZE
          524288
          PRIORITY
          100)
      endif()

      if(CONFIG_VELAROS_ROBOT_NODE)
        nuttx_add_application(
          NAME
          velaros_robot_node
          COMPILE_FLAGS
          ${VELAROS_CXX_FLAGS}
          SRCS
          "${CMAKE_CURRENT_LIST_DIR}/velaros_robot_node.cpp"
          INCLUDE_DIRECTORIES
          "${CMAKE_CURRENT_LIST_DIR}/include"
          "${RCL_SOURCE_DIR}/include"
          "${RCL_YAML_SOURCE_DIR}/include"
          "${RCL_ACTION_SOURCE_DIR}/include"
          "${GEOMETRY_MSGS_GENERATOR_CPP_DIR}"
          "${GEOMETRY_MSGS_FASTRTPS_CPP_DIR}"
          "${ACTION_GENERATOR_C_DIR}"
          "${ACTION_FASTRTPS_C_DIR}"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include"
          DEPENDS
          rcl
          rcl_action
          geometry_msgs_generator_c
          geometry_msgs_typesupport_fastrtps_cpp
          velaros_action_generator_c
          velaros_action_typesupport_fastrtps_c
          velaros_action_typesupport
          velaros_executor
          velaros_rclcpp
          velaros_uorb_bridge
          STACKSIZE
          524288
          PRIORITY
          100)
      endif()

      if(CONFIG_VELAROS_OPENVELA_INTEGRATION_SMOKE)
        nuttx_add_application(
          NAME
          velaros_openvela_integration_smoke
          COMPILE_FLAGS
          ${VELAROS_C_FLAGS}
          SRCS
          "${CMAKE_CURRENT_LIST_DIR}/velaros_openvela_integration_smoke.c"
          INCLUDE_DIRECTORIES
          "${CMAKE_CURRENT_LIST_DIR}/include"
          "${RCL_SOURCE_DIR}/include"
          "${RCL_YAML_SOURCE_DIR}/include"
          "${STD_MSGS_GENERATOR_C_DIR}"
          "${STD_MSGS_FASTRTPS_C_DIR}"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_c/include"
          "${NUTTX_APPS_DIR}/system/uorb"
          DEPENDS
          rcl
          std_msgs_generator_c
          std_msgs_typesupport_fastrtps_c
          uorb
          velaros_executor
          velaros_logging_syslog
          velaros_uorb_bridge
          STACKSIZE
          524288
          PRIORITY
          100)
      endif()

      if(CONFIG_VELAROS_BUFFER_BACKEND_SMOKE)
        nuttx_add_application(
          NAME
          velaros_buffer_backend_smoke
          COMPILE_FLAGS
          ${VELAROS_CXX_FLAGS}
          SRCS
          "${CMAKE_CURRENT_LIST_DIR}/velaros_buffer_backend_smoke.cpp"
          INCLUDE_DIRECTORIES
          "${CMAKE_CURRENT_LIST_DIR}/include"
          "${RMW_FASTRTPS_CPP_DIR}/src"
          "${ROSIDL_SOURCE_DIR}/rosidl_buffer/include"
          "${ROSIDL_BUFFER_BACKEND_DIR}/include"
          "${ROSIDL_BUFFER_REGISTRY_DIR}/include"
          "${ROSIDL_FASTRTPS_SOURCE_DIR}/rosidl_typesupport_fastrtps_cpp/include"
          DEPENDS
          fastcdr
          rmw_fastrtps_cpp
          rosidl_buffer
          rosidl_buffer_backend_registry
          uorb
          velaros_buffer_backend
          velaros_buffer_pool
          velaros_uorb_bridge
          STACKSIZE
          131072
          PRIORITY
          100)
      endif()
    endif()
  endif()

  if(CONFIG_VELAROS_CORE_SMOKE)
    set(VELAROS_CORE_SMOKE_INCLUDES
        "${RCUTILS_SOURCE_DIR}/include"
        "${RMW_SOURCE_DIR}/rmw/include")
    set(VELAROS_CORE_SMOKE_DEPENDS rcutils rmw)
    if(CONFIG_VELAROS_FASTRTPS_TYPESUPPORT)
      list(
        APPEND VELAROS_CORE_SMOKE_INCLUDES
        "${ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR}/include")
      list(
        APPEND VELAROS_CORE_SMOKE_DEPENDS
        rosidl_dynamic_typesupport_fastrtps)
    endif()

    nuttx_add_application(
      NAME
      velaros_core_smoke
      COMPILE_FLAGS
      ${VELAROS_C_FLAGS}
      SRCS
      "${CMAKE_CURRENT_LIST_DIR}/velaros_core_smoke.c"
      INCLUDE_DIRECTORIES
      ${VELAROS_CORE_SMOKE_INCLUDES}
      DEPENDS
      ${VELAROS_CORE_SMOKE_DEPENDS}
      STACKSIZE
      16384
      PRIORITY
      100)
  endif()
endif()
