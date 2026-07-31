# SPDX-License-Identifier: Apache-2.0

if(CONFIG_VELAROS_ROS2_CORE)
  set(VELAROS_ROS2_SOURCE_ROOT
      "${NUTTX_DIR}/../external/velaros_ros2_sources")
  set(VELAROS_ROS2_GENERATED_ROOT
      "${NUTTX_DIR}/../external/velaros_ros2_generated")
  set(RCUTILS_SOURCE_DIR "${VELAROS_ROS2_SOURCE_ROOT}/rcutils")
  set(ROSIDL_SOURCE_DIR "${VELAROS_ROS2_SOURCE_ROOT}/rosidl")
  set(ROSIDL_DYNAMIC_SOURCE_DIR
      "${VELAROS_ROS2_SOURCE_ROOT}/rosidl_dynamic_typesupport")
  set(RMW_SOURCE_DIR "${VELAROS_ROS2_SOURCE_ROOT}/rmw")
  set(RCPPUTILS_SOURCE_DIR "${VELAROS_ROS2_SOURCE_ROOT}/rcpputils")
  set(ROSIDL_DYNAMIC_FASTRTPS_SOURCE_DIR
      "${VELAROS_ROS2_SOURCE_ROOT}/rosidl_dynamic_typesupport_fastrtps")

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

  set(VELAROS_ROS2_C_FLAGS
      -Wall
      -Wextra
      -Wno-shadow
      -Wno-strict-prototypes
      -Wno-undef
      -Wno-unused-parameter
      -Wno-sign-conversion)

  set(VELAROS_ROS2_CXX_FLAGS
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
  target_compile_options(rcutils PRIVATE ${VELAROS_ROS2_C_FLAGS})
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
    rosidl_runtime_c PRIVATE ${VELAROS_ROS2_C_FLAGS})
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
    rosidl_dynamic_typesupport PRIVATE ${VELAROS_ROS2_C_FLAGS})
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
  target_compile_options(rmw PRIVATE ${VELAROS_ROS2_C_FLAGS})
  set_target_properties(rmw PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  nuttx_add_external_library(rmw)

  if(CONFIG_VELAROS_ROS2_FASTRTPS_TYPESUPPORT)
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
      rcpputils PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
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
      PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
    set_target_properties(
      rosidl_dynamic_typesupport_fastrtps
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rosidl_dynamic_typesupport_fastrtps)
  endif()

  if(CONFIG_VELAROS_ROS2_RMW_DDS_COMMON)
    set(RMW_DDS_COMMON_SOURCE_DIR
        "${VELAROS_ROS2_SOURCE_ROOT}/rmw_dds_common/rmw_dds_common")
    set(ROSIDL_FASTRTPS_SOURCE_DIR
        "${VELAROS_ROS2_SOURCE_ROOT}/rosidl_typesupport_fastrtps")
    set(RMW_DDS_COMMON_GENERATED_DIR
        "${VELAROS_ROS2_GENERATED_ROOT}/rmw_dds_common")
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
      rmw_dds_common_generator_c PRIVATE ${VELAROS_ROS2_C_FLAGS})
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
      PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
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
      rmw_dds_common PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
    set_target_properties(
      rmw_dds_common
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rmw_dds_common)

    if(CONFIG_VELAROS_ROS2_RMW_DDS_COMMON_SMOKE)
      nuttx_add_application(
        NAME
        velaros_rmw_dds_smoke
        COMPILE_FLAGS
        ${VELAROS_ROS2_CXX_FLAGS}
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

  if(CONFIG_VELAROS_ROS2_RMW_FASTRTPS_CPP)
    set(RMW_FASTRTPS_SOURCE_DIR
        "${VELAROS_ROS2_SOURCE_ROOT}/rmw_fastrtps")
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
        "${VELAROS_ROS2_SOURCE_ROOT}/rosidl_typesupport")
    set(TRACETOOLS_SOURCE_DIR
        "${VELAROS_ROS2_SOURCE_ROOT}/ros2_tracing/tracetools")

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
      rmw_security_common PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
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
      rosidl_typesupport_introspection_c PRIVATE ${VELAROS_ROS2_C_FLAGS})
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
      rosidl_typesupport_introspection_cpp PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
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
      rosidl_buffer_backend_registry PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
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
      rosidl_typesupport_fastrtps_c PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
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
      rmw_fastrtps_shared_cpp PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
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
    target_compile_definitions(
      rmw_fastrtps_cpp PRIVATE RMW_FASTRTPS_CPP_BUILDING_LIBRARY)
    target_compile_options(
      rmw_fastrtps_cpp PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
    set_target_properties(
      rmw_fastrtps_cpp
      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    nuttx_add_external_library(rmw_fastrtps_cpp)

    if(CONFIG_VELAROS_ROS2_RMW_FASTRTPS_SMOKE)
      nuttx_add_application(
        NAME
        velaros_rmw_fastrtps_smoke
        COMPILE_FLAGS
        ${VELAROS_ROS2_CXX_FLAGS}
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

    if(CONFIG_VELAROS_ROS2_RCL)
      set(RCL_SOURCE_DIR "${VELAROS_ROS2_SOURCE_ROOT}/rcl/rcl")
      set(RCL_YAML_SOURCE_DIR
          "${VELAROS_ROS2_SOURCE_ROOT}/rcl/rcl_yaml_param_parser")
      set(STD_MSGS_GENERATED_DIR
          "${VELAROS_ROS2_GENERATED_ROOT}/std_msgs")
      set(STD_MSGS_GENERATOR_C_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_generator_c")
      set(STD_MSGS_FASTRTPS_C_DIR
          "${STD_MSGS_GENERATED_DIR}/rosidl_typesupport_fastrtps_c")

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
        "${RCL_SOURCE_DIR}/src/rcl/validate_enclave_name.c"
        "${CMAKE_CURRENT_LIST_DIR}/rcl_nuttx_minimal_arguments.c")

      add_library(rcl STATIC ${VELAROS_RCL_MINIMAL_SOURCES})
      target_include_directories(
        rcl
        PUBLIC "${RCL_SOURCE_DIR}/include"
        PRIVATE
          "${RCL_SOURCE_DIR}/src/rcl"
          "${RCL_YAML_SOURCE_DIR}/include")
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
      target_compile_options(rcl PRIVATE ${VELAROS_ROS2_C_FLAGS})
      set_target_properties(
        rcl PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
      nuttx_add_external_library(rcl)

      set(
        STD_MSGS_GENERATOR_C_SOURCES
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
        std_msgs_generator_c PRIVATE ${VELAROS_ROS2_C_FLAGS})
      set_target_properties(
        std_msgs_generator_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
      nuttx_add_external_library(std_msgs_generator_c)

      add_library(
        std_msgs_typesupport_fastrtps_c STATIC
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
        std_msgs_typesupport_fastrtps_c PRIVATE ${VELAROS_ROS2_CXX_FLAGS})
      set_target_properties(
        std_msgs_typesupport_fastrtps_c
        PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
      nuttx_add_external_library(std_msgs_typesupport_fastrtps_c)

      if(CONFIG_VELAROS_ROS2_RCL_SMOKE)
        nuttx_add_application(
          NAME
          velaros_rcl_smoke
          COMPILE_FLAGS
          ${VELAROS_ROS2_C_FLAGS}
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

      if(CONFIG_VELAROS_ROS2_INTEROP)
        foreach(velaros_app velaros_ros2_talker velaros_ros2_listener)
          nuttx_add_application(
            NAME
            ${velaros_app}
            COMPILE_FLAGS
            ${VELAROS_ROS2_C_FLAGS}
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
            STACKSIZE
            524288
            PRIORITY
            100)
        endforeach()
      endif()
    endif()
  endif()

  if(CONFIG_VELAROS_ROS2_CORE_SMOKE)
    set(VELAROS_CORE_SMOKE_INCLUDES
        "${RCUTILS_SOURCE_DIR}/include"
        "${RMW_SOURCE_DIR}/rmw/include")
    set(VELAROS_CORE_SMOKE_DEPENDS rcutils rmw)
    if(CONFIG_VELAROS_ROS2_FASTRTPS_TYPESUPPORT)
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
      ${VELAROS_ROS2_C_FLAGS}
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
