#include <stddef.h>
#include "upb/msg.h"
#include "envoy/api/v2/route/route_components.upb.h"
#include "envoy/api/v2/core/base.upb.h"
#include "envoy/type/matcher/regex.upb.h"
#include "envoy/type/matcher/string.upb.h"
#include "envoy/type/percent.upb.h"
#include "envoy/type/range.upb.h"
#include "envoy/type/tracing/v2/custom_tag.upb.h"
#include "google/protobuf/any.upb.h"
#include "google/protobuf/duration.upb.h"
#include "google/protobuf/struct.upb.h"
#include "google/protobuf/wrappers.upb.h"
#include "envoy/annotations/deprecation.upb.h"
#include "udpa/annotations/migrate.upb.h"
#include "udpa/annotations/status.upb.h"
#include "validate/validate.upb.h"
#include "upb/port_def.inc"
static const upb_msglayout *const envoy_api_v2_route_VirtualHost_submsgs[12] = {
  &envoy_api_v2_core_HeaderValueOption_msginit,
  &envoy_api_v2_route_CorsPolicy_msginit,
  &envoy_api_v2_route_HedgePolicy_msginit,
  &envoy_api_v2_route_RateLimit_msginit,
  &envoy_api_v2_route_RetryPolicy_msginit,
  &envoy_api_v2_route_Route_msginit,
  &envoy_api_v2_route_VirtualCluster_msginit,
  &envoy_api_v2_route_VirtualHost_PerFilterConfigEntry_msginit,
  &envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry_msginit,
  &google_protobuf_Any_msginit,
  &google_protobuf_UInt32Value_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_VirtualHost__fields[19] = {
  {1, UPB_SIZE(12, 16), 0, 0, 9, 1},
  {2, UPB_SIZE(40, 72), 0, 0, 9, 3},
  {3, UPB_SIZE(44, 80), 0, 5, 11, 3},
  {4, UPB_SIZE(0, 0), 0, 0, 14, 1},
  {5, UPB_SIZE(48, 88), 0, 6, 11, 3},
  {6, UPB_SIZE(52, 96), 0, 3, 11, 3},
  {7, UPB_SIZE(56, 104), 0, 0, 11, 3},
  {8, UPB_SIZE(20, 32), 0, 1, 11, 1},
UPB_SIZE(56, 104), 0, 0, 11, 3}, {8, UPB_SIZE(20, 32), 0, 1, 11, 1}, {10, UPB_SIZE(60, 112), 0, 0, 11, 3}, {11, UPB_SIZE(64, 120), 0, 0, 9, 3}, {12, UPB_SIZE(68, 128), 0, 7, 11, 4}, {13, UPB_SIZE(72, 136), 0, 0, 9, 3}, {14, UPB_SIZE(8, 8), 0, 0, 8, 1},
  {14, UPB_SIZE(8, 8), 0, 0, 8, 1},
{14, UPB_SIZE(8, 8), 0, 0, 8, 1}, {15, UPB_SIZE(76, 144), 0, 8, 11, 4}, {16, UPB_SIZE(24, 40), 0, 4, 11, 1},
  {16, UPB_SIZE(24, 40), 0, 4, 11, 1},
  {17, UPB_SIZE(28, 48), 0, 2, 11, 1},
  {18, UPB_SIZE(32, 56), 0, 10, 11, 1},
  {19, UPB_SIZE(9, 9), 0, 0, 8, 1},
  {20, UPB_SIZE(36, 64), 0, 9, 11, 1},
};
const upb_msglayout envoy_api_v2_route_VirtualHost_msginit = {
  &envoy_api_v2_route_VirtualHost_submsgs[0],
  &envoy_api_v2_route_VirtualHost__fields[0],
  UPB_SIZE(80, 160), 19, false,
};
static const upb_msglayout *const envoy_api_v2_route_VirtualHost_PerFilterConfigEntry_submsgs[1] = {
  &google_protobuf_Struct_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_VirtualHost_PerFilterConfigEntry__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_VirtualHost_PerFilterConfigEntry_msginit = {
  &envoy_api_v2_route_VirtualHost_PerFilterConfigEntry_submsgs[0],
  &envoy_api_v2_route_VirtualHost_PerFilterConfigEntry__fields[0],
  UPB_SIZE(16, 32), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry_submsgs[1] = {
  &google_protobuf_Any_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry_msginit = {
  &envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry_submsgs[0],
  &envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry__fields[0],
  UPB_SIZE(16, 32), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_FilterAction_submsgs[1] = {
  &google_protobuf_Any_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_FilterAction__fields[1] = {
  {1, UPB_SIZE(0, 0), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_FilterAction_msginit = {
  &envoy_api_v2_route_FilterAction_submsgs[0],
  &envoy_api_v2_route_FilterAction__fields[0],
  UPB_SIZE(4, 8), 1, false,
};
static const upb_msglayout *const envoy_api_v2_route_Route_submsgs[13] = {
  &envoy_api_v2_core_HeaderValueOption_msginit,
  &envoy_api_v2_core_Metadata_msginit,
  &envoy_api_v2_route_Decorator_msginit,
  &envoy_api_v2_route_DirectResponseAction_msginit,
  &envoy_api_v2_route_FilterAction_msginit,
  &envoy_api_v2_route_RedirectAction_msginit,
  &envoy_api_v2_route_Route_PerFilterConfigEntry_msginit,
  &envoy_api_v2_route_Route_TypedPerFilterConfigEntry_msginit,
  &envoy_api_v2_route_RouteAction_msginit,
  &envoy_api_v2_route_RouteMatch_msginit,
  &envoy_api_v2_route_Tracing_msginit,
  &google_protobuf_UInt32Value_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_Route__fields[16] = {
  {1, UPB_SIZE(8, 16), 0, 9, 11, 1},
  {2, UPB_SIZE(52, 104), UPB_SIZE(-57, -113), 8, 11, 1},
  {3, UPB_SIZE(52, 104), UPB_SIZE(-57, -113), 5, 11, 1},
  {4, UPB_SIZE(12, 24), 0, 1, 11, 1},
  {5, UPB_SIZE(16, 32), 0, 2, 11, 1},
  {7, UPB_SIZE(52, 104), UPB_SIZE(-57, -113), 3, 11, 1},
  {8, UPB_SIZE(28, 56), 0, 6, 11, 4},
  {9, UPB_SIZE(32, 64), 0, 0, 11, 3},
  {10, UPB_SIZE(36, 72), 0, 0, 11, 3},
  {11, UPB_SIZE(40, 80), 0, 0, 9, 3},
  {12, UPB_SIZE(44, 88), 0, 0, 9, 3},
  {13, UPB_SIZE(48, 96), 0, 7, 11, 4},
  {14, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {15, UPB_SIZE(20, 40), 0, 10, 11, 1},
  {16, UPB_SIZE(24, 48), 0, 11, 11, 1},
  {17, UPB_SIZE(52, 104), UPB_SIZE(-57, -113), 4, 11, 1},
};
const upb_msglayout envoy_api_v2_route_Route_msginit = {
  &envoy_api_v2_route_Route_submsgs[0],
  &envoy_api_v2_route_Route__fields[0],
  UPB_SIZE(64, 128), 16, false,
};
static const upb_msglayout *const envoy_api_v2_route_Route_PerFilterConfigEntry_submsgs[1] = {
  &google_protobuf_Struct_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_Route_PerFilterConfigEntry__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_Route_PerFilterConfigEntry_msginit = {
  &envoy_api_v2_route_Route_PerFilterConfigEntry_submsgs[0],
  &envoy_api_v2_route_Route_PerFilterConfigEntry__fields[0],
  UPB_SIZE(16, 32), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_Route_TypedPerFilterConfigEntry_submsgs[1] = {
  &google_protobuf_Any_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_Route_TypedPerFilterConfigEntry__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_Route_TypedPerFilterConfigEntry_msginit = {
  &envoy_api_v2_route_Route_TypedPerFilterConfigEntry_submsgs[0],
  &envoy_api_v2_route_Route_TypedPerFilterConfigEntry__fields[0],
  UPB_SIZE(16, 32), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_WeightedCluster_submsgs[2] = {
  &envoy_api_v2_route_WeightedCluster_ClusterWeight_msginit,
  &google_protobuf_UInt32Value_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_WeightedCluster__fields[3] = {
  {1, UPB_SIZE(12, 24), 0, 0, 11, 3},
  {2, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {3, UPB_SIZE(8, 16), 0, 1, 11, 1},
};
const upb_msglayout envoy_api_v2_route_WeightedCluster_msginit = {
  &envoy_api_v2_route_WeightedCluster_submsgs[0],
  &envoy_api_v2_route_WeightedCluster__fields[0],
  UPB_SIZE(16, 32), 3, false,
};
static const upb_msglayout *const envoy_api_v2_route_WeightedCluster_ClusterWeight_submsgs[6] = {
  &envoy_api_v2_core_HeaderValueOption_msginit,
  &envoy_api_v2_core_Metadata_msginit,
  &envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry_msginit,
  &envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry_msginit,
  &google_protobuf_UInt32Value_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_WeightedCluster_ClusterWeight__fields[9] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 4, 11, 1},
  {3, UPB_SIZE(12, 24), 0, 1, 11, 1},
  {4, UPB_SIZE(16, 32), 0, 0, 11, 3},
  {5, UPB_SIZE(20, 40), 0, 0, 11, 3},
  {6, UPB_SIZE(24, 48), 0, 0, 9, 3},
  {8, UPB_SIZE(28, 56), 0, 2, 11, 4},
  {9, UPB_SIZE(32, 64), 0, 0, 9, 3},
  {10, UPB_SIZE(36, 72), 0, 3, 11, 4},
};
const upb_msglayout envoy_api_v2_route_WeightedCluster_ClusterWeight_msginit = {
  &envoy_api_v2_route_WeightedCluster_ClusterWeight_submsgs[0],
  &envoy_api_v2_route_WeightedCluster_ClusterWeight__fields[0],
  UPB_SIZE(40, 80), 9, false,
};
static const upb_msglayout *const envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry_submsgs[1] = {
  &google_protobuf_Struct_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry_msginit = {
  &envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry_submsgs[0],
  &envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry__fields[0],
  UPB_SIZE(16, 32), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry_submsgs[1] = {
  &google_protobuf_Any_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry_msginit = {
  &envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry_submsgs[0],
  &envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry__fields[0],
  UPB_SIZE(16, 32), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_RouteMatch_submsgs[7] = {
  &envoy_api_v2_core_RuntimeFractionalPercent_msginit,
  &envoy_api_v2_route_HeaderMatcher_msginit,
  &envoy_api_v2_route_QueryParameterMatcher_msginit,
  &envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_msginit,
  &envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_msginit,
  &envoy_type_matcher_RegexMatcher_msginit,
  &google_protobuf_BoolValue_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RouteMatch__fields[10] = {
  {1, UPB_SIZE(24, 48), UPB_SIZE(-33, -65), 0, 9, 1},
  {2, UPB_SIZE(24, 48), UPB_SIZE(-33, -65), 0, 9, 1},
  {3, UPB_SIZE(24, 48), UPB_SIZE(-33, -65), 0, 9, 1},
  {4, UPB_SIZE(0, 0), 0, 6, 11, 1},
  {6, UPB_SIZE(16, 32), 0, 1, 11, 3},
  {7, UPB_SIZE(20, 40), 0, 2, 11, 3},
  {8, UPB_SIZE(4, 8), 0, 3, 11, 1},
  {9, UPB_SIZE(8, 16), 0, 0, 11, 1},
  {10, UPB_SIZE(24, 48), UPB_SIZE(-33, -65), 5, 11, 1},
  {11, UPB_SIZE(12, 24), 0, 4, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RouteMatch_msginit = {
  &envoy_api_v2_route_RouteMatch_submsgs[0],
  &envoy_api_v2_route_RouteMatch__fields[0],
  UPB_SIZE(40, 80), 10, false,
};
const upb_msglayout envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_msginit = {
  NULL,
  NULL,
  UPB_SIZE(0, 0), 0, false,
};
static const upb_msglayout *const envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_submsgs[2] = {
  &google_protobuf_BoolValue_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RouteMatch_TlsContextMatchOptions__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 11, 1},
  {2, UPB_SIZE(4, 8), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_msginit = {
  &envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_submsgs[0],
  &envoy_api_v2_route_RouteMatch_TlsContextMatchOptions__fields[0],
  UPB_SIZE(8, 16), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_CorsPolicy_submsgs[5] = {
  &envoy_api_v2_core_RuntimeFractionalPercent_msginit,
  &envoy_type_matcher_StringMatcher_msginit,
  &google_protobuf_BoolValue_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_CorsPolicy__fields[11] = {
  {1, UPB_SIZE(40, 80), 0, 0, 9, 3},
  {2, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {3, UPB_SIZE(8, 16), 0, 0, 9, 1},
  {4, UPB_SIZE(16, 32), 0, 0, 9, 1},
  {5, UPB_SIZE(24, 48), 0, 0, 9, 1},
  {6, UPB_SIZE(32, 64), 0, 2, 11, 1},
  {7, UPB_SIZE(52, 104), UPB_SIZE(-57, -113), 2, 11, 1},
  {8, UPB_SIZE(44, 88), 0, 0, 9, 3},
  {9, UPB_SIZE(52, 104), UPB_SIZE(-57, -113), 0, 11, 1},
  {10, UPB_SIZE(36, 72), 0, 0, 11, 1},
  {11, UPB_SIZE(48, 96), 0, 1, 11, 3},
};
const upb_msglayout envoy_api_v2_route_CorsPolicy_msginit = {
  &envoy_api_v2_route_CorsPolicy_submsgs[0],
  &envoy_api_v2_route_CorsPolicy__fields[0],
  UPB_SIZE(64, 128), 11, false,
};
static const upb_msglayout *const envoy_api_v2_route_RouteAction_submsgs[19] = {
  &envoy_api_v2_core_Metadata_msginit,
  &envoy_api_v2_route_CorsPolicy_msginit,
  &envoy_api_v2_route_HedgePolicy_msginit,
  &envoy_api_v2_route_RateLimit_msginit,
  &envoy_api_v2_route_RetryPolicy_msginit,
  &envoy_api_v2_route_RouteAction_HashPolicy_msginit,
  &envoy_api_v2_route_RouteAction_RequestMirrorPolicy_msginit,
  &envoy_api_v2_route_RouteAction_UpgradeConfig_msginit,
  &envoy_api_v2_route_WeightedCluster_msginit,
  &envoy_type_matcher_RegexMatchAndSubstitute_msginit,
  &google_protobuf_Any_msginit,
  &google_protobuf_BoolValue_msginit,
  &google_protobuf_Duration_msginit,
  &google_protobuf_UInt32Value_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RouteAction__fields[27] = {
  {1, UPB_SIZE(100, 176), UPB_SIZE(-109, -193), 0, 9, 1},
  {2, UPB_SIZE(100, 176), UPB_SIZE(-109, -193), 0, 9, 1},
  {3, UPB_SIZE(100, 176), UPB_SIZE(-109, -193), 8, 11, 1},
  {4, UPB_SIZE(32, 40), 0, 0, 11, 1},
  {5, UPB_SIZE(24, 24), 0, 0, 9, 1},
  {6, UPB_SIZE(112, 200), UPB_SIZE(-121, -217), 0, 9, 1},
  {7, UPB_SIZE(112, 200), UPB_SIZE(-121, -217), 11, 11, 1},
  {8, UPB_SIZE(36, 48), 0, 12, 11, 1},
  {9, UPB_SIZE(40, 56), 0, 4, 11, 1},
  {10, UPB_SIZE(44, 64), 0, 6, 11, 1},
  {11, UPB_SIZE(0, 0), 0, 0, 14, 1},
  {13, UPB_SIZE(84, 144), 0, 3, 11, 3},
  {14, UPB_SIZE(48, 72), 0, 11, 11, 1},
  {15, UPB_SIZE(88, 152), 0, 5, 11, 3},
  {17, UPB_SIZE(52, 80), 0, 1, 11, 1},
  {20, UPB_SIZE(8, 8), 0, 0, 14, 1},
  {23, UPB_SIZE(56, 88), 0, 12, 11, 1},
  {24, UPB_SIZE(60, 96), 0, 12, 11, 1},
  {25, UPB_SIZE(92, 160), 0, 7, 11, 3},
  {26, UPB_SIZE(16, 16), 0, 0, 14, 1},
  {27, UPB_SIZE(64, 104), 0, 2, 11, 1},
  {28, UPB_SIZE(68, 112), 0, 12, 11, 1},
  {29, UPB_SIZE(112, 200), UPB_SIZE(-121, -217), 0, 9, 1},
  {30, UPB_SIZE(96, 168), 0, 6, 11, 3},
  {31, UPB_SIZE(72, 120), 0, 13, 11, 1},
  {32, UPB_SIZE(76, 128), 0, 9, 11, 1},
  {33, UPB_SIZE(80, 136), 0, 10, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RouteAction_msginit = {
  &envoy_api_v2_route_RouteAction_submsgs[0],
  &envoy_api_v2_route_RouteAction__fields[0],
  UPB_SIZE(128, 224), 27, false,
};
static const upb_msglayout *const envoy_api_v2_route_RouteAction_RequestMirrorPolicy_submsgs[2] = {
  &envoy_api_v2_core_RuntimeFractionalPercent_msginit,
  &google_protobuf_BoolValue_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RouteAction_RequestMirrorPolicy__fields[4] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 9, 1},
  {3, UPB_SIZE(16, 32), 0, 0, 11, 1},
  {4, UPB_SIZE(20, 40), 0, 1, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RouteAction_RequestMirrorPolicy_msginit = {
  &envoy_api_v2_route_RouteAction_RequestMirrorPolicy_submsgs[0],
  &envoy_api_v2_route_RouteAction_RequestMirrorPolicy__fields[0],
  UPB_SIZE(24, 48), 4, false,
};
static const upb_msglayout *const envoy_api_v2_route_RouteAction_HashPolicy_submsgs[5] = {
  &envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_msginit,
  &envoy_api_v2_route_RouteAction_HashPolicy_Cookie_msginit,
  &envoy_api_v2_route_RouteAction_HashPolicy_FilterState_msginit,
  &envoy_api_v2_route_RouteAction_HashPolicy_Header_msginit,
  &envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RouteAction_HashPolicy__fields[6] = {
  {1, UPB_SIZE(4, 8), UPB_SIZE(-9, -17), 3, 11, 1},
  {2, UPB_SIZE(4, 8), UPB_SIZE(-9, -17), 1, 11, 1},
  {3, UPB_SIZE(4, 8), UPB_SIZE(-9, -17), 0, 11, 1},
  {4, UPB_SIZE(0, 0), 0, 0, 8, 1},
  {5, UPB_SIZE(4, 8), UPB_SIZE(-9, -17), 4, 11, 1},
  {6, UPB_SIZE(4, 8), UPB_SIZE(-9, -17), 2, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_msginit = {
  &envoy_api_v2_route_RouteAction_HashPolicy_submsgs[0],
  &envoy_api_v2_route_RouteAction_HashPolicy__fields[0],
  UPB_SIZE(12, 24), 6, false,
};
static const upb_msglayout_field envoy_api_v2_route_RouteAction_HashPolicy_Header__fields[1] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
};
const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_Header_msginit = {
  NULL,
  &envoy_api_v2_route_RouteAction_HashPolicy_Header__fields[0],
  UPB_SIZE(8, 16), 1, false,
};
static const upb_msglayout *const envoy_api_v2_route_RouteAction_HashPolicy_Cookie_submsgs[1] = {
  &google_protobuf_Duration_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RouteAction_HashPolicy_Cookie__fields[3] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(16, 32), 0, 0, 11, 1},
  {3, UPB_SIZE(8, 16), 0, 0, 9, 1},
};
const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_Cookie_msginit = {
  &envoy_api_v2_route_RouteAction_HashPolicy_Cookie_submsgs[0],
  &envoy_api_v2_route_RouteAction_HashPolicy_Cookie__fields[0],
  UPB_SIZE(24, 48), 3, false,
};
static const upb_msglayout_field envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties__fields[1] = {
  {1, UPB_SIZE(0, 0), 0, 0, 8, 1},
};
const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_msginit = {
  NULL,
  &envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties__fields[0],
  UPB_SIZE(1, 1), 1, false,
};
static const upb_msglayout_field envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter__fields[1] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
};
const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_msginit = {
  NULL,
  &envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter__fields[0],
  UPB_SIZE(8, 16), 1, false,
};
static const upb_msglayout_field envoy_api_v2_route_RouteAction_HashPolicy_FilterState__fields[1] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
};
const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_FilterState_msginit = {
  NULL,
  &envoy_api_v2_route_RouteAction_HashPolicy_FilterState__fields[0],
  UPB_SIZE(8, 16), 1, false,
};
static const upb_msglayout *const envoy_api_v2_route_RouteAction_UpgradeConfig_submsgs[1] = {
  &google_protobuf_BoolValue_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RouteAction_UpgradeConfig__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RouteAction_UpgradeConfig_msginit = {
  &envoy_api_v2_route_RouteAction_UpgradeConfig_submsgs[0],
  &envoy_api_v2_route_RouteAction_UpgradeConfig__fields[0],
  UPB_SIZE(16, 32), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_RetryPolicy_submsgs[7] = {
  &envoy_api_v2_route_HeaderMatcher_msginit,
  &envoy_api_v2_route_RetryPolicy_RetryBackOff_msginit,
  &envoy_api_v2_route_RetryPolicy_RetryHostPredicate_msginit,
  &envoy_api_v2_route_RetryPolicy_RetryPriority_msginit,
  &google_protobuf_Duration_msginit,
  &google_protobuf_UInt32Value_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RetryPolicy__fields[10] = {
  {1, UPB_SIZE(8, 8), 0, 0, 9, 1},
  {2, UPB_SIZE(16, 24), 0, 5, 11, 1},
  {3, UPB_SIZE(20, 32), 0, 4, 11, 1},
  {4, UPB_SIZE(24, 40), 0, 3, 11, 1},
  {5, UPB_SIZE(32, 56), 0, 2, 11, 3},
  {6, UPB_SIZE(0, 0), 0, 0, 3, 1},
  {7, UPB_SIZE(36, 64), 0, 0, 13, 3},
  {8, UPB_SIZE(28, 48), 0, 1, 11, 1},
  {9, UPB_SIZE(40, 72), 0, 0, 11, 3},
  {10, UPB_SIZE(44, 80), 0, 0, 11, 3},
};
const upb_msglayout envoy_api_v2_route_RetryPolicy_msginit = {
  &envoy_api_v2_route_RetryPolicy_submsgs[0],
  &envoy_api_v2_route_RetryPolicy__fields[0],
  UPB_SIZE(48, 96), 10, false,
};
static const upb_msglayout *const envoy_api_v2_route_RetryPolicy_RetryPriority_submsgs[2] = {
  &google_protobuf_Any_msginit,
  &google_protobuf_Struct_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RetryPolicy_RetryPriority__fields[3] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), UPB_SIZE(-13, -25), 1, 11, 1},
  {3, UPB_SIZE(8, 16), UPB_SIZE(-13, -25), 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RetryPolicy_RetryPriority_msginit = {
  &envoy_api_v2_route_RetryPolicy_RetryPriority_submsgs[0],
  &envoy_api_v2_route_RetryPolicy_RetryPriority__fields[0],
  UPB_SIZE(16, 32), 3, false,
};
static const upb_msglayout *const envoy_api_v2_route_RetryPolicy_RetryHostPredicate_submsgs[2] = {
  &google_protobuf_Any_msginit,
  &google_protobuf_Struct_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RetryPolicy_RetryHostPredicate__fields[3] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), UPB_SIZE(-13, -25), 1, 11, 1},
  {3, UPB_SIZE(8, 16), UPB_SIZE(-13, -25), 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RetryPolicy_RetryHostPredicate_msginit = {
  &envoy_api_v2_route_RetryPolicy_RetryHostPredicate_submsgs[0],
  &envoy_api_v2_route_RetryPolicy_RetryHostPredicate__fields[0],
  UPB_SIZE(16, 32), 3, false,
};
static const upb_msglayout *const envoy_api_v2_route_RetryPolicy_RetryBackOff_submsgs[2] = {
  &google_protobuf_Duration_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RetryPolicy_RetryBackOff__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 11, 1},
  {2, UPB_SIZE(4, 8), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RetryPolicy_RetryBackOff_msginit = {
  &envoy_api_v2_route_RetryPolicy_RetryBackOff_submsgs[0],
  &envoy_api_v2_route_RetryPolicy_RetryBackOff__fields[0],
  UPB_SIZE(8, 16), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_HedgePolicy_submsgs[2] = {
  &envoy_type_FractionalPercent_msginit,
  &google_protobuf_UInt32Value_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_HedgePolicy__fields[3] = {
  {1, UPB_SIZE(4, 8), 0, 1, 11, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 11, 1},
  {3, UPB_SIZE(0, 0), 0, 0, 8, 1},
};
const upb_msglayout envoy_api_v2_route_HedgePolicy_msginit = {
  &envoy_api_v2_route_HedgePolicy_submsgs[0],
  &envoy_api_v2_route_HedgePolicy__fields[0],
  UPB_SIZE(12, 24), 3, false,
};
static const upb_msglayout_field envoy_api_v2_route_RedirectAction__fields[8] = {
  {1, UPB_SIZE(16, 16), 0, 0, 9, 1},
  {2, UPB_SIZE(24, 32), UPB_SIZE(-33, -49), 0, 9, 1},
  {3, UPB_SIZE(0, 0), 0, 0, 14, 1},
  {4, UPB_SIZE(36, 56), UPB_SIZE(-45, -73), 0, 8, 1},
  {5, UPB_SIZE(24, 32), UPB_SIZE(-33, -49), 0, 9, 1},
  {6, UPB_SIZE(12, 12), 0, 0, 8, 1},
  {7, UPB_SIZE(36, 56), UPB_SIZE(-45, -73), 0, 9, 1},
  {8, UPB_SIZE(8, 8), 0, 0, 13, 1},
};
const upb_msglayout envoy_api_v2_route_RedirectAction_msginit = {
  NULL,
  &envoy_api_v2_route_RedirectAction__fields[0],
  UPB_SIZE(48, 80), 8, false,
};
static const upb_msglayout *const envoy_api_v2_route_DirectResponseAction_submsgs[1] = {
  &envoy_api_v2_core_DataSource_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_DirectResponseAction__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 13, 1},
  {2, UPB_SIZE(4, 8), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_DirectResponseAction_msginit = {
  &envoy_api_v2_route_DirectResponseAction_submsgs[0],
  &envoy_api_v2_route_DirectResponseAction__fields[0],
  UPB_SIZE(8, 16), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_Decorator_submsgs[1] = {
  &google_protobuf_BoolValue_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_Decorator__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 11, 1},
};
const upb_msglayout envoy_api_v2_route_Decorator_msginit = {
  &envoy_api_v2_route_Decorator_submsgs[0],
  &envoy_api_v2_route_Decorator__fields[0],
  UPB_SIZE(16, 32), 2, false,
};
static const upb_msglayout *const envoy_api_v2_route_Tracing_submsgs[4] = {
  &envoy_type_FractionalPercent_msginit,
  &envoy_type_tracing_v2_CustomTag_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_Tracing__fields[4] = {
  {1, UPB_SIZE(0, 0), 0, 0, 11, 1},
  {2, UPB_SIZE(4, 8), 0, 0, 11, 1},
  {3, UPB_SIZE(8, 16), 0, 0, 11, 1},
  {4, UPB_SIZE(12, 24), 0, 1, 11, 3},
};
const upb_msglayout envoy_api_v2_route_Tracing_msginit = {
  &envoy_api_v2_route_Tracing_submsgs[0],
  &envoy_api_v2_route_Tracing__fields[0],
  UPB_SIZE(16, 32), 4, false,
};
static const upb_msglayout *const envoy_api_v2_route_VirtualCluster_submsgs[1] = {
  &envoy_api_v2_route_HeaderMatcher_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_VirtualCluster__fields[4] = {
  {1, UPB_SIZE(8, 8), 0, 0, 9, 1},
  {2, UPB_SIZE(16, 24), 0, 0, 9, 1},
  {3, UPB_SIZE(0, 0), 0, 0, 14, 1},
  {4, UPB_SIZE(24, 40), 0, 0, 11, 3},
};
const upb_msglayout envoy_api_v2_route_VirtualCluster_msginit = {
  &envoy_api_v2_route_VirtualCluster_submsgs[0],
  &envoy_api_v2_route_VirtualCluster__fields[0],
  UPB_SIZE(32, 48), 4, false,
};
static const upb_msglayout *const envoy_api_v2_route_RateLimit_submsgs[2] = {
  &envoy_api_v2_route_RateLimit_Action_msginit,
  &google_protobuf_UInt32Value_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RateLimit__fields[3] = {
  {1, UPB_SIZE(8, 16), 0, 1, 11, 1},
  {2, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {3, UPB_SIZE(12, 24), 0, 0, 11, 3},
};
const upb_msglayout envoy_api_v2_route_RateLimit_msginit = {
  &envoy_api_v2_route_RateLimit_submsgs[0],
  &envoy_api_v2_route_RateLimit__fields[0],
  UPB_SIZE(16, 32), 3, false,
};
static const upb_msglayout *const envoy_api_v2_route_RateLimit_Action_submsgs[6] = {
  &envoy_api_v2_route_RateLimit_Action_DestinationCluster_msginit,
  &envoy_api_v2_route_RateLimit_Action_GenericKey_msginit,
  &envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_msginit,
  &envoy_api_v2_route_RateLimit_Action_RemoteAddress_msginit,
  &envoy_api_v2_route_RateLimit_Action_RequestHeaders_msginit,
  &envoy_api_v2_route_RateLimit_Action_SourceCluster_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RateLimit_Action__fields[6] = {
  {1, UPB_SIZE(0, 0), UPB_SIZE(-5, -9), 5, 11, 1},
  {2, UPB_SIZE(0, 0), UPB_SIZE(-5, -9), 0, 11, 1},
  {3, UPB_SIZE(0, 0), UPB_SIZE(-5, -9), 4, 11, 1},
  {4, UPB_SIZE(0, 0), UPB_SIZE(-5, -9), 3, 11, 1},
  {5, UPB_SIZE(0, 0), UPB_SIZE(-5, -9), 1, 11, 1},
  {6, UPB_SIZE(0, 0), UPB_SIZE(-5, -9), 2, 11, 1},
};
const upb_msglayout envoy_api_v2_route_RateLimit_Action_msginit = {
  &envoy_api_v2_route_RateLimit_Action_submsgs[0],
  &envoy_api_v2_route_RateLimit_Action__fields[0],
  UPB_SIZE(8, 16), 6, false,
};
const upb_msglayout envoy_api_v2_route_RateLimit_Action_SourceCluster_msginit = {
  NULL,
  NULL,
  UPB_SIZE(0, 0), 0, false,
};
const upb_msglayout envoy_api_v2_route_RateLimit_Action_DestinationCluster_msginit = {
  NULL,
  NULL,
  UPB_SIZE(0, 0), 0, false,
};
static const upb_msglayout_field envoy_api_v2_route_RateLimit_Action_RequestHeaders__fields[2] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 0, 9, 1},
};
const upb_msglayout envoy_api_v2_route_RateLimit_Action_RequestHeaders_msginit = {
  NULL,
  &envoy_api_v2_route_RateLimit_Action_RequestHeaders__fields[0],
  UPB_SIZE(16, 32), 2, false,
};
const upb_msglayout envoy_api_v2_route_RateLimit_Action_RemoteAddress_msginit = {
  NULL,
  NULL,
  UPB_SIZE(0, 0), 0, false,
};
static const upb_msglayout_field envoy_api_v2_route_RateLimit_Action_GenericKey__fields[1] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
};
const upb_msglayout envoy_api_v2_route_RateLimit_Action_GenericKey_msginit = {
  NULL,
  &envoy_api_v2_route_RateLimit_Action_GenericKey__fields[0],
  UPB_SIZE(8, 16), 1, false,
};
static const upb_msglayout *const envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_submsgs[2] = {
  &envoy_api_v2_route_HeaderMatcher_msginit,
  &google_protobuf_BoolValue_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_RateLimit_Action_HeaderValueMatch__fields[3] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {2, UPB_SIZE(8, 16), 0, 1, 11, 1},
  {3, UPB_SIZE(12, 24), 0, 0, 11, 3},
};
const upb_msglayout envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_msginit = {
  &envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_submsgs[0],
  &envoy_api_v2_route_RateLimit_Action_HeaderValueMatch__fields[0],
  UPB_SIZE(16, 32), 3, false,
};
static const upb_msglayout *const envoy_api_v2_route_HeaderMatcher_submsgs[2] = {
  &envoy_type_Int64Range_msginit,
  &envoy_type_matcher_RegexMatcher_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_HeaderMatcher__fields[9] = {
  {1, UPB_SIZE(4, 8), 0, 0, 9, 1},
  {4, UPB_SIZE(12, 24), UPB_SIZE(-21, -41), 0, 9, 1},
  {5, UPB_SIZE(12, 24), UPB_SIZE(-21, -41), 0, 9, 1},
  {6, UPB_SIZE(12, 24), UPB_SIZE(-21, -41), 0, 11, 1},
  {7, UPB_SIZE(12, 24), UPB_SIZE(-21, -41), 0, 8, 1},
  {8, UPB_SIZE(0, 0), 0, 0, 8, 1},
  {9, UPB_SIZE(12, 24), UPB_SIZE(-21, -41), 0, 9, 1},
  {10, UPB_SIZE(12, 24), UPB_SIZE(-21, -41), 0, 9, 1},
  {11, UPB_SIZE(12, 24), UPB_SIZE(-21, -41), 1, 11, 1},
};
const upb_msglayout envoy_api_v2_route_HeaderMatcher_msginit = {
  &envoy_api_v2_route_HeaderMatcher_submsgs[0],
  &envoy_api_v2_route_HeaderMatcher__fields[0],
  UPB_SIZE(24, 48), 9, false,
};
static const upb_msglayout *const envoy_api_v2_route_QueryParameterMatcher_submsgs[2] = {
  &envoy_type_matcher_StringMatcher_msginit,
  &google_protobuf_BoolValue_msginit,
};
static const upb_msglayout_field envoy_api_v2_route_QueryParameterMatcher__fields[5] = {
  {1, UPB_SIZE(0, 0), 0, 0, 9, 1},
  {3, UPB_SIZE(8, 16), 0, 0, 9, 1},
  {4, UPB_SIZE(16, 32), 0, 1, 11, 1},
  {5, UPB_SIZE(20, 40), UPB_SIZE(-25, -49), 0, 11, 1},
  {6, UPB_SIZE(20, 40), UPB_SIZE(-25, -49), 0, 8, 1},
};
const upb_msglayout envoy_api_v2_route_QueryParameterMatcher_msginit = {
  &envoy_api_v2_route_QueryParameterMatcher_submsgs[0],
  &envoy_api_v2_route_QueryParameterMatcher__fields[0],
  UPB_SIZE(32, 64), 5, false,
};
#include "upb/port_undef.inc"
