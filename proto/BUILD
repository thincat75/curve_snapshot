#
#  Copyright (c) 2020 NetEase Inc.
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#

proto_library(
    name = "common_proto",
    srcs = ["common.proto"],
)

cc_proto_library(
    name = "common_cc_proto",
    deps = [":common_proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "nameserver2_cc_proto",
    visibility = ["//visibility:public"],
    deps = [":nameserver2_proto"],
)

proto_library(
    name = "nameserver2_proto",
    srcs = ["nameserver2.proto"],
    deps = [":common_proto"],
)

cc_proto_library(
    name = "topology_cc_proto",
    visibility = ["//visibility:public"],
    deps = [":topology_proto"],
)

proto_library(
    name = "topology_proto",
    srcs = ["topology.proto"],
    deps = [":common_proto"],
)

cc_proto_library(
    name = "heartbeat_cc_proto",
    visibility = ["//visibility:public"],
    deps = [":heartbeat_proto"],
)

proto_library(
    name = "heartbeat_proto",
    visibility = ["//visibility:public"],
    srcs = ["heartbeat.proto"],
    deps = [":common_proto", ":scan_proto"],
)

cc_proto_library(
    name = "chunkserver-cc-protos",
    visibility = ["//visibility:public"],
    deps = [":chunkserver-protos"],
)

proto_library(
    name = "chunkserver-protos",
    srcs = glob([
        "chunk.proto",
        "cli.proto",
        "cli2.proto",
        "copyset.proto",
        "integrity.proto",
        "configuration.proto",
        "chunkserver.proto",
        "curve_storage.proto",
    ]),
    deps = [":common_proto", ":scan_proto", ":heartbeat_proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "snapshotcloneserver_cc_proto",
    visibility = ["//visibility:public"],
    deps = [":snapshotcloneserver_proto"],
)

proto_library(
    name = "snapshotcloneserver_proto",
    srcs = ["snapshotcloneserver.proto"],
)

cc_proto_library(
    name = "schedule_cc_proto",
    visibility = ["//visibility:public"],
    deps = [":schedule_proto"],
)

proto_library(
    name = "schedule_proto",
    srcs = ["schedule.proto"],
)

cc_proto_library(
    name = "scan_cc_proto",
    visibility = ["//visibility:public"],
    deps = [":scan_proto"],
)

proto_library(
    name = "scan_proto",
    srcs = ["scan.proto"],
)
