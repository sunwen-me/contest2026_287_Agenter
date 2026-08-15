// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstring>

#include "rmw/types.h"
#include "rmw_dds_common/graph_cache.hpp"
#include "rmw_dds_common/msg/detail/gid__rosidl_typesupport_fastrtps_cpp.hpp"
#include "rosidl_typesupport_fastrtps_cpp/identifier.hpp"

#if defined(__NuttX__)
extern "C"
#endif
int main(int argc, char * argv[])
{
  (void)argc;
  (void)argv;

  rmw_dds_common::msg::Gid message;
  for (size_t index = 0; index < message.data.size(); ++index) {
    message.data[index] = static_cast<uint8_t>(index);
  }

  bool full_bounded = false;
  bool is_plain = false;
  const size_t serialized_size =
    rmw_dds_common::msg::typesupport_fastrtps_cpp::max_serialized_size_Gid(
    full_bounded, is_plain, 0);
  const rosidl_message_type_support_t * type_support =
    ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
    rosidl_typesupport_fastrtps_cpp, rmw_dds_common, msg, Gid)();

  if (serialized_size != RMW_GID_STORAGE_SIZE || !full_bounded || !is_plain ||
      type_support == nullptr || type_support->typesupport_identifier == nullptr ||
      std::strcmp(
        type_support->typesupport_identifier,
        rosidl_typesupport_fastrtps_cpp::typesupport_identifier) != 0) {
    std::printf("rmw_dds_common generated Fast RTPS typesupport: FAIL\n");
    return 1;
  }
  std::printf("rmw_dds_common generated Fast RTPS typesupport: PASS\n");

  rmw_gid_t participant_gid = {};
  participant_gid.implementation_identifier = "velaros_fastrtps";
  for (size_t index = 0; index < RMW_GID_STORAGE_SIZE; ++index) {
    participant_gid.data[index] = static_cast<uint8_t>(index);
  }

  rmw_dds_common::GraphCache graph;
  graph.add_participant(participant_gid, "/velaros");
  const auto graph_update = graph.add_node(
    participant_gid, "k1_node", "/velaros");

  if (graph_update.node_entities_info_seq.size() != 1 ||
      graph_update.node_entities_info_seq[0].node_name != "k1_node" ||
      graph_update.node_entities_info_seq[0].node_namespace != "/velaros") {
    std::printf("rmw_dds_common graph cache update: FAIL\n");
    return 1;
  }

  std::printf("rmw_dds_common graph cache update: PASS\n");
  std::printf("VelaROS rmw_dds_common smoke: PASS\n");
  return 0;
}
