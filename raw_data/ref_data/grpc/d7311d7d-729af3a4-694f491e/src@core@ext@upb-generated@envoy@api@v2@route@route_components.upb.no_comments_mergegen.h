#ifndef ENVOY_API_V2_ROUTE_ROUTE_COMPONENTS_PROTO_UPB_H_
#define ENVOY_API_V2_ROUTE_ROUTE_COMPONENTS_PROTO_UPB_H_ 
#include "upb/msg.h"
#include "upb/decode.h"
#include "upb/encode.h"
#include "upb/port_def.inc"
#ifdef __cplusplus
extern "C" {
#endif
struct envoy_api_v2_route_VirtualHost;
struct envoy_api_v2_route_VirtualHost_PerFilterConfigEntry;
struct envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry;
struct envoy_api_v2_route_FilterAction;
struct envoy_api_v2_route_Route;
struct envoy_api_v2_route_Route_PerFilterConfigEntry;
struct envoy_api_v2_route_Route_TypedPerFilterConfigEntry;
struct envoy_api_v2_route_WeightedCluster;
struct envoy_api_v2_route_WeightedCluster_ClusterWeight;
struct envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry;
struct envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry;
struct envoy_api_v2_route_RouteMatch;
struct envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions;
struct envoy_api_v2_route_RouteMatch_TlsContextMatchOptions;
struct envoy_api_v2_route_CorsPolicy;
struct envoy_api_v2_route_RouteAction;
struct envoy_api_v2_route_RouteAction_RequestMirrorPolicy;
struct envoy_api_v2_route_RouteAction_HashPolicy;
struct envoy_api_v2_route_RouteAction_HashPolicy_Header;
struct envoy_api_v2_route_RouteAction_HashPolicy_Cookie;
struct envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties;
struct envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter;
struct envoy_api_v2_route_RouteAction_HashPolicy_FilterState;
struct envoy_api_v2_route_RouteAction_UpgradeConfig;
struct envoy_api_v2_route_RetryPolicy;
struct envoy_api_v2_route_RetryPolicy_RetryPriority;
struct envoy_api_v2_route_RetryPolicy_RetryHostPredicate;
struct envoy_api_v2_route_RetryPolicy_RetryBackOff;
struct envoy_api_v2_route_HedgePolicy;
struct envoy_api_v2_route_RedirectAction;
struct envoy_api_v2_route_DirectResponseAction;
struct envoy_api_v2_route_Decorator;
struct envoy_api_v2_route_Tracing;
struct envoy_api_v2_route_VirtualCluster;
struct envoy_api_v2_route_RateLimit;
struct envoy_api_v2_route_RateLimit_Action;
struct envoy_api_v2_route_RateLimit_Action_SourceCluster;
struct envoy_api_v2_route_RateLimit_Action_DestinationCluster;
struct envoy_api_v2_route_RateLimit_Action_RequestHeaders;
struct envoy_api_v2_route_RateLimit_Action_RemoteAddress;
struct envoy_api_v2_route_RateLimit_Action_GenericKey;
struct envoy_api_v2_route_RateLimit_Action_HeaderValueMatch;
struct envoy_api_v2_route_HeaderMatcher;
struct envoy_api_v2_route_QueryParameterMatcher;
typedef struct envoy_api_v2_route_VirtualHost envoy_api_v2_route_VirtualHost;
typedef struct envoy_api_v2_route_VirtualHost_PerFilterConfigEntry envoy_api_v2_route_VirtualHost_PerFilterConfigEntry;
typedef struct envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry;
typedef struct envoy_api_v2_route_FilterAction envoy_api_v2_route_FilterAction;
typedef struct envoy_api_v2_route_Route envoy_api_v2_route_Route;
typedef struct envoy_api_v2_route_Route_PerFilterConfigEntry envoy_api_v2_route_Route_PerFilterConfigEntry;
typedef struct envoy_api_v2_route_Route_TypedPerFilterConfigEntry envoy_api_v2_route_Route_TypedPerFilterConfigEntry;
typedef struct envoy_api_v2_route_WeightedCluster envoy_api_v2_route_WeightedCluster;
typedef struct envoy_api_v2_route_WeightedCluster_ClusterWeight envoy_api_v2_route_WeightedCluster_ClusterWeight;
typedef struct envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry;
typedef struct envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry;
typedef struct envoy_api_v2_route_RouteMatch envoy_api_v2_route_RouteMatch;
typedef struct envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions;
typedef struct envoy_api_v2_route_RouteMatch_TlsContextMatchOptions envoy_api_v2_route_RouteMatch_TlsContextMatchOptions;
typedef struct envoy_api_v2_route_CorsPolicy envoy_api_v2_route_CorsPolicy;
typedef struct envoy_api_v2_route_RouteAction envoy_api_v2_route_RouteAction;
typedef struct envoy_api_v2_route_RouteAction_RequestMirrorPolicy envoy_api_v2_route_RouteAction_RequestMirrorPolicy;
typedef struct envoy_api_v2_route_RouteAction_HashPolicy envoy_api_v2_route_RouteAction_HashPolicy;
typedef struct envoy_api_v2_route_RouteAction_HashPolicy_Header envoy_api_v2_route_RouteAction_HashPolicy_Header;
typedef struct envoy_api_v2_route_RouteAction_HashPolicy_Cookie envoy_api_v2_route_RouteAction_HashPolicy_Cookie;
typedef struct envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties;
typedef struct envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter;
typedef struct envoy_api_v2_route_RouteAction_HashPolicy_FilterState envoy_api_v2_route_RouteAction_HashPolicy_FilterState;
typedef struct envoy_api_v2_route_RouteAction_UpgradeConfig envoy_api_v2_route_RouteAction_UpgradeConfig;
typedef struct envoy_api_v2_route_RetryPolicy envoy_api_v2_route_RetryPolicy;
typedef struct envoy_api_v2_route_RetryPolicy_RetryPriority envoy_api_v2_route_RetryPolicy_RetryPriority;
typedef struct envoy_api_v2_route_RetryPolicy_RetryHostPredicate envoy_api_v2_route_RetryPolicy_RetryHostPredicate;
typedef struct envoy_api_v2_route_RetryPolicy_RetryBackOff envoy_api_v2_route_RetryPolicy_RetryBackOff;
typedef struct envoy_api_v2_route_HedgePolicy envoy_api_v2_route_HedgePolicy;
typedef struct envoy_api_v2_route_RedirectAction envoy_api_v2_route_RedirectAction;
typedef struct envoy_api_v2_route_DirectResponseAction envoy_api_v2_route_DirectResponseAction;
typedef struct envoy_api_v2_route_Decorator envoy_api_v2_route_Decorator;
typedef struct envoy_api_v2_route_Tracing envoy_api_v2_route_Tracing;
typedef struct envoy_api_v2_route_VirtualCluster envoy_api_v2_route_VirtualCluster;
typedef struct envoy_api_v2_route_RateLimit envoy_api_v2_route_RateLimit;
typedef struct envoy_api_v2_route_RateLimit_Action envoy_api_v2_route_RateLimit_Action;
typedef struct envoy_api_v2_route_RateLimit_Action_SourceCluster envoy_api_v2_route_RateLimit_Action_SourceCluster;
typedef struct envoy_api_v2_route_RateLimit_Action_DestinationCluster envoy_api_v2_route_RateLimit_Action_DestinationCluster;
typedef struct envoy_api_v2_route_RateLimit_Action_RequestHeaders envoy_api_v2_route_RateLimit_Action_RequestHeaders;
typedef struct envoy_api_v2_route_RateLimit_Action_RemoteAddress envoy_api_v2_route_RateLimit_Action_RemoteAddress;
typedef struct envoy_api_v2_route_RateLimit_Action_GenericKey envoy_api_v2_route_RateLimit_Action_GenericKey;
typedef struct envoy_api_v2_route_RateLimit_Action_HeaderValueMatch envoy_api_v2_route_RateLimit_Action_HeaderValueMatch;
typedef struct envoy_api_v2_route_HeaderMatcher envoy_api_v2_route_HeaderMatcher;
typedef struct envoy_api_v2_route_QueryParameterMatcher envoy_api_v2_route_QueryParameterMatcher;
extern const upb_msglayout envoy_api_v2_route_VirtualHost_msginit;
extern const upb_msglayout envoy_api_v2_route_VirtualHost_PerFilterConfigEntry_msginit;
extern const upb_msglayout envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry_msginit;
extern const upb_msglayout envoy_api_v2_route_FilterAction_msginit;
extern const upb_msglayout envoy_api_v2_route_Route_msginit;
extern const upb_msglayout envoy_api_v2_route_Route_PerFilterConfigEntry_msginit;
extern const upb_msglayout envoy_api_v2_route_Route_TypedPerFilterConfigEntry_msginit;
extern const upb_msglayout envoy_api_v2_route_WeightedCluster_msginit;
extern const upb_msglayout envoy_api_v2_route_WeightedCluster_ClusterWeight_msginit;
extern const upb_msglayout envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry_msginit;
extern const upb_msglayout envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteMatch_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_msginit;
extern const upb_msglayout envoy_api_v2_route_CorsPolicy_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteAction_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteAction_RequestMirrorPolicy_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_Header_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_Cookie_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteAction_HashPolicy_FilterState_msginit;
extern const upb_msglayout envoy_api_v2_route_RouteAction_UpgradeConfig_msginit;
extern const upb_msglayout envoy_api_v2_route_RetryPolicy_msginit;
extern const upb_msglayout envoy_api_v2_route_RetryPolicy_RetryPriority_msginit;
extern const upb_msglayout envoy_api_v2_route_RetryPolicy_RetryHostPredicate_msginit;
extern const upb_msglayout envoy_api_v2_route_RetryPolicy_RetryBackOff_msginit;
extern const upb_msglayout envoy_api_v2_route_HedgePolicy_msginit;
extern const upb_msglayout envoy_api_v2_route_RedirectAction_msginit;
extern const upb_msglayout envoy_api_v2_route_DirectResponseAction_msginit;
extern const upb_msglayout envoy_api_v2_route_Decorator_msginit;
extern const upb_msglayout envoy_api_v2_route_Tracing_msginit;
extern const upb_msglayout envoy_api_v2_route_VirtualCluster_msginit;
extern const upb_msglayout envoy_api_v2_route_RateLimit_msginit;
extern const upb_msglayout envoy_api_v2_route_RateLimit_Action_msginit;
extern const upb_msglayout envoy_api_v2_route_RateLimit_Action_SourceCluster_msginit;
extern const upb_msglayout envoy_api_v2_route_RateLimit_Action_DestinationCluster_msginit;
extern const upb_msglayout envoy_api_v2_route_RateLimit_Action_RequestHeaders_msginit;
extern const upb_msglayout envoy_api_v2_route_RateLimit_Action_RemoteAddress_msginit;
extern const upb_msglayout envoy_api_v2_route_RateLimit_Action_GenericKey_msginit;
extern const upb_msglayout envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_msginit;
extern const upb_msglayout envoy_api_v2_route_HeaderMatcher_msginit;
extern const upb_msglayout envoy_api_v2_route_QueryParameterMatcher_msginit;
struct envoy_api_v2_core_DataSource;
struct envoy_api_v2_core_HeaderValueOption;
struct envoy_api_v2_core_Metadata;
struct envoy_api_v2_core_RuntimeFractionalPercent;
struct envoy_type_FractionalPercent;
struct envoy_type_Int64Range;
struct envoy_type_matcher_RegexMatchAndSubstitute;
struct envoy_type_matcher_RegexMatcher;
struct envoy_type_matcher_StringMatcher;
struct envoy_type_tracing_v2_CustomTag;
struct google_protobuf_Any;
struct google_protobuf_BoolValue;
struct google_protobuf_Duration;
struct google_protobuf_Struct;
struct google_protobuf_UInt32Value;
extern const upb_msglayout envoy_api_v2_core_DataSource_msginit;
extern const upb_msglayout envoy_api_v2_core_HeaderValueOption_msginit;
extern const upb_msglayout envoy_api_v2_core_Metadata_msginit;
extern const upb_msglayout envoy_api_v2_core_RuntimeFractionalPercent_msginit;
extern const upb_msglayout envoy_type_FractionalPercent_msginit;
extern const upb_msglayout envoy_type_Int64Range_msginit;
extern const upb_msglayout envoy_type_matcher_RegexMatchAndSubstitute_msginit;
extern const upb_msglayout envoy_type_matcher_RegexMatcher_msginit;
extern const upb_msglayout envoy_type_matcher_StringMatcher_msginit;
extern const upb_msglayout envoy_type_tracing_v2_CustomTag_msginit;
extern const upb_msglayout google_protobuf_Any_msginit;
extern const upb_msglayout google_protobuf_BoolValue_msginit;
extern const upb_msglayout google_protobuf_Duration_msginit;
extern const upb_msglayout google_protobuf_Struct_msginit;
extern const upb_msglayout google_protobuf_UInt32Value_msginit;
typedef enum {
  envoy_api_v2_route_RedirectAction_MOVED_PERMANENTLY = 0,
  envoy_api_v2_route_RedirectAction_FOUND = 1,
  envoy_api_v2_route_RedirectAction_SEE_OTHER = 2,
  envoy_api_v2_route_RedirectAction_TEMPORARY_REDIRECT = 3,
  envoy_api_v2_route_RedirectAction_PERMANENT_REDIRECT = 4
} envoy_api_v2_route_RedirectAction_RedirectResponseCode;
typedef enum {
  envoy_api_v2_route_RouteAction_SERVICE_UNAVAILABLE = 0,
  envoy_api_v2_route_RouteAction_NOT_FOUND = 1
} envoy_api_v2_route_RouteAction_ClusterNotFoundResponseCode;
typedef enum {
  envoy_api_v2_route_RouteAction_PASS_THROUGH_INTERNAL_REDIRECT = 0,
  envoy_api_v2_route_RouteAction_HANDLE_INTERNAL_REDIRECT = 1
} envoy_api_v2_route_RouteAction_InternalRedirectAction;
typedef enum {
  envoy_api_v2_route_VirtualHost_NONE = 0,
  envoy_api_v2_route_VirtualHost_EXTERNAL_ONLY = 1,
  envoy_api_v2_route_VirtualHost_ALL = 2
} envoy_api_v2_route_VirtualHost_TlsRequirementType;
UPB_INLINE envoy_api_v2_route_VirtualHost *envoy_api_v2_route_VirtualHost_new(upb_arena *arena) {
  return (envoy_api_v2_route_VirtualHost *)_upb_msg_new(&envoy_api_v2_route_VirtualHost_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_VirtualHost *envoy_api_v2_route_VirtualHost_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_VirtualHost *ret = envoy_api_v2_route_VirtualHost_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_VirtualHost_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_VirtualHost_serialize(const envoy_api_v2_route_VirtualHost *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_VirtualHost_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_VirtualHost_name(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(12, 16)); }
UPB_INLINE upb_strview const* envoy_api_v2_route_VirtualHost_domains(const envoy_api_v2_route_VirtualHost *msg, size_t *len) { return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(40, 72), len); }
UPB_INLINE const envoy_api_v2_route_Route* const* envoy_api_v2_route_VirtualHost_routes(const envoy_api_v2_route_VirtualHost *msg, size_t *len) { return (const envoy_api_v2_route_Route* const*)_upb_array_accessor(msg, UPB_SIZE(44, 80), len); }
UPB_INLINE int32_t envoy_api_v2_route_VirtualHost_require_tls(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(0, 0)); }
UPB_INLINE const envoy_api_v2_route_VirtualCluster* const* envoy_api_v2_route_VirtualHost_virtual_clusters(const envoy_api_v2_route_VirtualHost *msg, size_t *len) { return (const envoy_api_v2_route_VirtualCluster* const*)_upb_array_accessor(msg, UPB_SIZE(48, 88), len); }
UPB_INLINE const envoy_api_v2_route_RateLimit* const* envoy_api_v2_route_VirtualHost_rate_limits(const envoy_api_v2_route_VirtualHost *msg, size_t *len) { return (const envoy_api_v2_route_RateLimit* const*)_upb_array_accessor(msg, UPB_SIZE(52, 96), len); }
UPB_INLINE const struct envoy_api_v2_core_HeaderValueOption* const* envoy_api_v2_route_VirtualHost_request_headers_to_add(const envoy_api_v2_route_VirtualHost *msg, size_t *len) { return (const struct envoy_api_v2_core_HeaderValueOption* const*)_upb_array_accessor(msg, UPB_SIZE(56, 104), len); }
UPB_INLINE const envoy_api_v2_route_CorsPolicy* envoy_api_v2_route_VirtualHost_cors(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_CorsPolicy*, UPB_SIZE(20, 32)); }
UPB_INLINE const envoy_api_v2_route_CorsPolicy* envoy_api_v2_route_VirtualHost_cors(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_CorsPolicy*, UPB_SIZE(20, 32)); }
UPB_INLINE bool envoy_api_v2_route_VirtualHost_include_request_attempt_count(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, bool, UPB_SIZE(8, 8)); }
UPB_INLINE bool envoy_api_v2_route_VirtualHost_include_request_attempt_count(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, bool, UPB_SIZE(8, 8)); }
UPB_INLINE const envoy_api_v2_route_RetryPolicy* envoy_api_v2_route_VirtualHost_retry_policy(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_RetryPolicy*, UPB_SIZE(24, 40)); }
UPB_INLINE const envoy_api_v2_route_HedgePolicy* envoy_api_v2_route_VirtualHost_hedge_policy(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_HedgePolicy*, UPB_SIZE(28, 48)); }
UPB_INLINE const struct google_protobuf_UInt32Value* envoy_api_v2_route_VirtualHost_per_request_buffer_limit_bytes(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_UInt32Value*, UPB_SIZE(32, 56)); }
UPB_INLINE bool envoy_api_v2_route_VirtualHost_include_attempt_count_in_response(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, bool, UPB_SIZE(9, 9)); }
UPB_INLINE const struct google_protobuf_Any* envoy_api_v2_route_VirtualHost_retry_policy_typed_config(const envoy_api_v2_route_VirtualHost *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Any*, UPB_SIZE(36, 64)); }
UPB_INLINE void envoy_api_v2_route_VirtualHost_set_name(envoy_api_v2_route_VirtualHost *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(12, 16)) = value;
}
UPB_INLINE upb_strview* envoy_api_v2_route_VirtualHost_mutable_domains(envoy_api_v2_route_VirtualHost *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(40, 72), len);
}
UPB_INLINE upb_strview* envoy_api_v2_route_VirtualHost_resize_domains(envoy_api_v2_route_VirtualHost *msg, size_t len, upb_arena *arena) {
return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(40, 72), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_route_VirtualHost_add_domains(envoy_api_v2_route_VirtualHost *msg, upb_strview val, upb_arena *arena) {
return _upb_array_append_accessor(msg, UPB_SIZE(40, 72), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val, arena);
}
UPB_INLINE envoy_api_v2_route_Route** envoy_api_v2_route_VirtualHost_mutable_routes(envoy_api_v2_route_VirtualHost *msg, size_t *len) {
  return (envoy_api_v2_route_Route**)_upb_array_mutable_accessor(msg, UPB_SIZE(44, 80), len);
}
UPB_INLINE envoy_api_v2_route_Route** envoy_api_v2_route_VirtualHost_resize_routes(envoy_api_v2_route_VirtualHost *msg, size_t len, upb_arena *arena) {
return (envoy_api_v2_route_Route**)_upb_array_resize_accessor(msg, UPB_SIZE(44, 80), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_Route* envoy_api_v2_route_VirtualHost_add_routes(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct envoy_api_v2_route_Route* sub = (struct envoy_api_v2_route_Route*)_upb_msg_new(&envoy_api_v2_route_Route_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(44, 80), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_set_require_tls(envoy_api_v2_route_VirtualHost *msg, int32_t value) {
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_VirtualCluster** envoy_api_v2_route_VirtualHost_mutable_virtual_clusters(envoy_api_v2_route_VirtualHost *msg, size_t *len) {
  return (envoy_api_v2_route_VirtualCluster**)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 88), len);
}
UPB_INLINE envoy_api_v2_route_VirtualCluster** envoy_api_v2_route_VirtualHost_resize_virtual_clusters(envoy_api_v2_route_VirtualHost *msg, size_t len, upb_arena *arena) {
return (envoy_api_v2_route_VirtualCluster**)_upb_array_resize_accessor(msg, UPB_SIZE(48, 88), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_VirtualCluster* envoy_api_v2_route_VirtualHost_add_virtual_clusters(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct envoy_api_v2_route_VirtualCluster* sub = (struct envoy_api_v2_route_VirtualCluster*)_upb_msg_new(&envoy_api_v2_route_VirtualCluster_msginit, arena);
  bool ok = _upb_array_append_accessor(
bool ok = _upb_array_append_accessor( msg, UPB_SIZE(44, 80), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena); if (!ok) return NULL; return sub; } UPB_INLINE envoy_api_v2_route_RateLimit** envoy_api_v2_route_VirtualHost_mutable_rate_limits(envoy_api_v2_route_VirtualHost *msg, size_t *len) { return (envoy_api_v2_route_RateLimit**)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 88), len); } UPB_INLINE envoy_api_v2_route_RateLimit** envoy_api_v2_route_VirtualHost_resize_rate_limits(envoy_api_v2_route_VirtualHost *msg, size_t len, upb_arena *arena) { return (envoy_api_v2_route_RateLimit**)_upb_array_resize_accessor(msg, UPB_TYPE_MESSAGE, arena); } UPB_INLINE
      msg, UPB_SIZE(48, 88), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_route_RateLimit** envoy_api_v2_route_VirtualHost_mutable_rate_limits(envoy_api_v2_route_VirtualHost *msg, size_t *len) {
  return (envoy_api_v2_route_RateLimit**)_upb_array_mutable_accessor(msg, UPB_SIZE(52, 96), len);
}
UPB_INLINE envoy_api_v2_route_RateLimit** envoy_api_v2_route_VirtualHost_resize_rate_limits(envoy_api_v2_route_VirtualHost *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit**)_upb_array_resize_accessor(msg, UPB_SIZE(52, 96), len, UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_RateLimit* envoy_api_v2_route_VirtualHost_add_rate_limits(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RateLimit* sub = (struct envoy_api_v2_route_RateLimit*)upb_msg_new(&envoy_api_v2_route_RateLimit_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(52, 96), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_VirtualHost_mutable_request_headers_to_add(envoy_api_v2_route_VirtualHost *msg, size_t *len) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_mutable_accessor(msg, UPB_SIZE(56, 104), len);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_VirtualHost_resize_request_headers_to_add(envoy_api_v2_route_VirtualHost *msg, size_t len, upb_arena *arena) {
return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_resize_accessor(msg, UPB_SIZE(56, 104), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption* envoy_api_v2_route_VirtualHost_add_request_headers_to_add(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct envoy_api_v2_core_HeaderValueOption* sub = (struct envoy_api_v2_core_HeaderValueOption*)_upb_msg_new(&envoy_api_v2_core_HeaderValueOption_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(56, 104), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_set_cors(envoy_api_v2_route_VirtualHost *msg, envoy_api_v2_route_CorsPolicy* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_CorsPolicy*, UPB_SIZE(20, 32)) = value;
}
UPB_INLINE struct envoy_api_v2_route_CorsPolicy* envoy_api_v2_route_VirtualHost_mutable_cors(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct envoy_api_v2_route_CorsPolicy* sub = (struct envoy_api_v2_route_CorsPolicy*)envoy_api_v2_route_VirtualHost_cors(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_CorsPolicy*)_upb_msg_new(&envoy_api_v2_route_CorsPolicy_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_VirtualHost_set_cors(msg, sub);
  }
  return sub;
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_VirtualHost_mutable_response_headers_to_add(envoy_api_v2_route_VirtualHost *msg, size_t *len) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_mutable_accessor(msg, UPB_SIZE(60, 112), len);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_VirtualHost_resize_response_headers_to_add(envoy_api_v2_route_VirtualHost *msg, size_t len, upb_arena *arena) {
return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_resize_accessor(msg, UPB_SIZE(60, 112), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption* envoy_api_v2_route_VirtualHost_add_response_headers_to_add(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct envoy_api_v2_core_HeaderValueOption* sub = (struct envoy_api_v2_core_HeaderValueOption*)_upb_msg_new(&envoy_api_v2_core_HeaderValueOption_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(60, 112), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE upb_strview* envoy_api_v2_route_VirtualHost_mutable_response_headers_to_remove(envoy_api_v2_route_VirtualHost *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(64, 120), len);
}
UPB_INLINE upb_strview* envoy_api_v2_route_VirtualHost_resize_response_headers_to_remove(envoy_api_v2_route_VirtualHost *msg, size_t len, upb_arena *arena) {
return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(64, 120), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_route_VirtualHost_add_response_headers_to_remove(envoy_api_v2_route_VirtualHost *msg, upb_strview val, upb_arena *arena) {
return _upb_array_append_accessor(msg, UPB_SIZE(64, 120), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val, arena);
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_per_filter_config_clear(envoy_api_v2_route_VirtualHost *msg) { _upb_msg_map_clear(msg, UPB_SIZE(64, 120)); }
UPB_INLINE bool envoy_api_v2_route_VirtualHost_per_filter_config_set(envoy_api_v2_route_VirtualHost *msg, upb_strview key, struct google_protobuf_Struct* val, upb_arena *a) { return _upb_msg_map_set(msg, UPB_SIZE(64, 120), &key, 0, &val, sizeof(val), a); }
UPB_INLINE bool envoy_api_v2_route_VirtualHost_per_filter_config_delete(envoy_api_v2_route_VirtualHost *msg, upb_strview key) { return _upb_msg_map_delete(msg, UPB_SIZE(64, 120), &key, 0); }
UPB_INLINE envoy_api_v2_route_VirtualHost_PerFilterConfigEntry* envoy_api_v2_route_VirtualHost_per_filter_config_nextmutable(envoy_api_v2_route_VirtualHost *msg, size_t* iter) { return (envoy_api_v2_route_VirtualHost_PerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(64, 120), iter); }
UPB_INLINE upb_strview* envoy_api_v2_route_VirtualHost_mutable_request_headers_to_remove(envoy_api_v2_route_VirtualHost *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(72, 136), len);
}
UPB_INLINE upb_strview* envoy_api_v2_route_VirtualHost_resize_request_headers_to_remove(envoy_api_v2_route_VirtualHost *msg, size_t len, upb_arena *arena) {
return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(72, 136), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_route_VirtualHost_add_request_headers_to_remove(envoy_api_v2_route_VirtualHost *msg, upb_strview val, upb_arena *arena) {
return _upb_array_append_accessor(msg, UPB_SIZE(72, 136), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val, arena);
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_set_include_request_attempt_count(envoy_api_v2_route_VirtualHost *msg, bool value) {
  UPB_FIELD_AT(msg, bool, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_typed_per_filter_config_clear(envoy_api_v2_route_VirtualHost *msg) { _upb_msg_map_clear(msg, UPB_SIZE(76, 144)); } UPB_INLINE bool envoy_api_v2_route_VirtualHost_typed_per_filter_config_set(envoy_api_v2_route_VirtualHost *msg, upb_strview key, struct google_protobuf_Any* val, upb_arena *a) { return _upb_msg_map_set(msg, UPB_SIZE(76, 144), &key, 0, &val, sizeof(val), a); } UPB_INLINE bool envoy_api_v2_route_VirtualHost_typed_per_filter_config_delete(envoy_api_v2_route_VirtualHost *msg, upb_strview key) { return _upb_msg_map_delete(msg, UPB_SIZE(72, 136), &key, 0); } UPB_INLINE void envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry* envoy_api_v2_route_VirtualHost_typed_per_filter_config_nextmutable(envoy_api_v2_route_VirtualHost *msg, size_t* iter) { return (envoy_api_route_VirtualHost_TypedPerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(72, 136), &key, 0); } UPB_INLINE void envoy_api_route_VirtualHost_set_retry_policy(envoy_api_route_VirtualHost_set_retry_policy(envoy_api_route_VirtualHost_set_retry_policy(envoy_api_route_VirtualHost *msg, size_
UPB_INLINE void envoy_api_v2_route_VirtualHost_set_retry_policy(envoy_api_v2_route_VirtualHost *msg, envoy_api_v2_route_RetryPolicy* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_RetryPolicy*, UPB_SIZE(24, 40)) = value;
}
UPB_INLINE struct envoy_api_v2_route_RetryPolicy* envoy_api_v2_route_VirtualHost_mutable_retry_policy(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RetryPolicy* sub = (struct envoy_api_v2_route_RetryPolicy*)envoy_api_v2_route_VirtualHost_retry_policy(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RetryPolicy*)_upb_msg_new(&envoy_api_v2_route_RetryPolicy_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_VirtualHost_set_retry_policy(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_set_hedge_policy(envoy_api_v2_route_VirtualHost *msg, envoy_api_v2_route_HedgePolicy* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_HedgePolicy*, UPB_SIZE(28, 48)) = value;
}
UPB_INLINE struct envoy_api_v2_route_HedgePolicy* envoy_api_v2_route_VirtualHost_mutable_hedge_policy(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct envoy_api_v2_route_HedgePolicy* sub = (struct envoy_api_v2_route_HedgePolicy*)envoy_api_v2_route_VirtualHost_hedge_policy(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_HedgePolicy*)_upb_msg_new(&envoy_api_v2_route_HedgePolicy_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_VirtualHost_set_hedge_policy(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_set_per_request_buffer_limit_bytes(envoy_api_v2_route_VirtualHost *msg, struct google_protobuf_UInt32Value* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_UInt32Value*, UPB_SIZE(32, 56)) = value;
}
UPB_INLINE struct google_protobuf_UInt32Value* envoy_api_v2_route_VirtualHost_mutable_per_request_buffer_limit_bytes(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct google_protobuf_UInt32Value* sub = (struct google_protobuf_UInt32Value*)envoy_api_v2_route_VirtualHost_per_request_buffer_limit_bytes(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_UInt32Value*)_upb_msg_new(&google_protobuf_UInt32Value_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_VirtualHost_set_per_request_buffer_limit_bytes(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_set_include_attempt_count_in_response(envoy_api_v2_route_VirtualHost *msg, bool value) {
  UPB_FIELD_AT(msg, bool, UPB_SIZE(9, 9)) = value;
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_set_retry_policy_typed_config(envoy_api_v2_route_VirtualHost *msg, struct google_protobuf_Any* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Any*, UPB_SIZE(36, 64)) = value;
}
UPB_INLINE struct google_protobuf_Any* envoy_api_v2_route_VirtualHost_mutable_retry_policy_typed_config(envoy_api_v2_route_VirtualHost *msg, upb_arena *arena) {
  struct google_protobuf_Any* sub = (struct google_protobuf_Any*)envoy_api_v2_route_VirtualHost_retry_policy_typed_config(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Any*)upb_msg_new(&google_protobuf_Any_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_VirtualHost_set_retry_policy_typed_config(msg, sub);
  }
  return sub;
}
UPB_INLINE upb_strview envoy_api_v2_route_VirtualHost_PerFilterConfigEntry_key(const envoy_api_v2_route_VirtualHost_PerFilterConfigEntry *msg) {
  upb_strview ret;
  _upb_msg_map_key(msg, &ret, 0);
  return ret;
}
UPB_INLINE const struct google_protobuf_Struct* envoy_api_v2_route_VirtualHost_PerFilterConfigEntry_value(const envoy_api_v2_route_VirtualHost_PerFilterConfigEntry *msg) {
  struct google_protobuf_Struct* ret;
  _upb_msg_map_value(msg, &ret, sizeof(ret));
  return ret;
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_PerFilterConfigEntry_set_value(envoy_api_v2_route_VirtualHost_PerFilterConfigEntry *msg, struct google_protobuf_Struct* value) {
  _upb_msg_map_set_value(msg, &value, sizeof(struct google_protobuf_Struct*));
}
UPB_INLINE upb_strview envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry_key(const envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry *msg) {
  upb_strview ret;
  _upb_msg_map_key(msg, &ret, 0);
  return ret;
}
UPB_INLINE const struct google_protobuf_Any* envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry_value(const envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry *msg) {
  struct google_protobuf_Any* ret;
  _upb_msg_map_value(msg, &ret, sizeof(ret));
  return ret;
}
UPB_INLINE void envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry_set_value(envoy_api_v2_route_VirtualHost_TypedPerFilterConfigEntry *msg, struct google_protobuf_Any* value) {
  _upb_msg_map_set_value(msg, &value, sizeof(struct google_protobuf_Any*));
}
UPB_INLINE envoy_api_v2_route_FilterAction *envoy_api_v2_route_FilterAction_new(upb_arena *arena) {
  return (envoy_api_v2_route_FilterAction *)_upb_msg_new(&envoy_api_v2_route_FilterAction_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_FilterAction *envoy_api_v2_route_FilterAction_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_FilterAction *ret = envoy_api_v2_route_FilterAction_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_FilterAction_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_FilterAction_serialize(const envoy_api_v2_route_FilterAction *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_FilterAction_msginit, arena, len);
}
UPB_INLINE const struct google_protobuf_Any* envoy_api_v2_route_FilterAction_action(const envoy_api_v2_route_FilterAction *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Any*, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_route_FilterAction_set_action(envoy_api_v2_route_FilterAction *msg, struct google_protobuf_Any* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Any*, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE struct google_protobuf_Any* envoy_api_v2_route_FilterAction_mutable_action(envoy_api_v2_route_FilterAction *msg, upb_arena *arena) {
  struct google_protobuf_Any* sub = (struct google_protobuf_Any*)envoy_api_v2_route_FilterAction_action(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Any*)_upb_msg_new(&google_protobuf_Any_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_FilterAction_set_action(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_Route *envoy_api_v2_route_Route_new(upb_arena *arena) {
  return (envoy_api_v2_route_Route *)_upb_msg_new(&envoy_api_v2_route_Route_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_Route *envoy_api_v2_route_Route_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_Route *ret = envoy_api_v2_route_Route_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_Route_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_Route_serialize(const envoy_api_v2_route_Route *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_Route_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_Route_action_route = 2,
  envoy_api_v2_route_Route_action_redirect = 3,
  envoy_api_v2_route_Route_action_direct_response = 7,
  envoy_api_v2_route_Route_action_filter_action = 17,
  envoy_api_v2_route_Route_action_NOT_SET = 0
} envoy_api_v2_route_Route_action_oneofcases;
UPB_INLINE envoy_api_v2_route_Route_action_oneofcases envoy_api_v2_route_Route_action_case(const envoy_api_v2_route_Route* msg) { return (envoy_api_v2_route_Route_action_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(56, 112)); }
UPB_INLINE const envoy_api_v2_route_RouteMatch* envoy_api_v2_route_Route_match(const envoy_api_v2_route_Route *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_RouteMatch*, UPB_SIZE(8, 16)); }
UPB_INLINE bool envoy_api_v2_route_Route_has_route(const envoy_api_v2_route_Route *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(56, 112), 2); }
UPB_INLINE const envoy_api_v2_route_RouteAction* envoy_api_v2_route_Route_route(const envoy_api_v2_route_Route *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RouteAction*, UPB_SIZE(52, 104), UPB_SIZE(56, 112), 2, NULL); }
UPB_INLINE bool envoy_api_v2_route_Route_has_redirect(const envoy_api_v2_route_Route *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(56, 112), 3); }
UPB_INLINE const envoy_api_v2_route_RedirectAction* envoy_api_v2_route_Route_redirect(const envoy_api_v2_route_Route *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RedirectAction*, UPB_SIZE(52, 104), UPB_SIZE(56, 112), 3, NULL); }
UPB_INLINE const struct envoy_api_v2_core_Metadata* envoy_api_v2_route_Route_metadata(const envoy_api_v2_route_Route *msg) { return UPB_FIELD_AT(msg, const struct envoy_api_v2_core_Metadata*, UPB_SIZE(12, 24)); }
UPB_INLINE const envoy_api_v2_route_Decorator* envoy_api_v2_route_Route_decorator(const envoy_api_v2_route_Route *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_Decorator*, UPB_SIZE(16, 32)); }
UPB_INLINE bool envoy_api_v2_route_Route_has_direct_response(const envoy_api_v2_route_Route *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(56, 112), 7); }
UPB_INLINE const envoy_api_v2_route_DirectResponseAction* envoy_api_v2_route_Route_direct_response(const envoy_api_v2_route_Route *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_DirectResponseAction*, UPB_SIZE(52, 104), UPB_SIZE(56, 112), 7, NULL); }
UPB_INLINE size_t envoy_api_v2_route_Route_per_filter_config_size(const envoy_api_v2_route_Route *msg) {return _upb_msg_map_size(msg, UPB_SIZE(28, 56)); }
UPB_INLINE bool envoy_api_v2_route_Route_per_filter_config_get(const envoy_api_v2_route_Route *msg, upb_strview key, struct google_protobuf_Struct* *val) { return _upb_msg_map_get(msg, UPB_SIZE(28, 56), &key, 0, val, sizeof(*val)); }
UPB_INLINE const envoy_api_v2_route_Route_PerFilterConfigEntry* envoy_api_v2_route_Route_per_filter_config_next(const envoy_api_v2_route_Route *msg, size_t* iter) { return (const envoy_api_v2_route_Route_PerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(28, 56), iter); }
UPB_INLINE const struct envoy_api_v2_core_HeaderValueOption* const* envoy_api_v2_route_Route_request_headers_to_add(const envoy_api_v2_route_Route *msg, size_t *len) { return (const struct envoy_api_v2_core_HeaderValueOption* const*)_upb_array_accessor(msg, UPB_SIZE(32, 64), len); }
UPB_INLINE const struct envoy_api_v2_core_HeaderValueOption* const* envoy_api_v2_route_Route_response_headers_to_add(const envoy_api_v2_route_Route *msg, size_t *len) { return (const struct envoy_api_v2_core_HeaderValueOption* const*)_upb_array_accessor(msg, UPB_SIZE(36, 72), len); }
UPB_INLINE upb_strview const* envoy_api_v2_route_Route_response_headers_to_remove(const envoy_api_v2_route_Route *msg, size_t *len) { return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(40, 80), len); }
UPB_INLINE upb_strview const* envoy_api_v2_route_Route_request_headers_to_remove(const envoy_api_v2_route_Route *msg, size_t *len) { return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(44, 88), len); }
UPB_INLINE size_t envoy_api_v2_route_Route_typed_per_filter_config_size(const envoy_api_v2_route_Route *msg) {return _upb_msg_map_size(msg, UPB_SIZE(48, 96)); }
UPB_INLINE bool envoy_api_v2_route_Route_typed_per_filter_config_get(const envoy_api_v2_route_Route *msg, upb_strview key, struct google_protobuf_Any* *val) { return _upb_msg_map_get(msg, UPB_SIZE(48, 96), &key, 0, val, sizeof(*val)); }
UPB_INLINE const envoy_api_v2_route_Route_TypedPerFilterConfigEntry* envoy_api_v2_route_Route_typed_per_filter_config_next(const envoy_api_v2_route_Route *msg, size_t* iter) { return (const envoy_api_v2_route_Route_TypedPerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(48, 96), iter); }
UPB_INLINE upb_strview envoy_api_v2_route_Route_name(const envoy_api_v2_route_Route *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE const envoy_api_v2_route_Tracing* envoy_api_v2_route_Route_tracing(const envoy_api_v2_route_Route *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_Tracing*, UPB_SIZE(20, 40)); }
UPB_INLINE const struct google_protobuf_UInt32Value* envoy_api_v2_route_Route_per_request_buffer_limit_bytes(const envoy_api_v2_route_Route *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_UInt32Value*, UPB_SIZE(24, 48)); }
UPB_INLINE bool envoy_api_v2_route_Route_has_filter_action(const envoy_api_v2_route_Route *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(56, 112), 17); }
UPB_INLINE const envoy_api_v2_route_FilterAction* envoy_api_v2_route_Route_filter_action(const envoy_api_v2_route_Route *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_FilterAction*, UPB_SIZE(52, 104), UPB_SIZE(56, 112), 17, NULL); }
UPB_INLINE void envoy_api_v2_route_Route_set_match(envoy_api_v2_route_Route *msg, envoy_api_v2_route_RouteMatch* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_RouteMatch*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct envoy_api_v2_route_RouteMatch* envoy_api_v2_route_Route_mutable_match(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteMatch* sub = (struct envoy_api_v2_route_RouteMatch*)envoy_api_v2_route_Route_match(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteMatch*)_upb_msg_new(&envoy_api_v2_route_RouteMatch_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Route_set_match(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Route_set_route(envoy_api_v2_route_Route *msg, envoy_api_v2_route_RouteAction* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RouteAction*, UPB_SIZE(52, 104), value, UPB_SIZE(56, 112), 2);
}
UPB_INLINE struct envoy_api_v2_route_RouteAction* envoy_api_v2_route_Route_mutable_route(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction* sub = (struct envoy_api_v2_route_RouteAction*)envoy_api_v2_route_Route_route(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteAction*)_upb_msg_new(&envoy_api_v2_route_RouteAction_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Route_set_route(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Route_set_redirect(envoy_api_v2_route_Route *msg, envoy_api_v2_route_RedirectAction* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RedirectAction*, UPB_SIZE(52, 104), value, UPB_SIZE(56, 112), 3);
}
UPB_INLINE struct envoy_api_v2_route_RedirectAction* envoy_api_v2_route_Route_mutable_redirect(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RedirectAction* sub = (struct envoy_api_v2_route_RedirectAction*)envoy_api_v2_route_Route_redirect(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RedirectAction*)_upb_msg_new(&envoy_api_v2_route_RedirectAction_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Route_set_redirect(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Route_set_metadata(envoy_api_v2_route_Route *msg, struct envoy_api_v2_core_Metadata* value) {
  UPB_FIELD_AT(msg, struct envoy_api_v2_core_Metadata*, UPB_SIZE(12, 24)) = value;
}
UPB_INLINE struct envoy_api_v2_core_Metadata* envoy_api_v2_route_Route_mutable_metadata(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_core_Metadata* sub = (struct envoy_api_v2_core_Metadata*)envoy_api_v2_route_Route_metadata(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_Metadata*)_upb_msg_new(&envoy_api_v2_core_Metadata_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Route_set_metadata(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Route_set_decorator(envoy_api_v2_route_Route *msg, envoy_api_v2_route_Decorator* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_Decorator*, UPB_SIZE(16, 32)) = value;
}
UPB_INLINE struct envoy_api_v2_route_Decorator* envoy_api_v2_route_Route_mutable_decorator(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_route_Decorator* sub = (struct envoy_api_v2_route_Decorator*)envoy_api_v2_route_Route_decorator(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_Decorator*)_upb_msg_new(&envoy_api_v2_route_Decorator_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Route_set_decorator(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Route_set_direct_response(envoy_api_v2_route_Route *msg, envoy_api_v2_route_DirectResponseAction* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_DirectResponseAction*, UPB_SIZE(52, 104), value, UPB_SIZE(56, 112), 7);
}
UPB_INLINE struct envoy_api_v2_route_DirectResponseAction* envoy_api_v2_route_Route_mutable_direct_response(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_route_DirectResponseAction* sub = (struct envoy_api_v2_route_DirectResponseAction*)envoy_api_v2_route_Route_direct_response(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_DirectResponseAction*)_upb_msg_new(&envoy_api_v2_route_DirectResponseAction_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Route_set_direct_response(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Route_per_filter_config_clear(envoy_api_v2_route_Route *msg) { _upb_msg_map_clear(msg, UPB_SIZE(28, 56)); }
UPB_INLINE bool envoy_api_v2_route_Route_per_filter_config_set(envoy_api_v2_route_Route *msg, upb_strview key, struct google_protobuf_Struct* val, upb_arena *a) { return _upb_msg_map_set(msg, UPB_SIZE(28, 56), &key, 0, &val, sizeof(val), a); }
UPB_INLINE bool envoy_api_v2_route_Route_per_filter_config_delete(envoy_api_v2_route_Route *msg, upb_strview key) { return _upb_msg_map_delete(msg, UPB_SIZE(28, 56), &key, 0); }
UPB_INLINE envoy_api_v2_route_Route_PerFilterConfigEntry* envoy_api_v2_route_Route_per_filter_config_nextmutable(envoy_api_v2_route_Route *msg, size_t* iter) { return (envoy_api_v2_route_Route_PerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(28, 56), iter); }
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_Route_mutable_request_headers_to_add(envoy_api_v2_route_Route *msg, size_t *len) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_mutable_accessor(msg, UPB_SIZE(32, 64), len);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_Route_resize_request_headers_to_add(envoy_api_v2_route_Route *msg, size_t len, upb_arena *arena) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_resize_accessor(msg, UPB_SIZE(32, 64), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption* envoy_api_v2_route_Route_add_request_headers_to_add(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_core_HeaderValueOption* sub = (struct envoy_api_v2_core_HeaderValueOption*)_upb_msg_new(&envoy_api_v2_core_HeaderValueOption_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(32, 64), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_Route_mutable_response_headers_to_add(envoy_api_v2_route_Route *msg, size_t *len) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_mutable_accessor(msg, UPB_SIZE(36, 72), len);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_Route_resize_response_headers_to_add(envoy_api_v2_route_Route *msg, size_t len, upb_arena *arena) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_resize_accessor(msg, UPB_SIZE(36, 72), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption* envoy_api_v2_route_Route_add_response_headers_to_add(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_core_HeaderValueOption* sub = (struct envoy_api_v2_core_HeaderValueOption*)_upb_msg_new(&envoy_api_v2_core_HeaderValueOption_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(36, 72), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE upb_strview* envoy_api_v2_route_Route_mutable_response_headers_to_remove(envoy_api_v2_route_Route *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(40, 80), len);
}
UPB_INLINE upb_strview* envoy_api_v2_route_Route_resize_response_headers_to_remove(envoy_api_v2_route_Route *msg, size_t len, upb_arena *arena) {
  return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(40, 80), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_route_Route_add_response_headers_to_remove(envoy_api_v2_route_Route *msg, upb_strview val, upb_arena *arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(40, 80), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val,
      arena);
}
UPB_INLINE upb_strview* envoy_api_v2_route_Route_mutable_request_headers_to_remove(envoy_api_v2_route_Route *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(44, 88), len);
}
UPB_INLINE upb_strview* envoy_api_v2_route_Route_resize_request_headers_to_remove(envoy_api_v2_route_Route *msg, size_t len, upb_arena *arena) {
  return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(44, 88), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_route_Route_add_request_headers_to_remove(envoy_api_v2_route_Route *msg, upb_strview val, upb_arena *arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(44, 88), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val,
      arena);
}
UPB_INLINE void envoy_api_v2_route_Route_typed_per_filter_config_clear(envoy_api_v2_route_Route *msg) { _upb_msg_map_clear(msg, UPB_SIZE(48, 96)); }
UPB_INLINE bool envoy_api_v2_route_Route_typed_per_filter_config_set(envoy_api_v2_route_Route *msg, upb_strview key, struct google_protobuf_Any* val, upb_arena *a) { return _upb_msg_map_set(msg, UPB_SIZE(48, 96), &key, 0, &val, sizeof(val), a); }
UPB_INLINE bool envoy_api_v2_route_Route_typed_per_filter_config_delete(envoy_api_v2_route_Route *msg, upb_strview key) { return _upb_msg_map_delete(msg, UPB_SIZE(48, 96), &key, 0); }
UPB_INLINE envoy_api_v2_route_Route_TypedPerFilterConfigEntry* envoy_api_v2_route_Route_typed_per_filter_config_nextmutable(envoy_api_v2_route_Route *msg, size_t* iter) { return (envoy_api_v2_route_Route_TypedPerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(48, 96), iter); }
UPB_INLINE void envoy_api_v2_route_Route_set_name(envoy_api_v2_route_Route *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_Route_set_tracing(envoy_api_v2_route_Route *msg, envoy_api_v2_route_Tracing* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_Tracing*, UPB_SIZE(20, 40)) = value;
}
UPB_INLINE struct envoy_api_v2_route_Tracing* envoy_api_v2_route_Route_mutable_tracing(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_route_Tracing* sub = (struct envoy_api_v2_route_Tracing*)envoy_api_v2_route_Route_tracing(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_Tracing*)_upb_msg_new(&envoy_api_v2_route_Tracing_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Route_set_tracing(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Route_set_per_request_buffer_limit_bytes(envoy_api_v2_route_Route *msg, struct google_protobuf_UInt32Value* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_UInt32Value*, UPB_SIZE(24, 48)) = value;
}
UPB_INLINE struct google_protobuf_UInt32Value* envoy_api_v2_route_Route_mutable_per_request_buffer_limit_bytes(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct google_protobuf_UInt32Value* sub = (struct google_protobuf_UInt32Value*)envoy_api_v2_route_Route_per_request_buffer_limit_bytes(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_UInt32Value*)_upb_msg_new(&google_protobuf_UInt32Value_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Route_set_per_request_buffer_limit_bytes(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Route_set_filter_action(envoy_api_v2_route_Route *msg, envoy_api_v2_route_FilterAction* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_FilterAction*, UPB_SIZE(52, 104), value, UPB_SIZE(56, 112), 17);
}
UPB_INLINE struct envoy_api_v2_route_FilterAction* envoy_api_v2_route_Route_mutable_filter_action(envoy_api_v2_route_Route *msg, upb_arena *arena) {
  struct envoy_api_v2_route_FilterAction* sub = (struct envoy_api_v2_route_FilterAction*)envoy_api_v2_route_Route_filter_action(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_FilterAction*)_upb_msg_new(&envoy_api_v2_route_FilterAction_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Route_set_filter_action(msg, sub);
  }
  return sub;
}
UPB_INLINE upb_strview envoy_api_v2_route_Route_PerFilterConfigEntry_key(const envoy_api_v2_route_Route_PerFilterConfigEntry *msg) {
  upb_strview ret;
  _upb_msg_map_key(msg, &ret, 0);
  return ret;
}
UPB_INLINE const struct google_protobuf_Struct* envoy_api_v2_route_Route_PerFilterConfigEntry_value(const envoy_api_v2_route_Route_PerFilterConfigEntry *msg) {
  struct google_protobuf_Struct* ret;
  _upb_msg_map_value(msg, &ret, sizeof(ret));
  return ret;
}
UPB_INLINE void envoy_api_v2_route_Route_PerFilterConfigEntry_set_value(envoy_api_v2_route_Route_PerFilterConfigEntry *msg, struct google_protobuf_Struct* value) {
  _upb_msg_map_set_value(msg, &value, sizeof(struct google_protobuf_Struct*));
}
UPB_INLINE upb_strview envoy_api_v2_route_Route_TypedPerFilterConfigEntry_key(const envoy_api_v2_route_Route_TypedPerFilterConfigEntry *msg) {
  upb_strview ret;
  _upb_msg_map_key(msg, &ret, 0);
  return ret;
}
UPB_INLINE const struct google_protobuf_Any* envoy_api_v2_route_Route_TypedPerFilterConfigEntry_value(const envoy_api_v2_route_Route_TypedPerFilterConfigEntry *msg) {
  struct google_protobuf_Any* ret;
  _upb_msg_map_value(msg, &ret, sizeof(ret));
  return ret;
}
UPB_INLINE void envoy_api_v2_route_Route_TypedPerFilterConfigEntry_set_value(envoy_api_v2_route_Route_TypedPerFilterConfigEntry *msg, struct google_protobuf_Any* value) {
  _upb_msg_map_set_value(msg, &value, sizeof(struct google_protobuf_Any*));
}
UPB_INLINE envoy_api_v2_route_WeightedCluster *envoy_api_v2_route_WeightedCluster_new(upb_arena *arena) {
  return (envoy_api_v2_route_WeightedCluster *)_upb_msg_new(&envoy_api_v2_route_WeightedCluster_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_WeightedCluster *envoy_api_v2_route_WeightedCluster_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_WeightedCluster *ret = envoy_api_v2_route_WeightedCluster_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_WeightedCluster_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_WeightedCluster_serialize(const envoy_api_v2_route_WeightedCluster *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_WeightedCluster_msginit, arena, len);
}
UPB_INLINE const envoy_api_v2_route_WeightedCluster_ClusterWeight* const* envoy_api_v2_route_WeightedCluster_clusters(const envoy_api_v2_route_WeightedCluster *msg, size_t *len) { return (const envoy_api_v2_route_WeightedCluster_ClusterWeight* const*)_upb_array_accessor(msg, UPB_SIZE(12, 24), len); }
UPB_INLINE upb_strview envoy_api_v2_route_WeightedCluster_runtime_key_prefix(const envoy_api_v2_route_WeightedCluster *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_UInt32Value* envoy_api_v2_route_WeightedCluster_total_weight(const envoy_api_v2_route_WeightedCluster *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_UInt32Value*, UPB_SIZE(8, 16)); }
UPB_INLINE envoy_api_v2_route_WeightedCluster_ClusterWeight** envoy_api_v2_route_WeightedCluster_mutable_clusters(envoy_api_v2_route_WeightedCluster *msg, size_t *len) {
  return (envoy_api_v2_route_WeightedCluster_ClusterWeight**)_upb_array_mutable_accessor(msg, UPB_SIZE(12, 24), len);
}
UPB_INLINE envoy_api_v2_route_WeightedCluster_ClusterWeight** envoy_api_v2_route_WeightedCluster_resize_clusters(envoy_api_v2_route_WeightedCluster *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_WeightedCluster_ClusterWeight**)_upb_array_resize_accessor(msg, UPB_SIZE(12, 24), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_WeightedCluster_ClusterWeight* envoy_api_v2_route_WeightedCluster_add_clusters(envoy_api_v2_route_WeightedCluster *msg, upb_arena *arena) {
  struct envoy_api_v2_route_WeightedCluster_ClusterWeight* sub = (struct envoy_api_v2_route_WeightedCluster_ClusterWeight*)_upb_msg_new(&envoy_api_v2_route_WeightedCluster_ClusterWeight_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(12, 24), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE void envoy_api_v2_route_WeightedCluster_set_runtime_key_prefix(envoy_api_v2_route_WeightedCluster *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_WeightedCluster_set_total_weight(envoy_api_v2_route_WeightedCluster *msg, struct google_protobuf_UInt32Value* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_UInt32Value*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct google_protobuf_UInt32Value* envoy_api_v2_route_WeightedCluster_mutable_total_weight(envoy_api_v2_route_WeightedCluster *msg, upb_arena *arena) {
  struct google_protobuf_UInt32Value* sub = (struct google_protobuf_UInt32Value*)envoy_api_v2_route_WeightedCluster_total_weight(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_UInt32Value*)_upb_msg_new(&google_protobuf_UInt32Value_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_WeightedCluster_set_total_weight(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_WeightedCluster_ClusterWeight *envoy_api_v2_route_WeightedCluster_ClusterWeight_new(upb_arena *arena) {
  return (envoy_api_v2_route_WeightedCluster_ClusterWeight *)_upb_msg_new(&envoy_api_v2_route_WeightedCluster_ClusterWeight_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_WeightedCluster_ClusterWeight *envoy_api_v2_route_WeightedCluster_ClusterWeight_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_WeightedCluster_ClusterWeight *ret = envoy_api_v2_route_WeightedCluster_ClusterWeight_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_WeightedCluster_ClusterWeight_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_WeightedCluster_ClusterWeight_serialize(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_WeightedCluster_ClusterWeight_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_WeightedCluster_ClusterWeight_name(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_UInt32Value* envoy_api_v2_route_WeightedCluster_ClusterWeight_weight(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_UInt32Value*, UPB_SIZE(8, 16)); }
UPB_INLINE const struct envoy_api_v2_core_Metadata* envoy_api_v2_route_WeightedCluster_ClusterWeight_metadata_match(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg) { return UPB_FIELD_AT(msg, const struct envoy_api_v2_core_Metadata*, UPB_SIZE(12, 24)); }
UPB_INLINE const struct envoy_api_v2_core_HeaderValueOption* const* envoy_api_v2_route_WeightedCluster_ClusterWeight_request_headers_to_add(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t *len) { return (const struct envoy_api_v2_core_HeaderValueOption* const*)_upb_array_accessor(msg, UPB_SIZE(16, 32), len); }
UPB_INLINE const struct envoy_api_v2_core_HeaderValueOption* const* envoy_api_v2_route_WeightedCluster_ClusterWeight_response_headers_to_add(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t *len) { return (const struct envoy_api_v2_core_HeaderValueOption* const*)_upb_array_accessor(msg, UPB_SIZE(20, 40), len); }
UPB_INLINE upb_strview const* envoy_api_v2_route_WeightedCluster_ClusterWeight_response_headers_to_remove(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t *len) { return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(24, 48), len); }
UPB_INLINE size_t envoy_api_v2_route_WeightedCluster_ClusterWeight_per_filter_config_size(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg) {return _upb_msg_map_size(msg, UPB_SIZE(28, 56)); }
UPB_INLINE bool envoy_api_v2_route_WeightedCluster_ClusterWeight_per_filter_config_get(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_strview key, struct google_protobuf_Struct* *val) { return _upb_msg_map_get(msg, UPB_SIZE(28, 56), &key, 0, val, sizeof(*val)); }
UPB_INLINE const envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry* envoy_api_v2_route_WeightedCluster_ClusterWeight_per_filter_config_next(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t* iter) { return (const envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(28, 56), iter); }
UPB_INLINE upb_strview const* envoy_api_v2_route_WeightedCluster_ClusterWeight_request_headers_to_remove(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t *len) { return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(32, 64), len); }
UPB_INLINE size_t envoy_api_v2_route_WeightedCluster_ClusterWeight_typed_per_filter_config_size(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg) {return _upb_msg_map_size(msg, UPB_SIZE(36, 72)); }
UPB_INLINE bool envoy_api_v2_route_WeightedCluster_ClusterWeight_typed_per_filter_config_get(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_strview key, struct google_protobuf_Any* *val) { return _upb_msg_map_get(msg, UPB_SIZE(36, 72), &key, 0, val, sizeof(*val)); }
UPB_INLINE const envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry* envoy_api_v2_route_WeightedCluster_ClusterWeight_typed_per_filter_config_next(const envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t* iter) { return (const envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(36, 72), iter); }
UPB_INLINE void envoy_api_v2_route_WeightedCluster_ClusterWeight_set_name(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_WeightedCluster_ClusterWeight_set_weight(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, struct google_protobuf_UInt32Value* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_UInt32Value*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct google_protobuf_UInt32Value* envoy_api_v2_route_WeightedCluster_ClusterWeight_mutable_weight(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_arena *arena) {
  struct google_protobuf_UInt32Value* sub = (struct google_protobuf_UInt32Value*)envoy_api_v2_route_WeightedCluster_ClusterWeight_weight(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_UInt32Value*)_upb_msg_new(&google_protobuf_UInt32Value_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_WeightedCluster_ClusterWeight_set_weight(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_WeightedCluster_ClusterWeight_set_metadata_match(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, struct envoy_api_v2_core_Metadata* value) {
  UPB_FIELD_AT(msg, struct envoy_api_v2_core_Metadata*, UPB_SIZE(12, 24)) = value;
}
UPB_INLINE struct envoy_api_v2_core_Metadata* envoy_api_v2_route_WeightedCluster_ClusterWeight_mutable_metadata_match(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_arena *arena) {
  struct envoy_api_v2_core_Metadata* sub = (struct envoy_api_v2_core_Metadata*)envoy_api_v2_route_WeightedCluster_ClusterWeight_metadata_match(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_Metadata*)_upb_msg_new(&envoy_api_v2_core_Metadata_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_WeightedCluster_ClusterWeight_set_metadata_match(msg, sub);
  }
  return sub;
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_WeightedCluster_ClusterWeight_mutable_request_headers_to_add(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t *len) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_mutable_accessor(msg, UPB_SIZE(16, 32), len);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_WeightedCluster_ClusterWeight_resize_request_headers_to_add(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t len, upb_arena *arena) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_resize_accessor(msg, UPB_SIZE(16, 32), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption* envoy_api_v2_route_WeightedCluster_ClusterWeight_add_request_headers_to_add(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_arena *arena) {
  struct envoy_api_v2_core_HeaderValueOption* sub = (struct envoy_api_v2_core_HeaderValueOption*)_upb_msg_new(&envoy_api_v2_core_HeaderValueOption_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(16, 32), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_WeightedCluster_ClusterWeight_mutable_response_headers_to_add(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t *len) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_mutable_accessor(msg, UPB_SIZE(20, 40), len);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption** envoy_api_v2_route_WeightedCluster_ClusterWeight_resize_response_headers_to_add(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t len, upb_arena *arena) {
  return (struct envoy_api_v2_core_HeaderValueOption**)_upb_array_resize_accessor(msg, UPB_SIZE(20, 40), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValueOption* envoy_api_v2_route_WeightedCluster_ClusterWeight_add_response_headers_to_add(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_arena *arena) {
  struct envoy_api_v2_core_HeaderValueOption* sub = (struct envoy_api_v2_core_HeaderValueOption*)_upb_msg_new(&envoy_api_v2_core_HeaderValueOption_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(20, 40), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE upb_strview* envoy_api_v2_route_WeightedCluster_ClusterWeight_mutable_response_headers_to_remove(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(24, 48), len);
}
UPB_INLINE upb_strview* envoy_api_v2_route_WeightedCluster_ClusterWeight_resize_response_headers_to_remove(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t len, upb_arena *arena) {
  return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(24, 48), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_route_WeightedCluster_ClusterWeight_add_response_headers_to_remove(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_strview val, upb_arena *arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(24, 48), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val,
      arena);
}
UPB_INLINE void envoy_api_v2_route_WeightedCluster_ClusterWeight_per_filter_config_clear(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg) { _upb_msg_map_clear(msg, UPB_SIZE(28, 56)); }
UPB_INLINE bool envoy_api_v2_route_WeightedCluster_ClusterWeight_per_filter_config_set(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_strview key, struct google_protobuf_Struct* val, upb_arena *a) { return _upb_msg_map_set(msg, UPB_SIZE(28, 56), &key, 0, &val, sizeof(val), a); }
UPB_INLINE bool envoy_api_v2_route_WeightedCluster_ClusterWeight_per_filter_config_delete(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_strview key) { return _upb_msg_map_delete(msg, UPB_SIZE(28, 56), &key, 0); }
UPB_INLINE envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry* envoy_api_v2_route_WeightedCluster_ClusterWeight_per_filter_config_nextmutable(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t* iter) { return (envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(28, 56), iter); }
UPB_INLINE upb_strview* envoy_api_v2_route_WeightedCluster_ClusterWeight_mutable_request_headers_to_remove(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(32, 64), len);
}
UPB_INLINE upb_strview* envoy_api_v2_route_WeightedCluster_ClusterWeight_resize_request_headers_to_remove(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t len, upb_arena *arena) {
  return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(32, 64), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_route_WeightedCluster_ClusterWeight_add_request_headers_to_remove(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_strview val, upb_arena *arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(32, 64), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val,
      arena);
}
UPB_INLINE void envoy_api_v2_route_WeightedCluster_ClusterWeight_typed_per_filter_config_clear(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg) { _upb_msg_map_clear(msg, UPB_SIZE(36, 72)); }
UPB_INLINE bool envoy_api_v2_route_WeightedCluster_ClusterWeight_typed_per_filter_config_set(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_strview key, struct google_protobuf_Any* val, upb_arena *a) { return _upb_msg_map_set(msg, UPB_SIZE(36, 72), &key, 0, &val, sizeof(val), a); }
UPB_INLINE bool envoy_api_v2_route_WeightedCluster_ClusterWeight_typed_per_filter_config_delete(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, upb_strview key) { return _upb_msg_map_delete(msg, UPB_SIZE(36, 72), &key, 0); }
UPB_INLINE envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry* envoy_api_v2_route_WeightedCluster_ClusterWeight_typed_per_filter_config_nextmutable(envoy_api_v2_route_WeightedCluster_ClusterWeight *msg, size_t* iter) { return (envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry*)_upb_msg_map_next(msg, UPB_SIZE(36, 72), iter); }
UPB_INLINE upb_strview envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry_key(const envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry *msg) {
  upb_strview ret;
  _upb_msg_map_key(msg, &ret, 0);
  return ret;
}
UPB_INLINE const struct google_protobuf_Struct* envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry_value(const envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry *msg) {
  struct google_protobuf_Struct* ret;
  _upb_msg_map_value(msg, &ret, sizeof(ret));
  return ret;
}
UPB_INLINE void envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry_set_value(envoy_api_v2_route_WeightedCluster_ClusterWeight_PerFilterConfigEntry *msg, struct google_protobuf_Struct* value) {
  _upb_msg_map_set_value(msg, &value, sizeof(struct google_protobuf_Struct*));
}
UPB_INLINE upb_strview envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry_key(const envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry *msg) {
  upb_strview ret;
  _upb_msg_map_key(msg, &ret, 0);
  return ret;
}
UPB_INLINE const struct google_protobuf_Any* envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry_value(const envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry *msg) {
  struct google_protobuf_Any* ret;
  _upb_msg_map_value(msg, &ret, sizeof(ret));
  return ret;
}
UPB_INLINE void envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry_set_value(envoy_api_v2_route_WeightedCluster_ClusterWeight_TypedPerFilterConfigEntry *msg, struct google_protobuf_Any* value) {
  _upb_msg_map_set_value(msg, &value, sizeof(struct google_protobuf_Any*));
}
UPB_INLINE envoy_api_v2_route_RouteMatch *envoy_api_v2_route_RouteMatch_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteMatch *)_upb_msg_new(&envoy_api_v2_route_RouteMatch_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteMatch *envoy_api_v2_route_RouteMatch_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteMatch *ret = envoy_api_v2_route_RouteMatch_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteMatch_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteMatch_serialize(const envoy_api_v2_route_RouteMatch *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteMatch_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_RouteMatch_path_specifier_prefix = 1,
  envoy_api_v2_route_RouteMatch_path_specifier_path = 2,
  envoy_api_v2_route_RouteMatch_path_specifier_regex = 3,
  envoy_api_v2_route_RouteMatch_path_specifier_safe_regex = 10,
  envoy_api_v2_route_RouteMatch_path_specifier_NOT_SET = 0
} envoy_api_v2_route_RouteMatch_path_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_RouteMatch_path_specifier_oneofcases envoy_api_v2_route_RouteMatch_path_specifier_case(const envoy_api_v2_route_RouteMatch* msg) { return (envoy_api_v2_route_RouteMatch_path_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(32, 64)); }
UPB_INLINE bool envoy_api_v2_route_RouteMatch_has_prefix(const envoy_api_v2_route_RouteMatch *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(32, 64), 1); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteMatch_prefix(const envoy_api_v2_route_RouteMatch *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(24, 48), UPB_SIZE(32, 64), 1, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_RouteMatch_has_path(const envoy_api_v2_route_RouteMatch *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(32, 64), 2); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteMatch_path(const envoy_api_v2_route_RouteMatch *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(24, 48), UPB_SIZE(32, 64), 2, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_RouteMatch_has_regex(const envoy_api_v2_route_RouteMatch *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(32, 64), 3); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteMatch_regex(const envoy_api_v2_route_RouteMatch *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(24, 48), UPB_SIZE(32, 64), 3, upb_strview_make("", strlen(""))); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_RouteMatch_case_sensitive(const envoy_api_v2_route_RouteMatch *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(0, 0)); }
UPB_INLINE const envoy_api_v2_route_HeaderMatcher* const* envoy_api_v2_route_RouteMatch_headers(const envoy_api_v2_route_RouteMatch *msg, size_t *len) { return (const envoy_api_v2_route_HeaderMatcher* const*)_upb_array_accessor(msg, UPB_SIZE(16, 32), len); }
UPB_INLINE const envoy_api_v2_route_QueryParameterMatcher* const* envoy_api_v2_route_RouteMatch_query_parameters(const envoy_api_v2_route_RouteMatch *msg, size_t *len) { return (const envoy_api_v2_route_QueryParameterMatcher* const*)_upb_array_accessor(msg, UPB_SIZE(20, 40), len); }
UPB_INLINE const envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions* envoy_api_v2_route_RouteMatch_grpc(const envoy_api_v2_route_RouteMatch *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions*, UPB_SIZE(4, 8)); }
UPB_INLINE const struct envoy_api_v2_core_RuntimeFractionalPercent* envoy_api_v2_route_RouteMatch_runtime_fraction(const envoy_api_v2_route_RouteMatch *msg) { return UPB_FIELD_AT(msg, const struct envoy_api_v2_core_RuntimeFractionalPercent*, UPB_SIZE(8, 16)); }
UPB_INLINE bool envoy_api_v2_route_RouteMatch_has_safe_regex(const envoy_api_v2_route_RouteMatch *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(32, 64), 10); }
UPB_INLINE const struct envoy_type_matcher_RegexMatcher* envoy_api_v2_route_RouteMatch_safe_regex(const envoy_api_v2_route_RouteMatch *msg) { return UPB_READ_ONEOF(msg, const struct envoy_type_matcher_RegexMatcher*, UPB_SIZE(24, 48), UPB_SIZE(32, 64), 10, NULL); }
UPB_INLINE const envoy_api_v2_route_RouteMatch_TlsContextMatchOptions* envoy_api_v2_route_RouteMatch_tls_context(const envoy_api_v2_route_RouteMatch *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_RouteMatch_TlsContextMatchOptions*, UPB_SIZE(12, 24)); }
UPB_INLINE void envoy_api_v2_route_RouteMatch_set_prefix(envoy_api_v2_route_RouteMatch *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(24, 48), value, UPB_SIZE(32, 64), 1);
}
UPB_INLINE void envoy_api_v2_route_RouteMatch_set_path(envoy_api_v2_route_RouteMatch *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(24, 48), value, UPB_SIZE(32, 64), 2);
}
UPB_INLINE void envoy_api_v2_route_RouteMatch_set_regex(envoy_api_v2_route_RouteMatch *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(24, 48), value, UPB_SIZE(32, 64), 3);
}
UPB_INLINE void envoy_api_v2_route_RouteMatch_set_case_sensitive(envoy_api_v2_route_RouteMatch *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_RouteMatch_mutable_case_sensitive(envoy_api_v2_route_RouteMatch *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_RouteMatch_case_sensitive(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteMatch_set_case_sensitive(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_RouteMatch_mutable_headers(envoy_api_v2_route_RouteMatch *msg, size_t *len) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_mutable_accessor(msg, UPB_SIZE(16, 32), len);
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_RouteMatch_resize_headers(envoy_api_v2_route_RouteMatch *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_resize_accessor(msg, UPB_SIZE(16, 32), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_HeaderMatcher* envoy_api_v2_route_RouteMatch_add_headers(envoy_api_v2_route_RouteMatch *msg, upb_arena *arena) {
  struct envoy_api_v2_route_HeaderMatcher* sub = (struct envoy_api_v2_route_HeaderMatcher*)_upb_msg_new(&envoy_api_v2_route_HeaderMatcher_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(16, 32), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_route_QueryParameterMatcher** envoy_api_v2_route_RouteMatch_mutable_query_parameters(envoy_api_v2_route_RouteMatch *msg, size_t *len) {
  return (envoy_api_v2_route_QueryParameterMatcher**)_upb_array_mutable_accessor(msg, UPB_SIZE(20, 40), len);
}
UPB_INLINE envoy_api_v2_route_QueryParameterMatcher** envoy_api_v2_route_RouteMatch_resize_query_parameters(envoy_api_v2_route_RouteMatch *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_QueryParameterMatcher**)_upb_array_resize_accessor(msg, UPB_SIZE(20, 40), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_QueryParameterMatcher* envoy_api_v2_route_RouteMatch_add_query_parameters(envoy_api_v2_route_RouteMatch *msg, upb_arena *arena) {
  struct envoy_api_v2_route_QueryParameterMatcher* sub = (struct envoy_api_v2_route_QueryParameterMatcher*)_upb_msg_new(&envoy_api_v2_route_QueryParameterMatcher_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(20, 40), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteMatch_set_grpc(envoy_api_v2_route_RouteMatch *msg, envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions* envoy_api_v2_route_RouteMatch_mutable_grpc(envoy_api_v2_route_RouteMatch *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions* sub = (struct envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions*)envoy_api_v2_route_RouteMatch_grpc(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions*)_upb_msg_new(&envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteMatch_set_grpc(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteMatch_set_runtime_fraction(envoy_api_v2_route_RouteMatch *msg, struct envoy_api_v2_core_RuntimeFractionalPercent* value) {
  UPB_FIELD_AT(msg, struct envoy_api_v2_core_RuntimeFractionalPercent*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct envoy_api_v2_core_RuntimeFractionalPercent* envoy_api_v2_route_RouteMatch_mutable_runtime_fraction(envoy_api_v2_route_RouteMatch *msg, upb_arena *arena) {
  struct envoy_api_v2_core_RuntimeFractionalPercent* sub = (struct envoy_api_v2_core_RuntimeFractionalPercent*)envoy_api_v2_route_RouteMatch_runtime_fraction(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_RuntimeFractionalPercent*)_upb_msg_new(&envoy_api_v2_core_RuntimeFractionalPercent_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteMatch_set_runtime_fraction(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteMatch_set_safe_regex(envoy_api_v2_route_RouteMatch *msg, struct envoy_type_matcher_RegexMatcher* value) {
  UPB_WRITE_ONEOF(msg, struct envoy_type_matcher_RegexMatcher*, UPB_SIZE(24, 48), value, UPB_SIZE(32, 64), 10);
}
UPB_INLINE struct envoy_type_matcher_RegexMatcher* envoy_api_v2_route_RouteMatch_mutable_safe_regex(envoy_api_v2_route_RouteMatch *msg, upb_arena *arena) {
  struct envoy_type_matcher_RegexMatcher* sub = (struct envoy_type_matcher_RegexMatcher*)envoy_api_v2_route_RouteMatch_safe_regex(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_matcher_RegexMatcher*)_upb_msg_new(&envoy_type_matcher_RegexMatcher_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteMatch_set_safe_regex(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteMatch_set_tls_context(envoy_api_v2_route_RouteMatch *msg, envoy_api_v2_route_RouteMatch_TlsContextMatchOptions* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_RouteMatch_TlsContextMatchOptions*, UPB_SIZE(12, 24)) = value;
}
UPB_INLINE struct envoy_api_v2_route_RouteMatch_TlsContextMatchOptions* envoy_api_v2_route_RouteMatch_mutable_tls_context(envoy_api_v2_route_RouteMatch *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteMatch_TlsContextMatchOptions* sub = (struct envoy_api_v2_route_RouteMatch_TlsContextMatchOptions*)envoy_api_v2_route_RouteMatch_tls_context(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteMatch_TlsContextMatchOptions*)_upb_msg_new(&envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteMatch_set_tls_context(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions *envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions *)_upb_msg_new(&envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions *envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions *ret = envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_serialize(const envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteMatch_GrpcRouteMatchOptions_msginit, arena, len);
}
UPB_INLINE envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *)_upb_msg_new(&envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *ret = envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_serialize(const envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_msginit, arena, len);
}
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_presented(const envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_validated(const envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(4, 8)); }
UPB_INLINE void envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_set_presented(envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_mutable_presented(envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_presented(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_set_presented(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_set_validated(envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_mutable_validated(envoy_api_v2_route_RouteMatch_TlsContextMatchOptions *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_validated(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteMatch_TlsContextMatchOptions_set_validated(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_CorsPolicy *envoy_api_v2_route_CorsPolicy_new(upb_arena *arena) {
  return (envoy_api_v2_route_CorsPolicy *)_upb_msg_new(&envoy_api_v2_route_CorsPolicy_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_CorsPolicy *envoy_api_v2_route_CorsPolicy_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_CorsPolicy *ret = envoy_api_v2_route_CorsPolicy_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_CorsPolicy_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_CorsPolicy_serialize(const envoy_api_v2_route_CorsPolicy *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_CorsPolicy_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_CorsPolicy_enabled_specifier_enabled = 7,
  envoy_api_v2_route_CorsPolicy_enabled_specifier_filter_enabled = 9,
  envoy_api_v2_route_CorsPolicy_enabled_specifier_NOT_SET = 0
} envoy_api_v2_route_CorsPolicy_enabled_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_CorsPolicy_enabled_specifier_oneofcases envoy_api_v2_route_CorsPolicy_enabled_specifier_case(const envoy_api_v2_route_CorsPolicy* msg) { return (envoy_api_v2_route_CorsPolicy_enabled_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(56, 112)); }
UPB_INLINE upb_strview const* envoy_api_v2_route_CorsPolicy_allow_origin(const envoy_api_v2_route_CorsPolicy *msg, size_t *len) { return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(40, 80), len); }
UPB_INLINE upb_strview envoy_api_v2_route_CorsPolicy_allow_methods(const envoy_api_v2_route_CorsPolicy *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE upb_strview envoy_api_v2_route_CorsPolicy_allow_headers(const envoy_api_v2_route_CorsPolicy *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)); }
UPB_INLINE upb_strview envoy_api_v2_route_CorsPolicy_expose_headers(const envoy_api_v2_route_CorsPolicy *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 32)); }
UPB_INLINE upb_strview envoy_api_v2_route_CorsPolicy_max_age(const envoy_api_v2_route_CorsPolicy *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 48)); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_CorsPolicy_allow_credentials(const envoy_api_v2_route_CorsPolicy *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(32, 64)); }
UPB_INLINE bool envoy_api_v2_route_CorsPolicy_has_enabled(const envoy_api_v2_route_CorsPolicy *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(56, 112), 7); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_CorsPolicy_enabled(const envoy_api_v2_route_CorsPolicy *msg) { return UPB_READ_ONEOF(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(52, 104), UPB_SIZE(56, 112), 7, NULL); }
UPB_INLINE upb_strview const* envoy_api_v2_route_CorsPolicy_allow_origin_regex(const envoy_api_v2_route_CorsPolicy *msg, size_t *len) { return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(44, 88), len); }
UPB_INLINE bool envoy_api_v2_route_CorsPolicy_has_filter_enabled(const envoy_api_v2_route_CorsPolicy *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(56, 112), 9); }
UPB_INLINE const struct envoy_api_v2_core_RuntimeFractionalPercent* envoy_api_v2_route_CorsPolicy_filter_enabled(const envoy_api_v2_route_CorsPolicy *msg) { return UPB_READ_ONEOF(msg, const struct envoy_api_v2_core_RuntimeFractionalPercent*, UPB_SIZE(52, 104), UPB_SIZE(56, 112), 9, NULL); }
UPB_INLINE const struct envoy_api_v2_core_RuntimeFractionalPercent* envoy_api_v2_route_CorsPolicy_shadow_enabled(const envoy_api_v2_route_CorsPolicy *msg) { return UPB_FIELD_AT(msg, const struct envoy_api_v2_core_RuntimeFractionalPercent*, UPB_SIZE(36, 72)); }
UPB_INLINE const struct envoy_type_matcher_StringMatcher* const* envoy_api_v2_route_CorsPolicy_allow_origin_string_match(const envoy_api_v2_route_CorsPolicy *msg, size_t *len) { return (const struct envoy_type_matcher_StringMatcher* const*)_upb_array_accessor(msg, UPB_SIZE(48, 96), len); }
UPB_INLINE upb_strview* envoy_api_v2_route_CorsPolicy_mutable_allow_origin(envoy_api_v2_route_CorsPolicy *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(40, 80), len);
}
UPB_INLINE upb_strview* envoy_api_v2_route_CorsPolicy_resize_allow_origin(envoy_api_v2_route_CorsPolicy *msg, size_t len, upb_arena *arena) {
  return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(40, 80), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_route_CorsPolicy_add_allow_origin(envoy_api_v2_route_CorsPolicy *msg, upb_strview val, upb_arena *arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(40, 80), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val,
      arena);
}
UPB_INLINE void envoy_api_v2_route_CorsPolicy_set_allow_methods(envoy_api_v2_route_CorsPolicy *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_CorsPolicy_set_allow_headers(envoy_api_v2_route_CorsPolicy *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE void envoy_api_v2_route_CorsPolicy_set_expose_headers(envoy_api_v2_route_CorsPolicy *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 32)) = value;
}
UPB_INLINE void envoy_api_v2_route_CorsPolicy_set_max_age(envoy_api_v2_route_CorsPolicy *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 48)) = value;
}
UPB_INLINE void envoy_api_v2_route_CorsPolicy_set_allow_credentials(envoy_api_v2_route_CorsPolicy *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(32, 64)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_CorsPolicy_mutable_allow_credentials(envoy_api_v2_route_CorsPolicy *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_CorsPolicy_allow_credentials(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_CorsPolicy_set_allow_credentials(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_CorsPolicy_set_enabled(envoy_api_v2_route_CorsPolicy *msg, struct google_protobuf_BoolValue* value) {
  UPB_WRITE_ONEOF(msg, struct google_protobuf_BoolValue*, UPB_SIZE(52, 104), value, UPB_SIZE(56, 112), 7);
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_CorsPolicy_mutable_enabled(envoy_api_v2_route_CorsPolicy *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_CorsPolicy_enabled(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_CorsPolicy_set_enabled(msg, sub);
  }
  return sub;
}
UPB_INLINE upb_strview* envoy_api_v2_route_CorsPolicy_mutable_allow_origin_regex(envoy_api_v2_route_CorsPolicy *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(44, 88), len);
}
UPB_INLINE upb_strview* envoy_api_v2_route_CorsPolicy_resize_allow_origin_regex(envoy_api_v2_route_CorsPolicy *msg, size_t len, upb_arena *arena) {
  return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(44, 88), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_route_CorsPolicy_add_allow_origin_regex(envoy_api_v2_route_CorsPolicy *msg, upb_strview val, upb_arena *arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(44, 88), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val,
      arena);
}
UPB_INLINE void envoy_api_v2_route_CorsPolicy_set_filter_enabled(envoy_api_v2_route_CorsPolicy *msg, struct envoy_api_v2_core_RuntimeFractionalPercent* value) {
  UPB_WRITE_ONEOF(msg, struct envoy_api_v2_core_RuntimeFractionalPercent*, UPB_SIZE(52, 104), value, UPB_SIZE(56, 112), 9);
}
UPB_INLINE struct envoy_api_v2_core_RuntimeFractionalPercent* envoy_api_v2_route_CorsPolicy_mutable_filter_enabled(envoy_api_v2_route_CorsPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_core_RuntimeFractionalPercent* sub = (struct envoy_api_v2_core_RuntimeFractionalPercent*)envoy_api_v2_route_CorsPolicy_filter_enabled(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_RuntimeFractionalPercent*)_upb_msg_new(&envoy_api_v2_core_RuntimeFractionalPercent_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_CorsPolicy_set_filter_enabled(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_CorsPolicy_set_shadow_enabled(envoy_api_v2_route_CorsPolicy *msg, struct envoy_api_v2_core_RuntimeFractionalPercent* value) {
  UPB_FIELD_AT(msg, struct envoy_api_v2_core_RuntimeFractionalPercent*, UPB_SIZE(36, 72)) = value;
}
UPB_INLINE struct envoy_api_v2_core_RuntimeFractionalPercent* envoy_api_v2_route_CorsPolicy_mutable_shadow_enabled(envoy_api_v2_route_CorsPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_core_RuntimeFractionalPercent* sub = (struct envoy_api_v2_core_RuntimeFractionalPercent*)envoy_api_v2_route_CorsPolicy_shadow_enabled(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_RuntimeFractionalPercent*)_upb_msg_new(&envoy_api_v2_core_RuntimeFractionalPercent_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_CorsPolicy_set_shadow_enabled(msg, sub);
  }
  return sub;
}
UPB_INLINE struct envoy_type_matcher_StringMatcher** envoy_api_v2_route_CorsPolicy_mutable_allow_origin_string_match(envoy_api_v2_route_CorsPolicy *msg, size_t *len) {
  return (struct envoy_type_matcher_StringMatcher**)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 96), len);
}
UPB_INLINE struct envoy_type_matcher_StringMatcher** envoy_api_v2_route_CorsPolicy_resize_allow_origin_string_match(envoy_api_v2_route_CorsPolicy *msg, size_t len, upb_arena *arena) {
  return (struct envoy_type_matcher_StringMatcher**)_upb_array_resize_accessor(msg, UPB_SIZE(48, 96), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_type_matcher_StringMatcher* envoy_api_v2_route_CorsPolicy_add_allow_origin_string_match(envoy_api_v2_route_CorsPolicy *msg, upb_arena *arena) {
  struct envoy_type_matcher_StringMatcher* sub = (struct envoy_type_matcher_StringMatcher*)_upb_msg_new(&envoy_type_matcher_StringMatcher_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(48, 96), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_route_RouteAction *envoy_api_v2_route_RouteAction_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteAction *)_upb_msg_new(&envoy_api_v2_route_RouteAction_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteAction *envoy_api_v2_route_RouteAction_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteAction *ret = envoy_api_v2_route_RouteAction_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteAction_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteAction_serialize(const envoy_api_v2_route_RouteAction *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteAction_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_RouteAction_cluster_specifier_cluster = 1,
  envoy_api_v2_route_RouteAction_cluster_specifier_cluster_header = 2,
  envoy_api_v2_route_RouteAction_cluster_specifier_weighted_clusters = 3,
  envoy_api_v2_route_RouteAction_cluster_specifier_NOT_SET = 0
} envoy_api_v2_route_RouteAction_cluster_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_RouteAction_cluster_specifier_oneofcases envoy_api_v2_route_RouteAction_cluster_specifier_case(const envoy_api_v2_route_RouteAction* msg) { return (envoy_api_v2_route_RouteAction_cluster_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(108, 192)); }
typedef enum {
  envoy_api_v2_route_RouteAction_host_rewrite_specifier_host_rewrite = 6,
  envoy_api_v2_route_RouteAction_host_rewrite_specifier_auto_host_rewrite = 7,
  envoy_api_v2_route_RouteAction_host_rewrite_specifier_auto_host_rewrite_header = 29,
  envoy_api_v2_route_RouteAction_host_rewrite_specifier_NOT_SET = 0
} envoy_api_v2_route_RouteAction_host_rewrite_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_RouteAction_host_rewrite_specifier_oneofcases envoy_api_v2_route_RouteAction_host_rewrite_specifier_case(const envoy_api_v2_route_RouteAction* msg) { return (envoy_api_v2_route_RouteAction_host_rewrite_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(120, 216)); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_has_cluster(const envoy_api_v2_route_RouteAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(108, 192), 1); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_cluster(const envoy_api_v2_route_RouteAction *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(100, 176), UPB_SIZE(108, 192), 1, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_has_cluster_header(const envoy_api_v2_route_RouteAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(108, 192), 2); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_cluster_header(const envoy_api_v2_route_RouteAction *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(100, 176), UPB_SIZE(108, 192), 2, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_has_weighted_clusters(const envoy_api_v2_route_RouteAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(108, 192), 3); }
UPB_INLINE const envoy_api_v2_route_WeightedCluster* envoy_api_v2_route_RouteAction_weighted_clusters(const envoy_api_v2_route_RouteAction *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_WeightedCluster*, UPB_SIZE(100, 176), UPB_SIZE(108, 192), 3, NULL); }
UPB_INLINE const struct envoy_api_v2_core_Metadata* envoy_api_v2_route_RouteAction_metadata_match(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const struct envoy_api_v2_core_Metadata*, UPB_SIZE(32, 40)); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_prefix_rewrite(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 24)); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_has_host_rewrite(const envoy_api_v2_route_RouteAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(120, 216), 6); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_host_rewrite(const envoy_api_v2_route_RouteAction *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(112, 200), UPB_SIZE(120, 216), 6, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_has_auto_host_rewrite(const envoy_api_v2_route_RouteAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(120, 216), 7); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_RouteAction_auto_host_rewrite(const envoy_api_v2_route_RouteAction *msg) { return UPB_READ_ONEOF(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(112, 200), UPB_SIZE(120, 216), 7, NULL); }
UPB_INLINE const struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_timeout(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Duration*, UPB_SIZE(36, 48)); }
UPB_INLINE const envoy_api_v2_route_RetryPolicy* envoy_api_v2_route_RouteAction_retry_policy(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_RetryPolicy*, UPB_SIZE(40, 56)); }
UPB_INLINE const envoy_api_v2_route_RouteAction_RequestMirrorPolicy* envoy_api_v2_route_RouteAction_request_mirror_policy(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_RouteAction_RequestMirrorPolicy*, UPB_SIZE(44, 64)); }
UPB_INLINE int32_t envoy_api_v2_route_RouteAction_priority(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(0, 0)); }
UPB_INLINE const envoy_api_v2_route_RateLimit* const* envoy_api_v2_route_RouteAction_rate_limits(const envoy_api_v2_route_RouteAction *msg, size_t *len) { return (const envoy_api_v2_route_RateLimit* const*)_upb_array_accessor(msg, UPB_SIZE(84, 144), len); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_RouteAction_include_vh_rate_limits(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(48, 72)); }
UPB_INLINE const envoy_api_v2_route_RouteAction_HashPolicy* const* envoy_api_v2_route_RouteAction_hash_policy(const envoy_api_v2_route_RouteAction *msg, size_t *len) { return (const envoy_api_v2_route_RouteAction_HashPolicy* const*)_upb_array_accessor(msg, UPB_SIZE(88, 152), len); }
UPB_INLINE const envoy_api_v2_route_CorsPolicy* envoy_api_v2_route_RouteAction_cors(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_CorsPolicy*, UPB_SIZE(52, 80)); }
UPB_INLINE int32_t envoy_api_v2_route_RouteAction_cluster_not_found_response_code(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 8)); }
UPB_INLINE const struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_max_grpc_timeout(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Duration*, UPB_SIZE(56, 88)); }
UPB_INLINE const struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_idle_timeout(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Duration*, UPB_SIZE(60, 96)); }
UPB_INLINE const envoy_api_v2_route_RouteAction_UpgradeConfig* const* envoy_api_v2_route_RouteAction_upgrade_configs(const envoy_api_v2_route_RouteAction *msg, size_t *len) { return (const envoy_api_v2_route_RouteAction_UpgradeConfig* const*)_upb_array_accessor(msg, UPB_SIZE(92, 160), len); }
UPB_INLINE int32_t envoy_api_v2_route_RouteAction_internal_redirect_action(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16)); }
UPB_INLINE const envoy_api_v2_route_HedgePolicy* envoy_api_v2_route_RouteAction_hedge_policy(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_HedgePolicy*, UPB_SIZE(64, 104)); }
UPB_INLINE const struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_grpc_timeout_offset(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Duration*, UPB_SIZE(68, 112)); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_has_auto_host_rewrite_header(const envoy_api_v2_route_RouteAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(120, 216), 29); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_auto_host_rewrite_header(const envoy_api_v2_route_RouteAction *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(112, 200), UPB_SIZE(120, 216), 29, upb_strview_make("", strlen(""))); }
UPB_INLINE const envoy_api_v2_route_RouteAction_RequestMirrorPolicy* const* envoy_api_v2_route_RouteAction_request_mirror_policies(const envoy_api_v2_route_RouteAction *msg, size_t *len) { return (const envoy_api_v2_route_RouteAction_RequestMirrorPolicy* const*)_upb_array_accessor(msg, UPB_SIZE(96, 168), len); }
UPB_INLINE const struct google_protobuf_UInt32Value* envoy_api_v2_route_RouteAction_max_internal_redirects(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_UInt32Value*, UPB_SIZE(72, 120)); }
UPB_INLINE const struct envoy_type_matcher_RegexMatchAndSubstitute* envoy_api_v2_route_RouteAction_regex_rewrite(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const struct envoy_type_matcher_RegexMatchAndSubstitute*, UPB_SIZE(76, 128)); }
UPB_INLINE const struct google_protobuf_Any* envoy_api_v2_route_RouteAction_retry_policy_typed_config(const envoy_api_v2_route_RouteAction *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Any*, UPB_SIZE(80, 136)); }
UPB_INLINE void envoy_api_v2_route_RouteAction_set_cluster(envoy_api_v2_route_RouteAction *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(100, 176), value, UPB_SIZE(108, 192), 1);
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_cluster_header(envoy_api_v2_route_RouteAction *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(100, 176), value, UPB_SIZE(108, 192), 2);
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_weighted_clusters(envoy_api_v2_route_RouteAction *msg, envoy_api_v2_route_WeightedCluster* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_WeightedCluster*, UPB_SIZE(100, 176), value, UPB_SIZE(108, 192), 3);
}
UPB_INLINE struct envoy_api_v2_route_WeightedCluster* envoy_api_v2_route_RouteAction_mutable_weighted_clusters(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_route_WeightedCluster* sub = (struct envoy_api_v2_route_WeightedCluster*)envoy_api_v2_route_RouteAction_weighted_clusters(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_WeightedCluster*)_upb_msg_new(&envoy_api_v2_route_WeightedCluster_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_weighted_clusters(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_metadata_match(envoy_api_v2_route_RouteAction *msg, struct envoy_api_v2_core_Metadata* value) {
  UPB_FIELD_AT(msg, struct envoy_api_v2_core_Metadata*, UPB_SIZE(32, 40)) = value;
}
UPB_INLINE struct envoy_api_v2_core_Metadata* envoy_api_v2_route_RouteAction_mutable_metadata_match(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_core_Metadata* sub = (struct envoy_api_v2_core_Metadata*)envoy_api_v2_route_RouteAction_metadata_match(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_Metadata*)_upb_msg_new(&envoy_api_v2_core_Metadata_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_metadata_match(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_prefix_rewrite(envoy_api_v2_route_RouteAction *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_host_rewrite(envoy_api_v2_route_RouteAction *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(112, 200), value, UPB_SIZE(120, 216), 6);
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_auto_host_rewrite(envoy_api_v2_route_RouteAction *msg, struct google_protobuf_BoolValue* value) {
  UPB_WRITE_ONEOF(msg, struct google_protobuf_BoolValue*, UPB_SIZE(112, 200), value, UPB_SIZE(120, 216), 7);
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_RouteAction_mutable_auto_host_rewrite(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_RouteAction_auto_host_rewrite(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_auto_host_rewrite(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_timeout(envoy_api_v2_route_RouteAction *msg, struct google_protobuf_Duration* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Duration*, UPB_SIZE(36, 48)) = value;
}
UPB_INLINE struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_mutable_timeout(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct google_protobuf_Duration* sub = (struct google_protobuf_Duration*)envoy_api_v2_route_RouteAction_timeout(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Duration*)_upb_msg_new(&google_protobuf_Duration_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_timeout(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_retry_policy(envoy_api_v2_route_RouteAction *msg, envoy_api_v2_route_RetryPolicy* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_RetryPolicy*, UPB_SIZE(40, 56)) = value;
}
UPB_INLINE struct envoy_api_v2_route_RetryPolicy* envoy_api_v2_route_RouteAction_mutable_retry_policy(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RetryPolicy* sub = (struct envoy_api_v2_route_RetryPolicy*)envoy_api_v2_route_RouteAction_retry_policy(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RetryPolicy*)_upb_msg_new(&envoy_api_v2_route_RetryPolicy_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_retry_policy(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_request_mirror_policy(envoy_api_v2_route_RouteAction *msg, envoy_api_v2_route_RouteAction_RequestMirrorPolicy* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_RouteAction_RequestMirrorPolicy*, UPB_SIZE(44, 64)) = value;
}
UPB_INLINE struct envoy_api_v2_route_RouteAction_RequestMirrorPolicy* envoy_api_v2_route_RouteAction_mutable_request_mirror_policy(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction_RequestMirrorPolicy* sub = (struct envoy_api_v2_route_RouteAction_RequestMirrorPolicy*)envoy_api_v2_route_RouteAction_request_mirror_policy(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteAction_RequestMirrorPolicy*)_upb_msg_new(&envoy_api_v2_route_RouteAction_RequestMirrorPolicy_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_request_mirror_policy(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_priority(envoy_api_v2_route_RouteAction *msg, int32_t value) {
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_RateLimit** envoy_api_v2_route_RouteAction_mutable_rate_limits(envoy_api_v2_route_RouteAction *msg, size_t *len) {
  return (envoy_api_v2_route_RateLimit**)_upb_array_mutable_accessor(msg, UPB_SIZE(84, 144), len);
}
UPB_INLINE envoy_api_v2_route_RateLimit** envoy_api_v2_route_RouteAction_resize_rate_limits(envoy_api_v2_route_RouteAction *msg, size_t len, upb_arena *arena) {
return (envoy_api_v2_route_RateLimit**)_upb_array_resize_accessor(msg, UPB_SIZE(84, 144), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_RateLimit* envoy_api_v2_route_RouteAction_add_rate_limits(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RateLimit* sub = (struct envoy_api_v2_route_RateLimit*)_upb_msg_new(&envoy_api_v2_route_RateLimit_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(84, 144), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_include_vh_rate_limits(envoy_api_v2_route_RouteAction *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(48, 72)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_RouteAction_mutable_include_vh_rate_limits(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_RouteAction_include_vh_rate_limits(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_include_vh_rate_limits(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy** envoy_api_v2_route_RouteAction_mutable_hash_policy(envoy_api_v2_route_RouteAction *msg, size_t *len) {
  return (envoy_api_v2_route_RouteAction_HashPolicy**)_upb_array_mutable_accessor(msg, UPB_SIZE(88, 152), len);
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy** envoy_api_v2_route_RouteAction_resize_hash_policy(envoy_api_v2_route_RouteAction *msg, size_t len, upb_arena *arena) {
return (envoy_api_v2_route_RouteAction_HashPolicy**)_upb_array_resize_accessor(msg, UPB_SIZE(88, 152), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_RouteAction_HashPolicy* envoy_api_v2_route_RouteAction_add_hash_policy(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction_HashPolicy* sub = (struct envoy_api_v2_route_RouteAction_HashPolicy*)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(88, 152), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_cors(envoy_api_v2_route_RouteAction *msg, envoy_api_v2_route_CorsPolicy* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_CorsPolicy*, UPB_SIZE(52, 80)) = value;
}
UPB_INLINE struct envoy_api_v2_route_CorsPolicy* envoy_api_v2_route_RouteAction_mutable_cors(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_route_CorsPolicy* sub = (struct envoy_api_v2_route_CorsPolicy*)envoy_api_v2_route_RouteAction_cors(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_CorsPolicy*)_upb_msg_new(&envoy_api_v2_route_CorsPolicy_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_cors(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_cluster_not_found_response_code(envoy_api_v2_route_RouteAction *msg, int32_t value) {
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_max_grpc_timeout(envoy_api_v2_route_RouteAction *msg, struct google_protobuf_Duration* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Duration*, UPB_SIZE(56, 88)) = value;
}
UPB_INLINE struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_mutable_max_grpc_timeout(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct google_protobuf_Duration* sub = (struct google_protobuf_Duration*)envoy_api_v2_route_RouteAction_max_grpc_timeout(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Duration*)_upb_msg_new(&google_protobuf_Duration_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_max_grpc_timeout(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_idle_timeout(envoy_api_v2_route_RouteAction *msg, struct google_protobuf_Duration* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Duration*, UPB_SIZE(60, 96)) = value;
}
UPB_INLINE struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_mutable_idle_timeout(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct google_protobuf_Duration* sub = (struct google_protobuf_Duration*)envoy_api_v2_route_RouteAction_idle_timeout(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Duration*)_upb_msg_new(&google_protobuf_Duration_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_idle_timeout(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RouteAction_UpgradeConfig** envoy_api_v2_route_RouteAction_mutable_upgrade_configs(envoy_api_v2_route_RouteAction *msg, size_t *len) {
  return (envoy_api_v2_route_RouteAction_UpgradeConfig**)_upb_array_mutable_accessor(msg, UPB_SIZE(92, 160), len);
}
UPB_INLINE envoy_api_v2_route_RouteAction_UpgradeConfig** envoy_api_v2_route_RouteAction_resize_upgrade_configs(envoy_api_v2_route_RouteAction *msg, size_t len, upb_arena *arena) {
return (envoy_api_v2_route_RouteAction_UpgradeConfig**)_upb_array_resize_accessor(msg, UPB_SIZE(92, 160), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_RouteAction_UpgradeConfig* envoy_api_v2_route_RouteAction_add_upgrade_configs(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction_UpgradeConfig* sub = (struct envoy_api_v2_route_RouteAction_UpgradeConfig*)_upb_msg_new(&envoy_api_v2_route_RouteAction_UpgradeConfig_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(92, 160), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_internal_redirect_action(envoy_api_v2_route_RouteAction *msg, int32_t value) {
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_hedge_policy(envoy_api_v2_route_RouteAction *msg, envoy_api_v2_route_HedgePolicy* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_HedgePolicy*, UPB_SIZE(64, 104)) = value;
}
UPB_INLINE struct envoy_api_v2_route_HedgePolicy* envoy_api_v2_route_RouteAction_mutable_hedge_policy(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_route_HedgePolicy* sub = (struct envoy_api_v2_route_HedgePolicy*)envoy_api_v2_route_RouteAction_hedge_policy(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_HedgePolicy*)_upb_msg_new(&envoy_api_v2_route_HedgePolicy_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_hedge_policy(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_grpc_timeout_offset(envoy_api_v2_route_RouteAction *msg, struct google_protobuf_Duration* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Duration*, UPB_SIZE(68, 112)) = value;
}
UPB_INLINE struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_mutable_grpc_timeout_offset(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct google_protobuf_Duration* sub = (struct google_protobuf_Duration*)envoy_api_v2_route_RouteAction_grpc_timeout_offset(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Duration*)_upb_msg_new(&google_protobuf_Duration_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_grpc_timeout_offset(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_auto_host_rewrite_header(envoy_api_v2_route_RouteAction *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(112, 200), value, UPB_SIZE(120, 216), 29);
}
UPB_INLINE envoy_api_v2_route_RouteAction_RequestMirrorPolicy** envoy_api_v2_route_RouteAction_mutable_request_mirror_policies(envoy_api_v2_route_RouteAction *msg, size_t *len) {
  return (envoy_api_v2_route_RouteAction_RequestMirrorPolicy**)_upb_array_mutable_accessor(msg, UPB_SIZE(96, 168), len);
}
UPB_INLINE envoy_api_v2_route_RouteAction_RequestMirrorPolicy** envoy_api_v2_route_RouteAction_resize_request_mirror_policies(envoy_api_v2_route_RouteAction *msg, size_t len, upb_arena *arena) {
return (envoy_api_v2_route_RouteAction_RequestMirrorPolicy**)_upb_array_resize_accessor(msg, UPB_SIZE(96, 168), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_RouteAction_RequestMirrorPolicy* envoy_api_v2_route_RouteAction_add_request_mirror_policies(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction_RequestMirrorPolicy* sub = (struct envoy_api_v2_route_RouteAction_RequestMirrorPolicy*)_upb_msg_new(&envoy_api_v2_route_RouteAction_RequestMirrorPolicy_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(96, 168), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_max_internal_redirects(envoy_api_v2_route_RouteAction *msg, struct google_protobuf_UInt32Value* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_UInt32Value*, UPB_SIZE(72, 120)) = value;
}
UPB_INLINE struct google_protobuf_UInt32Value* envoy_api_v2_route_RouteAction_mutable_max_internal_redirects(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct google_protobuf_UInt32Value* sub = (struct google_protobuf_UInt32Value*)envoy_api_v2_route_RouteAction_max_internal_redirects(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_UInt32Value*)_upb_msg_new(&google_protobuf_UInt32Value_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_max_internal_redirects(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_regex_rewrite(envoy_api_v2_route_RouteAction *msg, struct envoy_type_matcher_RegexMatchAndSubstitute* value) {
  UPB_FIELD_AT(msg, struct envoy_type_matcher_RegexMatchAndSubstitute*, UPB_SIZE(76, 128)) = value;
}
UPB_INLINE struct envoy_type_matcher_RegexMatchAndSubstitute* envoy_api_v2_route_RouteAction_mutable_regex_rewrite(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct envoy_type_matcher_RegexMatchAndSubstitute* sub = (struct envoy_type_matcher_RegexMatchAndSubstitute*)envoy_api_v2_route_RouteAction_regex_rewrite(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_matcher_RegexMatchAndSubstitute*)upb_msg_new(&envoy_type_matcher_RegexMatchAndSubstitute_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_regex_rewrite(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_set_retry_policy_typed_config(envoy_api_v2_route_RouteAction *msg, struct google_protobuf_Any* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Any*, UPB_SIZE(80, 136)) = value;
}
UPB_INLINE struct google_protobuf_Any* envoy_api_v2_route_RouteAction_mutable_retry_policy_typed_config(envoy_api_v2_route_RouteAction *msg, upb_arena *arena) {
  struct google_protobuf_Any* sub = (struct google_protobuf_Any*)envoy_api_v2_route_RouteAction_retry_policy_typed_config(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Any*)upb_msg_new(&google_protobuf_Any_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_set_retry_policy_typed_config(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RouteAction_RequestMirrorPolicy *envoy_api_v2_route_RouteAction_RequestMirrorPolicy_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteAction_RequestMirrorPolicy *)_upb_msg_new(&envoy_api_v2_route_RouteAction_RequestMirrorPolicy_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteAction_RequestMirrorPolicy *envoy_api_v2_route_RouteAction_RequestMirrorPolicy_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteAction_RequestMirrorPolicy *ret = envoy_api_v2_route_RouteAction_RequestMirrorPolicy_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteAction_RequestMirrorPolicy_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteAction_RequestMirrorPolicy_serialize(const envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteAction_RequestMirrorPolicy_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_RequestMirrorPolicy_cluster(const envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_RequestMirrorPolicy_runtime_key(const envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)); }
UPB_INLINE const struct envoy_api_v2_core_RuntimeFractionalPercent* envoy_api_v2_route_RouteAction_RequestMirrorPolicy_runtime_fraction(const envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg) { return UPB_FIELD_AT(msg, const struct envoy_api_v2_core_RuntimeFractionalPercent*, UPB_SIZE(16, 32)); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_RouteAction_RequestMirrorPolicy_trace_sampled(const envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(20, 40)); }
UPB_INLINE void envoy_api_v2_route_RouteAction_RequestMirrorPolicy_set_cluster(envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_RequestMirrorPolicy_set_runtime_key(envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_RequestMirrorPolicy_set_runtime_fraction(envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg, struct envoy_api_v2_core_RuntimeFractionalPercent* value) {
  UPB_FIELD_AT(msg, struct envoy_api_v2_core_RuntimeFractionalPercent*, UPB_SIZE(16, 32)) = value;
}
UPB_INLINE struct envoy_api_v2_core_RuntimeFractionalPercent* envoy_api_v2_route_RouteAction_RequestMirrorPolicy_mutable_runtime_fraction(envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_core_RuntimeFractionalPercent* sub = (struct envoy_api_v2_core_RuntimeFractionalPercent*)envoy_api_v2_route_RouteAction_RequestMirrorPolicy_runtime_fraction(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_RuntimeFractionalPercent*)_upb_msg_new(&envoy_api_v2_core_RuntimeFractionalPercent_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_RequestMirrorPolicy_set_runtime_fraction(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_RequestMirrorPolicy_set_trace_sampled(envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(20, 40)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_RouteAction_RequestMirrorPolicy_mutable_trace_sampled(envoy_api_v2_route_RouteAction_RequestMirrorPolicy *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_RouteAction_RequestMirrorPolicy_trace_sampled(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_RequestMirrorPolicy_set_trace_sampled(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy *envoy_api_v2_route_RouteAction_HashPolicy_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteAction_HashPolicy *)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy *envoy_api_v2_route_RouteAction_HashPolicy_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteAction_HashPolicy *ret = envoy_api_v2_route_RouteAction_HashPolicy_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteAction_HashPolicy_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteAction_HashPolicy_serialize(const envoy_api_v2_route_RouteAction_HashPolicy *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteAction_HashPolicy_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_header = 1,
  envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_cookie = 2,
  envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_connection_properties = 3,
  envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_query_parameter = 5,
  envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_filter_state = 6,
  envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_NOT_SET = 0
} envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_oneofcases envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_case(const envoy_api_v2_route_RouteAction_HashPolicy* msg) { return (envoy_api_v2_route_RouteAction_HashPolicy_policy_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 16)); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_HashPolicy_has_header(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(8, 16), 1); }
UPB_INLINE const envoy_api_v2_route_RouteAction_HashPolicy_Header* envoy_api_v2_route_RouteAction_HashPolicy_header(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RouteAction_HashPolicy_Header*, UPB_SIZE(4, 8), UPB_SIZE(8, 16), 1, NULL); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_HashPolicy_has_cookie(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(8, 16), 2); }
UPB_INLINE const envoy_api_v2_route_RouteAction_HashPolicy_Cookie* envoy_api_v2_route_RouteAction_HashPolicy_cookie(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RouteAction_HashPolicy_Cookie*, UPB_SIZE(4, 8), UPB_SIZE(8, 16), 2, NULL); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_HashPolicy_has_connection_properties(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(8, 16), 3); }
UPB_INLINE const envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties* envoy_api_v2_route_RouteAction_HashPolicy_connection_properties(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties*, UPB_SIZE(4, 8), UPB_SIZE(8, 16), 3, NULL); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_HashPolicy_terminal(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_HashPolicy_has_query_parameter(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(8, 16), 5); }
UPB_INLINE const envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter* envoy_api_v2_route_RouteAction_HashPolicy_query_parameter(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter*, UPB_SIZE(4, 8), UPB_SIZE(8, 16), 5, NULL); }
UPB_INLINE bool envoy_api_v2_route_RouteAction_HashPolicy_has_filter_state(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(8, 16), 6); }
UPB_INLINE const envoy_api_v2_route_RouteAction_HashPolicy_FilterState* envoy_api_v2_route_RouteAction_HashPolicy_filter_state(const envoy_api_v2_route_RouteAction_HashPolicy *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RouteAction_HashPolicy_FilterState*, UPB_SIZE(4, 8), UPB_SIZE(8, 16), 6, NULL); }
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_set_header(envoy_api_v2_route_RouteAction_HashPolicy *msg, envoy_api_v2_route_RouteAction_HashPolicy_Header* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RouteAction_HashPolicy_Header*, UPB_SIZE(4, 8), value, UPB_SIZE(8, 16), 1);
}
UPB_INLINE struct envoy_api_v2_route_RouteAction_HashPolicy_Header* envoy_api_v2_route_RouteAction_HashPolicy_mutable_header(envoy_api_v2_route_RouteAction_HashPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction_HashPolicy_Header* sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_Header*)envoy_api_v2_route_RouteAction_HashPolicy_header(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_Header*)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_Header_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_HashPolicy_set_header(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_set_cookie(envoy_api_v2_route_RouteAction_HashPolicy *msg, envoy_api_v2_route_RouteAction_HashPolicy_Cookie* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RouteAction_HashPolicy_Cookie*, UPB_SIZE(4, 8), value, UPB_SIZE(8, 16), 2);
}
UPB_INLINE struct envoy_api_v2_route_RouteAction_HashPolicy_Cookie* envoy_api_v2_route_RouteAction_HashPolicy_mutable_cookie(envoy_api_v2_route_RouteAction_HashPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction_HashPolicy_Cookie* sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_Cookie*)envoy_api_v2_route_RouteAction_HashPolicy_cookie(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_Cookie*)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_Cookie_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_HashPolicy_set_cookie(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_set_connection_properties(envoy_api_v2_route_RouteAction_HashPolicy *msg, envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties*, UPB_SIZE(4, 8), value, UPB_SIZE(8, 16), 3);
}
UPB_INLINE struct envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties* envoy_api_v2_route_RouteAction_HashPolicy_mutable_connection_properties(envoy_api_v2_route_RouteAction_HashPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties* sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties*)envoy_api_v2_route_RouteAction_HashPolicy_connection_properties(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties*)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_HashPolicy_set_connection_properties(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_set_terminal(envoy_api_v2_route_RouteAction_HashPolicy *msg, bool value) {
  UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_set_query_parameter(envoy_api_v2_route_RouteAction_HashPolicy *msg, envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter*, UPB_SIZE(4, 8), value, UPB_SIZE(8, 16), 5);
}
UPB_INLINE struct envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter* envoy_api_v2_route_RouteAction_HashPolicy_mutable_query_parameter(envoy_api_v2_route_RouteAction_HashPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter* sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter*)envoy_api_v2_route_RouteAction_HashPolicy_query_parameter(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter*)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_HashPolicy_set_query_parameter(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_set_filter_state(envoy_api_v2_route_RouteAction_HashPolicy *msg, envoy_api_v2_route_RouteAction_HashPolicy_FilterState* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RouteAction_HashPolicy_FilterState*, UPB_SIZE(4, 8), value, UPB_SIZE(8, 16), 6);
}
UPB_INLINE struct envoy_api_v2_route_RouteAction_HashPolicy_FilterState* envoy_api_v2_route_RouteAction_HashPolicy_mutable_filter_state(envoy_api_v2_route_RouteAction_HashPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RouteAction_HashPolicy_FilterState* sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_FilterState*)envoy_api_v2_route_RouteAction_HashPolicy_filter_state(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RouteAction_HashPolicy_FilterState*)upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_FilterState_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_HashPolicy_set_filter_state(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_Header *envoy_api_v2_route_RouteAction_HashPolicy_Header_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteAction_HashPolicy_Header *)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_Header_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_Header *envoy_api_v2_route_RouteAction_HashPolicy_Header_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteAction_HashPolicy_Header *ret = envoy_api_v2_route_RouteAction_HashPolicy_Header_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteAction_HashPolicy_Header_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteAction_HashPolicy_Header_serialize(const envoy_api_v2_route_RouteAction_HashPolicy_Header *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteAction_HashPolicy_Header_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_HashPolicy_Header_header_name(const envoy_api_v2_route_RouteAction_HashPolicy_Header *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_Header_set_header_name(envoy_api_v2_route_RouteAction_HashPolicy_Header *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_Cookie *envoy_api_v2_route_RouteAction_HashPolicy_Cookie_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteAction_HashPolicy_Cookie *)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_Cookie_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_Cookie *envoy_api_v2_route_RouteAction_HashPolicy_Cookie_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteAction_HashPolicy_Cookie *ret = envoy_api_v2_route_RouteAction_HashPolicy_Cookie_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteAction_HashPolicy_Cookie_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteAction_HashPolicy_Cookie_serialize(const envoy_api_v2_route_RouteAction_HashPolicy_Cookie *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteAction_HashPolicy_Cookie_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_HashPolicy_Cookie_name(const envoy_api_v2_route_RouteAction_HashPolicy_Cookie *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_HashPolicy_Cookie_ttl(const envoy_api_v2_route_RouteAction_HashPolicy_Cookie *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Duration*, UPB_SIZE(16, 32)); }
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_HashPolicy_Cookie_path(const envoy_api_v2_route_RouteAction_HashPolicy_Cookie *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)); }
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_Cookie_set_name(envoy_api_v2_route_RouteAction_HashPolicy_Cookie *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_Cookie_set_ttl(envoy_api_v2_route_RouteAction_HashPolicy_Cookie *msg, struct google_protobuf_Duration* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Duration*, UPB_SIZE(16, 32)) = value;
}
UPB_INLINE struct google_protobuf_Duration* envoy_api_v2_route_RouteAction_HashPolicy_Cookie_mutable_ttl(envoy_api_v2_route_RouteAction_HashPolicy_Cookie *msg, upb_arena *arena) {
  struct google_protobuf_Duration* sub = (struct google_protobuf_Duration*)envoy_api_v2_route_RouteAction_HashPolicy_Cookie_ttl(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Duration*)_upb_msg_new(&google_protobuf_Duration_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_HashPolicy_Cookie_set_ttl(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_Cookie_set_path(envoy_api_v2_route_RouteAction_HashPolicy_Cookie *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties *envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties *)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties *envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties *ret = envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_serialize(const envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_msginit, arena, len);
}
UPB_INLINE bool envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_source_ip(const envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties *msg) { return UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties_set_source_ip(envoy_api_v2_route_RouteAction_HashPolicy_ConnectionProperties *msg, bool value) {
  UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter *envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter *)_upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter *envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter *ret = envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_serialize(const envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_name(const envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter_set_name(envoy_api_v2_route_RouteAction_HashPolicy_QueryParameter *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_FilterState *envoy_api_v2_route_RouteAction_HashPolicy_FilterState_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteAction_HashPolicy_FilterState *)upb_msg_new(&envoy_api_v2_route_RouteAction_HashPolicy_FilterState_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteAction_HashPolicy_FilterState *envoy_api_v2_route_RouteAction_HashPolicy_FilterState_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteAction_HashPolicy_FilterState *ret = envoy_api_v2_route_RouteAction_HashPolicy_FilterState_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteAction_HashPolicy_FilterState_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteAction_HashPolicy_FilterState_serialize(const envoy_api_v2_route_RouteAction_HashPolicy_FilterState *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteAction_HashPolicy_FilterState_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_HashPolicy_FilterState_key(const envoy_api_v2_route_RouteAction_HashPolicy_FilterState *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_route_RouteAction_HashPolicy_FilterState_set_key(envoy_api_v2_route_RouteAction_HashPolicy_FilterState *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_RouteAction_UpgradeConfig *envoy_api_v2_route_RouteAction_UpgradeConfig_new(upb_arena *arena) {
  return (envoy_api_v2_route_RouteAction_UpgradeConfig *)_upb_msg_new(&envoy_api_v2_route_RouteAction_UpgradeConfig_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RouteAction_UpgradeConfig *envoy_api_v2_route_RouteAction_UpgradeConfig_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RouteAction_UpgradeConfig *ret = envoy_api_v2_route_RouteAction_UpgradeConfig_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RouteAction_UpgradeConfig_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RouteAction_UpgradeConfig_serialize(const envoy_api_v2_route_RouteAction_UpgradeConfig *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RouteAction_UpgradeConfig_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RouteAction_UpgradeConfig_upgrade_type(const envoy_api_v2_route_RouteAction_UpgradeConfig *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_RouteAction_UpgradeConfig_enabled(const envoy_api_v2_route_RouteAction_UpgradeConfig *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(8, 16)); }
UPB_INLINE void envoy_api_v2_route_RouteAction_UpgradeConfig_set_upgrade_type(envoy_api_v2_route_RouteAction_UpgradeConfig *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_RouteAction_UpgradeConfig_set_enabled(envoy_api_v2_route_RouteAction_UpgradeConfig *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_RouteAction_UpgradeConfig_mutable_enabled(envoy_api_v2_route_RouteAction_UpgradeConfig *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_RouteAction_UpgradeConfig_enabled(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RouteAction_UpgradeConfig_set_enabled(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RetryPolicy *envoy_api_v2_route_RetryPolicy_new(upb_arena *arena) {
  return (envoy_api_v2_route_RetryPolicy *)_upb_msg_new(&envoy_api_v2_route_RetryPolicy_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RetryPolicy *envoy_api_v2_route_RetryPolicy_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RetryPolicy *ret = envoy_api_v2_route_RetryPolicy_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RetryPolicy_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RetryPolicy_serialize(const envoy_api_v2_route_RetryPolicy *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RetryPolicy_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RetryPolicy_retry_on(const envoy_api_v2_route_RetryPolicy *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 8)); }
UPB_INLINE const struct google_protobuf_UInt32Value* envoy_api_v2_route_RetryPolicy_num_retries(const envoy_api_v2_route_RetryPolicy *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_UInt32Value*, UPB_SIZE(16, 24)); }
UPB_INLINE const struct google_protobuf_Duration* envoy_api_v2_route_RetryPolicy_per_try_timeout(const envoy_api_v2_route_RetryPolicy *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Duration*, UPB_SIZE(20, 32)); }
UPB_INLINE const envoy_api_v2_route_RetryPolicy_RetryPriority* envoy_api_v2_route_RetryPolicy_retry_priority(const envoy_api_v2_route_RetryPolicy *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_RetryPolicy_RetryPriority*, UPB_SIZE(24, 40)); }
UPB_INLINE const envoy_api_v2_route_RetryPolicy_RetryHostPredicate* const* envoy_api_v2_route_RetryPolicy_retry_host_predicate(const envoy_api_v2_route_RetryPolicy *msg, size_t *len) { return (const envoy_api_v2_route_RetryPolicy_RetryHostPredicate* const*)_upb_array_accessor(msg, UPB_SIZE(32, 56), len); }
UPB_INLINE int64_t envoy_api_v2_route_RetryPolicy_host_selection_retry_max_attempts(const envoy_api_v2_route_RetryPolicy *msg) { return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(0, 0)); }
UPB_INLINE uint32_t const* envoy_api_v2_route_RetryPolicy_retriable_status_codes(const envoy_api_v2_route_RetryPolicy *msg, size_t *len) { return (uint32_t const*)_upb_array_accessor(msg, UPB_SIZE(36, 64), len); }
UPB_INLINE const envoy_api_v2_route_RetryPolicy_RetryBackOff* envoy_api_v2_route_RetryPolicy_retry_back_off(const envoy_api_v2_route_RetryPolicy *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_route_RetryPolicy_RetryBackOff*, UPB_SIZE(28, 48)); }
UPB_INLINE const envoy_api_v2_route_HeaderMatcher* const* envoy_api_v2_route_RetryPolicy_retriable_headers(const envoy_api_v2_route_RetryPolicy *msg, size_t *len) { return (const envoy_api_v2_route_HeaderMatcher* const*)_upb_array_accessor(msg, UPB_SIZE(40, 72), len); }
UPB_INLINE const envoy_api_v2_route_HeaderMatcher* const* envoy_api_v2_route_RetryPolicy_retriable_request_headers(const envoy_api_v2_route_RetryPolicy *msg, size_t *len) { return (const envoy_api_v2_route_HeaderMatcher* const*)_upb_array_accessor(msg, UPB_SIZE(44, 80), len); }
UPB_INLINE void envoy_api_v2_route_RetryPolicy_set_retry_on(envoy_api_v2_route_RetryPolicy *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_set_num_retries(envoy_api_v2_route_RetryPolicy *msg, struct google_protobuf_UInt32Value* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_UInt32Value*, UPB_SIZE(16, 24)) = value;
}
UPB_INLINE struct google_protobuf_UInt32Value* envoy_api_v2_route_RetryPolicy_mutable_num_retries(envoy_api_v2_route_RetryPolicy *msg, upb_arena *arena) {
  struct google_protobuf_UInt32Value* sub = (struct google_protobuf_UInt32Value*)envoy_api_v2_route_RetryPolicy_num_retries(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_UInt32Value*)_upb_msg_new(&google_protobuf_UInt32Value_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_set_num_retries(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_set_per_try_timeout(envoy_api_v2_route_RetryPolicy *msg, struct google_protobuf_Duration* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Duration*, UPB_SIZE(20, 32)) = value;
}
UPB_INLINE struct google_protobuf_Duration* envoy_api_v2_route_RetryPolicy_mutable_per_try_timeout(envoy_api_v2_route_RetryPolicy *msg, upb_arena *arena) {
  struct google_protobuf_Duration* sub = (struct google_protobuf_Duration*)envoy_api_v2_route_RetryPolicy_per_try_timeout(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Duration*)_upb_msg_new(&google_protobuf_Duration_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_set_per_try_timeout(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_set_retry_priority(envoy_api_v2_route_RetryPolicy *msg, envoy_api_v2_route_RetryPolicy_RetryPriority* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_RetryPolicy_RetryPriority*, UPB_SIZE(24, 40)) = value;
}
UPB_INLINE struct envoy_api_v2_route_RetryPolicy_RetryPriority* envoy_api_v2_route_RetryPolicy_mutable_retry_priority(envoy_api_v2_route_RetryPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RetryPolicy_RetryPriority* sub = (struct envoy_api_v2_route_RetryPolicy_RetryPriority*)envoy_api_v2_route_RetryPolicy_retry_priority(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RetryPolicy_RetryPriority*)_upb_msg_new(&envoy_api_v2_route_RetryPolicy_RetryPriority_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_set_retry_priority(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryHostPredicate** envoy_api_v2_route_RetryPolicy_mutable_retry_host_predicate(envoy_api_v2_route_RetryPolicy *msg, size_t *len) {
  return (envoy_api_v2_route_RetryPolicy_RetryHostPredicate**)_upb_array_mutable_accessor(msg, UPB_SIZE(32, 56), len);
}
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryHostPredicate** envoy_api_v2_route_RetryPolicy_resize_retry_host_predicate(envoy_api_v2_route_RetryPolicy *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_RetryPolicy_RetryHostPredicate**)_upb_array_resize_accessor(msg, UPB_SIZE(32, 56), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_RetryPolicy_RetryHostPredicate* envoy_api_v2_route_RetryPolicy_add_retry_host_predicate(envoy_api_v2_route_RetryPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RetryPolicy_RetryHostPredicate* sub = (struct envoy_api_v2_route_RetryPolicy_RetryHostPredicate*)_upb_msg_new(&envoy_api_v2_route_RetryPolicy_RetryHostPredicate_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(32, 56), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_set_host_selection_retry_max_attempts(envoy_api_v2_route_RetryPolicy *msg, int64_t value) {
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE uint32_t* envoy_api_v2_route_RetryPolicy_mutable_retriable_status_codes(envoy_api_v2_route_RetryPolicy *msg, size_t *len) {
  return (uint32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(36, 64), len);
}
UPB_INLINE uint32_t* envoy_api_v2_route_RetryPolicy_resize_retriable_status_codes(envoy_api_v2_route_RetryPolicy *msg, size_t len, upb_arena *arena) {
  return (uint32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(36, 64), len, UPB_TYPE_UINT32, arena);
}
UPB_INLINE bool envoy_api_v2_route_RetryPolicy_add_retriable_status_codes(envoy_api_v2_route_RetryPolicy *msg, uint32_t val, upb_arena *arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(36, 64), UPB_SIZE(4, 4), UPB_TYPE_UINT32, &val,
      arena);
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_set_retry_back_off(envoy_api_v2_route_RetryPolicy *msg, envoy_api_v2_route_RetryPolicy_RetryBackOff* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_route_RetryPolicy_RetryBackOff*, UPB_SIZE(28, 48)) = value;
}
UPB_INLINE struct envoy_api_v2_route_RetryPolicy_RetryBackOff* envoy_api_v2_route_RetryPolicy_mutable_retry_back_off(envoy_api_v2_route_RetryPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RetryPolicy_RetryBackOff* sub = (struct envoy_api_v2_route_RetryPolicy_RetryBackOff*)envoy_api_v2_route_RetryPolicy_retry_back_off(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RetryPolicy_RetryBackOff*)_upb_msg_new(&envoy_api_v2_route_RetryPolicy_RetryBackOff_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_set_retry_back_off(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_RetryPolicy_mutable_retriable_headers(envoy_api_v2_route_RetryPolicy *msg, size_t *len) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_mutable_accessor(msg, UPB_SIZE(40, 72), len);
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_RetryPolicy_resize_retriable_headers(envoy_api_v2_route_RetryPolicy *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_resize_accessor(msg, UPB_SIZE(40, 72), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_HeaderMatcher* envoy_api_v2_route_RetryPolicy_add_retriable_headers(envoy_api_v2_route_RetryPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_HeaderMatcher* sub = (struct envoy_api_v2_route_HeaderMatcher*)_upb_msg_new(&envoy_api_v2_route_HeaderMatcher_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(40, 72), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_RetryPolicy_mutable_retriable_request_headers(envoy_api_v2_route_RetryPolicy *msg, size_t *len) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_mutable_accessor(msg, UPB_SIZE(44, 80), len);
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_RetryPolicy_resize_retriable_request_headers(envoy_api_v2_route_RetryPolicy *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_resize_accessor(msg, UPB_SIZE(44, 80), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_HeaderMatcher* envoy_api_v2_route_RetryPolicy_add_retriable_request_headers(envoy_api_v2_route_RetryPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_route_HeaderMatcher* sub = (struct envoy_api_v2_route_HeaderMatcher*)_upb_msg_new(&envoy_api_v2_route_HeaderMatcher_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(44, 80), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryPriority *envoy_api_v2_route_RetryPolicy_RetryPriority_new(upb_arena *arena) {
  return (envoy_api_v2_route_RetryPolicy_RetryPriority *)_upb_msg_new(&envoy_api_v2_route_RetryPolicy_RetryPriority_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryPriority *envoy_api_v2_route_RetryPolicy_RetryPriority_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RetryPolicy_RetryPriority *ret = envoy_api_v2_route_RetryPolicy_RetryPriority_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RetryPolicy_RetryPriority_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RetryPolicy_RetryPriority_serialize(const envoy_api_v2_route_RetryPolicy_RetryPriority *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RetryPolicy_RetryPriority_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_RetryPolicy_RetryPriority_config_type_config = 2,
  envoy_api_v2_route_RetryPolicy_RetryPriority_config_type_typed_config = 3,
  envoy_api_v2_route_RetryPolicy_RetryPriority_config_type_NOT_SET = 0
} envoy_api_v2_route_RetryPolicy_RetryPriority_config_type_oneofcases;
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryPriority_config_type_oneofcases envoy_api_v2_route_RetryPolicy_RetryPriority_config_type_case(const envoy_api_v2_route_RetryPolicy_RetryPriority* msg) { return (envoy_api_v2_route_RetryPolicy_RetryPriority_config_type_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(12, 24)); }
UPB_INLINE upb_strview envoy_api_v2_route_RetryPolicy_RetryPriority_name(const envoy_api_v2_route_RetryPolicy_RetryPriority *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE bool envoy_api_v2_route_RetryPolicy_RetryPriority_has_config(const envoy_api_v2_route_RetryPolicy_RetryPriority *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 2); }
UPB_INLINE const struct google_protobuf_Struct* envoy_api_v2_route_RetryPolicy_RetryPriority_config(const envoy_api_v2_route_RetryPolicy_RetryPriority *msg) { return UPB_READ_ONEOF(msg, const struct google_protobuf_Struct*, UPB_SIZE(8, 16), UPB_SIZE(12, 24), 2, NULL); }
UPB_INLINE bool envoy_api_v2_route_RetryPolicy_RetryPriority_has_typed_config(const envoy_api_v2_route_RetryPolicy_RetryPriority *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 3); }
UPB_INLINE const struct google_protobuf_Any* envoy_api_v2_route_RetryPolicy_RetryPriority_typed_config(const envoy_api_v2_route_RetryPolicy_RetryPriority *msg) { return UPB_READ_ONEOF(msg, const struct google_protobuf_Any*, UPB_SIZE(8, 16), UPB_SIZE(12, 24), 3, NULL); }
UPB_INLINE void envoy_api_v2_route_RetryPolicy_RetryPriority_set_name(envoy_api_v2_route_RetryPolicy_RetryPriority *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_RetryPriority_set_config(envoy_api_v2_route_RetryPolicy_RetryPriority *msg, struct google_protobuf_Struct* value) {
  UPB_WRITE_ONEOF(msg, struct google_protobuf_Struct*, UPB_SIZE(8, 16), value, UPB_SIZE(12, 24), 2);
}
UPB_INLINE struct google_protobuf_Struct* envoy_api_v2_route_RetryPolicy_RetryPriority_mutable_config(envoy_api_v2_route_RetryPolicy_RetryPriority *msg, upb_arena *arena) {
  struct google_protobuf_Struct* sub = (struct google_protobuf_Struct*)envoy_api_v2_route_RetryPolicy_RetryPriority_config(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Struct*)_upb_msg_new(&google_protobuf_Struct_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_RetryPriority_set_config(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_RetryPriority_set_typed_config(envoy_api_v2_route_RetryPolicy_RetryPriority *msg, struct google_protobuf_Any* value) {
  UPB_WRITE_ONEOF(msg, struct google_protobuf_Any*, UPB_SIZE(8, 16), value, UPB_SIZE(12, 24), 3);
}
UPB_INLINE struct google_protobuf_Any* envoy_api_v2_route_RetryPolicy_RetryPriority_mutable_typed_config(envoy_api_v2_route_RetryPolicy_RetryPriority *msg, upb_arena *arena) {
  struct google_protobuf_Any* sub = (struct google_protobuf_Any*)envoy_api_v2_route_RetryPolicy_RetryPriority_typed_config(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Any*)_upb_msg_new(&google_protobuf_Any_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_RetryPriority_set_typed_config(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryHostPredicate *envoy_api_v2_route_RetryPolicy_RetryHostPredicate_new(upb_arena *arena) {
  return (envoy_api_v2_route_RetryPolicy_RetryHostPredicate *)_upb_msg_new(&envoy_api_v2_route_RetryPolicy_RetryHostPredicate_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryHostPredicate *envoy_api_v2_route_RetryPolicy_RetryHostPredicate_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RetryPolicy_RetryHostPredicate *ret = envoy_api_v2_route_RetryPolicy_RetryHostPredicate_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RetryPolicy_RetryHostPredicate_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RetryPolicy_RetryHostPredicate_serialize(const envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RetryPolicy_RetryHostPredicate_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_RetryPolicy_RetryHostPredicate_config_type_config = 2,
  envoy_api_v2_route_RetryPolicy_RetryHostPredicate_config_type_typed_config = 3,
  envoy_api_v2_route_RetryPolicy_RetryHostPredicate_config_type_NOT_SET = 0
} envoy_api_v2_route_RetryPolicy_RetryHostPredicate_config_type_oneofcases;
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryHostPredicate_config_type_oneofcases envoy_api_v2_route_RetryPolicy_RetryHostPredicate_config_type_case(const envoy_api_v2_route_RetryPolicy_RetryHostPredicate* msg) { return (envoy_api_v2_route_RetryPolicy_RetryHostPredicate_config_type_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(12, 24)); }
UPB_INLINE upb_strview envoy_api_v2_route_RetryPolicy_RetryHostPredicate_name(const envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE bool envoy_api_v2_route_RetryPolicy_RetryHostPredicate_has_config(const envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 2); }
UPB_INLINE const struct google_protobuf_Struct* envoy_api_v2_route_RetryPolicy_RetryHostPredicate_config(const envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg) { return UPB_READ_ONEOF(msg, const struct google_protobuf_Struct*, UPB_SIZE(8, 16), UPB_SIZE(12, 24), 2, NULL); }
UPB_INLINE bool envoy_api_v2_route_RetryPolicy_RetryHostPredicate_has_typed_config(const envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 3); }
UPB_INLINE const struct google_protobuf_Any* envoy_api_v2_route_RetryPolicy_RetryHostPredicate_typed_config(const envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg) { return UPB_READ_ONEOF(msg, const struct google_protobuf_Any*, UPB_SIZE(8, 16), UPB_SIZE(12, 24), 3, NULL); }
UPB_INLINE void envoy_api_v2_route_RetryPolicy_RetryHostPredicate_set_name(envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_RetryHostPredicate_set_config(envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg, struct google_protobuf_Struct* value) {
  UPB_WRITE_ONEOF(msg, struct google_protobuf_Struct*, UPB_SIZE(8, 16), value, UPB_SIZE(12, 24), 2);
}
UPB_INLINE struct google_protobuf_Struct* envoy_api_v2_route_RetryPolicy_RetryHostPredicate_mutable_config(envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg, upb_arena *arena) {
  struct google_protobuf_Struct* sub = (struct google_protobuf_Struct*)envoy_api_v2_route_RetryPolicy_RetryHostPredicate_config(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Struct*)_upb_msg_new(&google_protobuf_Struct_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_RetryHostPredicate_set_config(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_RetryHostPredicate_set_typed_config(envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg, struct google_protobuf_Any* value) {
  UPB_WRITE_ONEOF(msg, struct google_protobuf_Any*, UPB_SIZE(8, 16), value, UPB_SIZE(12, 24), 3);
}
UPB_INLINE struct google_protobuf_Any* envoy_api_v2_route_RetryPolicy_RetryHostPredicate_mutable_typed_config(envoy_api_v2_route_RetryPolicy_RetryHostPredicate *msg, upb_arena *arena) {
  struct google_protobuf_Any* sub = (struct google_protobuf_Any*)envoy_api_v2_route_RetryPolicy_RetryHostPredicate_typed_config(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Any*)_upb_msg_new(&google_protobuf_Any_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_RetryHostPredicate_set_typed_config(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryBackOff *envoy_api_v2_route_RetryPolicy_RetryBackOff_new(upb_arena *arena) {
  return (envoy_api_v2_route_RetryPolicy_RetryBackOff *)_upb_msg_new(&envoy_api_v2_route_RetryPolicy_RetryBackOff_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RetryPolicy_RetryBackOff *envoy_api_v2_route_RetryPolicy_RetryBackOff_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RetryPolicy_RetryBackOff *ret = envoy_api_v2_route_RetryPolicy_RetryBackOff_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RetryPolicy_RetryBackOff_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RetryPolicy_RetryBackOff_serialize(const envoy_api_v2_route_RetryPolicy_RetryBackOff *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RetryPolicy_RetryBackOff_msginit, arena, len);
}
UPB_INLINE const struct google_protobuf_Duration* envoy_api_v2_route_RetryPolicy_RetryBackOff_base_interval(const envoy_api_v2_route_RetryPolicy_RetryBackOff *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Duration*, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_Duration* envoy_api_v2_route_RetryPolicy_RetryBackOff_max_interval(const envoy_api_v2_route_RetryPolicy_RetryBackOff *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Duration*, UPB_SIZE(4, 8)); }
UPB_INLINE void envoy_api_v2_route_RetryPolicy_RetryBackOff_set_base_interval(envoy_api_v2_route_RetryPolicy_RetryBackOff *msg, struct google_protobuf_Duration* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Duration*, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE struct google_protobuf_Duration* envoy_api_v2_route_RetryPolicy_RetryBackOff_mutable_base_interval(envoy_api_v2_route_RetryPolicy_RetryBackOff *msg, upb_arena *arena) {
  struct google_protobuf_Duration* sub = (struct google_protobuf_Duration*)envoy_api_v2_route_RetryPolicy_RetryBackOff_base_interval(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Duration*)_upb_msg_new(&google_protobuf_Duration_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_RetryBackOff_set_base_interval(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RetryPolicy_RetryBackOff_set_max_interval(envoy_api_v2_route_RetryPolicy_RetryBackOff *msg, struct google_protobuf_Duration* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Duration*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct google_protobuf_Duration* envoy_api_v2_route_RetryPolicy_RetryBackOff_mutable_max_interval(envoy_api_v2_route_RetryPolicy_RetryBackOff *msg, upb_arena *arena) {
  struct google_protobuf_Duration* sub = (struct google_protobuf_Duration*)envoy_api_v2_route_RetryPolicy_RetryBackOff_max_interval(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Duration*)_upb_msg_new(&google_protobuf_Duration_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RetryPolicy_RetryBackOff_set_max_interval(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_HedgePolicy *envoy_api_v2_route_HedgePolicy_new(upb_arena *arena) {
  return (envoy_api_v2_route_HedgePolicy *)_upb_msg_new(&envoy_api_v2_route_HedgePolicy_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_HedgePolicy *envoy_api_v2_route_HedgePolicy_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_HedgePolicy *ret = envoy_api_v2_route_HedgePolicy_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_HedgePolicy_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_HedgePolicy_serialize(const envoy_api_v2_route_HedgePolicy *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_HedgePolicy_msginit, arena, len);
}
UPB_INLINE const struct google_protobuf_UInt32Value* envoy_api_v2_route_HedgePolicy_initial_requests(const envoy_api_v2_route_HedgePolicy *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_UInt32Value*, UPB_SIZE(4, 8)); }
UPB_INLINE const struct envoy_type_FractionalPercent* envoy_api_v2_route_HedgePolicy_additional_request_chance(const envoy_api_v2_route_HedgePolicy *msg) { return UPB_FIELD_AT(msg, const struct envoy_type_FractionalPercent*, UPB_SIZE(8, 16)); }
UPB_INLINE bool envoy_api_v2_route_HedgePolicy_hedge_on_per_try_timeout(const envoy_api_v2_route_HedgePolicy *msg) { return UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_route_HedgePolicy_set_initial_requests(envoy_api_v2_route_HedgePolicy *msg, struct google_protobuf_UInt32Value* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_UInt32Value*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct google_protobuf_UInt32Value* envoy_api_v2_route_HedgePolicy_mutable_initial_requests(envoy_api_v2_route_HedgePolicy *msg, upb_arena *arena) {
  struct google_protobuf_UInt32Value* sub = (struct google_protobuf_UInt32Value*)envoy_api_v2_route_HedgePolicy_initial_requests(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_UInt32Value*)_upb_msg_new(&google_protobuf_UInt32Value_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_HedgePolicy_set_initial_requests(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_HedgePolicy_set_additional_request_chance(envoy_api_v2_route_HedgePolicy *msg, struct envoy_type_FractionalPercent* value) {
  UPB_FIELD_AT(msg, struct envoy_type_FractionalPercent*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct envoy_type_FractionalPercent* envoy_api_v2_route_HedgePolicy_mutable_additional_request_chance(envoy_api_v2_route_HedgePolicy *msg, upb_arena *arena) {
  struct envoy_type_FractionalPercent* sub = (struct envoy_type_FractionalPercent*)envoy_api_v2_route_HedgePolicy_additional_request_chance(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_FractionalPercent*)_upb_msg_new(&envoy_type_FractionalPercent_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_HedgePolicy_set_additional_request_chance(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_HedgePolicy_set_hedge_on_per_try_timeout(envoy_api_v2_route_HedgePolicy *msg, bool value) {
  UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_RedirectAction *envoy_api_v2_route_RedirectAction_new(upb_arena *arena) {
  return (envoy_api_v2_route_RedirectAction *)_upb_msg_new(&envoy_api_v2_route_RedirectAction_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RedirectAction *envoy_api_v2_route_RedirectAction_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RedirectAction *ret = envoy_api_v2_route_RedirectAction_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RedirectAction_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RedirectAction_serialize(const envoy_api_v2_route_RedirectAction *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RedirectAction_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_RedirectAction_scheme_rewrite_specifier_https_redirect = 4,
  envoy_api_v2_route_RedirectAction_scheme_rewrite_specifier_scheme_redirect = 7,
  envoy_api_v2_route_RedirectAction_scheme_rewrite_specifier_NOT_SET = 0
} envoy_api_v2_route_RedirectAction_scheme_rewrite_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_RedirectAction_scheme_rewrite_specifier_oneofcases envoy_api_v2_route_RedirectAction_scheme_rewrite_specifier_case(const envoy_api_v2_route_RedirectAction* msg) { return (envoy_api_v2_route_RedirectAction_scheme_rewrite_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(44, 72)); }
typedef enum {
  envoy_api_v2_route_RedirectAction_path_rewrite_specifier_path_redirect = 2,
  envoy_api_v2_route_RedirectAction_path_rewrite_specifier_prefix_rewrite = 5,
  envoy_api_v2_route_RedirectAction_path_rewrite_specifier_NOT_SET = 0
} envoy_api_v2_route_RedirectAction_path_rewrite_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_RedirectAction_path_rewrite_specifier_oneofcases envoy_api_v2_route_RedirectAction_path_rewrite_specifier_case(const envoy_api_v2_route_RedirectAction* msg) { return (envoy_api_v2_route_RedirectAction_path_rewrite_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(32, 48)); }
UPB_INLINE upb_strview envoy_api_v2_route_RedirectAction_host_redirect(const envoy_api_v2_route_RedirectAction *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 16)); }
UPB_INLINE bool envoy_api_v2_route_RedirectAction_has_path_redirect(const envoy_api_v2_route_RedirectAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(32, 48), 2); }
UPB_INLINE upb_strview envoy_api_v2_route_RedirectAction_path_redirect(const envoy_api_v2_route_RedirectAction *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(24, 32), UPB_SIZE(32, 48), 2, upb_strview_make("", strlen(""))); }
UPB_INLINE int32_t envoy_api_v2_route_RedirectAction_response_code(const envoy_api_v2_route_RedirectAction *msg) { return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(0, 0)); }
UPB_INLINE bool envoy_api_v2_route_RedirectAction_has_https_redirect(const envoy_api_v2_route_RedirectAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(44, 72), 4); }
UPB_INLINE bool envoy_api_v2_route_RedirectAction_https_redirect(const envoy_api_v2_route_RedirectAction *msg) { return UPB_READ_ONEOF(msg, bool, UPB_SIZE(36, 56), UPB_SIZE(44, 72), 4, false); }
UPB_INLINE bool envoy_api_v2_route_RedirectAction_has_prefix_rewrite(const envoy_api_v2_route_RedirectAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(32, 48), 5); }
UPB_INLINE upb_strview envoy_api_v2_route_RedirectAction_prefix_rewrite(const envoy_api_v2_route_RedirectAction *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(24, 32), UPB_SIZE(32, 48), 5, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_RedirectAction_strip_query(const envoy_api_v2_route_RedirectAction *msg) { return UPB_FIELD_AT(msg, bool, UPB_SIZE(12, 12)); }
UPB_INLINE bool envoy_api_v2_route_RedirectAction_has_scheme_redirect(const envoy_api_v2_route_RedirectAction *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(44, 72), 7); }
UPB_INLINE upb_strview envoy_api_v2_route_RedirectAction_scheme_redirect(const envoy_api_v2_route_RedirectAction *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(36, 56), UPB_SIZE(44, 72), 7, upb_strview_make("", strlen(""))); }
UPB_INLINE uint32_t envoy_api_v2_route_RedirectAction_port_redirect(const envoy_api_v2_route_RedirectAction *msg) { return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(8, 8)); }
UPB_INLINE void envoy_api_v2_route_RedirectAction_set_host_redirect(envoy_api_v2_route_RedirectAction *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void envoy_api_v2_route_RedirectAction_set_path_redirect(envoy_api_v2_route_RedirectAction *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(24, 32), value, UPB_SIZE(32, 48), 2);
}
UPB_INLINE void envoy_api_v2_route_RedirectAction_set_response_code(envoy_api_v2_route_RedirectAction *msg, int32_t value) {
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_RedirectAction_set_https_redirect(envoy_api_v2_route_RedirectAction *msg, bool value) {
  UPB_WRITE_ONEOF(msg, bool, UPB_SIZE(36, 56), value, UPB_SIZE(44, 72), 4);
}
UPB_INLINE void envoy_api_v2_route_RedirectAction_set_prefix_rewrite(envoy_api_v2_route_RedirectAction *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(24, 32), value, UPB_SIZE(32, 48), 5);
}
UPB_INLINE void envoy_api_v2_route_RedirectAction_set_strip_query(envoy_api_v2_route_RedirectAction *msg, bool value) {
  UPB_FIELD_AT(msg, bool, UPB_SIZE(12, 12)) = value;
}
UPB_INLINE void envoy_api_v2_route_RedirectAction_set_scheme_redirect(envoy_api_v2_route_RedirectAction *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(36, 56), value, UPB_SIZE(44, 72), 7);
}
UPB_INLINE void envoy_api_v2_route_RedirectAction_set_port_redirect(envoy_api_v2_route_RedirectAction *msg, uint32_t value) {
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE envoy_api_v2_route_DirectResponseAction *envoy_api_v2_route_DirectResponseAction_new(upb_arena *arena) {
  return (envoy_api_v2_route_DirectResponseAction *)_upb_msg_new(&envoy_api_v2_route_DirectResponseAction_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_DirectResponseAction *envoy_api_v2_route_DirectResponseAction_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_DirectResponseAction *ret = envoy_api_v2_route_DirectResponseAction_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_DirectResponseAction_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_DirectResponseAction_serialize(const envoy_api_v2_route_DirectResponseAction *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_DirectResponseAction_msginit, arena, len);
}
UPB_INLINE uint32_t envoy_api_v2_route_DirectResponseAction_status(const envoy_api_v2_route_DirectResponseAction *msg) { return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(0, 0)); }
UPB_INLINE const struct envoy_api_v2_core_DataSource* envoy_api_v2_route_DirectResponseAction_body(const envoy_api_v2_route_DirectResponseAction *msg) { return UPB_FIELD_AT(msg, const struct envoy_api_v2_core_DataSource*, UPB_SIZE(4, 8)); }
UPB_INLINE void envoy_api_v2_route_DirectResponseAction_set_status(envoy_api_v2_route_DirectResponseAction *msg, uint32_t value) {
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_DirectResponseAction_set_body(envoy_api_v2_route_DirectResponseAction *msg, struct envoy_api_v2_core_DataSource* value) {
  UPB_FIELD_AT(msg, struct envoy_api_v2_core_DataSource*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct envoy_api_v2_core_DataSource* envoy_api_v2_route_DirectResponseAction_mutable_body(envoy_api_v2_route_DirectResponseAction *msg, upb_arena *arena) {
  struct envoy_api_v2_core_DataSource* sub = (struct envoy_api_v2_core_DataSource*)envoy_api_v2_route_DirectResponseAction_body(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_DataSource*)_upb_msg_new(&envoy_api_v2_core_DataSource_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_DirectResponseAction_set_body(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_Decorator *envoy_api_v2_route_Decorator_new(upb_arena *arena) {
  return (envoy_api_v2_route_Decorator *)_upb_msg_new(&envoy_api_v2_route_Decorator_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_Decorator *envoy_api_v2_route_Decorator_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_Decorator *ret = envoy_api_v2_route_Decorator_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_Decorator_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_Decorator_serialize(const envoy_api_v2_route_Decorator *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_Decorator_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_Decorator_operation(const envoy_api_v2_route_Decorator *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_Decorator_propagate(const envoy_api_v2_route_Decorator *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(8, 16)); }
UPB_INLINE void envoy_api_v2_route_Decorator_set_operation(envoy_api_v2_route_Decorator *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_Decorator_set_propagate(envoy_api_v2_route_Decorator *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_Decorator_mutable_propagate(envoy_api_v2_route_Decorator *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_Decorator_propagate(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Decorator_set_propagate(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_Tracing *envoy_api_v2_route_Tracing_new(upb_arena *arena) {
  return (envoy_api_v2_route_Tracing *)_upb_msg_new(&envoy_api_v2_route_Tracing_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_Tracing *envoy_api_v2_route_Tracing_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_Tracing *ret = envoy_api_v2_route_Tracing_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_Tracing_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_Tracing_serialize(const envoy_api_v2_route_Tracing *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_Tracing_msginit, arena, len);
}
UPB_INLINE const struct envoy_type_FractionalPercent* envoy_api_v2_route_Tracing_client_sampling(const envoy_api_v2_route_Tracing *msg) { return UPB_FIELD_AT(msg, const struct envoy_type_FractionalPercent*, UPB_SIZE(0, 0)); }
UPB_INLINE const struct envoy_type_FractionalPercent* envoy_api_v2_route_Tracing_random_sampling(const envoy_api_v2_route_Tracing *msg) { return UPB_FIELD_AT(msg, const struct envoy_type_FractionalPercent*, UPB_SIZE(4, 8)); }
UPB_INLINE const struct envoy_type_FractionalPercent* envoy_api_v2_route_Tracing_overall_sampling(const envoy_api_v2_route_Tracing *msg) { return UPB_FIELD_AT(msg, const struct envoy_type_FractionalPercent*, UPB_SIZE(8, 16)); }
UPB_INLINE const struct envoy_type_tracing_v2_CustomTag* const* envoy_api_v2_route_Tracing_custom_tags(const envoy_api_v2_route_Tracing *msg, size_t *len) { return (const struct envoy_type_tracing_v2_CustomTag* const*)_upb_array_accessor(msg, UPB_SIZE(12, 24), len); }
UPB_INLINE void envoy_api_v2_route_Tracing_set_client_sampling(envoy_api_v2_route_Tracing *msg, struct envoy_type_FractionalPercent* value) {
  UPB_FIELD_AT(msg, struct envoy_type_FractionalPercent*, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE struct envoy_type_FractionalPercent* envoy_api_v2_route_Tracing_mutable_client_sampling(envoy_api_v2_route_Tracing *msg, upb_arena *arena) {
  struct envoy_type_FractionalPercent* sub = (struct envoy_type_FractionalPercent*)envoy_api_v2_route_Tracing_client_sampling(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_FractionalPercent*)_upb_msg_new(&envoy_type_FractionalPercent_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Tracing_set_client_sampling(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Tracing_set_random_sampling(envoy_api_v2_route_Tracing *msg, struct envoy_type_FractionalPercent* value) {
  UPB_FIELD_AT(msg, struct envoy_type_FractionalPercent*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct envoy_type_FractionalPercent* envoy_api_v2_route_Tracing_mutable_random_sampling(envoy_api_v2_route_Tracing *msg, upb_arena *arena) {
  struct envoy_type_FractionalPercent* sub = (struct envoy_type_FractionalPercent*)envoy_api_v2_route_Tracing_random_sampling(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_FractionalPercent*)_upb_msg_new(&envoy_type_FractionalPercent_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Tracing_set_random_sampling(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_Tracing_set_overall_sampling(envoy_api_v2_route_Tracing *msg, struct envoy_type_FractionalPercent* value) {
  UPB_FIELD_AT(msg, struct envoy_type_FractionalPercent*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct envoy_type_FractionalPercent* envoy_api_v2_route_Tracing_mutable_overall_sampling(envoy_api_v2_route_Tracing *msg, upb_arena *arena) {
  struct envoy_type_FractionalPercent* sub = (struct envoy_type_FractionalPercent*)envoy_api_v2_route_Tracing_overall_sampling(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_FractionalPercent*)_upb_msg_new(&envoy_type_FractionalPercent_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_Tracing_set_overall_sampling(msg, sub);
  }
  return sub;
}
UPB_INLINE struct envoy_type_tracing_v2_CustomTag** envoy_api_v2_route_Tracing_mutable_custom_tags(envoy_api_v2_route_Tracing *msg, size_t *len) {
  return (struct envoy_type_tracing_v2_CustomTag**)_upb_array_mutable_accessor(msg, UPB_SIZE(12, 24), len);
}
UPB_INLINE struct envoy_type_tracing_v2_CustomTag** envoy_api_v2_route_Tracing_resize_custom_tags(envoy_api_v2_route_Tracing *msg, size_t len, upb_arena *arena) {
  return (struct envoy_type_tracing_v2_CustomTag**)_upb_array_resize_accessor(msg, UPB_SIZE(12, 24), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_type_tracing_v2_CustomTag* envoy_api_v2_route_Tracing_add_custom_tags(envoy_api_v2_route_Tracing *msg, upb_arena *arena) {
  struct envoy_type_tracing_v2_CustomTag* sub = (struct envoy_type_tracing_v2_CustomTag*)_upb_msg_new(&envoy_type_tracing_v2_CustomTag_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(12, 24), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_route_VirtualCluster *envoy_api_v2_route_VirtualCluster_new(upb_arena *arena) {
  return (envoy_api_v2_route_VirtualCluster *)_upb_msg_new(&envoy_api_v2_route_VirtualCluster_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_VirtualCluster *envoy_api_v2_route_VirtualCluster_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_VirtualCluster *ret = envoy_api_v2_route_VirtualCluster_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_VirtualCluster_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_VirtualCluster_serialize(const envoy_api_v2_route_VirtualCluster *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_VirtualCluster_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_VirtualCluster_pattern(const envoy_api_v2_route_VirtualCluster *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 8)); }
UPB_INLINE upb_strview envoy_api_v2_route_VirtualCluster_name(const envoy_api_v2_route_VirtualCluster *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 24)); }
UPB_INLINE int32_t envoy_api_v2_route_VirtualCluster_method(const envoy_api_v2_route_VirtualCluster *msg) { return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(0, 0)); }
UPB_INLINE const envoy_api_v2_route_HeaderMatcher* const* envoy_api_v2_route_VirtualCluster_headers(const envoy_api_v2_route_VirtualCluster *msg, size_t *len) { return (const envoy_api_v2_route_HeaderMatcher* const*)_upb_array_accessor(msg, UPB_SIZE(24, 40), len); }
UPB_INLINE void envoy_api_v2_route_VirtualCluster_set_pattern(envoy_api_v2_route_VirtualCluster *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void envoy_api_v2_route_VirtualCluster_set_name(envoy_api_v2_route_VirtualCluster *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 24)) = value;
}
UPB_INLINE void envoy_api_v2_route_VirtualCluster_set_method(envoy_api_v2_route_VirtualCluster *msg, int32_t value) {
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_VirtualCluster_mutable_headers(envoy_api_v2_route_VirtualCluster *msg, size_t *len) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_mutable_accessor(msg, UPB_SIZE(24, 40), len);
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_VirtualCluster_resize_headers(envoy_api_v2_route_VirtualCluster *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_resize_accessor(msg, UPB_SIZE(24, 40), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_HeaderMatcher* envoy_api_v2_route_VirtualCluster_add_headers(envoy_api_v2_route_VirtualCluster *msg, upb_arena *arena) {
  struct envoy_api_v2_route_HeaderMatcher* sub = (struct envoy_api_v2_route_HeaderMatcher*)_upb_msg_new(&envoy_api_v2_route_HeaderMatcher_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(24, 40), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_route_RateLimit *envoy_api_v2_route_RateLimit_new(upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit *)_upb_msg_new(&envoy_api_v2_route_RateLimit_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RateLimit *envoy_api_v2_route_RateLimit_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RateLimit *ret = envoy_api_v2_route_RateLimit_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RateLimit_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RateLimit_serialize(const envoy_api_v2_route_RateLimit *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RateLimit_msginit, arena, len);
}
UPB_INLINE const struct google_protobuf_UInt32Value* envoy_api_v2_route_RateLimit_stage(const envoy_api_v2_route_RateLimit *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_UInt32Value*, UPB_SIZE(8, 16)); }
UPB_INLINE upb_strview envoy_api_v2_route_RateLimit_disable_key(const envoy_api_v2_route_RateLimit *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE const envoy_api_v2_route_RateLimit_Action* const* envoy_api_v2_route_RateLimit_actions(const envoy_api_v2_route_RateLimit *msg, size_t *len) { return (const envoy_api_v2_route_RateLimit_Action* const*)_upb_array_accessor(msg, UPB_SIZE(12, 24), len); }
UPB_INLINE void envoy_api_v2_route_RateLimit_set_stage(envoy_api_v2_route_RateLimit *msg, struct google_protobuf_UInt32Value* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_UInt32Value*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct google_protobuf_UInt32Value* envoy_api_v2_route_RateLimit_mutable_stage(envoy_api_v2_route_RateLimit *msg, upb_arena *arena) {
  struct google_protobuf_UInt32Value* sub = (struct google_protobuf_UInt32Value*)envoy_api_v2_route_RateLimit_stage(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_UInt32Value*)_upb_msg_new(&google_protobuf_UInt32Value_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RateLimit_set_stage(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RateLimit_set_disable_key(envoy_api_v2_route_RateLimit *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action** envoy_api_v2_route_RateLimit_mutable_actions(envoy_api_v2_route_RateLimit *msg, size_t *len) {
  return (envoy_api_v2_route_RateLimit_Action**)_upb_array_mutable_accessor(msg, UPB_SIZE(12, 24), len);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action** envoy_api_v2_route_RateLimit_resize_actions(envoy_api_v2_route_RateLimit *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit_Action**)_upb_array_resize_accessor(msg, UPB_SIZE(12, 24), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_RateLimit_Action* envoy_api_v2_route_RateLimit_add_actions(envoy_api_v2_route_RateLimit *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RateLimit_Action* sub = (struct envoy_api_v2_route_RateLimit_Action*)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(12, 24), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action *envoy_api_v2_route_RateLimit_Action_new(upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit_Action *)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action *envoy_api_v2_route_RateLimit_Action_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RateLimit_Action *ret = envoy_api_v2_route_RateLimit_Action_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RateLimit_Action_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RateLimit_Action_serialize(const envoy_api_v2_route_RateLimit_Action *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RateLimit_Action_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_RateLimit_Action_action_specifier_source_cluster = 1,
  envoy_api_v2_route_RateLimit_Action_action_specifier_destination_cluster = 2,
  envoy_api_v2_route_RateLimit_Action_action_specifier_request_headers = 3,
  envoy_api_v2_route_RateLimit_Action_action_specifier_remote_address = 4,
  envoy_api_v2_route_RateLimit_Action_action_specifier_generic_key = 5,
  envoy_api_v2_route_RateLimit_Action_action_specifier_header_value_match = 6,
  envoy_api_v2_route_RateLimit_Action_action_specifier_NOT_SET = 0
} envoy_api_v2_route_RateLimit_Action_action_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_RateLimit_Action_action_specifier_oneofcases envoy_api_v2_route_RateLimit_Action_action_specifier_case(const envoy_api_v2_route_RateLimit_Action* msg) { return (envoy_api_v2_route_RateLimit_Action_action_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(4, 8)); }
UPB_INLINE bool envoy_api_v2_route_RateLimit_Action_has_source_cluster(const envoy_api_v2_route_RateLimit_Action *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(4, 8), 1); }
UPB_INLINE const envoy_api_v2_route_RateLimit_Action_SourceCluster* envoy_api_v2_route_RateLimit_Action_source_cluster(const envoy_api_v2_route_RateLimit_Action *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RateLimit_Action_SourceCluster*, UPB_SIZE(0, 0), UPB_SIZE(4, 8), 1, NULL); }
UPB_INLINE bool envoy_api_v2_route_RateLimit_Action_has_destination_cluster(const envoy_api_v2_route_RateLimit_Action *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(4, 8), 2); }
UPB_INLINE const envoy_api_v2_route_RateLimit_Action_DestinationCluster* envoy_api_v2_route_RateLimit_Action_destination_cluster(const envoy_api_v2_route_RateLimit_Action *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RateLimit_Action_DestinationCluster*, UPB_SIZE(0, 0), UPB_SIZE(4, 8), 2, NULL); }
UPB_INLINE bool envoy_api_v2_route_RateLimit_Action_has_request_headers(const envoy_api_v2_route_RateLimit_Action *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(4, 8), 3); }
UPB_INLINE const envoy_api_v2_route_RateLimit_Action_RequestHeaders* envoy_api_v2_route_RateLimit_Action_request_headers(const envoy_api_v2_route_RateLimit_Action *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RateLimit_Action_RequestHeaders*, UPB_SIZE(0, 0), UPB_SIZE(4, 8), 3, NULL); }
UPB_INLINE bool envoy_api_v2_route_RateLimit_Action_has_remote_address(const envoy_api_v2_route_RateLimit_Action *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(4, 8), 4); }
UPB_INLINE const envoy_api_v2_route_RateLimit_Action_RemoteAddress* envoy_api_v2_route_RateLimit_Action_remote_address(const envoy_api_v2_route_RateLimit_Action *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RateLimit_Action_RemoteAddress*, UPB_SIZE(0, 0), UPB_SIZE(4, 8), 4, NULL); }
UPB_INLINE bool envoy_api_v2_route_RateLimit_Action_has_generic_key(const envoy_api_v2_route_RateLimit_Action *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(4, 8), 5); }
UPB_INLINE const envoy_api_v2_route_RateLimit_Action_GenericKey* envoy_api_v2_route_RateLimit_Action_generic_key(const envoy_api_v2_route_RateLimit_Action *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RateLimit_Action_GenericKey*, UPB_SIZE(0, 0), UPB_SIZE(4, 8), 5, NULL); }
UPB_INLINE bool envoy_api_v2_route_RateLimit_Action_has_header_value_match(const envoy_api_v2_route_RateLimit_Action *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(4, 8), 6); }
UPB_INLINE const envoy_api_v2_route_RateLimit_Action_HeaderValueMatch* envoy_api_v2_route_RateLimit_Action_header_value_match(const envoy_api_v2_route_RateLimit_Action *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_route_RateLimit_Action_HeaderValueMatch*, UPB_SIZE(0, 0), UPB_SIZE(4, 8), 6, NULL); }
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_set_source_cluster(envoy_api_v2_route_RateLimit_Action *msg, envoy_api_v2_route_RateLimit_Action_SourceCluster* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RateLimit_Action_SourceCluster*, UPB_SIZE(0, 0), value, UPB_SIZE(4, 8), 1);
}
UPB_INLINE struct envoy_api_v2_route_RateLimit_Action_SourceCluster* envoy_api_v2_route_RateLimit_Action_mutable_source_cluster(envoy_api_v2_route_RateLimit_Action *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RateLimit_Action_SourceCluster* sub = (struct envoy_api_v2_route_RateLimit_Action_SourceCluster*)envoy_api_v2_route_RateLimit_Action_source_cluster(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RateLimit_Action_SourceCluster*)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_SourceCluster_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RateLimit_Action_set_source_cluster(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_set_destination_cluster(envoy_api_v2_route_RateLimit_Action *msg, envoy_api_v2_route_RateLimit_Action_DestinationCluster* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RateLimit_Action_DestinationCluster*, UPB_SIZE(0, 0), value, UPB_SIZE(4, 8), 2);
}
UPB_INLINE struct envoy_api_v2_route_RateLimit_Action_DestinationCluster* envoy_api_v2_route_RateLimit_Action_mutable_destination_cluster(envoy_api_v2_route_RateLimit_Action *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RateLimit_Action_DestinationCluster* sub = (struct envoy_api_v2_route_RateLimit_Action_DestinationCluster*)envoy_api_v2_route_RateLimit_Action_destination_cluster(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RateLimit_Action_DestinationCluster*)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_DestinationCluster_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RateLimit_Action_set_destination_cluster(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_set_request_headers(envoy_api_v2_route_RateLimit_Action *msg, envoy_api_v2_route_RateLimit_Action_RequestHeaders* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RateLimit_Action_RequestHeaders*, UPB_SIZE(0, 0), value, UPB_SIZE(4, 8), 3);
}
UPB_INLINE struct envoy_api_v2_route_RateLimit_Action_RequestHeaders* envoy_api_v2_route_RateLimit_Action_mutable_request_headers(envoy_api_v2_route_RateLimit_Action *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RateLimit_Action_RequestHeaders* sub = (struct envoy_api_v2_route_RateLimit_Action_RequestHeaders*)envoy_api_v2_route_RateLimit_Action_request_headers(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RateLimit_Action_RequestHeaders*)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_RequestHeaders_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RateLimit_Action_set_request_headers(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_set_remote_address(envoy_api_v2_route_RateLimit_Action *msg, envoy_api_v2_route_RateLimit_Action_RemoteAddress* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RateLimit_Action_RemoteAddress*, UPB_SIZE(0, 0), value, UPB_SIZE(4, 8), 4);
}
UPB_INLINE struct envoy_api_v2_route_RateLimit_Action_RemoteAddress* envoy_api_v2_route_RateLimit_Action_mutable_remote_address(envoy_api_v2_route_RateLimit_Action *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RateLimit_Action_RemoteAddress* sub = (struct envoy_api_v2_route_RateLimit_Action_RemoteAddress*)envoy_api_v2_route_RateLimit_Action_remote_address(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RateLimit_Action_RemoteAddress*)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_RemoteAddress_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RateLimit_Action_set_remote_address(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_set_generic_key(envoy_api_v2_route_RateLimit_Action *msg, envoy_api_v2_route_RateLimit_Action_GenericKey* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RateLimit_Action_GenericKey*, UPB_SIZE(0, 0), value, UPB_SIZE(4, 8), 5);
}
UPB_INLINE struct envoy_api_v2_route_RateLimit_Action_GenericKey* envoy_api_v2_route_RateLimit_Action_mutable_generic_key(envoy_api_v2_route_RateLimit_Action *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RateLimit_Action_GenericKey* sub = (struct envoy_api_v2_route_RateLimit_Action_GenericKey*)envoy_api_v2_route_RateLimit_Action_generic_key(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RateLimit_Action_GenericKey*)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_GenericKey_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RateLimit_Action_set_generic_key(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_set_header_value_match(envoy_api_v2_route_RateLimit_Action *msg, envoy_api_v2_route_RateLimit_Action_HeaderValueMatch* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_route_RateLimit_Action_HeaderValueMatch*, UPB_SIZE(0, 0), value, UPB_SIZE(4, 8), 6);
}
UPB_INLINE struct envoy_api_v2_route_RateLimit_Action_HeaderValueMatch* envoy_api_v2_route_RateLimit_Action_mutable_header_value_match(envoy_api_v2_route_RateLimit_Action *msg, upb_arena *arena) {
  struct envoy_api_v2_route_RateLimit_Action_HeaderValueMatch* sub = (struct envoy_api_v2_route_RateLimit_Action_HeaderValueMatch*)envoy_api_v2_route_RateLimit_Action_header_value_match(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_route_RateLimit_Action_HeaderValueMatch*)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RateLimit_Action_set_header_value_match(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_SourceCluster *envoy_api_v2_route_RateLimit_Action_SourceCluster_new(upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit_Action_SourceCluster *)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_SourceCluster_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_SourceCluster *envoy_api_v2_route_RateLimit_Action_SourceCluster_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RateLimit_Action_SourceCluster *ret = envoy_api_v2_route_RateLimit_Action_SourceCluster_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RateLimit_Action_SourceCluster_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RateLimit_Action_SourceCluster_serialize(const envoy_api_v2_route_RateLimit_Action_SourceCluster *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RateLimit_Action_SourceCluster_msginit, arena, len);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_DestinationCluster *envoy_api_v2_route_RateLimit_Action_DestinationCluster_new(upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit_Action_DestinationCluster *)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_DestinationCluster_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_DestinationCluster *envoy_api_v2_route_RateLimit_Action_DestinationCluster_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RateLimit_Action_DestinationCluster *ret = envoy_api_v2_route_RateLimit_Action_DestinationCluster_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RateLimit_Action_DestinationCluster_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RateLimit_Action_DestinationCluster_serialize(const envoy_api_v2_route_RateLimit_Action_DestinationCluster *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RateLimit_Action_DestinationCluster_msginit, arena, len);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_RequestHeaders *envoy_api_v2_route_RateLimit_Action_RequestHeaders_new(upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit_Action_RequestHeaders *)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_RequestHeaders_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_RequestHeaders *envoy_api_v2_route_RateLimit_Action_RequestHeaders_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RateLimit_Action_RequestHeaders *ret = envoy_api_v2_route_RateLimit_Action_RequestHeaders_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RateLimit_Action_RequestHeaders_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RateLimit_Action_RequestHeaders_serialize(const envoy_api_v2_route_RateLimit_Action_RequestHeaders *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RateLimit_Action_RequestHeaders_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RateLimit_Action_RequestHeaders_header_name(const envoy_api_v2_route_RateLimit_Action_RequestHeaders *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE upb_strview envoy_api_v2_route_RateLimit_Action_RequestHeaders_descriptor_key(const envoy_api_v2_route_RateLimit_Action_RequestHeaders *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)); }
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_RequestHeaders_set_header_name(envoy_api_v2_route_RateLimit_Action_RequestHeaders *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_RequestHeaders_set_descriptor_key(envoy_api_v2_route_RateLimit_Action_RequestHeaders *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_RemoteAddress *envoy_api_v2_route_RateLimit_Action_RemoteAddress_new(upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit_Action_RemoteAddress *)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_RemoteAddress_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_RemoteAddress *envoy_api_v2_route_RateLimit_Action_RemoteAddress_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RateLimit_Action_RemoteAddress *ret = envoy_api_v2_route_RateLimit_Action_RemoteAddress_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RateLimit_Action_RemoteAddress_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RateLimit_Action_RemoteAddress_serialize(const envoy_api_v2_route_RateLimit_Action_RemoteAddress *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RateLimit_Action_RemoteAddress_msginit, arena, len);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_GenericKey *envoy_api_v2_route_RateLimit_Action_GenericKey_new(upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit_Action_GenericKey *)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_GenericKey_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_GenericKey *envoy_api_v2_route_RateLimit_Action_GenericKey_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RateLimit_Action_GenericKey *ret = envoy_api_v2_route_RateLimit_Action_GenericKey_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RateLimit_Action_GenericKey_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RateLimit_Action_GenericKey_serialize(const envoy_api_v2_route_RateLimit_Action_GenericKey *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RateLimit_Action_GenericKey_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RateLimit_Action_GenericKey_descriptor_value(const envoy_api_v2_route_RateLimit_Action_GenericKey *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_GenericKey_set_descriptor_value(envoy_api_v2_route_RateLimit_Action_GenericKey *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_new(upb_arena *arena) {
  return (envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *)_upb_msg_new(&envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *ret = envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_serialize(const envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_descriptor_value(const envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_expect_match(const envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(8, 16)); }
UPB_INLINE const envoy_api_v2_route_HeaderMatcher* const* envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_headers(const envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg, size_t *len) { return (const envoy_api_v2_route_HeaderMatcher* const*)_upb_array_accessor(msg, UPB_SIZE(12, 24), len); }
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_set_descriptor_value(envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_set_expect_match(envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_mutable_expect_match(envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_expect_match(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_set_expect_match(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_mutable_headers(envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg, size_t *len) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_mutable_accessor(msg, UPB_SIZE(12, 24), len);
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher** envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_resize_headers(envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_route_HeaderMatcher**)_upb_array_resize_accessor(msg, UPB_SIZE(12, 24), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_route_HeaderMatcher* envoy_api_v2_route_RateLimit_Action_HeaderValueMatch_add_headers(envoy_api_v2_route_RateLimit_Action_HeaderValueMatch *msg, upb_arena *arena) {
  struct envoy_api_v2_route_HeaderMatcher* sub = (struct envoy_api_v2_route_HeaderMatcher*)_upb_msg_new(&envoy_api_v2_route_HeaderMatcher_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(12, 24), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher *envoy_api_v2_route_HeaderMatcher_new(upb_arena *arena) {
  return (envoy_api_v2_route_HeaderMatcher *)_upb_msg_new(&envoy_api_v2_route_HeaderMatcher_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_HeaderMatcher *envoy_api_v2_route_HeaderMatcher_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_HeaderMatcher *ret = envoy_api_v2_route_HeaderMatcher_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_HeaderMatcher_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_HeaderMatcher_serialize(const envoy_api_v2_route_HeaderMatcher *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_HeaderMatcher_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_HeaderMatcher_header_match_specifier_exact_match = 4,
  envoy_api_v2_route_HeaderMatcher_header_match_specifier_regex_match = 5,
  envoy_api_v2_route_HeaderMatcher_header_match_specifier_safe_regex_match = 11,
  envoy_api_v2_route_HeaderMatcher_header_match_specifier_range_match = 6,
  envoy_api_v2_route_HeaderMatcher_header_match_specifier_present_match = 7,
  envoy_api_v2_route_HeaderMatcher_header_match_specifier_prefix_match = 9,
  envoy_api_v2_route_HeaderMatcher_header_match_specifier_suffix_match = 10,
  envoy_api_v2_route_HeaderMatcher_header_match_specifier_NOT_SET = 0
} envoy_api_v2_route_HeaderMatcher_header_match_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_HeaderMatcher_header_match_specifier_oneofcases envoy_api_v2_route_HeaderMatcher_header_match_specifier_case(const envoy_api_v2_route_HeaderMatcher* msg) { return (envoy_api_v2_route_HeaderMatcher_header_match_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(20, 40)); }
UPB_INLINE upb_strview envoy_api_v2_route_HeaderMatcher_name(const envoy_api_v2_route_HeaderMatcher *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(4, 8)); }
UPB_INLINE bool envoy_api_v2_route_HeaderMatcher_has_exact_match(const envoy_api_v2_route_HeaderMatcher *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(20, 40), 4); }
UPB_INLINE upb_strview envoy_api_v2_route_HeaderMatcher_exact_match(const envoy_api_v2_route_HeaderMatcher *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(12, 24), UPB_SIZE(20, 40), 4, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_HeaderMatcher_has_regex_match(const envoy_api_v2_route_HeaderMatcher *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(20, 40), 5); }
UPB_INLINE upb_strview envoy_api_v2_route_HeaderMatcher_regex_match(const envoy_api_v2_route_HeaderMatcher *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(12, 24), UPB_SIZE(20, 40), 5, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_HeaderMatcher_has_range_match(const envoy_api_v2_route_HeaderMatcher *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(20, 40), 6); }
UPB_INLINE const struct envoy_type_Int64Range* envoy_api_v2_route_HeaderMatcher_range_match(const envoy_api_v2_route_HeaderMatcher *msg) { return UPB_READ_ONEOF(msg, const struct envoy_type_Int64Range*, UPB_SIZE(12, 24), UPB_SIZE(20, 40), 6, NULL); }
UPB_INLINE bool envoy_api_v2_route_HeaderMatcher_has_present_match(const envoy_api_v2_route_HeaderMatcher *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(20, 40), 7); }
UPB_INLINE bool envoy_api_v2_route_HeaderMatcher_present_match(const envoy_api_v2_route_HeaderMatcher *msg) { return UPB_READ_ONEOF(msg, bool, UPB_SIZE(12, 24), UPB_SIZE(20, 40), 7, false); }
UPB_INLINE bool envoy_api_v2_route_HeaderMatcher_invert_match(const envoy_api_v2_route_HeaderMatcher *msg) { return UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)); }
UPB_INLINE bool envoy_api_v2_route_HeaderMatcher_has_prefix_match(const envoy_api_v2_route_HeaderMatcher *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(20, 40), 9); }
UPB_INLINE upb_strview envoy_api_v2_route_HeaderMatcher_prefix_match(const envoy_api_v2_route_HeaderMatcher *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(12, 24), UPB_SIZE(20, 40), 9, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_HeaderMatcher_has_suffix_match(const envoy_api_v2_route_HeaderMatcher *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(20, 40), 10); }
UPB_INLINE upb_strview envoy_api_v2_route_HeaderMatcher_suffix_match(const envoy_api_v2_route_HeaderMatcher *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(12, 24), UPB_SIZE(20, 40), 10, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_route_HeaderMatcher_has_safe_regex_match(const envoy_api_v2_route_HeaderMatcher *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(20, 40), 11); }
UPB_INLINE const struct envoy_type_matcher_RegexMatcher* envoy_api_v2_route_HeaderMatcher_safe_regex_match(const envoy_api_v2_route_HeaderMatcher *msg) { return UPB_READ_ONEOF(msg, const struct envoy_type_matcher_RegexMatcher*, UPB_SIZE(12, 24), UPB_SIZE(20, 40), 11, NULL); }
UPB_INLINE void envoy_api_v2_route_HeaderMatcher_set_name(envoy_api_v2_route_HeaderMatcher *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE void envoy_api_v2_route_HeaderMatcher_set_exact_match(envoy_api_v2_route_HeaderMatcher *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(12, 24), value, UPB_SIZE(20, 40), 4);
}
UPB_INLINE void envoy_api_v2_route_HeaderMatcher_set_regex_match(envoy_api_v2_route_HeaderMatcher *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(12, 24), value, UPB_SIZE(20, 40), 5);
}
UPB_INLINE void envoy_api_v2_route_HeaderMatcher_set_range_match(envoy_api_v2_route_HeaderMatcher *msg, struct envoy_type_Int64Range* value) {
  UPB_WRITE_ONEOF(msg, struct envoy_type_Int64Range*, UPB_SIZE(12, 24), value, UPB_SIZE(20, 40), 6);
}
UPB_INLINE struct envoy_type_Int64Range* envoy_api_v2_route_HeaderMatcher_mutable_range_match(envoy_api_v2_route_HeaderMatcher *msg, upb_arena *arena) {
  struct envoy_type_Int64Range* sub = (struct envoy_type_Int64Range*)envoy_api_v2_route_HeaderMatcher_range_match(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_Int64Range*)_upb_msg_new(&envoy_type_Int64Range_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_HeaderMatcher_set_range_match(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_HeaderMatcher_set_present_match(envoy_api_v2_route_HeaderMatcher *msg, bool value) {
  UPB_WRITE_ONEOF(msg, bool, UPB_SIZE(12, 24), value, UPB_SIZE(20, 40), 7);
}
UPB_INLINE void envoy_api_v2_route_HeaderMatcher_set_invert_match(envoy_api_v2_route_HeaderMatcher *msg, bool value) {
  UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_HeaderMatcher_set_prefix_match(envoy_api_v2_route_HeaderMatcher *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(12, 24), value, UPB_SIZE(20, 40), 9);
}
UPB_INLINE void envoy_api_v2_route_HeaderMatcher_set_suffix_match(envoy_api_v2_route_HeaderMatcher *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(12, 24), value, UPB_SIZE(20, 40), 10);
}
UPB_INLINE void envoy_api_v2_route_HeaderMatcher_set_safe_regex_match(envoy_api_v2_route_HeaderMatcher *msg, struct envoy_type_matcher_RegexMatcher* value) {
  UPB_WRITE_ONEOF(msg, struct envoy_type_matcher_RegexMatcher*, UPB_SIZE(12, 24), value, UPB_SIZE(20, 40), 11);
}
UPB_INLINE struct envoy_type_matcher_RegexMatcher* envoy_api_v2_route_HeaderMatcher_mutable_safe_regex_match(envoy_api_v2_route_HeaderMatcher *msg, upb_arena *arena) {
  struct envoy_type_matcher_RegexMatcher* sub = (struct envoy_type_matcher_RegexMatcher*)envoy_api_v2_route_HeaderMatcher_safe_regex_match(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_matcher_RegexMatcher*)_upb_msg_new(&envoy_type_matcher_RegexMatcher_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_HeaderMatcher_set_safe_regex_match(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_route_QueryParameterMatcher *envoy_api_v2_route_QueryParameterMatcher_new(upb_arena *arena) {
  return (envoy_api_v2_route_QueryParameterMatcher *)_upb_msg_new(&envoy_api_v2_route_QueryParameterMatcher_msginit, arena);
}
UPB_INLINE envoy_api_v2_route_QueryParameterMatcher *envoy_api_v2_route_QueryParameterMatcher_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_route_QueryParameterMatcher *ret = envoy_api_v2_route_QueryParameterMatcher_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_route_QueryParameterMatcher_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_route_QueryParameterMatcher_serialize(const envoy_api_v2_route_QueryParameterMatcher *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_route_QueryParameterMatcher_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_route_QueryParameterMatcher_query_parameter_match_specifier_string_match = 5,
  envoy_api_v2_route_QueryParameterMatcher_query_parameter_match_specifier_present_match = 6,
  envoy_api_v2_route_QueryParameterMatcher_query_parameter_match_specifier_NOT_SET = 0
} envoy_api_v2_route_QueryParameterMatcher_query_parameter_match_specifier_oneofcases;
UPB_INLINE envoy_api_v2_route_QueryParameterMatcher_query_parameter_match_specifier_oneofcases envoy_api_v2_route_QueryParameterMatcher_query_parameter_match_specifier_case(const envoy_api_v2_route_QueryParameterMatcher* msg) { return (envoy_api_v2_route_QueryParameterMatcher_query_parameter_match_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(24, 48)); }
UPB_INLINE upb_strview envoy_api_v2_route_QueryParameterMatcher_name(const envoy_api_v2_route_QueryParameterMatcher *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE upb_strview envoy_api_v2_route_QueryParameterMatcher_value(const envoy_api_v2_route_QueryParameterMatcher *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_route_QueryParameterMatcher_regex(const envoy_api_v2_route_QueryParameterMatcher *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(16, 32)); }
UPB_INLINE bool envoy_api_v2_route_QueryParameterMatcher_has_string_match(const envoy_api_v2_route_QueryParameterMatcher *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(24, 48), 5); }
UPB_INLINE const struct envoy_type_matcher_StringMatcher* envoy_api_v2_route_QueryParameterMatcher_string_match(const envoy_api_v2_route_QueryParameterMatcher *msg) { return UPB_READ_ONEOF(msg, const struct envoy_type_matcher_StringMatcher*, UPB_SIZE(20, 40), UPB_SIZE(24, 48), 5, NULL); }
UPB_INLINE bool envoy_api_v2_route_QueryParameterMatcher_has_present_match(const envoy_api_v2_route_QueryParameterMatcher *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(24, 48), 6); }
UPB_INLINE bool envoy_api_v2_route_QueryParameterMatcher_present_match(const envoy_api_v2_route_QueryParameterMatcher *msg) { return UPB_READ_ONEOF(msg, bool, UPB_SIZE(20, 40), UPB_SIZE(24, 48), 6, false); }
UPB_INLINE void envoy_api_v2_route_QueryParameterMatcher_set_name(envoy_api_v2_route_QueryParameterMatcher *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_route_QueryParameterMatcher_set_value(envoy_api_v2_route_QueryParameterMatcher *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE void envoy_api_v2_route_QueryParameterMatcher_set_regex(envoy_api_v2_route_QueryParameterMatcher *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(16, 32)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_route_QueryParameterMatcher_mutable_regex(envoy_api_v2_route_QueryParameterMatcher *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_route_QueryParameterMatcher_regex(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_QueryParameterMatcher_set_regex(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_QueryParameterMatcher_set_string_match(envoy_api_v2_route_QueryParameterMatcher *msg, struct envoy_type_matcher_StringMatcher* value) {
  UPB_WRITE_ONEOF(msg, struct envoy_type_matcher_StringMatcher*, UPB_SIZE(20, 40), value, UPB_SIZE(24, 48), 5);
}
UPB_INLINE struct envoy_type_matcher_StringMatcher* envoy_api_v2_route_QueryParameterMatcher_mutable_string_match(envoy_api_v2_route_QueryParameterMatcher *msg, upb_arena *arena) {
  struct envoy_type_matcher_StringMatcher* sub = (struct envoy_type_matcher_StringMatcher*)envoy_api_v2_route_QueryParameterMatcher_string_match(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_matcher_StringMatcher*)_upb_msg_new(&envoy_type_matcher_StringMatcher_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_route_QueryParameterMatcher_set_string_match(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_route_QueryParameterMatcher_set_present_match(envoy_api_v2_route_QueryParameterMatcher *msg, bool value) {
  UPB_WRITE_ONEOF(msg, bool, UPB_SIZE(20, 40), value, UPB_SIZE(24, 48), 6);
}
#ifdef __cplusplus
}
#endif
#include "upb/port_undef.inc"
#endif
