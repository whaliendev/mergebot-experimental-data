#ifndef ENVOY_API_V2_CORE_BASE_PROTO_UPB_H_
#define ENVOY_API_V2_CORE_BASE_PROTO_UPB_H_ 
#include "upb/msg.h"
#include "upb/decode.h"
#include "upb/encode.h"
#include "envoy/api/v2/core/socket_option.upb.h"
#include "upb/port_def.inc"
#ifdef __cplusplus
extern "C" {
#endif
struct envoy_api_v2_core_Locality;
struct envoy_api_v2_core_BuildVersion;
struct envoy_api_v2_core_Extension;
struct envoy_api_v2_core_Node;
struct envoy_api_v2_core_Metadata;
struct envoy_api_v2_core_Metadata_FilterMetadataEntry;
struct envoy_api_v2_core_RuntimeUInt32;
struct envoy_api_v2_core_RuntimeDouble;
struct envoy_api_v2_core_RuntimeFeatureFlag;
struct envoy_api_v2_core_HeaderValue;
struct envoy_api_v2_core_HeaderValueOption;
struct envoy_api_v2_core_HeaderMap;
struct envoy_api_v2_core_DataSource;
struct envoy_api_v2_core_RetryPolicy;
struct envoy_api_v2_core_RemoteDataSource;
struct envoy_api_v2_core_AsyncDataSource;
struct envoy_api_v2_core_TransportSocket;
struct envoy_api_v2_core_RuntimeFractionalPercent;
struct envoy_api_v2_core_ControlPlane;
typedef struct envoy_api_v2_core_Locality envoy_api_v2_core_Locality;
typedef struct envoy_api_v2_core_BuildVersion envoy_api_v2_core_BuildVersion;
typedef struct envoy_api_v2_core_Extension envoy_api_v2_core_Extension;
typedef struct envoy_api_v2_core_Node envoy_api_v2_core_Node;
typedef struct envoy_api_v2_core_Metadata envoy_api_v2_core_Metadata;
typedef struct envoy_api_v2_core_Metadata_FilterMetadataEntry envoy_api_v2_core_Metadata_FilterMetadataEntry;
typedef struct envoy_api_v2_core_RuntimeUInt32 envoy_api_v2_core_RuntimeUInt32;
typedef struct envoy_api_v2_core_RuntimeDouble envoy_api_v2_core_RuntimeDouble;
typedef struct envoy_api_v2_core_RuntimeFeatureFlag envoy_api_v2_core_RuntimeFeatureFlag;
typedef struct envoy_api_v2_core_HeaderValue envoy_api_v2_core_HeaderValue;
typedef struct envoy_api_v2_core_HeaderValueOption envoy_api_v2_core_HeaderValueOption;
typedef struct envoy_api_v2_core_HeaderMap envoy_api_v2_core_HeaderMap;
typedef struct envoy_api_v2_core_DataSource envoy_api_v2_core_DataSource;
typedef struct envoy_api_v2_core_RetryPolicy envoy_api_v2_core_RetryPolicy;
typedef struct envoy_api_v2_core_RemoteDataSource envoy_api_v2_core_RemoteDataSource;
typedef struct envoy_api_v2_core_AsyncDataSource envoy_api_v2_core_AsyncDataSource;
typedef struct envoy_api_v2_core_TransportSocket envoy_api_v2_core_TransportSocket;
typedef struct envoy_api_v2_core_RuntimeFractionalPercent envoy_api_v2_core_RuntimeFractionalPercent;
typedef struct envoy_api_v2_core_ControlPlane envoy_api_v2_core_ControlPlane;
extern const upb_msglayout envoy_api_v2_core_Locality_msginit;
extern const upb_msglayout envoy_api_v2_core_BuildVersion_msginit;
extern const upb_msglayout envoy_api_v2_core_Extension_msginit;
extern const upb_msglayout envoy_api_v2_core_Node_msginit;
extern const upb_msglayout envoy_api_v2_core_Metadata_msginit;
extern const upb_msglayout envoy_api_v2_core_Metadata_FilterMetadataEntry_msginit;
extern const upb_msglayout envoy_api_v2_core_RuntimeUInt32_msginit;
extern const upb_msglayout envoy_api_v2_core_RuntimeDouble_msginit;
extern const upb_msglayout envoy_api_v2_core_RuntimeFeatureFlag_msginit;
extern const upb_msglayout envoy_api_v2_core_HeaderValue_msginit;
extern const upb_msglayout envoy_api_v2_core_HeaderValueOption_msginit;
extern const upb_msglayout envoy_api_v2_core_HeaderMap_msginit;
extern const upb_msglayout envoy_api_v2_core_DataSource_msginit;
extern const upb_msglayout envoy_api_v2_core_RetryPolicy_msginit;
extern const upb_msglayout envoy_api_v2_core_RemoteDataSource_msginit;
extern const upb_msglayout envoy_api_v2_core_AsyncDataSource_msginit;
extern const upb_msglayout envoy_api_v2_core_TransportSocket_msginit;
extern const upb_msglayout envoy_api_v2_core_RuntimeFractionalPercent_msginit;
extern const upb_msglayout envoy_api_v2_core_ControlPlane_msginit;
struct envoy_api_v2_core_Address;
struct envoy_api_v2_core_BackoffStrategy;
struct envoy_api_v2_core_HttpUri;
struct envoy_type_FractionalPercent;
struct envoy_type_SemanticVersion;
struct google_protobuf_Any;
struct google_protobuf_BoolValue;
struct google_protobuf_Struct;
struct google_protobuf_UInt32Value;
extern const upb_msglayout envoy_api_v2_core_Address_msginit;
extern const upb_msglayout envoy_api_v2_core_BackoffStrategy_msginit;
extern const upb_msglayout envoy_api_v2_core_HttpUri_msginit;
extern const upb_msglayout envoy_type_FractionalPercent_msginit;
extern const upb_msglayout envoy_type_SemanticVersion_msginit;
extern const upb_msglayout google_protobuf_Any_msginit;
extern const upb_msglayout google_protobuf_BoolValue_msginit;
extern const upb_msglayout google_protobuf_Struct_msginit;
extern const upb_msglayout google_protobuf_UInt32Value_msginit;
typedef enum {
  envoy_api_v2_core_METHOD_UNSPECIFIED = 0,
  envoy_api_v2_core_GET = 1,
  envoy_api_v2_core_HEAD = 2,
  envoy_api_v2_core_POST = 3,
  envoy_api_v2_core_PUT = 4,
  envoy_api_v2_core_DELETE = 5,
  envoy_api_v2_core_CONNECT = 6,
  envoy_api_v2_core_OPTIONS = 7,
  envoy_api_v2_core_TRACE = 8,
  envoy_api_v2_core_PATCH = 9
} envoy_api_v2_core_RequestMethod;
typedef enum {
  envoy_api_v2_core_DEFAULT = 0,
  envoy_api_v2_core_HIGH = 1
} envoy_api_v2_core_RoutingPriority;
typedef enum {
  envoy_api_v2_core_UNSPECIFIED = 0,
  envoy_api_v2_core_INBOUND = 1,
  envoy_api_v2_core_OUTBOUND = 2
} envoy_api_v2_core_TrafficDirection;
UPB_INLINE envoy_api_v2_core_Locality *envoy_api_v2_core_Locality_new(upb_arena *arena) {
  return (envoy_api_v2_core_Locality *)_upb_msg_new(&envoy_api_v2_core_Locality_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_Locality *envoy_api_v2_core_Locality_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_Locality *ret = envoy_api_v2_core_Locality_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_Locality_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_Locality_serialize(const envoy_api_v2_core_Locality *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_Locality_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_core_Locality_region(const envoy_api_v2_core_Locality *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE upb_strview envoy_api_v2_core_Locality_zone(const envoy_api_v2_core_Locality *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)); }
UPB_INLINE upb_strview envoy_api_v2_core_Locality_sub_zone(const envoy_api_v2_core_Locality *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 32)); }
UPB_INLINE void envoy_api_v2_core_Locality_set_region(envoy_api_v2_core_Locality *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_core_Locality_set_zone(envoy_api_v2_core_Locality *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE void envoy_api_v2_core_Locality_set_sub_zone(envoy_api_v2_core_Locality *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 32)) = value;
}
UPB_INLINE envoy_api_v2_core_BuildVersion *envoy_api_v2_core_BuildVersion_new(upb_arena *arena) {
  return (envoy_api_v2_core_BuildVersion *)_upb_msg_new(&envoy_api_v2_core_BuildVersion_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_BuildVersion *envoy_api_v2_core_BuildVersion_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_BuildVersion *ret = envoy_api_v2_core_BuildVersion_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_BuildVersion_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_BuildVersion_serialize(const envoy_api_v2_core_BuildVersion *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_BuildVersion_msginit, arena, len);
}
UPB_INLINE const struct envoy_type_SemanticVersion* envoy_api_v2_core_BuildVersion_version(const envoy_api_v2_core_BuildVersion *msg) { return UPB_FIELD_AT(msg, const struct envoy_type_SemanticVersion*, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_Struct* envoy_api_v2_core_BuildVersion_metadata(const envoy_api_v2_core_BuildVersion *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Struct*, UPB_SIZE(4, 8)); }
UPB_INLINE void envoy_api_v2_core_BuildVersion_set_version(envoy_api_v2_core_BuildVersion *msg, struct envoy_type_SemanticVersion* value) {
  UPB_FIELD_AT(msg, struct envoy_type_SemanticVersion*, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE struct envoy_type_SemanticVersion* envoy_api_v2_core_BuildVersion_mutable_version(envoy_api_v2_core_BuildVersion *msg, upb_arena *arena) {
  struct envoy_type_SemanticVersion* sub = (struct envoy_type_SemanticVersion*)envoy_api_v2_core_BuildVersion_version(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_SemanticVersion*)_upb_msg_new(&envoy_type_SemanticVersion_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_BuildVersion_set_version(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_BuildVersion_set_metadata(envoy_api_v2_core_BuildVersion *msg, struct google_protobuf_Struct* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Struct*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct google_protobuf_Struct* envoy_api_v2_core_BuildVersion_mutable_metadata(envoy_api_v2_core_BuildVersion *msg, upb_arena *arena) {
  struct google_protobuf_Struct* sub = (struct google_protobuf_Struct*)envoy_api_v2_core_BuildVersion_metadata(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Struct*)_upb_msg_new(&google_protobuf_Struct_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_BuildVersion_set_metadata(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_core_Extension *envoy_api_v2_core_Extension_new(upb_arena *arena) {
  return (envoy_api_v2_core_Extension *)_upb_msg_new(&envoy_api_v2_core_Extension_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_Extension *envoy_api_v2_core_Extension_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_Extension *ret = envoy_api_v2_core_Extension_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_Extension_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_Extension_serialize(const envoy_api_v2_core_Extension *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_Extension_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_core_Extension_name(const envoy_api_v2_core_Extension *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(4, 8)); }
UPB_INLINE upb_strview envoy_api_v2_core_Extension_category(const envoy_api_v2_core_Extension *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(12, 24)); }
UPB_INLINE upb_strview envoy_api_v2_core_Extension_type_descriptor(const envoy_api_v2_core_Extension *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(20, 40)); }
UPB_INLINE const envoy_api_v2_core_BuildVersion* envoy_api_v2_core_Extension_version(const envoy_api_v2_core_Extension *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_core_BuildVersion*, UPB_SIZE(28, 56)); }
UPB_INLINE bool envoy_api_v2_core_Extension_disabled(const envoy_api_v2_core_Extension *msg) { return UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_core_Extension_set_name(envoy_api_v2_core_Extension *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE void envoy_api_v2_core_Extension_set_category(envoy_api_v2_core_Extension *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(12, 24)) = value;
}
UPB_INLINE void envoy_api_v2_core_Extension_set_type_descriptor(envoy_api_v2_core_Extension *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(20, 40)) = value;
}
UPB_INLINE void envoy_api_v2_core_Extension_set_version(envoy_api_v2_core_Extension *msg, envoy_api_v2_core_BuildVersion* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_core_BuildVersion*, UPB_SIZE(28, 56)) = value;
}
UPB_INLINE struct envoy_api_v2_core_BuildVersion* envoy_api_v2_core_Extension_mutable_version(envoy_api_v2_core_Extension *msg, upb_arena *arena) {
  struct envoy_api_v2_core_BuildVersion* sub = (struct envoy_api_v2_core_BuildVersion*)envoy_api_v2_core_Extension_version(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_BuildVersion*)_upb_msg_new(&envoy_api_v2_core_BuildVersion_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_Extension_set_version(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_Extension_set_disabled(envoy_api_v2_core_Extension *msg, bool value) {
  UPB_FIELD_AT(msg, bool, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_core_Node *envoy_api_v2_core_Node_new(upb_arena *arena) {
  return (envoy_api_v2_core_Node *)_upb_msg_new(&envoy_api_v2_core_Node_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_Node *envoy_api_v2_core_Node_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_Node *ret = envoy_api_v2_core_Node_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_Node_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_Node_serialize(const envoy_api_v2_core_Node *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_Node_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_core_Node_user_agent_version_type_user_agent_version = 7,
  envoy_api_v2_core_Node_user_agent_version_type_user_agent_build_version = 8,
  envoy_api_v2_core_Node_user_agent_version_type_NOT_SET = 0
} envoy_api_v2_core_Node_user_agent_version_type_oneofcases;
UPB_INLINE envoy_api_v2_core_Node_user_agent_version_type_oneofcases envoy_api_v2_core_Node_user_agent_version_type_case(const envoy_api_v2_core_Node* msg) { return (envoy_api_v2_core_Node_user_agent_version_type_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(60, 120)); }
UPB_INLINE upb_strview envoy_api_v2_core_Node_id(const envoy_api_v2_core_Node *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE upb_strview envoy_api_v2_core_Node_cluster(const envoy_api_v2_core_Node *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)); }
UPB_INLINE const struct google_protobuf_Struct* envoy_api_v2_core_Node_metadata(const envoy_api_v2_core_Node *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_Struct*, UPB_SIZE(32, 64)); }
UPB_INLINE const envoy_api_v2_core_Locality* envoy_api_v2_core_Node_locality(const envoy_api_v2_core_Node *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_core_Locality*, UPB_SIZE(36, 72)); }
UPB_INLINE upb_strview envoy_api_v2_core_Node_build_version(const envoy_api_v2_core_Node *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 32)); }
UPB_INLINE upb_strview envoy_api_v2_core_Node_user_agent_name(const envoy_api_v2_core_Node *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 48)); }
UPB_INLINE bool envoy_api_v2_core_Node_has_user_agent_version(const envoy_api_v2_core_Node *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(60, 120), 7); }
UPB_INLINE upb_strview envoy_api_v2_core_Node_user_agent_version(const envoy_api_v2_core_Node *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(52, 104), UPB_SIZE(60, 120), 7, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_core_Node_has_user_agent_build_version(const envoy_api_v2_core_Node *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(60, 120), 8); }
UPB_INLINE const envoy_api_v2_core_BuildVersion* envoy_api_v2_core_Node_user_agent_build_version(const envoy_api_v2_core_Node *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_core_BuildVersion*, UPB_SIZE(52, 104), UPB_SIZE(60, 120), 8, NULL); }
UPB_INLINE const envoy_api_v2_core_Extension* const* envoy_api_v2_core_Node_extensions(const envoy_api_v2_core_Node *msg, size_t *len) { return (const envoy_api_v2_core_Extension* const*)_upb_array_accessor(msg, UPB_SIZE(40, 80), len); }
UPB_INLINE upb_strview const* envoy_api_v2_core_Node_client_features(const envoy_api_v2_core_Node *msg, size_t *len) { return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(44, 88), len); }
UPB_INLINE const struct envoy_api_v2_core_Address* const* envoy_api_v2_core_Node_listening_addresses(const envoy_api_v2_core_Node *msg, size_t *len) { return (const struct envoy_api_v2_core_Address* const*)_upb_array_accessor(msg, UPB_SIZE(48, 96), len); }
UPB_INLINE void envoy_api_v2_core_Node_set_id(envoy_api_v2_core_Node *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_core_Node_set_cluster(envoy_api_v2_core_Node *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE void envoy_api_v2_core_Node_set_metadata(envoy_api_v2_core_Node *msg, struct google_protobuf_Struct* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_Struct*, UPB_SIZE(32, 64)) = value;
}
UPB_INLINE struct google_protobuf_Struct* envoy_api_v2_core_Node_mutable_metadata(envoy_api_v2_core_Node *msg, upb_arena *arena) {
  struct google_protobuf_Struct* sub = (struct google_protobuf_Struct*)envoy_api_v2_core_Node_metadata(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Struct*)_upb_msg_new(&google_protobuf_Struct_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_Node_set_metadata(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_Node_set_locality(envoy_api_v2_core_Node *msg, envoy_api_v2_core_Locality* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_core_Locality*, UPB_SIZE(36, 72)) = value;
}
UPB_INLINE struct envoy_api_v2_core_Locality* envoy_api_v2_core_Node_mutable_locality(envoy_api_v2_core_Node *msg, upb_arena *arena) {
  struct envoy_api_v2_core_Locality* sub = (struct envoy_api_v2_core_Locality*)envoy_api_v2_core_Node_locality(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_Locality*)_upb_msg_new(&envoy_api_v2_core_Locality_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_Node_set_locality(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_Node_set_build_version(envoy_api_v2_core_Node *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(16, 32)) = value;
}
UPB_INLINE void envoy_api_v2_core_Node_set_user_agent_name(envoy_api_v2_core_Node *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 48)) = value;
}
UPB_INLINE void envoy_api_v2_core_Node_set_user_agent_version(envoy_api_v2_core_Node *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(52, 104), value, UPB_SIZE(60, 120), 7);
}
UPB_INLINE void envoy_api_v2_core_Node_set_user_agent_build_version(envoy_api_v2_core_Node *msg, envoy_api_v2_core_BuildVersion* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_core_BuildVersion*, UPB_SIZE(52, 104), value, UPB_SIZE(60, 120), 8);
}
UPB_INLINE struct envoy_api_v2_core_BuildVersion* envoy_api_v2_core_Node_mutable_user_agent_build_version(envoy_api_v2_core_Node *msg, upb_arena *arena) {
  struct envoy_api_v2_core_BuildVersion* sub = (struct envoy_api_v2_core_BuildVersion*)envoy_api_v2_core_Node_user_agent_build_version(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_BuildVersion*)_upb_msg_new(&envoy_api_v2_core_BuildVersion_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_Node_set_user_agent_build_version(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_core_Extension** envoy_api_v2_core_Node_mutable_extensions(envoy_api_v2_core_Node *msg, size_t *len) {
  return (envoy_api_v2_core_Extension**)_upb_array_mutable_accessor(msg, UPB_SIZE(40, 80), len);
}
UPB_INLINE envoy_api_v2_core_Extension** envoy_api_v2_core_Node_resize_extensions(envoy_api_v2_core_Node *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_core_Extension**)_upb_array_resize_accessor(msg, UPB_SIZE(40, 80), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_core_Extension* envoy_api_v2_core_Node_add_extensions(envoy_api_v2_core_Node *msg, upb_arena *arena) {
  struct envoy_api_v2_core_Extension* sub = (struct envoy_api_v2_core_Extension*)_upb_msg_new(&envoy_api_v2_core_Extension_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(40, 80), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE upb_strview* envoy_api_v2_core_Node_mutable_client_features(envoy_api_v2_core_Node *msg, size_t *len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(44, 88), len);
}
UPB_INLINE upb_strview* envoy_api_v2_core_Node_resize_client_features(envoy_api_v2_core_Node *msg, size_t len, upb_arena *arena) {
  return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(44, 88), len, UPB_TYPE_STRING, arena);
}
UPB_INLINE bool envoy_api_v2_core_Node_add_client_features(envoy_api_v2_core_Node *msg, upb_strview val, upb_arena *arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(44, 88), UPB_SIZE(8, 16), UPB_TYPE_STRING, &val,
      arena);
}
UPB_INLINE struct envoy_api_v2_core_Address** envoy_api_v2_core_Node_mutable_listening_addresses(envoy_api_v2_core_Node *msg, size_t *len) {
  return (struct envoy_api_v2_core_Address**)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 96), len);
}
UPB_INLINE struct envoy_api_v2_core_Address** envoy_api_v2_core_Node_resize_listening_addresses(envoy_api_v2_core_Node *msg, size_t len, upb_arena *arena) {
  return (struct envoy_api_v2_core_Address**)_upb_array_resize_accessor(msg, UPB_SIZE(48, 96), len, UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_core_Address* envoy_api_v2_core_Node_add_listening_addresses(envoy_api_v2_core_Node *msg, upb_arena *arena) {
  struct envoy_api_v2_core_Address* sub = (struct envoy_api_v2_core_Address*)upb_msg_new(&envoy_api_v2_core_Address_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(48, 96), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_core_Metadata *envoy_api_v2_core_Metadata_new(upb_arena *arena) {
  return (envoy_api_v2_core_Metadata *)_upb_msg_new(&envoy_api_v2_core_Metadata_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_Metadata *envoy_api_v2_core_Metadata_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_Metadata *ret = envoy_api_v2_core_Metadata_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_Metadata_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_Metadata_serialize(const envoy_api_v2_core_Metadata *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_Metadata_msginit, arena, len);
}
UPB_INLINE size_t envoy_api_v2_core_Metadata_filter_metadata_size(const envoy_api_v2_core_Metadata *msg) {return _upb_msg_map_size(msg, UPB_SIZE(0, 0)); }
UPB_INLINE bool envoy_api_v2_core_Metadata_filter_metadata_get(const envoy_api_v2_core_Metadata *msg, upb_strview key, struct google_protobuf_Struct* *val) { return _upb_msg_map_get(msg, UPB_SIZE(0, 0), &key, 0, val, sizeof(*val)); }
UPB_INLINE const envoy_api_v2_core_Metadata_FilterMetadataEntry* envoy_api_v2_core_Metadata_filter_metadata_next(const envoy_api_v2_core_Metadata *msg, size_t* iter) { return (const envoy_api_v2_core_Metadata_FilterMetadataEntry*)_upb_msg_map_next(msg, UPB_SIZE(0, 0), iter); }
UPB_INLINE void envoy_api_v2_core_Metadata_filter_metadata_clear(envoy_api_v2_core_Metadata *msg) { _upb_msg_map_clear(msg, UPB_SIZE(0, 0)); }
UPB_INLINE bool envoy_api_v2_core_Metadata_filter_metadata_set(envoy_api_v2_core_Metadata *msg, upb_strview key, struct google_protobuf_Struct* val, upb_arena *a) { return _upb_msg_map_set(msg, UPB_SIZE(0, 0), &key, 0, &val, sizeof(val), a); }
UPB_INLINE bool envoy_api_v2_core_Metadata_filter_metadata_delete(envoy_api_v2_core_Metadata *msg, upb_strview key) { return _upb_msg_map_delete(msg, UPB_SIZE(0, 0), &key, 0); }
UPB_INLINE envoy_api_v2_core_Metadata_FilterMetadataEntry* envoy_api_v2_core_Metadata_filter_metadata_nextmutable(envoy_api_v2_core_Metadata *msg, size_t* iter) { return (envoy_api_v2_core_Metadata_FilterMetadataEntry*)_upb_msg_map_next(msg, UPB_SIZE(0, 0), iter); }
UPB_INLINE upb_strview envoy_api_v2_core_Metadata_FilterMetadataEntry_key(const envoy_api_v2_core_Metadata_FilterMetadataEntry *msg) {
  upb_strview ret;
  _upb_msg_map_key(msg, &ret, 0);
  return ret;
}
UPB_INLINE const struct google_protobuf_Struct* envoy_api_v2_core_Metadata_FilterMetadataEntry_value(const envoy_api_v2_core_Metadata_FilterMetadataEntry *msg) {
  struct google_protobuf_Struct* ret;
  _upb_msg_map_value(msg, &ret, sizeof(ret));
  return ret;
}
UPB_INLINE void envoy_api_v2_core_Metadata_FilterMetadataEntry_set_value(envoy_api_v2_core_Metadata_FilterMetadataEntry *msg, struct google_protobuf_Struct* value) {
  _upb_msg_map_set_value(msg, &value, sizeof(struct google_protobuf_Struct*));
}
UPB_INLINE envoy_api_v2_core_RuntimeUInt32 *envoy_api_v2_core_RuntimeUInt32_new(upb_arena *arena) {
  return (envoy_api_v2_core_RuntimeUInt32 *)_upb_msg_new(&envoy_api_v2_core_RuntimeUInt32_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_RuntimeUInt32 *envoy_api_v2_core_RuntimeUInt32_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_RuntimeUInt32 *ret = envoy_api_v2_core_RuntimeUInt32_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_RuntimeUInt32_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_RuntimeUInt32_serialize(const envoy_api_v2_core_RuntimeUInt32 *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_RuntimeUInt32_msginit, arena, len);
}
UPB_INLINE uint32_t envoy_api_v2_core_RuntimeUInt32_default_value(const envoy_api_v2_core_RuntimeUInt32 *msg) { return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(0, 0)); }
UPB_INLINE upb_strview envoy_api_v2_core_RuntimeUInt32_runtime_key(const envoy_api_v2_core_RuntimeUInt32 *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(4, 8)); }
UPB_INLINE void envoy_api_v2_core_RuntimeUInt32_set_default_value(envoy_api_v2_core_RuntimeUInt32 *msg, uint32_t value) {
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_core_RuntimeUInt32_set_runtime_key(envoy_api_v2_core_RuntimeUInt32 *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE envoy_api_v2_core_RuntimeDouble *envoy_api_v2_core_RuntimeDouble_new(upb_arena *arena) {
  return (envoy_api_v2_core_RuntimeDouble *)upb_msg_new(&envoy_api_v2_core_RuntimeDouble_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_RuntimeDouble *envoy_api_v2_core_RuntimeDouble_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_RuntimeDouble *ret = envoy_api_v2_core_RuntimeDouble_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_RuntimeDouble_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_RuntimeDouble_serialize(const envoy_api_v2_core_RuntimeDouble *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_RuntimeDouble_msginit, arena, len);
}
UPB_INLINE double envoy_api_v2_core_RuntimeDouble_default_value(const envoy_api_v2_core_RuntimeDouble *msg) { return UPB_FIELD_AT(msg, double, UPB_SIZE(0, 0)); }
UPB_INLINE upb_strview envoy_api_v2_core_RuntimeDouble_runtime_key(const envoy_api_v2_core_RuntimeDouble *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 8)); }
UPB_INLINE void envoy_api_v2_core_RuntimeDouble_set_default_value(envoy_api_v2_core_RuntimeDouble *msg, double value) {
  UPB_FIELD_AT(msg, double, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_core_RuntimeDouble_set_runtime_key(envoy_api_v2_core_RuntimeDouble *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE envoy_api_v2_core_RuntimeFeatureFlag *envoy_api_v2_core_RuntimeFeatureFlag_new(upb_arena *arena) {
  return (envoy_api_v2_core_RuntimeFeatureFlag *)_upb_msg_new(&envoy_api_v2_core_RuntimeFeatureFlag_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_RuntimeFeatureFlag *envoy_api_v2_core_RuntimeFeatureFlag_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_RuntimeFeatureFlag *ret = envoy_api_v2_core_RuntimeFeatureFlag_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_RuntimeFeatureFlag_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_RuntimeFeatureFlag_serialize(const envoy_api_v2_core_RuntimeFeatureFlag *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_RuntimeFeatureFlag_msginit, arena, len);
}
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_core_RuntimeFeatureFlag_default_value(const envoy_api_v2_core_RuntimeFeatureFlag *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(8, 16)); }
UPB_INLINE upb_strview envoy_api_v2_core_RuntimeFeatureFlag_runtime_key(const envoy_api_v2_core_RuntimeFeatureFlag *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_core_RuntimeFeatureFlag_set_default_value(envoy_api_v2_core_RuntimeFeatureFlag *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_core_RuntimeFeatureFlag_mutable_default_value(envoy_api_v2_core_RuntimeFeatureFlag *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_core_RuntimeFeatureFlag_default_value(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_RuntimeFeatureFlag_set_default_value(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_RuntimeFeatureFlag_set_runtime_key(envoy_api_v2_core_RuntimeFeatureFlag *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_core_HeaderValue *envoy_api_v2_core_HeaderValue_new(upb_arena *arena) {
  return (envoy_api_v2_core_HeaderValue *)_upb_msg_new(&envoy_api_v2_core_HeaderValue_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_HeaderValue *envoy_api_v2_core_HeaderValue_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_HeaderValue *ret = envoy_api_v2_core_HeaderValue_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_HeaderValue_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_HeaderValue_serialize(const envoy_api_v2_core_HeaderValue *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_HeaderValue_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_core_HeaderValue_key(const envoy_api_v2_core_HeaderValue *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE upb_strview envoy_api_v2_core_HeaderValue_value(const envoy_api_v2_core_HeaderValue *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)); }
UPB_INLINE void envoy_api_v2_core_HeaderValue_set_key(envoy_api_v2_core_HeaderValue *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_core_HeaderValue_set_value(envoy_api_v2_core_HeaderValue *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE envoy_api_v2_core_HeaderValueOption *envoy_api_v2_core_HeaderValueOption_new(upb_arena *arena) {
  return (envoy_api_v2_core_HeaderValueOption *)_upb_msg_new(&envoy_api_v2_core_HeaderValueOption_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_HeaderValueOption *envoy_api_v2_core_HeaderValueOption_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_HeaderValueOption *ret = envoy_api_v2_core_HeaderValueOption_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_HeaderValueOption_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_HeaderValueOption_serialize(const envoy_api_v2_core_HeaderValueOption *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_HeaderValueOption_msginit, arena, len);
}
UPB_INLINE const envoy_api_v2_core_HeaderValue* envoy_api_v2_core_HeaderValueOption_header(const envoy_api_v2_core_HeaderValueOption *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_core_HeaderValue*, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_BoolValue* envoy_api_v2_core_HeaderValueOption_append(const envoy_api_v2_core_HeaderValueOption *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_BoolValue*, UPB_SIZE(4, 8)); }
UPB_INLINE void envoy_api_v2_core_HeaderValueOption_set_header(envoy_api_v2_core_HeaderValueOption *msg, envoy_api_v2_core_HeaderValue* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_core_HeaderValue*, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE struct envoy_api_v2_core_HeaderValue* envoy_api_v2_core_HeaderValueOption_mutable_header(envoy_api_v2_core_HeaderValueOption *msg, upb_arena *arena) {
  struct envoy_api_v2_core_HeaderValue* sub = (struct envoy_api_v2_core_HeaderValue*)envoy_api_v2_core_HeaderValueOption_header(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_HeaderValue*)_upb_msg_new(&envoy_api_v2_core_HeaderValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_HeaderValueOption_set_header(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_HeaderValueOption_set_append(envoy_api_v2_core_HeaderValueOption *msg, struct google_protobuf_BoolValue* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_BoolValue*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct google_protobuf_BoolValue* envoy_api_v2_core_HeaderValueOption_mutable_append(envoy_api_v2_core_HeaderValueOption *msg, upb_arena *arena) {
  struct google_protobuf_BoolValue* sub = (struct google_protobuf_BoolValue*)envoy_api_v2_core_HeaderValueOption_append(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_BoolValue*)_upb_msg_new(&google_protobuf_BoolValue_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_HeaderValueOption_set_append(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_core_HeaderMap *envoy_api_v2_core_HeaderMap_new(upb_arena *arena) {
  return (envoy_api_v2_core_HeaderMap *)_upb_msg_new(&envoy_api_v2_core_HeaderMap_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_HeaderMap *envoy_api_v2_core_HeaderMap_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_HeaderMap *ret = envoy_api_v2_core_HeaderMap_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_HeaderMap_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_HeaderMap_serialize(const envoy_api_v2_core_HeaderMap *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_HeaderMap_msginit, arena, len);
}
UPB_INLINE const envoy_api_v2_core_HeaderValue* const* envoy_api_v2_core_HeaderMap_headers(const envoy_api_v2_core_HeaderMap *msg, size_t *len) { return (const envoy_api_v2_core_HeaderValue* const*)_upb_array_accessor(msg, UPB_SIZE(0, 0), len); }
UPB_INLINE envoy_api_v2_core_HeaderValue** envoy_api_v2_core_HeaderMap_mutable_headers(envoy_api_v2_core_HeaderMap *msg, size_t *len) {
  return (envoy_api_v2_core_HeaderValue**)_upb_array_mutable_accessor(msg, UPB_SIZE(0, 0), len);
}
UPB_INLINE envoy_api_v2_core_HeaderValue** envoy_api_v2_core_HeaderMap_resize_headers(envoy_api_v2_core_HeaderMap *msg, size_t len, upb_arena *arena) {
  return (envoy_api_v2_core_HeaderValue**)_upb_array_resize_accessor(msg, UPB_SIZE(0, 0), len, UPB_TYPE_MESSAGE, arena);
}
UPB_INLINE struct envoy_api_v2_core_HeaderValue* envoy_api_v2_core_HeaderMap_add_headers(envoy_api_v2_core_HeaderMap *msg, upb_arena *arena) {
  struct envoy_api_v2_core_HeaderValue* sub = (struct envoy_api_v2_core_HeaderValue*)_upb_msg_new(&envoy_api_v2_core_HeaderValue_msginit, arena);
  bool ok = _upb_array_append_accessor(
      msg, UPB_SIZE(0, 0), UPB_SIZE(4, 8), UPB_TYPE_MESSAGE, &sub, arena);
  if (!ok) return NULL;
  return sub;
}
UPB_INLINE envoy_api_v2_core_DataSource *envoy_api_v2_core_DataSource_new(upb_arena *arena) {
  return (envoy_api_v2_core_DataSource *)_upb_msg_new(&envoy_api_v2_core_DataSource_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_DataSource *envoy_api_v2_core_DataSource_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_DataSource *ret = envoy_api_v2_core_DataSource_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_DataSource_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_DataSource_serialize(const envoy_api_v2_core_DataSource *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_DataSource_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_core_DataSource_specifier_filename = 1,
  envoy_api_v2_core_DataSource_specifier_inline_bytes = 2,
  envoy_api_v2_core_DataSource_specifier_inline_string = 3,
  envoy_api_v2_core_DataSource_specifier_NOT_SET = 0
} envoy_api_v2_core_DataSource_specifier_oneofcases;
UPB_INLINE envoy_api_v2_core_DataSource_specifier_oneofcases envoy_api_v2_core_DataSource_specifier_case(const envoy_api_v2_core_DataSource* msg) { return (envoy_api_v2_core_DataSource_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 16)); }
UPB_INLINE bool envoy_api_v2_core_DataSource_has_filename(const envoy_api_v2_core_DataSource *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(8, 16), 1); }
UPB_INLINE upb_strview envoy_api_v2_core_DataSource_filename(const envoy_api_v2_core_DataSource *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(0, 0), UPB_SIZE(8, 16), 1, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_core_DataSource_has_inline_bytes(const envoy_api_v2_core_DataSource *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(8, 16), 2); }
UPB_INLINE upb_strview envoy_api_v2_core_DataSource_inline_bytes(const envoy_api_v2_core_DataSource *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(0, 0), UPB_SIZE(8, 16), 2, upb_strview_make("", strlen(""))); }
UPB_INLINE bool envoy_api_v2_core_DataSource_has_inline_string(const envoy_api_v2_core_DataSource *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(8, 16), 3); }
UPB_INLINE upb_strview envoy_api_v2_core_DataSource_inline_string(const envoy_api_v2_core_DataSource *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(0, 0), UPB_SIZE(8, 16), 3, upb_strview_make("", strlen(""))); }
UPB_INLINE void envoy_api_v2_core_DataSource_set_filename(envoy_api_v2_core_DataSource *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(0, 0), value, UPB_SIZE(8, 16), 1);
}
UPB_INLINE void envoy_api_v2_core_DataSource_set_inline_bytes(envoy_api_v2_core_DataSource *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(0, 0), value, UPB_SIZE(8, 16), 2);
}
UPB_INLINE void envoy_api_v2_core_DataSource_set_inline_string(envoy_api_v2_core_DataSource *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(0, 0), value, UPB_SIZE(8, 16), 3);
}
UPB_INLINE envoy_api_v2_core_RetryPolicy *envoy_api_v2_core_RetryPolicy_new(upb_arena *arena) {
  return (envoy_api_v2_core_RetryPolicy *)upb_msg_new(&envoy_api_v2_core_RetryPolicy_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_RetryPolicy *envoy_api_v2_core_RetryPolicy_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_RetryPolicy *ret = envoy_api_v2_core_RetryPolicy_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_RetryPolicy_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_RetryPolicy_serialize(const envoy_api_v2_core_RetryPolicy *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_RetryPolicy_msginit, arena, len);
}
UPB_INLINE const struct envoy_api_v2_core_BackoffStrategy* envoy_api_v2_core_RetryPolicy_retry_back_off(const envoy_api_v2_core_RetryPolicy *msg) { return UPB_FIELD_AT(msg, const struct envoy_api_v2_core_BackoffStrategy*, UPB_SIZE(0, 0)); }
UPB_INLINE const struct google_protobuf_UInt32Value* envoy_api_v2_core_RetryPolicy_num_retries(const envoy_api_v2_core_RetryPolicy *msg) { return UPB_FIELD_AT(msg, const struct google_protobuf_UInt32Value*, UPB_SIZE(4, 8)); }
UPB_INLINE void envoy_api_v2_core_RetryPolicy_set_retry_back_off(envoy_api_v2_core_RetryPolicy *msg, struct envoy_api_v2_core_BackoffStrategy* value) {
  UPB_FIELD_AT(msg, struct envoy_api_v2_core_BackoffStrategy*, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE struct envoy_api_v2_core_BackoffStrategy* envoy_api_v2_core_RetryPolicy_mutable_retry_back_off(envoy_api_v2_core_RetryPolicy *msg, upb_arena *arena) {
  struct envoy_api_v2_core_BackoffStrategy* sub = (struct envoy_api_v2_core_BackoffStrategy*)envoy_api_v2_core_RetryPolicy_retry_back_off(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_BackoffStrategy*)upb_msg_new(&envoy_api_v2_core_BackoffStrategy_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_RetryPolicy_set_retry_back_off(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_RetryPolicy_set_num_retries(envoy_api_v2_core_RetryPolicy *msg, struct google_protobuf_UInt32Value* value) {
  UPB_FIELD_AT(msg, struct google_protobuf_UInt32Value*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct google_protobuf_UInt32Value* envoy_api_v2_core_RetryPolicy_mutable_num_retries(envoy_api_v2_core_RetryPolicy *msg, upb_arena *arena) {
  struct google_protobuf_UInt32Value* sub = (struct google_protobuf_UInt32Value*)envoy_api_v2_core_RetryPolicy_num_retries(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_UInt32Value*)upb_msg_new(&google_protobuf_UInt32Value_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_RetryPolicy_set_num_retries(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_core_RemoteDataSource *envoy_api_v2_core_RemoteDataSource_new(upb_arena *arena) {
  return (envoy_api_v2_core_RemoteDataSource *)_upb_msg_new(&envoy_api_v2_core_RemoteDataSource_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_RemoteDataSource *envoy_api_v2_core_RemoteDataSource_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_RemoteDataSource *ret = envoy_api_v2_core_RemoteDataSource_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_RemoteDataSource_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_RemoteDataSource_serialize(const envoy_api_v2_core_RemoteDataSource *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_RemoteDataSource_msginit, arena, len);
}
UPB_INLINE const struct envoy_api_v2_core_HttpUri* envoy_api_v2_core_RemoteDataSource_http_uri(const envoy_api_v2_core_RemoteDataSource *msg) { return UPB_FIELD_AT(msg, const struct envoy_api_v2_core_HttpUri*, UPB_SIZE(8, 16)); }
UPB_INLINE upb_strview envoy_api_v2_core_RemoteDataSource_sha256(const envoy_api_v2_core_RemoteDataSource *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE const envoy_api_v2_core_RetryPolicy* envoy_api_v2_core_RemoteDataSource_retry_policy(const envoy_api_v2_core_RemoteDataSource *msg) { return UPB_FIELD_AT(msg, const envoy_api_v2_core_RetryPolicy*, UPB_SIZE(12, 24)); }
UPB_INLINE void envoy_api_v2_core_RemoteDataSource_set_http_uri(envoy_api_v2_core_RemoteDataSource *msg, struct envoy_api_v2_core_HttpUri* value) {
  UPB_FIELD_AT(msg, struct envoy_api_v2_core_HttpUri*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct envoy_api_v2_core_HttpUri* envoy_api_v2_core_RemoteDataSource_mutable_http_uri(envoy_api_v2_core_RemoteDataSource *msg, upb_arena *arena) {
  struct envoy_api_v2_core_HttpUri* sub = (struct envoy_api_v2_core_HttpUri*)envoy_api_v2_core_RemoteDataSource_http_uri(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_HttpUri*)_upb_msg_new(&envoy_api_v2_core_HttpUri_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_RemoteDataSource_set_http_uri(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_RemoteDataSource_set_sha256(envoy_api_v2_core_RemoteDataSource *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_core_RemoteDataSource_set_retry_policy(envoy_api_v2_core_RemoteDataSource *msg, envoy_api_v2_core_RetryPolicy* value) {
  UPB_FIELD_AT(msg, envoy_api_v2_core_RetryPolicy*, UPB_SIZE(12, 24)) = value;
}
UPB_INLINE struct envoy_api_v2_core_RetryPolicy* envoy_api_v2_core_RemoteDataSource_mutable_retry_policy(envoy_api_v2_core_RemoteDataSource *msg, upb_arena *arena) {
  struct envoy_api_v2_core_RetryPolicy* sub = (struct envoy_api_v2_core_RetryPolicy*)envoy_api_v2_core_RemoteDataSource_retry_policy(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_RetryPolicy*)upb_msg_new(&envoy_api_v2_core_RetryPolicy_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_RemoteDataSource_set_retry_policy(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_core_AsyncDataSource *envoy_api_v2_core_AsyncDataSource_new(upb_arena *arena) {
  return (envoy_api_v2_core_AsyncDataSource *)_upb_msg_new(&envoy_api_v2_core_AsyncDataSource_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_AsyncDataSource *envoy_api_v2_core_AsyncDataSource_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_AsyncDataSource *ret = envoy_api_v2_core_AsyncDataSource_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_AsyncDataSource_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_AsyncDataSource_serialize(const envoy_api_v2_core_AsyncDataSource *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_AsyncDataSource_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_core_AsyncDataSource_specifier_local = 1,
  envoy_api_v2_core_AsyncDataSource_specifier_remote = 2,
  envoy_api_v2_core_AsyncDataSource_specifier_NOT_SET = 0
} envoy_api_v2_core_AsyncDataSource_specifier_oneofcases;
UPB_INLINE envoy_api_v2_core_AsyncDataSource_specifier_oneofcases envoy_api_v2_core_AsyncDataSource_specifier_case(const envoy_api_v2_core_AsyncDataSource* msg) { return (envoy_api_v2_core_AsyncDataSource_specifier_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(4, 8)); }
UPB_INLINE bool envoy_api_v2_core_AsyncDataSource_has_local(const envoy_api_v2_core_AsyncDataSource *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(4, 8), 1); }
UPB_INLINE const envoy_api_v2_core_DataSource* envoy_api_v2_core_AsyncDataSource_local(const envoy_api_v2_core_AsyncDataSource *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_core_DataSource*, UPB_SIZE(0, 0), UPB_SIZE(4, 8), 1, NULL); }
UPB_INLINE bool envoy_api_v2_core_AsyncDataSource_has_remote(const envoy_api_v2_core_AsyncDataSource *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(4, 8), 2); }
UPB_INLINE const envoy_api_v2_core_RemoteDataSource* envoy_api_v2_core_AsyncDataSource_remote(const envoy_api_v2_core_AsyncDataSource *msg) { return UPB_READ_ONEOF(msg, const envoy_api_v2_core_RemoteDataSource*, UPB_SIZE(0, 0), UPB_SIZE(4, 8), 2, NULL); }
UPB_INLINE void envoy_api_v2_core_AsyncDataSource_set_local(envoy_api_v2_core_AsyncDataSource *msg, envoy_api_v2_core_DataSource* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_core_DataSource*, UPB_SIZE(0, 0), value, UPB_SIZE(4, 8), 1);
}
UPB_INLINE struct envoy_api_v2_core_DataSource* envoy_api_v2_core_AsyncDataSource_mutable_local(envoy_api_v2_core_AsyncDataSource *msg, upb_arena *arena) {
  struct envoy_api_v2_core_DataSource* sub = (struct envoy_api_v2_core_DataSource*)envoy_api_v2_core_AsyncDataSource_local(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_DataSource*)_upb_msg_new(&envoy_api_v2_core_DataSource_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_AsyncDataSource_set_local(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_AsyncDataSource_set_remote(envoy_api_v2_core_AsyncDataSource *msg, envoy_api_v2_core_RemoteDataSource* value) {
  UPB_WRITE_ONEOF(msg, envoy_api_v2_core_RemoteDataSource*, UPB_SIZE(0, 0), value, UPB_SIZE(4, 8), 2);
}
UPB_INLINE struct envoy_api_v2_core_RemoteDataSource* envoy_api_v2_core_AsyncDataSource_mutable_remote(envoy_api_v2_core_AsyncDataSource *msg, upb_arena *arena) {
  struct envoy_api_v2_core_RemoteDataSource* sub = (struct envoy_api_v2_core_RemoteDataSource*)envoy_api_v2_core_AsyncDataSource_remote(msg);
  if (sub == NULL) {
    sub = (struct envoy_api_v2_core_RemoteDataSource*)_upb_msg_new(&envoy_api_v2_core_RemoteDataSource_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_AsyncDataSource_set_remote(msg, sub);
  }
  return sub;
}
UPB_INLINE envoy_api_v2_core_TransportSocket *envoy_api_v2_core_TransportSocket_new(upb_arena *arena) {
  return (envoy_api_v2_core_TransportSocket *)_upb_msg_new(&envoy_api_v2_core_TransportSocket_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_TransportSocket *envoy_api_v2_core_TransportSocket_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_TransportSocket *ret = envoy_api_v2_core_TransportSocket_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_TransportSocket_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_TransportSocket_serialize(const envoy_api_v2_core_TransportSocket *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_TransportSocket_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_core_TransportSocket_config_type_config = 2,
  envoy_api_v2_core_TransportSocket_config_type_typed_config = 3,
  envoy_api_v2_core_TransportSocket_config_type_NOT_SET = 0
} envoy_api_v2_core_TransportSocket_config_type_oneofcases;
UPB_INLINE envoy_api_v2_core_TransportSocket_config_type_oneofcases envoy_api_v2_core_TransportSocket_config_type_case(const envoy_api_v2_core_TransportSocket* msg) { return (envoy_api_v2_core_TransportSocket_config_type_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(12, 24)); }
UPB_INLINE upb_strview envoy_api_v2_core_TransportSocket_name(const envoy_api_v2_core_TransportSocket *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE bool envoy_api_v2_core_TransportSocket_has_config(const envoy_api_v2_core_TransportSocket *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 2); }
UPB_INLINE const struct google_protobuf_Struct* envoy_api_v2_core_TransportSocket_config(const envoy_api_v2_core_TransportSocket *msg) { return UPB_READ_ONEOF(msg, const struct google_protobuf_Struct*, UPB_SIZE(8, 16), UPB_SIZE(12, 24), 2, NULL); }
UPB_INLINE bool envoy_api_v2_core_TransportSocket_has_typed_config(const envoy_api_v2_core_TransportSocket *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 3); }
UPB_INLINE const struct google_protobuf_Any* envoy_api_v2_core_TransportSocket_typed_config(const envoy_api_v2_core_TransportSocket *msg) { return UPB_READ_ONEOF(msg, const struct google_protobuf_Any*, UPB_SIZE(8, 16), UPB_SIZE(12, 24), 3, NULL); }
UPB_INLINE void envoy_api_v2_core_TransportSocket_set_name(envoy_api_v2_core_TransportSocket *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_core_TransportSocket_set_config(envoy_api_v2_core_TransportSocket *msg, struct google_protobuf_Struct* value) {
  UPB_WRITE_ONEOF(msg, struct google_protobuf_Struct*, UPB_SIZE(8, 16), value, UPB_SIZE(12, 24), 2);
}
UPB_INLINE struct google_protobuf_Struct* envoy_api_v2_core_TransportSocket_mutable_config(envoy_api_v2_core_TransportSocket *msg, upb_arena *arena) {
  struct google_protobuf_Struct* sub = (struct google_protobuf_Struct*)envoy_api_v2_core_TransportSocket_config(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Struct*)_upb_msg_new(&google_protobuf_Struct_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_TransportSocket_set_config(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_TransportSocket_set_typed_config(envoy_api_v2_core_TransportSocket *msg, struct google_protobuf_Any* value) {
  UPB_WRITE_ONEOF(msg, struct google_protobuf_Any*, UPB_SIZE(8, 16), value, UPB_SIZE(12, 24), 3);
}
UPB_INLINE struct google_protobuf_Any* envoy_api_v2_core_TransportSocket_mutable_typed_config(envoy_api_v2_core_TransportSocket *msg, upb_arena *arena) {
  struct google_protobuf_Any* sub = (struct google_protobuf_Any*)envoy_api_v2_core_TransportSocket_typed_config(msg);
  if (sub == NULL) {
    sub = (struct google_protobuf_Any*)_upb_msg_new(&google_protobuf_Any_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_TransportSocket_set_typed_config(msg, sub);
  }
  return sub;
}
<<<<<<< HEAD
UPB_INLINE envoy_api_v2_core_SocketOption *envoy_api_v2_core_SocketOption_new(upb_arena *arena) {
  return (envoy_api_v2_core_SocketOption *)_upb_msg_new(&envoy_api_v2_core_SocketOption_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_SocketOption *envoy_api_v2_core_SocketOption_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_SocketOption *ret = envoy_api_v2_core_SocketOption_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_SocketOption_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_SocketOption_serialize(const envoy_api_v2_core_SocketOption *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_SocketOption_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_core_SocketOption_value_int_value = 4,
  envoy_api_v2_core_SocketOption_value_buf_value = 5,
  envoy_api_v2_core_SocketOption_value_NOT_SET = 0
} envoy_api_v2_core_SocketOption_value_oneofcases;
UPB_INLINE envoy_api_v2_core_SocketOption_value_oneofcases envoy_api_v2_core_SocketOption_value_case(const envoy_api_v2_core_SocketOption* msg) { return (envoy_api_v2_core_SocketOption_value_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(40, 56)); }
UPB_INLINE upb_strview envoy_api_v2_core_SocketOption_description(const envoy_api_v2_core_SocketOption *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 24)); }
UPB_INLINE int64_t envoy_api_v2_core_SocketOption_level(const envoy_api_v2_core_SocketOption *msg) { return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(0, 0)); }
UPB_INLINE int64_t envoy_api_v2_core_SocketOption_name(const envoy_api_v2_core_SocketOption *msg) { return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8)); }
UPB_INLINE bool envoy_api_v2_core_SocketOption_has_int_value(const envoy_api_v2_core_SocketOption *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(40, 56), 4); }
UPB_INLINE int64_t envoy_api_v2_core_SocketOption_int_value(const envoy_api_v2_core_SocketOption *msg) { return UPB_READ_ONEOF(msg, int64_t, UPB_SIZE(32, 40), UPB_SIZE(40, 56), 4, 0); }
UPB_INLINE bool envoy_api_v2_core_SocketOption_has_buf_value(const envoy_api_v2_core_SocketOption *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(40, 56), 5); }
UPB_INLINE upb_strview envoy_api_v2_core_SocketOption_buf_value(const envoy_api_v2_core_SocketOption *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(32, 40), UPB_SIZE(40, 56), 5, upb_strview_make("", strlen(""))); }
UPB_INLINE int32_t envoy_api_v2_core_SocketOption_state(const envoy_api_v2_core_SocketOption *msg) { return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16)); }
UPB_INLINE void envoy_api_v2_core_SocketOption_set_description(envoy_api_v2_core_SocketOption *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_level(envoy_api_v2_core_SocketOption *msg, int64_t value) {
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_name(envoy_api_v2_core_SocketOption *msg, int64_t value) {
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_int_value(envoy_api_v2_core_SocketOption *msg, int64_t value) {
  UPB_WRITE_ONEOF(msg, int64_t, UPB_SIZE(32, 40), value, UPB_SIZE(40, 56), 4);
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_buf_value(envoy_api_v2_core_SocketOption *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(32, 40), value, UPB_SIZE(40, 56), 5);
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_state(envoy_api_v2_core_SocketOption *msg, int32_t value) {
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16)) = value;
}
||||||| 694f491e06
UPB_INLINE envoy_api_v2_core_SocketOption *envoy_api_v2_core_SocketOption_new(upb_arena *arena) {
  return (envoy_api_v2_core_SocketOption *)upb_msg_new(&envoy_api_v2_core_SocketOption_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_SocketOption *envoy_api_v2_core_SocketOption_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_SocketOption *ret = envoy_api_v2_core_SocketOption_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_SocketOption_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_SocketOption_serialize(const envoy_api_v2_core_SocketOption *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_SocketOption_msginit, arena, len);
}
typedef enum {
  envoy_api_v2_core_SocketOption_value_int_value = 4,
  envoy_api_v2_core_SocketOption_value_buf_value = 5,
  envoy_api_v2_core_SocketOption_value_NOT_SET = 0
} envoy_api_v2_core_SocketOption_value_oneofcases;
UPB_INLINE envoy_api_v2_core_SocketOption_value_oneofcases envoy_api_v2_core_SocketOption_value_case(const envoy_api_v2_core_SocketOption* msg) { return (envoy_api_v2_core_SocketOption_value_oneofcases)UPB_FIELD_AT(msg, int32_t, UPB_SIZE(40, 56)); }
UPB_INLINE upb_strview envoy_api_v2_core_SocketOption_description(const envoy_api_v2_core_SocketOption *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 24)); }
UPB_INLINE int64_t envoy_api_v2_core_SocketOption_level(const envoy_api_v2_core_SocketOption *msg) { return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(0, 0)); }
UPB_INLINE int64_t envoy_api_v2_core_SocketOption_name(const envoy_api_v2_core_SocketOption *msg) { return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8)); }
UPB_INLINE bool envoy_api_v2_core_SocketOption_has_int_value(const envoy_api_v2_core_SocketOption *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(40, 56), 4); }
UPB_INLINE int64_t envoy_api_v2_core_SocketOption_int_value(const envoy_api_v2_core_SocketOption *msg) { return UPB_READ_ONEOF(msg, int64_t, UPB_SIZE(32, 40), UPB_SIZE(40, 56), 4, 0); }
UPB_INLINE bool envoy_api_v2_core_SocketOption_has_buf_value(const envoy_api_v2_core_SocketOption *msg) { return _upb_has_oneof_field(msg, UPB_SIZE(40, 56), 5); }
UPB_INLINE upb_strview envoy_api_v2_core_SocketOption_buf_value(const envoy_api_v2_core_SocketOption *msg) { return UPB_READ_ONEOF(msg, upb_strview, UPB_SIZE(32, 40), UPB_SIZE(40, 56), 5, upb_strview_make("", strlen(""))); }
UPB_INLINE int32_t envoy_api_v2_core_SocketOption_state(const envoy_api_v2_core_SocketOption *msg) { return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16)); }
UPB_INLINE void envoy_api_v2_core_SocketOption_set_description(envoy_api_v2_core_SocketOption *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_level(envoy_api_v2_core_SocketOption *msg, int64_t value) {
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_name(envoy_api_v2_core_SocketOption *msg, int64_t value) {
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_int_value(envoy_api_v2_core_SocketOption *msg, int64_t value) {
  UPB_WRITE_ONEOF(msg, int64_t, UPB_SIZE(32, 40), value, UPB_SIZE(40, 56), 4);
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_buf_value(envoy_api_v2_core_SocketOption *msg, upb_strview value) {
  UPB_WRITE_ONEOF(msg, upb_strview, UPB_SIZE(32, 40), value, UPB_SIZE(40, 56), 5);
}
UPB_INLINE void envoy_api_v2_core_SocketOption_set_state(envoy_api_v2_core_SocketOption *msg, int32_t value) {
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16)) = value;
}
=======
>>>>>>> 729af3a4
UPB_INLINE envoy_api_v2_core_RuntimeFractionalPercent *envoy_api_v2_core_RuntimeFractionalPercent_new(upb_arena *arena) {
  return (envoy_api_v2_core_RuntimeFractionalPercent *)_upb_msg_new(&envoy_api_v2_core_RuntimeFractionalPercent_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_RuntimeFractionalPercent *envoy_api_v2_core_RuntimeFractionalPercent_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_RuntimeFractionalPercent *ret = envoy_api_v2_core_RuntimeFractionalPercent_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_RuntimeFractionalPercent_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_RuntimeFractionalPercent_serialize(const envoy_api_v2_core_RuntimeFractionalPercent *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_RuntimeFractionalPercent_msginit, arena, len);
}
UPB_INLINE const struct envoy_type_FractionalPercent* envoy_api_v2_core_RuntimeFractionalPercent_default_value(const envoy_api_v2_core_RuntimeFractionalPercent *msg) { return UPB_FIELD_AT(msg, const struct envoy_type_FractionalPercent*, UPB_SIZE(8, 16)); }
UPB_INLINE upb_strview envoy_api_v2_core_RuntimeFractionalPercent_runtime_key(const envoy_api_v2_core_RuntimeFractionalPercent *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_core_RuntimeFractionalPercent_set_default_value(envoy_api_v2_core_RuntimeFractionalPercent *msg, struct envoy_type_FractionalPercent* value) {
  UPB_FIELD_AT(msg, struct envoy_type_FractionalPercent*, UPB_SIZE(8, 16)) = value;
}
UPB_INLINE struct envoy_type_FractionalPercent* envoy_api_v2_core_RuntimeFractionalPercent_mutable_default_value(envoy_api_v2_core_RuntimeFractionalPercent *msg, upb_arena *arena) {
  struct envoy_type_FractionalPercent* sub = (struct envoy_type_FractionalPercent*)envoy_api_v2_core_RuntimeFractionalPercent_default_value(msg);
  if (sub == NULL) {
    sub = (struct envoy_type_FractionalPercent*)_upb_msg_new(&envoy_type_FractionalPercent_msginit, arena);
    if (!sub) return NULL;
    envoy_api_v2_core_RuntimeFractionalPercent_set_default_value(msg, sub);
  }
  return sub;
}
UPB_INLINE void envoy_api_v2_core_RuntimeFractionalPercent_set_runtime_key(envoy_api_v2_core_RuntimeFractionalPercent *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
UPB_INLINE envoy_api_v2_core_ControlPlane *envoy_api_v2_core_ControlPlane_new(upb_arena *arena) {
  return (envoy_api_v2_core_ControlPlane *)_upb_msg_new(&envoy_api_v2_core_ControlPlane_msginit, arena);
}
UPB_INLINE envoy_api_v2_core_ControlPlane *envoy_api_v2_core_ControlPlane_parse(const char *buf, size_t size,
                        upb_arena *arena) {
  envoy_api_v2_core_ControlPlane *ret = envoy_api_v2_core_ControlPlane_new(arena);
  return (ret && upb_decode(buf, size, ret, &envoy_api_v2_core_ControlPlane_msginit, arena)) ? ret : NULL;
}
UPB_INLINE char *envoy_api_v2_core_ControlPlane_serialize(const envoy_api_v2_core_ControlPlane *msg, upb_arena *arena, size_t *len) {
  return upb_encode(msg, &envoy_api_v2_core_ControlPlane_msginit, arena, len);
}
UPB_INLINE upb_strview envoy_api_v2_core_ControlPlane_identifier(const envoy_api_v2_core_ControlPlane *msg) { return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)); }
UPB_INLINE void envoy_api_v2_core_ControlPlane_set_identifier(envoy_api_v2_core_ControlPlane *msg, upb_strview value) {
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0)) = value;
}
#ifdef __cplusplus
}
#endif
#include "upb/port_undef.inc"
#endif
