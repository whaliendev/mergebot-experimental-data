#define VALIDATE_VALIDATE_PROTO_UPB_H_ 
struct validate_FieldRules;
struct validate_FloatRules;
struct validate_DoubleRules;
struct validate_Int32Rules;
struct validate_Int64Rules;
struct validate_UInt32Rules;
struct validate_UInt64Rules;
struct validate_SInt32Rules;
struct validate_SInt64Rules;
struct validate_Fixed32Rules;
struct validate_Fixed64Rules;
struct validate_SFixed32Rules;
struct validate_SFixed64Rules;
struct validate_BoolRules;
struct validate_StringRules;
struct validate_BytesRules;
struct validate_EnumRules;
struct validate_MessageRules;
struct validate_RepeatedRules;
struct validate_MapRules;
struct validate_AnyRules;
struct validate_DurationRules;
struct validate_TimestampRules;
typedef struct validate_FieldRules validate_FieldRules;
typedef struct validate_FloatRules validate_FloatRules;
typedef struct validate_DoubleRules validate_DoubleRules;
typedef struct validate_Int32Rules validate_Int32Rules;
typedef struct validate_Int64Rules validate_Int64Rules;
typedef struct validate_UInt32Rules validate_UInt32Rules;
typedef struct validate_UInt64Rules validate_UInt64Rules;
typedef struct validate_SInt32Rules validate_SInt32Rules;
typedef struct validate_SInt64Rules validate_SInt64Rules;
typedef struct validate_Fixed32Rules validate_Fixed32Rules;
typedef struct validate_Fixed64Rules validate_Fixed64Rules;
typedef struct validate_SFixed32Rules validate_SFixed32Rules;
typedef struct validate_SFixed64Rules validate_SFixed64Rules;
typedef struct validate_BoolRules validate_BoolRules;
typedef struct validate_StringRules validate_StringRules;
typedef struct validate_BytesRules validate_BytesRules;
typedef struct validate_EnumRules validate_EnumRules;
typedef struct validate_MessageRules validate_MessageRules;
typedef struct validate_RepeatedRules validate_RepeatedRules;
typedef struct validate_MapRules validate_MapRules;
typedef struct validate_AnyRules validate_AnyRules;
typedef struct validate_DurationRules validate_DurationRules;
typedef struct validate_TimestampRules validate_TimestampRules;
extern const upb_msglayout validate_FieldRules_msginit;
extern const upb_msglayout validate_FloatRules_msginit;
extern const upb_msglayout validate_DoubleRules_msginit;
extern const upb_msglayout validate_Int32Rules_msginit;
extern const upb_msglayout validate_Int64Rules_msginit;
extern const upb_msglayout validate_UInt32Rules_msginit;
extern const upb_msglayout validate_UInt64Rules_msginit;
extern const upb_msglayout validate_SInt32Rules_msginit;
extern const upb_msglayout validate_SInt64Rules_msginit;
extern const upb_msglayout validate_Fixed32Rules_msginit;
extern const upb_msglayout validate_Fixed64Rules_msginit;
extern const upb_msglayout validate_SFixed32Rules_msginit;
extern const upb_msglayout validate_SFixed64Rules_msginit;
extern const upb_msglayout validate_BoolRules_msginit;
extern const upb_msglayout validate_StringRules_msginit;
extern const upb_msglayout validate_BytesRules_msginit;
extern const upb_msglayout validate_EnumRules_msginit;
extern const upb_msglayout validate_MessageRules_msginit;
extern const upb_msglayout validate_RepeatedRules_msginit;
extern const upb_msglayout validate_MapRules_msginit;
extern const upb_msglayout validate_AnyRules_msginit;
extern const upb_msglayout validate_DurationRules_msginit;
extern const upb_msglayout validate_TimestampRules_msginit;
struct google_protobuf_Duration;
struct google_protobuf_Timestamp;
extern const upb_msglayout google_protobuf_Duration_msginit;
extern const upb_msglayout google_protobuf_Timestamp_msginit;
typedef enum {
  validate_UNKNOWN = 0,
  validate_HTTP_HEADER_NAME = 1,
  validate_HTTP_HEADER_VALUE = 2
} validate_KnownRegex;
UPB_INLINE validate_FieldRules* validate_FieldRules_new(upb_arena* arena) {
  return (validate_FieldRules*)_upb_msg_new(&validate_FieldRules_msginit,
                                            arena);
}
UPB_INLINE validate_FieldRules* validate_FieldRules_parse(const char* buf,
                                                          size_t size,
                                                          upb_arena* arena) {
  validate_FieldRules* ret = validate_FieldRules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_FieldRules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_FieldRules_serialize(const validate_FieldRules* msg,
                                               upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_FieldRules_msginit, arena, len);
}
typedef enum {
  validate_FieldRules_type_float = 1,
  validate_FieldRules_type_double = 2,
  validate_FieldRules_type_int32 = 3,
  validate_FieldRules_type_int64 = 4,
  validate_FieldRules_type_uint32 = 5,
  validate_FieldRules_type_uint64 = 6,
  validate_FieldRules_type_sint32 = 7,
  validate_FieldRules_type_sint64 = 8,
  validate_FieldRules_type_fixed32 = 9,
  validate_FieldRules_type_fixed64 = 10,
  validate_FieldRules_type_sfixed32 = 11,
  validate_FieldRules_type_sfixed64 = 12,
  validate_FieldRules_type_bool = 13,
  validate_FieldRules_type_string = 14,
  validate_FieldRules_type_bytes = 15,
  validate_FieldRules_type_enum = 16,
  validate_FieldRules_type_repeated = 18,
  validate_FieldRules_type_map = 19,
  validate_FieldRules_type_any = 20,
  validate_FieldRules_type_duration = 21,
  validate_FieldRules_type_timestamp = 22,
  validate_FieldRules_type_NOT_SET = 0
} validate_FieldRules_type_oneofcases;
UPB_INLINE validate_FieldRules_type_oneofcases
validate_FieldRules_type_case(const validate_FieldRules* msg) {
  return (validate_FieldRules_type_oneofcases)UPB_FIELD_AT(msg, int32_t,
                                                           UPB_SIZE(12, 24));
}
UPB_INLINE bool validate_FieldRules_has_float(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 1);
}
UPB_INLINE const validate_FloatRules* validate_FieldRules_float(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_FloatRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 1, NULL);
}
UPB_INLINE bool validate_FieldRules_has_double(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 2);
}
UPB_INLINE const validate_DoubleRules* validate_FieldRules_double(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_DoubleRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 2, NULL);
}
UPB_INLINE bool validate_FieldRules_has_int32(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 3);
}
UPB_INLINE const validate_Int32Rules* validate_FieldRules_int32(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_Int32Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 3, NULL);
}
UPB_INLINE bool validate_FieldRules_has_int64(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 4);
}
UPB_INLINE const validate_Int64Rules* validate_FieldRules_int64(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_Int64Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 4, NULL);
}
UPB_INLINE bool validate_FieldRules_has_uint32(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 5);
}
UPB_INLINE const validate_UInt32Rules* validate_FieldRules_uint32(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_UInt32Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 5, NULL);
}
UPB_INLINE bool validate_FieldRules_has_uint64(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 6);
}
UPB_INLINE const validate_UInt64Rules* validate_FieldRules_uint64(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_UInt64Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 6, NULL);
}
UPB_INLINE bool validate_FieldRules_has_sint32(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 7);
}
UPB_INLINE const validate_SInt32Rules* validate_FieldRules_sint32(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_SInt32Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 7, NULL);
}
UPB_INLINE bool validate_FieldRules_has_sint64(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 8);
}
UPB_INLINE const validate_SInt64Rules* validate_FieldRules_sint64(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_SInt64Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 8, NULL);
}
UPB_INLINE bool validate_FieldRules_has_fixed32(
    const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 9);
}
UPB_INLINE const validate_Fixed32Rules* validate_FieldRules_fixed32(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_Fixed32Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 9, NULL);
}
UPB_INLINE bool validate_FieldRules_has_fixed64(
    const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 10);
}
UPB_INLINE const validate_Fixed64Rules* validate_FieldRules_fixed64(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_Fixed64Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 10, NULL);
}
UPB_INLINE bool validate_FieldRules_has_sfixed32(
    const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 11);
}
UPB_INLINE const validate_SFixed32Rules* validate_FieldRules_sfixed32(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_SFixed32Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 11, NULL);
}
UPB_INLINE bool validate_FieldRules_has_sfixed64(
    const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 12);
}
UPB_INLINE const validate_SFixed64Rules* validate_FieldRules_sfixed64(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_SFixed64Rules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 12, NULL);
}
UPB_INLINE bool validate_FieldRules_has_bool(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 13);
}
UPB_INLINE const validate_BoolRules* validate_FieldRules_bool(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_BoolRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 13, NULL);
}
UPB_INLINE bool validate_FieldRules_has_string(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 14);
}
UPB_INLINE const validate_StringRules* validate_FieldRules_string(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_StringRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 14, NULL);
}
UPB_INLINE bool validate_FieldRules_has_bytes(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 15);
}
UPB_INLINE const validate_BytesRules* validate_FieldRules_bytes(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_BytesRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 15, NULL);
}
UPB_INLINE bool validate_FieldRules_has_enum(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 16);
}
UPB_INLINE const validate_EnumRules* validate_FieldRules_enum(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_EnumRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 16, NULL);
}
UPB_INLINE bool validate_FieldRules_has_message(
    const validate_FieldRules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE const validate_MessageRules* validate_FieldRules_message(
    const validate_FieldRules* msg) {
  return UPB_FIELD_AT(msg, const validate_MessageRules*, UPB_SIZE(4, 8));
}
UPB_INLINE bool validate_FieldRules_has_repeated(
    const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 18);
}
UPB_INLINE const validate_RepeatedRules* validate_FieldRules_repeated(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_RepeatedRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 18, NULL);
}
UPB_INLINE bool validate_FieldRules_has_map(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 19);
}
UPB_INLINE const validate_MapRules* validate_FieldRules_map(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_MapRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 19, NULL);
}
UPB_INLINE bool validate_FieldRules_has_any(const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 20);
}
UPB_INLINE const validate_AnyRules* validate_FieldRules_any(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_AnyRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 20, NULL);
}
UPB_INLINE bool validate_FieldRules_has_duration(
    const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 21);
}
UPB_INLINE const validate_DurationRules* validate_FieldRules_duration(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_DurationRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 21, NULL);
}
UPB_INLINE bool validate_FieldRules_has_timestamp(
    const validate_FieldRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(12, 24), 22);
}
UPB_INLINE const validate_TimestampRules* validate_FieldRules_timestamp(
    const validate_FieldRules* msg) {
  return UPB_READ_ONEOF(msg, const validate_TimestampRules*, UPB_SIZE(8, 16),
                        UPB_SIZE(12, 24), 22, NULL);
}
UPB_WRITE_ONEOF(msg, validate_FloatRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 1);
}
UPB_INLINE struct validate_FloatRules* validate_FieldRules_mutable_float(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_FloatRules* sub =
      (struct validate_FloatRules*)validate_FieldRules_float(msg);
  if (sub == NULL) {
    sub = (struct validate_FloatRules*)_upb_msg_new(
        &validate_FloatRules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_float(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_DoubleRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 2);
}
UPB_INLINE struct validate_DoubleRules* validate_FieldRules_mutable_double(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_DoubleRules* sub =
      (struct validate_DoubleRules*)validate_FieldRules_double(msg);
  if (sub == NULL) {
    sub = (struct validate_DoubleRules*)_upb_msg_new(
        &validate_DoubleRules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_double(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_Int32Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 3);
}
UPB_INLINE struct validate_Int32Rules* validate_FieldRules_mutable_int32(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_Int32Rules* sub =
      (struct validate_Int32Rules*)validate_FieldRules_int32(msg);
  if (sub == NULL) {
    sub = (struct validate_Int32Rules*)_upb_msg_new(
        &validate_Int32Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_int32(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_Int64Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 4);
}
UPB_INLINE struct validate_Int64Rules* validate_FieldRules_mutable_int64(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_Int64Rules* sub =
      (struct validate_Int64Rules*)validate_FieldRules_int64(msg);
  if (sub == NULL) {
    sub = (struct validate_Int64Rules*)_upb_msg_new(
        &validate_Int64Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_int64(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_UInt32Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 5);
}
UPB_INLINE struct validate_UInt32Rules* validate_FieldRules_mutable_uint32(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_UInt32Rules* sub =
      (struct validate_UInt32Rules*)validate_FieldRules_uint32(msg);
  if (sub == NULL) {
    sub = (struct validate_UInt32Rules*)_upb_msg_new(
        &validate_UInt32Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_uint32(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_UInt64Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 6);
}
UPB_INLINE struct validate_UInt64Rules* validate_FieldRules_mutable_uint64(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_UInt64Rules* sub =
      (struct validate_UInt64Rules*)validate_FieldRules_uint64(msg);
  if (sub == NULL) {
    sub = (struct validate_UInt64Rules*)_upb_msg_new(
        &validate_UInt64Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_uint64(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_SInt32Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 7);
}
UPB_INLINE struct validate_SInt32Rules* validate_FieldRules_mutable_sint32(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_SInt32Rules* sub =
      (struct validate_SInt32Rules*)validate_FieldRules_sint32(msg);
  if (sub == NULL) {
    sub = (struct validate_SInt32Rules*)_upb_msg_new(
        &validate_SInt32Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_sint32(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_SInt64Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 8);
}
UPB_INLINE struct validate_SInt64Rules* validate_FieldRules_mutable_sint64(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_SInt64Rules* sub =
      (struct validate_SInt64Rules*)validate_FieldRules_sint64(msg);
  if (sub == NULL) {
    sub = (struct validate_SInt64Rules*)_upb_msg_new(
        &validate_SInt64Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_sint64(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_Fixed32Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 9);
}
UPB_INLINE struct validate_Fixed32Rules* validate_FieldRules_mutable_fixed32(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_Fixed32Rules* sub =
      (struct validate_Fixed32Rules*)validate_FieldRules_fixed32(msg);
  if (sub == NULL) {
    sub = (struct validate_Fixed32Rules*)_upb_msg_new(
        &validate_Fixed32Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_fixed32(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_Fixed64Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 10);
}
UPB_INLINE struct validate_Fixed64Rules* validate_FieldRules_mutable_fixed64(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_Fixed64Rules* sub =
      (struct validate_Fixed64Rules*)validate_FieldRules_fixed64(msg);
  if (sub == NULL) {
    sub = (struct validate_Fixed64Rules*)_upb_msg_new(
        &validate_Fixed64Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_fixed64(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_SFixed32Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 11);
}
UPB_INLINE struct validate_SFixed32Rules* validate_FieldRules_mutable_sfixed32(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_SFixed32Rules* sub =
      (struct validate_SFixed32Rules*)validate_FieldRules_sfixed32(msg);
  if (sub == NULL) {
    sub = (struct validate_SFixed32Rules*)_upb_msg_new(
        &validate_SFixed32Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_sfixed32(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_SFixed64Rules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 12);
}
UPB_INLINE struct validate_SFixed64Rules* validate_FieldRules_mutable_sfixed64(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_SFixed64Rules* sub =
      (struct validate_SFixed64Rules*)validate_FieldRules_sfixed64(msg);
  if (sub == NULL) {
    sub = (struct validate_SFixed64Rules*)_upb_msg_new(
        &validate_SFixed64Rules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_sfixed64(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_BoolRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 13);
}
UPB_INLINE struct validate_BoolRules* validate_FieldRules_mutable_bool(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_BoolRules* sub =
      (struct validate_BoolRules*)validate_FieldRules_bool(msg);
  if (sub == NULL) {
    sub = (struct validate_BoolRules*)_upb_msg_new(&validate_BoolRules_msginit,
                                                   arena);
    if (!sub) return NULL;
    validate_FieldRules_set_bool(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_StringRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 14);
}
UPB_INLINE struct validate_StringRules* validate_FieldRules_mutable_string(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_StringRules* sub =
      (struct validate_StringRules*)validate_FieldRules_string(msg);
  if (sub == NULL) {
    sub = (struct validate_StringRules*)_upb_msg_new(
        &validate_StringRules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_string(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_BytesRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 15);
}
UPB_INLINE struct validate_BytesRules* validate_FieldRules_mutable_bytes(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_BytesRules* sub =
      (struct validate_BytesRules*)validate_FieldRules_bytes(msg);
  if (sub == NULL) {
    sub = (struct validate_BytesRules*)_upb_msg_new(
        &validate_BytesRules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_bytes(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_EnumRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 16);
}
UPB_INLINE struct validate_EnumRules* validate_FieldRules_mutable_enum(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_EnumRules* sub =
      (struct validate_EnumRules*)validate_FieldRules_enum(msg);
  if (sub == NULL) {
    sub = (struct validate_EnumRules*)_upb_msg_new(&validate_EnumRules_msginit,
                                                   arena);
    if (!sub) return NULL;
    validate_FieldRules_set_enum(msg, sub);
  }
  return sub;
}
UPB_INLINE void validate_FieldRules_set_message(validate_FieldRules* msg,
                                                validate_MessageRules* value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, validate_MessageRules*, UPB_SIZE(4, 8)) = value;
}
UPB_INLINE struct validate_MessageRules* validate_FieldRules_mutable_message(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_MessageRules* sub =
      (struct validate_MessageRules*)validate_FieldRules_message(msg);
  if (sub == NULL) {
    sub = (struct validate_MessageRules*)_upb_msg_new(
        &validate_MessageRules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_message(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_RepeatedRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 18);
}
UPB_INLINE struct validate_RepeatedRules* validate_FieldRules_mutable_repeated(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_RepeatedRules* sub =
      (struct validate_RepeatedRules*)validate_FieldRules_repeated(msg);
  if (sub == NULL) {
    sub = (struct validate_RepeatedRules*)_upb_msg_new(
        &validate_RepeatedRules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_repeated(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_MapRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 19);
}
UPB_INLINE struct validate_MapRules* validate_FieldRules_mutable_map(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_MapRules* sub =
      (struct validate_MapRules*)validate_FieldRules_map(msg);
  if (sub == NULL) {
    sub = (struct validate_MapRules*)_upb_msg_new(&validate_MapRules_msginit,
                                                  arena);
    if (!sub) return NULL;
    validate_FieldRules_set_map(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_AnyRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 20);
}
UPB_INLINE struct validate_AnyRules* validate_FieldRules_mutable_any(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_AnyRules* sub =
      (struct validate_AnyRules*)validate_FieldRules_any(msg);
  if (sub == NULL) {
    sub = (struct validate_AnyRules*)_upb_msg_new(&validate_AnyRules_msginit,
                                                  arena);
    if (!sub) return NULL;
    validate_FieldRules_set_any(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_DurationRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 21);
}
UPB_INLINE struct validate_DurationRules* validate_FieldRules_mutable_duration(
    validate_FieldRules* msg, upb_arena* arena) {
  struct validate_DurationRules* sub =
      (struct validate_DurationRules*)validate_FieldRules_duration(msg);
  if (sub == NULL) {
    sub = (struct validate_DurationRules*)_upb_msg_new(
        &validate_DurationRules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_duration(msg, sub);
  }
  return sub;
}
UPB_WRITE_ONEOF(msg, validate_TimestampRules*, UPB_SIZE(8, 16), value,
                UPB_SIZE(12, 24), 22);
}
UPB_INLINE struct validate_TimestampRules*
validate_FieldRules_mutable_timestamp(validate_FieldRules* msg,
                                      upb_arena* arena) {
  struct validate_TimestampRules* sub =
      (struct validate_TimestampRules*)validate_FieldRules_timestamp(msg);
  if (sub == NULL) {
    sub = (struct validate_TimestampRules*)_upb_msg_new(
        &validate_TimestampRules_msginit, arena);
    if (!sub) return NULL;
    validate_FieldRules_set_timestamp(msg, sub);
  }
  return sub;
}
UPB_INLINE validate_FloatRules* validate_FloatRules_new(upb_arena* arena) {
  return (validate_FloatRules*)_upb_msg_new(&validate_FloatRules_msginit,
                                            arena);
}
UPB_INLINE validate_FloatRules* validate_FloatRules_parse(const char* buf,
                                                          size_t size,
                                                          upb_arena* arena) {
  validate_FloatRules* ret = validate_FloatRules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_FloatRules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_FloatRules_serialize(const validate_FloatRules* msg,
                                               upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_FloatRules_msginit, arena, len);
}
UPB_INLINE bool validate_FloatRules_has_const(const validate_FloatRules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE float validate_FloatRules_const(const validate_FloatRules* msg) {
  return UPB_FIELD_AT(msg, float, UPB_SIZE(4, 4));
}
UPB_INLINE bool validate_FloatRules_has_lt(const validate_FloatRules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE float validate_FloatRules_lt(const validate_FloatRules* msg) {
  return UPB_FIELD_AT(msg, float, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_FloatRules_has_lte(const validate_FloatRules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE float validate_FloatRules_lte(const validate_FloatRules* msg) {
  return UPB_FIELD_AT(msg, float, UPB_SIZE(12, 12));
}
UPB_INLINE bool validate_FloatRules_has_gt(const validate_FloatRules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE float validate_FloatRules_gt(const validate_FloatRules* msg) {
  return UPB_FIELD_AT(msg, float, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_FloatRules_has_gte(const validate_FloatRules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE float validate_FloatRules_gte(const validate_FloatRules* msg) {
  return UPB_FIELD_AT(msg, float, UPB_SIZE(20, 20));
}
UPB_INLINE float const* validate_FloatRules_in(const validate_FloatRules* msg,
                                               size_t* len) {
  return (float const*)_upb_array_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE float const* validate_FloatRules_not_in(
    const validate_FloatRules* msg, size_t* len) {
  return (float const*)_upb_array_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE void validate_FloatRules_set_const(validate_FloatRules* msg,
                                              float value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, float, UPB_SIZE(4, 4)) = value;
}
UPB_INLINE void validate_FloatRules_set_lt(validate_FloatRules* msg,
                                           float value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, float, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_FloatRules_set_lte(validate_FloatRules* msg,
                                            float value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, float, UPB_SIZE(12, 12)) = value;
}
UPB_INLINE void validate_FloatRules_set_gt(validate_FloatRules* msg,
                                           float value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, float, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_FloatRules_set_gte(validate_FloatRules* msg,
                                            float value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, float, UPB_SIZE(20, 20)) = value;
}
UPB_INLINE float* validate_FloatRules_mutable_in(validate_FloatRules* msg,
                                                 size_t* len) {
  return (float*)_upb_array_mutable_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE float* validate_FloatRules_resize_in(validate_FloatRules* msg,
                                                size_t len, upb_arena* arena) {
  return (float*)_upb_array_resize_accessor(msg, UPB_SIZE(24, 24), len,
                                            UPB_TYPE_FLOAT, arena);
}
UPB_INLINE bool validate_FloatRules_add_in(validate_FloatRules* msg, float val,
                                           upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(24, 24), UPB_SIZE(4, 4),
                                    UPB_TYPE_FLOAT, &val, arena);
}
UPB_INLINE float* validate_FloatRules_mutable_not_in(validate_FloatRules* msg,
                                                     size_t* len) {
  return (float*)_upb_array_mutable_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE float* validate_FloatRules_resize_not_in(validate_FloatRules* msg,
                                                    size_t len,
                                                    upb_arena* arena) {
  return (float*)_upb_array_resize_accessor(msg, UPB_SIZE(28, 32), len,
                                            UPB_TYPE_FLOAT, arena);
}
UPB_INLINE bool validate_FloatRules_add_not_in(validate_FloatRules* msg,
                                               float val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(28, 32), UPB_SIZE(4, 4),
                                    UPB_TYPE_FLOAT, &val, arena);
}
UPB_INLINE validate_DoubleRules* validate_DoubleRules_new(upb_arena* arena) {
  return (validate_DoubleRules*)_upb_msg_new(&validate_DoubleRules_msginit,
                                             arena);
}
UPB_INLINE validate_DoubleRules* validate_DoubleRules_parse(const char* buf,
                                                            size_t size,
                                                            upb_arena* arena) {
  validate_DoubleRules* ret = validate_DoubleRules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_DoubleRules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_DoubleRules_serialize(const validate_DoubleRules* msg,
                                                upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_DoubleRules_msginit, arena, len);
}
UPB_INLINE bool validate_DoubleRules_has_const(
    const validate_DoubleRules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE double validate_DoubleRules_const(const validate_DoubleRules* msg) {
  return UPB_FIELD_AT(msg, double, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_DoubleRules_has_lt(const validate_DoubleRules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE double validate_DoubleRules_lt(const validate_DoubleRules* msg) {
  return UPB_FIELD_AT(msg, double, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_DoubleRules_has_lte(const validate_DoubleRules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE double validate_DoubleRules_lte(const validate_DoubleRules* msg) {
  return UPB_FIELD_AT(msg, double, UPB_SIZE(24, 24));
}
UPB_INLINE bool validate_DoubleRules_has_gt(const validate_DoubleRules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE double validate_DoubleRules_gt(const validate_DoubleRules* msg) {
  return UPB_FIELD_AT(msg, double, UPB_SIZE(32, 32));
}
UPB_INLINE bool validate_DoubleRules_has_gte(const validate_DoubleRules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE double validate_DoubleRules_gte(const validate_DoubleRules* msg) {
  return UPB_FIELD_AT(msg, double, UPB_SIZE(40, 40));
}
UPB_INLINE double const* validate_DoubleRules_in(
    const validate_DoubleRules* msg, size_t* len) {
  return (double const*)_upb_array_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE double const* validate_DoubleRules_not_in(
    const validate_DoubleRules* msg, size_t* len) {
  return (double const*)_upb_array_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE void validate_DoubleRules_set_const(validate_DoubleRules* msg,
                                               double value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, double, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_DoubleRules_set_lt(validate_DoubleRules* msg,
                                            double value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, double, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_DoubleRules_set_lte(validate_DoubleRules* msg,
                                             double value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, double, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void validate_DoubleRules_set_gt(validate_DoubleRules* msg,
                                            double value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, double, UPB_SIZE(32, 32)) = value;
}
UPB_INLINE void validate_DoubleRules_set_gte(validate_DoubleRules* msg,
                                             double value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, double, UPB_SIZE(40, 40)) = value;
}
UPB_INLINE double* validate_DoubleRules_mutable_in(validate_DoubleRules* msg,
                                                   size_t* len) {
  return (double*)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE double* validate_DoubleRules_resize_in(validate_DoubleRules* msg,
                                                  size_t len,
                                                  upb_arena* arena) {
  return (double*)_upb_array_resize_accessor(msg, UPB_SIZE(48, 48), len,
                                             UPB_TYPE_DOUBLE, arena);
}
UPB_INLINE bool validate_DoubleRules_add_in(validate_DoubleRules* msg,
                                            double val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(48, 48), UPB_SIZE(8, 8),
                                    UPB_TYPE_DOUBLE, &val, arena);
}
UPB_INLINE double* validate_DoubleRules_mutable_not_in(
    validate_DoubleRules* msg, size_t* len) {
  return (double*)_upb_array_mutable_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE double* validate_DoubleRules_resize_not_in(validate_DoubleRules* msg,
                                                      size_t len,
                                                      upb_arena* arena) {
  return (double*)_upb_array_resize_accessor(msg, UPB_SIZE(52, 56), len,
                                             UPB_TYPE_DOUBLE, arena);
}
UPB_INLINE bool validate_DoubleRules_add_not_in(validate_DoubleRules* msg,
                                                double val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(52, 56), UPB_SIZE(8, 8),
                                    UPB_TYPE_DOUBLE, &val, arena);
}
UPB_INLINE validate_Int32Rules* validate_Int32Rules_new(upb_arena* arena) {
  return (validate_Int32Rules*)_upb_msg_new(&validate_Int32Rules_msginit,
                                            arena);
}
UPB_INLINE validate_Int32Rules* validate_Int32Rules_parse(const char* buf,
                                                          size_t size,
                                                          upb_arena* arena) {
  validate_Int32Rules* ret = validate_Int32Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_Int32Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_Int32Rules_serialize(const validate_Int32Rules* msg,
                                               upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_Int32Rules_msginit, arena, len);
}
UPB_INLINE bool validate_Int32Rules_has_const(const validate_Int32Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE int32_t validate_Int32Rules_const(const validate_Int32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(4, 4));
}
UPB_INLINE bool validate_Int32Rules_has_lt(const validate_Int32Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE int32_t validate_Int32Rules_lt(const validate_Int32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_Int32Rules_has_lte(const validate_Int32Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE int32_t validate_Int32Rules_lte(const validate_Int32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(12, 12));
}
UPB_INLINE bool validate_Int32Rules_has_gt(const validate_Int32Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE int32_t validate_Int32Rules_gt(const validate_Int32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_Int32Rules_has_gte(const validate_Int32Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE int32_t validate_Int32Rules_gte(const validate_Int32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(20, 20));
}
UPB_INLINE int32_t const* validate_Int32Rules_in(const validate_Int32Rules* msg,
                                                 size_t* len) {
  return (int32_t const*)_upb_array_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE int32_t const* validate_Int32Rules_not_in(
    const validate_Int32Rules* msg, size_t* len) {
  return (int32_t const*)_upb_array_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE void validate_Int32Rules_set_const(validate_Int32Rules* msg,
                                              int32_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(4, 4)) = value;
}
UPB_INLINE void validate_Int32Rules_set_lt(validate_Int32Rules* msg,
                                           int32_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_Int32Rules_set_lte(validate_Int32Rules* msg,
                                            int32_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(12, 12)) = value;
}
UPB_INLINE void validate_Int32Rules_set_gt(validate_Int32Rules* msg,
                                           int32_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_Int32Rules_set_gte(validate_Int32Rules* msg,
                                            int32_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(20, 20)) = value;
}
UPB_INLINE int32_t* validate_Int32Rules_mutable_in(validate_Int32Rules* msg,
                                                   size_t* len) {
  return (int32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE int32_t* validate_Int32Rules_resize_in(validate_Int32Rules* msg,
                                                  size_t len,
                                                  upb_arena* arena) {
  return (int32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(24, 24), len,
                                              UPB_TYPE_INT32, arena);
}
UPB_INLINE bool validate_Int32Rules_add_in(validate_Int32Rules* msg,
                                           int32_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(24, 24), UPB_SIZE(4, 4),
                                    UPB_TYPE_INT32, &val, arena);
}
UPB_INLINE int32_t* validate_Int32Rules_mutable_not_in(validate_Int32Rules* msg,
                                                       size_t* len) {
  return (int32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE int32_t* validate_Int32Rules_resize_not_in(validate_Int32Rules* msg,
                                                      size_t len,
                                                      upb_arena* arena) {
  return (int32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(28, 32), len,
                                              UPB_TYPE_INT32, arena);
}
UPB_INLINE bool validate_Int32Rules_add_not_in(validate_Int32Rules* msg,
                                               int32_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(28, 32), UPB_SIZE(4, 4),
                                    UPB_TYPE_INT32, &val, arena);
}
UPB_INLINE validate_Int64Rules* validate_Int64Rules_new(upb_arena* arena) {
  return (validate_Int64Rules*)_upb_msg_new(&validate_Int64Rules_msginit,
                                            arena);
}
UPB_INLINE validate_Int64Rules* validate_Int64Rules_parse(const char* buf,
                                                          size_t size,
                                                          upb_arena* arena) {
  validate_Int64Rules* ret = validate_Int64Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_Int64Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_Int64Rules_serialize(const validate_Int64Rules* msg,
                                               upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_Int64Rules_msginit, arena, len);
}
UPB_INLINE bool validate_Int64Rules_has_const(const validate_Int64Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE int64_t validate_Int64Rules_const(const validate_Int64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_Int64Rules_has_lt(const validate_Int64Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE int64_t validate_Int64Rules_lt(const validate_Int64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_Int64Rules_has_lte(const validate_Int64Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE int64_t validate_Int64Rules_lte(const validate_Int64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(24, 24));
}
UPB_INLINE bool validate_Int64Rules_has_gt(const validate_Int64Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE int64_t validate_Int64Rules_gt(const validate_Int64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(32, 32));
}
UPB_INLINE bool validate_Int64Rules_has_gte(const validate_Int64Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE int64_t validate_Int64Rules_gte(const validate_Int64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(40, 40));
}
UPB_INLINE int64_t const* validate_Int64Rules_in(const validate_Int64Rules* msg,
                                                 size_t* len) {
  return (int64_t const*)_upb_array_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE int64_t const* validate_Int64Rules_not_in(
    const validate_Int64Rules* msg, size_t* len) {
  return (int64_t const*)_upb_array_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE void validate_Int64Rules_set_const(validate_Int64Rules* msg,
                                              int64_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_Int64Rules_set_lt(validate_Int64Rules* msg,
                                           int64_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_Int64Rules_set_lte(validate_Int64Rules* msg,
                                            int64_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void validate_Int64Rules_set_gt(validate_Int64Rules* msg,
                                           int64_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(32, 32)) = value;
}
UPB_INLINE void validate_Int64Rules_set_gte(validate_Int64Rules* msg,
                                            int64_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(40, 40)) = value;
}
UPB_INLINE int64_t* validate_Int64Rules_mutable_in(validate_Int64Rules* msg,
                                                   size_t* len) {
  return (int64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE int64_t* validate_Int64Rules_resize_in(validate_Int64Rules* msg,
                                                  size_t len,
                                                  upb_arena* arena) {
  return (int64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(48, 48), len,
                                              UPB_TYPE_INT64, arena);
}
UPB_INLINE bool validate_Int64Rules_add_in(validate_Int64Rules* msg,
                                           int64_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(48, 48), UPB_SIZE(8, 8),
                                    UPB_TYPE_INT64, &val, arena);
}
UPB_INLINE int64_t* validate_Int64Rules_mutable_not_in(validate_Int64Rules* msg,
                                                       size_t* len) {
  return (int64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE int64_t* validate_Int64Rules_resize_not_in(validate_Int64Rules* msg,
                                                      size_t len,
                                                      upb_arena* arena) {
  return (int64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(52, 56), len,
                                              UPB_TYPE_INT64, arena);
}
UPB_INLINE bool validate_Int64Rules_add_not_in(validate_Int64Rules* msg,
                                               int64_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(52, 56), UPB_SIZE(8, 8),
                                    UPB_TYPE_INT64, &val, arena);
}
UPB_INLINE validate_UInt32Rules* validate_UInt32Rules_new(upb_arena* arena) {
  return (validate_UInt32Rules*)_upb_msg_new(&validate_UInt32Rules_msginit,
                                             arena);
}
UPB_INLINE validate_UInt32Rules* validate_UInt32Rules_parse(const char* buf,
                                                            size_t size,
                                                            upb_arena* arena) {
  validate_UInt32Rules* ret = validate_UInt32Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_UInt32Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_UInt32Rules_serialize(const validate_UInt32Rules* msg,
                                                upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_UInt32Rules_msginit, arena, len);
}
UPB_INLINE bool validate_UInt32Rules_has_const(
    const validate_UInt32Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE uint32_t
validate_UInt32Rules_const(const validate_UInt32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(4, 4));
}
UPB_INLINE bool validate_UInt32Rules_has_lt(const validate_UInt32Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE uint32_t validate_UInt32Rules_lt(const validate_UInt32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_UInt32Rules_has_lte(const validate_UInt32Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE uint32_t validate_UInt32Rules_lte(const validate_UInt32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(12, 12));
}
UPB_INLINE bool validate_UInt32Rules_has_gt(const validate_UInt32Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE uint32_t validate_UInt32Rules_gt(const validate_UInt32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_UInt32Rules_has_gte(const validate_UInt32Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE uint32_t validate_UInt32Rules_gte(const validate_UInt32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(20, 20));
}
UPB_INLINE uint32_t const* validate_UInt32Rules_in(
    const validate_UInt32Rules* msg, size_t* len) {
  return (uint32_t const*)_upb_array_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE uint32_t const* validate_UInt32Rules_not_in(
    const validate_UInt32Rules* msg, size_t* len) {
  return (uint32_t const*)_upb_array_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE void validate_UInt32Rules_set_const(validate_UInt32Rules* msg,
                                               uint32_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(4, 4)) = value;
}
UPB_INLINE void validate_UInt32Rules_set_lt(validate_UInt32Rules* msg,
                                            uint32_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_UInt32Rules_set_lte(validate_UInt32Rules* msg,
                                             uint32_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(12, 12)) = value;
}
UPB_INLINE void validate_UInt32Rules_set_gt(validate_UInt32Rules* msg,
                                            uint32_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_UInt32Rules_set_gte(validate_UInt32Rules* msg,
                                             uint32_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(20, 20)) = value;
}
UPB_INLINE uint32_t* validate_UInt32Rules_mutable_in(validate_UInt32Rules* msg,
                                                     size_t* len) {
  return (uint32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE uint32_t* validate_UInt32Rules_resize_in(validate_UInt32Rules* msg,
                                                    size_t len,
                                                    upb_arena* arena) {
  return (uint32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(24, 24), len,
                                               UPB_TYPE_UINT32, arena);
}
UPB_INLINE bool validate_UInt32Rules_add_in(validate_UInt32Rules* msg,
                                            uint32_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(24, 24), UPB_SIZE(4, 4),
                                    UPB_TYPE_UINT32, &val, arena);
}
UPB_INLINE uint32_t* validate_UInt32Rules_mutable_not_in(
    validate_UInt32Rules* msg, size_t* len) {
  return (uint32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE uint32_t* validate_UInt32Rules_resize_not_in(
    validate_UInt32Rules* msg, size_t len, upb_arena* arena) {
  return (uint32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(28, 32), len,
                                               UPB_TYPE_UINT32, arena);
}
UPB_INLINE bool validate_UInt32Rules_add_not_in(validate_UInt32Rules* msg,
                                                uint32_t val,
                                                upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(28, 32), UPB_SIZE(4, 4),
                                    UPB_TYPE_UINT32, &val, arena);
}
UPB_INLINE validate_UInt64Rules* validate_UInt64Rules_new(upb_arena* arena) {
  return (validate_UInt64Rules*)_upb_msg_new(&validate_UInt64Rules_msginit,
                                             arena);
}
UPB_INLINE validate_UInt64Rules* validate_UInt64Rules_parse(const char* buf,
                                                            size_t size,
                                                            upb_arena* arena) {
  validate_UInt64Rules* ret = validate_UInt64Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_UInt64Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_UInt64Rules_serialize(const validate_UInt64Rules* msg,
                                                upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_UInt64Rules_msginit, arena, len);
}
UPB_INLINE bool validate_UInt64Rules_has_const(
    const validate_UInt64Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE uint64_t
validate_UInt64Rules_const(const validate_UInt64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_UInt64Rules_has_lt(const validate_UInt64Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE uint64_t validate_UInt64Rules_lt(const validate_UInt64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_UInt64Rules_has_lte(const validate_UInt64Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE uint64_t validate_UInt64Rules_lte(const validate_UInt64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(24, 24));
}
UPB_INLINE bool validate_UInt64Rules_has_gt(const validate_UInt64Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE uint64_t validate_UInt64Rules_gt(const validate_UInt64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(32, 32));
}
UPB_INLINE bool validate_UInt64Rules_has_gte(const validate_UInt64Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE uint64_t validate_UInt64Rules_gte(const validate_UInt64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(40, 40));
}
UPB_INLINE uint64_t const* validate_UInt64Rules_in(
    const validate_UInt64Rules* msg, size_t* len) {
  return (uint64_t const*)_upb_array_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE uint64_t const* validate_UInt64Rules_not_in(
    const validate_UInt64Rules* msg, size_t* len) {
  return (uint64_t const*)_upb_array_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE void validate_UInt64Rules_set_const(validate_UInt64Rules* msg,
                                               uint64_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_UInt64Rules_set_lt(validate_UInt64Rules* msg,
                                            uint64_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_UInt64Rules_set_lte(validate_UInt64Rules* msg,
                                             uint64_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void validate_UInt64Rules_set_gt(validate_UInt64Rules* msg,
                                            uint64_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(32, 32)) = value;
}
UPB_INLINE void validate_UInt64Rules_set_gte(validate_UInt64Rules* msg,
                                             uint64_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(40, 40)) = value;
}
UPB_INLINE uint64_t* validate_UInt64Rules_mutable_in(validate_UInt64Rules* msg,
                                                     size_t* len) {
  return (uint64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE uint64_t* validate_UInt64Rules_resize_in(validate_UInt64Rules* msg,
                                                    size_t len,
                                                    upb_arena* arena) {
  return (uint64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(48, 48), len,
                                               UPB_TYPE_UINT64, arena);
}
UPB_INLINE bool validate_UInt64Rules_add_in(validate_UInt64Rules* msg,
                                            uint64_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(48, 48), UPB_SIZE(8, 8),
                                    UPB_TYPE_UINT64, &val, arena);
}
UPB_INLINE uint64_t* validate_UInt64Rules_mutable_not_in(
    validate_UInt64Rules* msg, size_t* len) {
  return (uint64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE uint64_t* validate_UInt64Rules_resize_not_in(
    validate_UInt64Rules* msg, size_t len, upb_arena* arena) {
  return (uint64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(52, 56), len,
                                               UPB_TYPE_UINT64, arena);
}
UPB_INLINE bool validate_UInt64Rules_add_not_in(validate_UInt64Rules* msg,
                                                uint64_t val,
                                                upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(52, 56), UPB_SIZE(8, 8),
                                    UPB_TYPE_UINT64, &val, arena);
}
UPB_INLINE validate_SInt32Rules* validate_SInt32Rules_new(upb_arena* arena) {
  return (validate_SInt32Rules*)_upb_msg_new(&validate_SInt32Rules_msginit,
                                             arena);
}
UPB_INLINE validate_SInt32Rules* validate_SInt32Rules_parse(const char* buf,
                                                            size_t size,
                                                            upb_arena* arena) {
  validate_SInt32Rules* ret = validate_SInt32Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_SInt32Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_SInt32Rules_serialize(const validate_SInt32Rules* msg,
                                                upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_SInt32Rules_msginit, arena, len);
}
UPB_INLINE bool validate_SInt32Rules_has_const(
    const validate_SInt32Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE int32_t validate_SInt32Rules_const(const validate_SInt32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(4, 4));
}
UPB_INLINE bool validate_SInt32Rules_has_lt(const validate_SInt32Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE int32_t validate_SInt32Rules_lt(const validate_SInt32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_SInt32Rules_has_lte(const validate_SInt32Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE int32_t validate_SInt32Rules_lte(const validate_SInt32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(12, 12));
}
UPB_INLINE bool validate_SInt32Rules_has_gt(const validate_SInt32Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE int32_t validate_SInt32Rules_gt(const validate_SInt32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_SInt32Rules_has_gte(const validate_SInt32Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE int32_t validate_SInt32Rules_gte(const validate_SInt32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(20, 20));
}
UPB_INLINE int32_t const* validate_SInt32Rules_in(
    const validate_SInt32Rules* msg, size_t* len) {
  return (int32_t const*)_upb_array_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE int32_t const* validate_SInt32Rules_not_in(
    const validate_SInt32Rules* msg, size_t* len) {
  return (int32_t const*)_upb_array_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE void validate_SInt32Rules_set_const(validate_SInt32Rules* msg,
                                               int32_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(4, 4)) = value;
}
UPB_INLINE void validate_SInt32Rules_set_lt(validate_SInt32Rules* msg,
                                            int32_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_SInt32Rules_set_lte(validate_SInt32Rules* msg,
                                             int32_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(12, 12)) = value;
}
UPB_INLINE void validate_SInt32Rules_set_gt(validate_SInt32Rules* msg,
                                            int32_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_SInt32Rules_set_gte(validate_SInt32Rules* msg,
                                             int32_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(20, 20)) = value;
}
UPB_INLINE int32_t* validate_SInt32Rules_mutable_in(validate_SInt32Rules* msg,
                                                    size_t* len) {
  return (int32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE int32_t* validate_SInt32Rules_resize_in(validate_SInt32Rules* msg,
                                                   size_t len,
                                                   upb_arena* arena) {
  return (int32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(24, 24), len,
                                              UPB_TYPE_INT32, arena);
}
UPB_INLINE bool validate_SInt32Rules_add_in(validate_SInt32Rules* msg,
                                            int32_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(24, 24), UPB_SIZE(4, 4),
                                    UPB_TYPE_INT32, &val, arena);
}
UPB_INLINE int32_t* validate_SInt32Rules_mutable_not_in(
    validate_SInt32Rules* msg, size_t* len) {
  return (int32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE int32_t* validate_SInt32Rules_resize_not_in(
    validate_SInt32Rules* msg, size_t len, upb_arena* arena) {
  return (int32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(28, 32), len,
                                              UPB_TYPE_INT32, arena);
}
UPB_INLINE bool validate_SInt32Rules_add_not_in(validate_SInt32Rules* msg,
                                                int32_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(28, 32), UPB_SIZE(4, 4),
                                    UPB_TYPE_INT32, &val, arena);
}
UPB_INLINE validate_SInt64Rules* validate_SInt64Rules_new(upb_arena* arena) {
  return (validate_SInt64Rules*)_upb_msg_new(&validate_SInt64Rules_msginit,
                                             arena);
}
UPB_INLINE validate_SInt64Rules* validate_SInt64Rules_parse(const char* buf,
                                                            size_t size,
                                                            upb_arena* arena) {
  validate_SInt64Rules* ret = validate_SInt64Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_SInt64Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_SInt64Rules_serialize(const validate_SInt64Rules* msg,
                                                upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_SInt64Rules_msginit, arena, len);
}
UPB_INLINE bool validate_SInt64Rules_has_const(
    const validate_SInt64Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE int64_t validate_SInt64Rules_const(const validate_SInt64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_SInt64Rules_has_lt(const validate_SInt64Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE int64_t validate_SInt64Rules_lt(const validate_SInt64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_SInt64Rules_has_lte(const validate_SInt64Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE int64_t validate_SInt64Rules_lte(const validate_SInt64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(24, 24));
}
UPB_INLINE bool validate_SInt64Rules_has_gt(const validate_SInt64Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE int64_t validate_SInt64Rules_gt(const validate_SInt64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(32, 32));
}
UPB_INLINE bool validate_SInt64Rules_has_gte(const validate_SInt64Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE int64_t validate_SInt64Rules_gte(const validate_SInt64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(40, 40));
}
UPB_INLINE int64_t const* validate_SInt64Rules_in(
    const validate_SInt64Rules* msg, size_t* len) {
  return (int64_t const*)_upb_array_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE int64_t const* validate_SInt64Rules_not_in(
    const validate_SInt64Rules* msg, size_t* len) {
  return (int64_t const*)_upb_array_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE void validate_SInt64Rules_set_const(validate_SInt64Rules* msg,
                                               int64_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_SInt64Rules_set_lt(validate_SInt64Rules* msg,
                                            int64_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_SInt64Rules_set_lte(validate_SInt64Rules* msg,
                                             int64_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void validate_SInt64Rules_set_gt(validate_SInt64Rules* msg,
                                            int64_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(32, 32)) = value;
}
UPB_INLINE void validate_SInt64Rules_set_gte(validate_SInt64Rules* msg,
                                             int64_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(40, 40)) = value;
}
UPB_INLINE int64_t* validate_SInt64Rules_mutable_in(validate_SInt64Rules* msg,
                                                    size_t* len) {
  return (int64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE int64_t* validate_SInt64Rules_resize_in(validate_SInt64Rules* msg,
                                                   size_t len,
                                                   upb_arena* arena) {
  return (int64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(48, 48), len,
                                              UPB_TYPE_INT64, arena);
}
UPB_INLINE bool validate_SInt64Rules_add_in(validate_SInt64Rules* msg,
                                            int64_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(48, 48), UPB_SIZE(8, 8),
                                    UPB_TYPE_INT64, &val, arena);
}
UPB_INLINE int64_t* validate_SInt64Rules_mutable_not_in(
    validate_SInt64Rules* msg, size_t* len) {
  return (int64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE int64_t* validate_SInt64Rules_resize_not_in(
    validate_SInt64Rules* msg, size_t len, upb_arena* arena) {
  return (int64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(52, 56), len,
                                              UPB_TYPE_INT64, arena);
}
UPB_INLINE bool validate_SInt64Rules_add_not_in(validate_SInt64Rules* msg,
                                                int64_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(52, 56), UPB_SIZE(8, 8),
                                    UPB_TYPE_INT64, &val, arena);
}
UPB_INLINE validate_Fixed32Rules* validate_Fixed32Rules_new(upb_arena* arena) {
  return (validate_Fixed32Rules*)_upb_msg_new(&validate_Fixed32Rules_msginit,
                                              arena);
}
UPB_INLINE validate_Fixed32Rules* validate_Fixed32Rules_parse(
    const char* buf, size_t size, upb_arena* arena) {
  validate_Fixed32Rules* ret = validate_Fixed32Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_Fixed32Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_Fixed32Rules_serialize(
    const validate_Fixed32Rules* msg, upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_Fixed32Rules_msginit, arena, len);
}
UPB_INLINE bool validate_Fixed32Rules_has_const(
    const validate_Fixed32Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE uint32_t
validate_Fixed32Rules_const(const validate_Fixed32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(4, 4));
}
UPB_INLINE bool validate_Fixed32Rules_has_lt(const validate_Fixed32Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE uint32_t validate_Fixed32Rules_lt(const validate_Fixed32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_Fixed32Rules_has_lte(
    const validate_Fixed32Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE uint32_t
validate_Fixed32Rules_lte(const validate_Fixed32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(12, 12));
}
UPB_INLINE bool validate_Fixed32Rules_has_gt(const validate_Fixed32Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE uint32_t validate_Fixed32Rules_gt(const validate_Fixed32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_Fixed32Rules_has_gte(
    const validate_Fixed32Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE uint32_t
validate_Fixed32Rules_gte(const validate_Fixed32Rules* msg) {
  return UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(20, 20));
}
UPB_INLINE uint32_t const* validate_Fixed32Rules_in(
    const validate_Fixed32Rules* msg, size_t* len) {
  return (uint32_t const*)_upb_array_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE uint32_t const* validate_Fixed32Rules_not_in(
    const validate_Fixed32Rules* msg, size_t* len) {
  return (uint32_t const*)_upb_array_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE void validate_Fixed32Rules_set_const(validate_Fixed32Rules* msg,
                                                uint32_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(4, 4)) = value;
}
UPB_INLINE void validate_Fixed32Rules_set_lt(validate_Fixed32Rules* msg,
                                             uint32_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_Fixed32Rules_set_lte(validate_Fixed32Rules* msg,
                                              uint32_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(12, 12)) = value;
}
UPB_INLINE void validate_Fixed32Rules_set_gt(validate_Fixed32Rules* msg,
                                             uint32_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_Fixed32Rules_set_gte(validate_Fixed32Rules* msg,
                                              uint32_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, uint32_t, UPB_SIZE(20, 20)) = value;
}
UPB_INLINE uint32_t* validate_Fixed32Rules_mutable_in(
    validate_Fixed32Rules* msg, size_t* len) {
  return (uint32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE uint32_t* validate_Fixed32Rules_resize_in(validate_Fixed32Rules* msg,
                                                     size_t len,
                                                     upb_arena* arena) {
  return (uint32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(24, 24), len,
                                               UPB_TYPE_UINT32, arena);
}
UPB_INLINE bool validate_Fixed32Rules_add_in(validate_Fixed32Rules* msg,
                                             uint32_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(24, 24), UPB_SIZE(4, 4),
                                    UPB_TYPE_UINT32, &val, arena);
}
UPB_INLINE uint32_t* validate_Fixed32Rules_mutable_not_in(
    validate_Fixed32Rules* msg, size_t* len) {
  return (uint32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE uint32_t* validate_Fixed32Rules_resize_not_in(
    validate_Fixed32Rules* msg, size_t len, upb_arena* arena) {
  return (uint32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(28, 32), len,
                                               UPB_TYPE_UINT32, arena);
}
UPB_INLINE bool validate_Fixed32Rules_add_not_in(validate_Fixed32Rules* msg,
                                                 uint32_t val,
                                                 upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(28, 32), UPB_SIZE(4, 4),
                                    UPB_TYPE_UINT32, &val, arena);
}
UPB_INLINE validate_Fixed64Rules* validate_Fixed64Rules_new(upb_arena* arena) {
  return (validate_Fixed64Rules*)_upb_msg_new(&validate_Fixed64Rules_msginit,
                                              arena);
}
UPB_INLINE validate_Fixed64Rules* validate_Fixed64Rules_parse(
    const char* buf, size_t size, upb_arena* arena) {
  validate_Fixed64Rules* ret = validate_Fixed64Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_Fixed64Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_Fixed64Rules_serialize(
    const validate_Fixed64Rules* msg, upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_Fixed64Rules_msginit, arena, len);
}
UPB_INLINE bool validate_Fixed64Rules_has_const(
    const validate_Fixed64Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE uint64_t
validate_Fixed64Rules_const(const validate_Fixed64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_Fixed64Rules_has_lt(const validate_Fixed64Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE uint64_t validate_Fixed64Rules_lt(const validate_Fixed64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_Fixed64Rules_has_lte(
    const validate_Fixed64Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE uint64_t
validate_Fixed64Rules_lte(const validate_Fixed64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(24, 24));
}
UPB_INLINE bool validate_Fixed64Rules_has_gt(const validate_Fixed64Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE uint64_t validate_Fixed64Rules_gt(const validate_Fixed64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(32, 32));
}
UPB_INLINE bool validate_Fixed64Rules_has_gte(
    const validate_Fixed64Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE uint64_t
validate_Fixed64Rules_gte(const validate_Fixed64Rules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(40, 40));
}
UPB_INLINE uint64_t const* validate_Fixed64Rules_in(
    const validate_Fixed64Rules* msg, size_t* len) {
  return (uint64_t const*)_upb_array_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE uint64_t const* validate_Fixed64Rules_not_in(
    const validate_Fixed64Rules* msg, size_t* len) {
  return (uint64_t const*)_upb_array_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE void validate_Fixed64Rules_set_const(validate_Fixed64Rules* msg,
                                                uint64_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_Fixed64Rules_set_lt(validate_Fixed64Rules* msg,
                                             uint64_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_Fixed64Rules_set_lte(validate_Fixed64Rules* msg,
                                              uint64_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void validate_Fixed64Rules_set_gt(validate_Fixed64Rules* msg,
                                             uint64_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(32, 32)) = value;
}
UPB_INLINE void validate_Fixed64Rules_set_gte(validate_Fixed64Rules* msg,
                                              uint64_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(40, 40)) = value;
}
UPB_INLINE uint64_t* validate_Fixed64Rules_mutable_in(
    validate_Fixed64Rules* msg, size_t* len) {
  return (uint64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE uint64_t* validate_Fixed64Rules_resize_in(validate_Fixed64Rules* msg,
                                                     size_t len,
                                                     upb_arena* arena) {
  return (uint64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(48, 48), len,
                                               UPB_TYPE_UINT64, arena);
}
UPB_INLINE bool validate_Fixed64Rules_add_in(validate_Fixed64Rules* msg,
                                             uint64_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(48, 48), UPB_SIZE(8, 8),
                                    UPB_TYPE_UINT64, &val, arena);
}
UPB_INLINE uint64_t* validate_Fixed64Rules_mutable_not_in(
    validate_Fixed64Rules* msg, size_t* len) {
  return (uint64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE uint64_t* validate_Fixed64Rules_resize_not_in(
    validate_Fixed64Rules* msg, size_t len, upb_arena* arena) {
  return (uint64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(52, 56), len,
                                               UPB_TYPE_UINT64, arena);
}
UPB_INLINE bool validate_Fixed64Rules_add_not_in(validate_Fixed64Rules* msg,
                                                 uint64_t val,
                                                 upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(52, 56), UPB_SIZE(8, 8),
                                    UPB_TYPE_UINT64, &val, arena);
}
UPB_INLINE validate_SFixed32Rules* validate_SFixed32Rules_new(
    upb_arena* arena) {
  return (validate_SFixed32Rules*)_upb_msg_new(&validate_SFixed32Rules_msginit,
                                               arena);
}
UPB_INLINE validate_SFixed32Rules* validate_SFixed32Rules_parse(
    const char* buf, size_t size, upb_arena* arena) {
  validate_SFixed32Rules* ret = validate_SFixed32Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_SFixed32Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_SFixed32Rules_serialize(
    const validate_SFixed32Rules* msg, upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_SFixed32Rules_msginit, arena, len);
}
UPB_INLINE bool validate_SFixed32Rules_has_const(
    const validate_SFixed32Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE int32_t
validate_SFixed32Rules_const(const validate_SFixed32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(4, 4));
}
UPB_INLINE bool validate_SFixed32Rules_has_lt(
    const validate_SFixed32Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE int32_t
validate_SFixed32Rules_lt(const validate_SFixed32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_SFixed32Rules_has_lte(
    const validate_SFixed32Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE int32_t
validate_SFixed32Rules_lte(const validate_SFixed32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(12, 12));
}
UPB_INLINE bool validate_SFixed32Rules_has_gt(
    const validate_SFixed32Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE int32_t
validate_SFixed32Rules_gt(const validate_SFixed32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_SFixed32Rules_has_gte(
    const validate_SFixed32Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE int32_t
validate_SFixed32Rules_gte(const validate_SFixed32Rules* msg) {
  return UPB_FIELD_AT(msg, int32_t, UPB_SIZE(20, 20));
}
UPB_INLINE int32_t const* validate_SFixed32Rules_in(
    const validate_SFixed32Rules* msg, size_t* len) {
  return (int32_t const*)_upb_array_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE int32_t const* validate_SFixed32Rules_not_in(
    const validate_SFixed32Rules* msg, size_t* len) {
  return (int32_t const*)_upb_array_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE void validate_SFixed32Rules_set_const(validate_SFixed32Rules* msg,
                                                 int32_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(4, 4)) = value;
}
UPB_INLINE void validate_SFixed32Rules_set_lt(validate_SFixed32Rules* msg,
                                              int32_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_SFixed32Rules_set_lte(validate_SFixed32Rules* msg,
                                               int32_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(12, 12)) = value;
}
UPB_INLINE void validate_SFixed32Rules_set_gt(validate_SFixed32Rules* msg,
                                              int32_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_SFixed32Rules_set_gte(validate_SFixed32Rules* msg,
                                               int32_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, int32_t, UPB_SIZE(20, 20)) = value;
}
UPB_INLINE int32_t* validate_SFixed32Rules_mutable_in(
    validate_SFixed32Rules* msg, size_t* len) {
  return (int32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(24, 24), len);
}
UPB_INLINE int32_t* validate_SFixed32Rules_resize_in(
    validate_SFixed32Rules* msg, size_t len, upb_arena* arena) {
  return (int32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(24, 24), len,
                                              UPB_TYPE_INT32, arena);
}
UPB_INLINE bool validate_SFixed32Rules_add_in(validate_SFixed32Rules* msg,
                                              int32_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(24, 24), UPB_SIZE(4, 4),
                                    UPB_TYPE_INT32, &val, arena);
}
UPB_INLINE int32_t* validate_SFixed32Rules_mutable_not_in(
    validate_SFixed32Rules* msg, size_t* len) {
  return (int32_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(28, 32), len);
}
UPB_INLINE int32_t* validate_SFixed32Rules_resize_not_in(
    validate_SFixed32Rules* msg, size_t len, upb_arena* arena) {
  return (int32_t*)_upb_array_resize_accessor(msg, UPB_SIZE(28, 32), len,
                                              UPB_TYPE_INT32, arena);
}
UPB_INLINE bool validate_SFixed32Rules_add_not_in(validate_SFixed32Rules* msg,
                                                  int32_t val,
                                                  upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(28, 32), UPB_SIZE(4, 4),
                                    UPB_TYPE_INT32, &val, arena);
}
UPB_INLINE validate_SFixed64Rules* validate_SFixed64Rules_new(
    upb_arena* arena) {
  return (validate_SFixed64Rules*)_upb_msg_new(&validate_SFixed64Rules_msginit,
                                               arena);
}
UPB_INLINE validate_SFixed64Rules* validate_SFixed64Rules_parse(
    const char* buf, size_t size, upb_arena* arena) {
  validate_SFixed64Rules* ret = validate_SFixed64Rules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_SFixed64Rules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_SFixed64Rules_serialize(
    const validate_SFixed64Rules* msg, upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_SFixed64Rules_msginit, arena, len);
}
UPB_INLINE bool validate_SFixed64Rules_has_const(
    const validate_SFixed64Rules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE int64_t
validate_SFixed64Rules_const(const validate_SFixed64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_SFixed64Rules_has_lt(
    const validate_SFixed64Rules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE int64_t
validate_SFixed64Rules_lt(const validate_SFixed64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_SFixed64Rules_has_lte(
    const validate_SFixed64Rules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE int64_t
validate_SFixed64Rules_lte(const validate_SFixed64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(24, 24));
}
UPB_INLINE bool validate_SFixed64Rules_has_gt(
    const validate_SFixed64Rules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE int64_t
validate_SFixed64Rules_gt(const validate_SFixed64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(32, 32));
}
UPB_INLINE bool validate_SFixed64Rules_has_gte(
    const validate_SFixed64Rules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE int64_t
validate_SFixed64Rules_gte(const validate_SFixed64Rules* msg) {
  return UPB_FIELD_AT(msg, int64_t, UPB_SIZE(40, 40));
}
UPB_INLINE int64_t const* validate_SFixed64Rules_in(
    const validate_SFixed64Rules* msg, size_t* len) {
  return (int64_t const*)_upb_array_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE int64_t const* validate_SFixed64Rules_not_in(
    const validate_SFixed64Rules* msg, size_t* len) {
  return (int64_t const*)_upb_array_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE void validate_SFixed64Rules_set_const(validate_SFixed64Rules* msg,
                                                 int64_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_SFixed64Rules_set_lt(validate_SFixed64Rules* msg,
                                              int64_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_SFixed64Rules_set_lte(validate_SFixed64Rules* msg,
                                               int64_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void validate_SFixed64Rules_set_gt(validate_SFixed64Rules* msg,
                                              int64_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(32, 32)) = value;
}
UPB_INLINE void validate_SFixed64Rules_set_gte(validate_SFixed64Rules* msg,
                                               int64_t value) {
  _upb_sethas(msg, 5);
  UPB_FIELD_AT(msg, int64_t, UPB_SIZE(40, 40)) = value;
}
UPB_INLINE int64_t* validate_SFixed64Rules_mutable_in(
    validate_SFixed64Rules* msg, size_t* len) {
  return (int64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(48, 48), len);
}
UPB_INLINE int64_t* validate_SFixed64Rules_resize_in(
    validate_SFixed64Rules* msg, size_t len, upb_arena* arena) {
  return (int64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(48, 48), len,
                                              UPB_TYPE_INT64, arena);
}
UPB_INLINE bool validate_SFixed64Rules_add_in(validate_SFixed64Rules* msg,
                                              int64_t val, upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(48, 48), UPB_SIZE(8, 8),
                                    UPB_TYPE_INT64, &val, arena);
}
UPB_INLINE int64_t* validate_SFixed64Rules_mutable_not_in(
    validate_SFixed64Rules* msg, size_t* len) {
  return (int64_t*)_upb_array_mutable_accessor(msg, UPB_SIZE(52, 56), len);
}
UPB_INLINE int64_t* validate_SFixed64Rules_resize_not_in(
    validate_SFixed64Rules* msg, size_t len, upb_arena* arena) {
  return (int64_t*)_upb_array_resize_accessor(msg, UPB_SIZE(52, 56), len,
                                              UPB_TYPE_INT64, arena);
}
UPB_INLINE bool validate_SFixed64Rules_add_not_in(validate_SFixed64Rules* msg,
                                                  int64_t val,
                                                  upb_arena* arena) {
  return _upb_array_append_accessor(msg, UPB_SIZE(52, 56), UPB_SIZE(8, 8),
                                    UPB_TYPE_INT64, &val, arena);
}
UPB_INLINE validate_BoolRules* validate_BoolRules_new(upb_arena* arena) {
  return (validate_BoolRules*)_upb_msg_new(&validate_BoolRules_msginit, arena);
}
UPB_INLINE validate_BoolRules* validate_BoolRules_parse(const char* buf,
                                                        size_t size,
                                                        upb_arena* arena) {
  validate_BoolRules* ret = validate_BoolRules_new(arena);
  return (ret && upb_decode(buf, size, ret, &validate_BoolRules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_BoolRules_serialize(const validate_BoolRules* msg,
                                              upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_BoolRules_msginit, arena, len);
}
UPB_INLINE bool validate_BoolRules_has_const(const validate_BoolRules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE bool validate_BoolRules_const(const validate_BoolRules* msg) {
  return UPB_FIELD_AT(msg, bool, UPB_SIZE(1, 1));
}
UPB_INLINE void validate_BoolRules_set_const(validate_BoolRules* msg,
                                             bool value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, bool, UPB_SIZE(1, 1)) = value;
}
UPB_INLINE validate_StringRules* validate_StringRules_new(upb_arena* arena) {
  return (validate_StringRules*)_upb_msg_new(&validate_StringRules_msginit,
                                             arena);
}
UPB_INLINE validate_StringRules* validate_StringRules_parse(const char* buf,
                                                            size_t size,
                                                            upb_arena* arena) {
  validate_StringRules* ret = validate_StringRules_new(arena);
  return (ret &&
          upb_decode(buf, size, ret, &validate_StringRules_msginit, arena))
             ? ret
             : NULL;
}
UPB_INLINE char* validate_StringRules_serialize(const validate_StringRules* msg,
                                                upb_arena* arena, size_t* len) {
  return upb_encode(msg, &validate_StringRules_msginit, arena, len);
}
typedef enum {
  validate_StringRules_well_known_email = 12,
  validate_StringRules_well_known_hostname = 13,
  validate_StringRules_well_known_ip = 14,
  validate_StringRules_well_known_ipv4 = 15,
  validate_StringRules_well_known_ipv6 = 16,
  validate_StringRules_well_known_uri = 17,
  validate_StringRules_well_known_uri_ref = 18,
  validate_StringRules_well_known_address = 21,
  validate_StringRules_well_known_uuid = 22,
  validate_StringRules_well_known_well_known_regex = 24,
  validate_StringRules_well_known_NOT_SET = 0
} validate_StringRules_well_known_oneofcases;
UPB_INLINE validate_StringRules_well_known_oneofcases
validate_StringRules_well_known_case(const validate_StringRules* msg) {
  return (validate_StringRules_well_known_oneofcases)UPB_FIELD_AT(
      msg, int32_t, UPB_SIZE(128, 184));
}
UPB_INLINE bool validate_StringRules_has_const(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 8);
}
UPB_INLINE upb_strview
envoy_api_v2_auth_Secret_name(const envoy_api_v2_auth_Secret* msg) {
  return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0));
}
UPB_INLINE bool validate_StringRules_has_min_len(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 1);
}
UPB_INLINE uint64_t
validate_StringRules_min_len(const validate_StringRules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(8, 8));
}
UPB_INLINE bool validate_StringRules_has_max_len(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 2);
}
UPB_INLINE uint64_t
validate_StringRules_max_len(const validate_StringRules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(16, 16));
}
UPB_INLINE bool validate_StringRules_has_min_bytes(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 3);
}
UPB_INLINE uint64_t
validate_StringRules_min_bytes(const validate_StringRules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(24, 24));
}
UPB_INLINE bool validate_StringRules_has_max_bytes(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 4);
}
UPB_INLINE uint64_t
validate_StringRules_max_bytes(const validate_StringRules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(32, 32));
}
UPB_INLINE bool validate_StringRules_has_pattern(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 9);
}
UPB_INLINE upb_strview
envoy_api_v2_auth_Secret_name(const envoy_api_v2_auth_Secret* msg) {
  return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0));
}
UPB_INLINE bool validate_StringRules_has_prefix(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 10);
}
UPB_INLINE upb_strview
envoy_api_v2_auth_Secret_name(const envoy_api_v2_auth_Secret* msg) {
  return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0));
}
UPB_INLINE bool validate_StringRules_has_suffix(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 11);
}
UPB_INLINE upb_strview
envoy_api_v2_auth_Secret_name(const envoy_api_v2_auth_Secret* msg) {
  return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0));
}
UPB_INLINE bool validate_StringRules_has_contains(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 12);
}
UPB_INLINE upb_strview
envoy_api_v2_auth_Secret_name(const envoy_api_v2_auth_Secret* msg) {
  return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0));
}
UPB_INLINE upb_strview const* validate_StringRules_in(
    const validate_StringRules* msg, size_t* len) {
  return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(108, 160), len);
}
UPB_INLINE upb_strview const* validate_StringRules_not_in(
    const validate_StringRules* msg, size_t* len) {
  return (upb_strview const*)_upb_array_accessor(msg, UPB_SIZE(112, 168), len);
}
UPB_INLINE bool validate_StringRules_has_email(
    const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 12);
}
UPB_INLINE bool validate_StringRules_email(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, bool, UPB_SIZE(120, 176), UPB_SIZE(128, 184), 12,
                        false);
}
UPB_INLINE bool validate_StringRules_has_hostname(
    const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 13);
}
UPB_INLINE bool validate_StringRules_hostname(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, bool, UPB_SIZE(120, 176), UPB_SIZE(128, 184), 13,
                        false);
}
UPB_INLINE bool validate_StringRules_has_ip(const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 14);
}
UPB_INLINE bool validate_StringRules_ip(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, bool, UPB_SIZE(120, 176), UPB_SIZE(128, 184), 14,
                        false);
}
UPB_INLINE bool validate_StringRules_has_ipv4(const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 15);
}
UPB_INLINE bool validate_StringRules_ipv4(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, bool, UPB_SIZE(120, 176), UPB_SIZE(128, 184), 15,
                        false);
}
UPB_INLINE bool validate_StringRules_has_ipv6(const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 16);
}
UPB_INLINE bool validate_StringRules_ipv6(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, bool, UPB_SIZE(120, 176), UPB_SIZE(128, 184), 16,
                        false);
}
UPB_INLINE bool validate_StringRules_has_uri(const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 17);
}
UPB_INLINE bool validate_StringRules_uri(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, bool, UPB_SIZE(120, 176), UPB_SIZE(128, 184), 17,
                        false);
}
UPB_INLINE bool validate_StringRules_has_uri_ref(
    const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 18);
}
UPB_INLINE bool validate_StringRules_uri_ref(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, bool, UPB_SIZE(120, 176), UPB_SIZE(128, 184), 18,
                        false);
}
UPB_INLINE bool validate_StringRules_has_len(const validate_StringRules* msg) {
  return _upb_has_field(msg, 5);
}
UPB_INLINE uint64_t validate_StringRules_len(const validate_StringRules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(40, 40));
}
UPB_INLINE bool validate_StringRules_has_len_bytes(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 6);
}
UPB_INLINE uint64_t
validate_StringRules_len_bytes(const validate_StringRules* msg) {
  return UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(48, 48));
}
UPB_INLINE bool validate_StringRules_has_address(
    const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 21);
}
UPB_INLINE bool validate_StringRules_address(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, bool, UPB_SIZE(120, 176), UPB_SIZE(128, 184), 21,
                        false);
}
UPB_INLINE bool validate_StringRules_has_uuid(const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 22);
}
UPB_INLINE bool validate_StringRules_uuid(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, bool, UPB_SIZE(120, 176), UPB_SIZE(128, 184), 22,
                        false);
}
UPB_INLINE bool validate_StringRules_has_not_contains(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 13);
}
UPB_INLINE upb_strview
envoy_api_v2_auth_Secret_name(const envoy_api_v2_auth_Secret* msg) {
  return UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(0, 0));
}
UPB_INLINE bool validate_StringRules_has_well_known_regex(
    const validate_StringRules* msg) {
  return _upb_has_oneof_field(msg, UPB_SIZE(128, 184), 24);
}
UPB_INLINE int32_t
validate_StringRules_well_known_regex(const validate_StringRules* msg) {
  return UPB_READ_ONEOF(msg, int32_t, UPB_SIZE(120, 176), UPB_SIZE(128, 184),
                        24, validate_UNKNOWN);
}
UPB_INLINE bool validate_StringRules_has_strict(
    const validate_StringRules* msg) {
  return _upb_has_field(msg, 7);
}
UPB_INLINE bool validate_StringRules_strict(const validate_StringRules* msg) {
  return UPB_FIELD_AT(msg, bool, UPB_SIZE(56, 56));
}
UPB_INLINE void validate_StringRules_set_const(validate_StringRules* msg,
                                               upb_strview value) {
  _upb_sethas(msg, 8);
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(60, 64)) = value;
}
UPB_INLINE void validate_StringRules_set_min_len(validate_StringRules* msg,
                                                 uint64_t value) {
  _upb_sethas(msg, 1);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(8, 8)) = value;
}
UPB_INLINE void validate_StringRules_set_max_len(validate_StringRules* msg,
                                                 uint64_t value) {
  _upb_sethas(msg, 2);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(16, 16)) = value;
}
UPB_INLINE void validate_StringRules_set_min_bytes(validate_StringRules* msg,
                                                   uint64_t value) {
  _upb_sethas(msg, 3);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(24, 24)) = value;
}
UPB_INLINE void validate_StringRules_set_max_bytes(validate_StringRules* msg,
                                                   uint64_t value) {
  _upb_sethas(msg, 4);
  UPB_FIELD_AT(msg, uint64_t, UPB_SIZE(32, 32)) = value;
}
UPB_INLINE void validate_StringRules_set_pattern(validate_StringRules* msg,
                                                 upb_strview value) {
  _upb_sethas(msg, 9);
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(68, 80)) = value;
}
UPB_INLINE void validate_StringRules_set_prefix(validate_StringRules* msg,
                                                upb_strview value) {
  _upb_sethas(msg, 10);
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(76, 96)) = value;
}
UPB_INLINE void validate_StringRules_set_suffix(validate_StringRules* msg,
                                                upb_strview value) {
  _upb_sethas(msg, 11);
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(84, 112)) = value;
}
UPB_INLINE void validate_StringRules_set_contains(validate_StringRules* msg,
                                                  upb_strview value) {
  _upb_sethas(msg, 12);
  UPB_FIELD_AT(msg, upb_strview, UPB_SIZE(92, 128)) = value;
}
UPB_INLINE upb_strview* validate_StringRules_mutable_in(
    validate_StringRules* msg, size_t* len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(108, 160),
                                                   len);
}
UPB_INLINE upb_strview* validate_StringRules_resize_in(
    validate_StringRules* msg, size_t len, upb_arena* arena) {
<<<<<<< HEAD
  return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(104, 152), len,
                                                  UPB_TYPE_STRING, arena);
|||||||
  return (upb_strview*)_upb_array_resize_accessor(
      msg, UPB_SIZE(104, 152), len, UPB_SIZE(8, 16), UPB_TYPE_STRING, arena);
=======
  return (upb_strview*)_upb_array_resize_accessor(
      msg, UPB_SIZE(108, 160), len, UPB_SIZE(8, 16), UPB_TYPE_STRING, arena);
>>>>>>> 729af3a43d16584684f6f70349ed34133e1b59f1
}
UPB_INLINE bool validate_StringRules_add_in(validate_StringRules* msg,
                                            upb_strview val, upb_arena* arena) {
<<<<<<< HEAD
  return _upb_array_append_accessor(msg, UPB_SIZE(104, 152), UPB_SIZE(8, 16),
                                    UPB_TYPE_STRING, &val, arena);
|||||||
  return _upb_array_append_accessor(msg, UPB_SIZE(104, 152), UPB_SIZE(8, 16),
                                    UPB_TYPE_STRING, &val, arena);
=======
  return _upb_array_append_accessor(msg, UPB_SIZE(108, 160), UPB_SIZE(8, 16),
                                    UPB_TYPE_STRING, &val, arena);
>>>>>>> 729af3a43d16584684f6f70349ed34133e1b59f1
}
UPB_INLINE upb_strview* validate_StringRules_mutable_not_in(
    validate_StringRules* msg, size_t* len) {
  return (upb_strview*)_upb_array_mutable_accessor(msg, UPB_SIZE(112, 168),
                                                   len);
}
UPB_INLINE upb_strview* validate_StringRules_resize_not_in(
    validate_StringRules* msg, size_t len, upb_arena* arena) {
<<<<<<< HEAD
  return (upb_strview*)_upb_array_resize_accessor(msg, UPB_SIZE(108, 160), len,
                                                  UPB_TYPE_STRING, arena);
|||||||
  return (upb_strview*)_upb_array_resize_accessor(
      msg, UPB_SIZE(108, 160), len, UPB_SIZE(8, 16), UPB_TYPE_STRING, arena);
=======
  return (upb_strview*)_upb_array_resize_accessor(
      msg, UPB_SIZE(112, 168), len, UPB_SIZE(8, 16), UPB_TYPE_STRING, arena);
>>>>>>> 729af3a43d16584684f6f70349ed34133e1b59f1
}
UPB_INLINE bool validate_StringRules_add_not_in(validate_StringRules* msg,
                                                upb_strview val,
                                                upb_arena* arena) {
<<<<<<< HEAD
  return _upb_array_append_accessor(msg, UPB_SIZE(108, 160), UPB_SIZE(8, 16),
                                    UPB_TYPE_STRING, &val, arena);
|||||||
  return _upb_array_append_accessor(msg, UPB_SIZE(108, 160), UPB_SIZE(8, 16),
                                    UPB_TYPE_STRING, &val, arena);
=======
  return _upb_array_append_accessor(msg, UPB_SIZE(112, 168), UPB_SIZE(8, 16),
                                    UPB_TYPE_STRING, &val, arena);
>>>>>>> 729af3a43d16584684f6f70349ed34133e1b59f1
}
