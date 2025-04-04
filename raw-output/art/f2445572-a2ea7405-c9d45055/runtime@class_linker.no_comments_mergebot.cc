#include "class_linker.h"
#include <deque>
#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/arena_allocator.h"
#include "base/casts.h"
#include "base/logging.h"
#include "base/scoped_arena_containers.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "base/value_object.h"
#include "class_linker-inl.h"
#include "compiler_callbacks.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc_root-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "handle_scope.h"
#include "intern_table.h"
#include "interpreter/interpreter.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "leb128.h"
#include "linear_alloc.h"
#include "oat.h"
#include "oat_file.h"
#include "oat_file_assistant.h"
#include "object_lock.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/field.h"
#include "mirror/iftable-inl.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/proxy.h"
#include "mirror/reference-inl.h"
#include "mirror/stack_trace_element.h"
#include "mirror/string-inl.h"
#include "os.h"
#include "runtime.h"
#include "entrypoints/entrypoint_utils.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"
#include "thread-inl.h"
#include "utils.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"
namespace art {
static constexpr bool kSanityCheckObjects = kIsDebugBuild;
static void ThrowNoClassDefFoundError(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
static void ThrowNoClassDefFoundError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread* self = Thread::Current();
  self->ThrowNewExceptionV("Ljava/lang/NoClassDefFoundError;", fmt, args);
  va_end(args);
}
bool ClassLinker::HasInitWithString(Thread* self, ClassLinker* class_linker, const char* descriptor) {
  ArtMethod* method = self->GetCurrentMethod(nullptr);
  StackHandleScope<1> hs(self);
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(method != nullptr ?
      method->GetDeclaringClass()->GetClassLoader()
      : nullptr));
  mirror::Class* exception_class = class_linker->FindClass(self, descriptor, class_loader);
  if (exception_class == nullptr) {
    CHECK(self->IsExceptionPending());
    self->ClearException();
    return false;
  }
  ArtMethod* exception_init_method = exception_class->FindDeclaredDirectMethod(
      "<init>", "(Ljava/lang/String;)V", image_pointer_size_);
  return exception_init_method != nullptr;
}
void ClassLinker::ThrowEarlierClassFailure(mirror::Class* c) {
  Runtime* const runtime = Runtime::Current();
  if (!runtime->IsAotCompiler()) {
    LOG(INFO) << "Rejecting re-init on previously-failed class " << PrettyClass(c);
  }
  CHECK(c->IsErroneous()) << PrettyClass(c) << " " << c->GetStatus();
  Thread* self = Thread::Current();
  if (runtime->IsAotCompiler()) {
    mirror::Throwable* pre_allocated = runtime->GetPreAllocatedNoClassDefFoundError();
    self->SetException(pre_allocated);
  } else {
    if (c->GetVerifyErrorClass() != nullptr) {
      std::string temp;
      const char* descriptor = c->GetVerifyErrorClass()->GetDescriptor(&temp);
      if (HasInitWithString(self, this, descriptor)) {
        self->ThrowNewException(descriptor, PrettyDescriptor(c).c_str());
      } else {
        self->ThrowNewException(descriptor, nullptr);
      }
    } else {
      self->ThrowNewException("Ljava/lang/NoClassDefFoundError;",
                              PrettyDescriptor(c).c_str());
    }
  }
}
static void VlogClassInitializationFailure(Handle<mirror::Class> klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (VLOG_IS_ON(class_linker)) {
    std::string temp;
    LOG(INFO) << "Failed to initialize class " << klass->GetDescriptor(&temp) << " from "
              << klass->GetLocation() << "\n" << Thread::Current()->GetException()->Dump();
    }
    }
static void WrapExceptionInInitializer(Handle<mirror::Class> klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    JNIEnv* env = self->GetJniEnv();
    ScopedLocalRef<jthrowable> cause(env, env->ExceptionOccurred());
    CHECK(cause.get() != nullptr);
    env->ExceptionClear();
    bool is_error = env->IsInstanceOf(cause.get(), WellKnownClasses::java_lang_Error);
    env->Throw(cause.get());
    if (!is_error) {
    self->ThrowNewWrappedException("Ljava/lang/ExceptionInInitializerError;", nullptr);
    }
    VlogClassInitializationFailure(klass);
    }
struct FieldGap {
uint32_t start_offset;
uint32_t size;
};
struct FieldGapsComparator {
  explicit FieldGapsComparator() {
  }
  bool operator() (const FieldGap& lhs, const FieldGap& rhs)
      NO_THREAD_SAFETY_ANALYSIS {
    return lhs.size > rhs.size || (lhs.size == rhs.size && lhs.start_offset < rhs.start_offset);
  }
};
typedef std::priority_queue<FieldGap, std::vector<FieldGap>, FieldGapsComparator> FieldGaps;
static void AddFieldGap(uint32_t gap_start, uint32_t gap_end, FieldGaps* gaps) {
  DCHECK(gaps != nullptr);
  uint32_t current_offset = gap_start;
  while (current_offset != gap_end) {
    size_t remaining = gap_end - current_offset;
    if (remaining >= sizeof(uint32_t) && IsAligned<4>(current_offset)) {
      gaps->push(FieldGap {current_offset, sizeof(uint32_t)});
      current_offset += sizeof(uint32_t);
    } else if (remaining >= sizeof(uint16_t) && IsAligned<2>(current_offset)) {
      gaps->push(FieldGap {current_offset, sizeof(uint16_t)});
      current_offset += sizeof(uint16_t);
    } else {
      gaps->push(FieldGap {current_offset, sizeof(uint8_t)});
      current_offset += sizeof(uint8_t);
    }
    DCHECK_LE(current_offset, gap_end) << "Overran gap";
  }
}
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(current_field_idx != nullptr);
    DCHECK(grouped_and_sorted_fields != nullptr);
    DCHECK(gaps != nullptr);
    DCHECK(field_offset != nullptr);
    DCHECK(IsPowerOfTwo(n));
    while (!grouped_and_sorted_fields->empty()) {
    ArtField* field = grouped_and_sorted_fields->front();
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    if (Primitive::ComponentSize(type) < n) {
      break;
    }
    if (!IsAligned<n>(field_offset->Uint32Value())) {
      MemberOffset old_offset = *field_offset;
      *field_offset = MemberOffset(RoundUp(field_offset->Uint32Value(), n));
      AddFieldGap(old_offset.Uint32Value(), field_offset->Uint32Value(), gaps);
    }
    CHECK(type != Primitive::kPrimNot) << PrettyField(field);
    grouped_and_sorted_fields->pop_front();
    if (!gaps->empty() && gaps->top().size >= n) {
      FieldGap gap = gaps->top();
      gaps->pop();
      DCHECK(IsAligned<n>(gap.start_offset));
      field->SetOffset(MemberOffset(gap.start_offset));
      if (gap.size > n) {
        AddFieldGap(gap.start_offset + n, gap.start_offset + gap.size, gaps);
      }
    } else {
      DCHECK(IsAligned<n>(field_offset->Uint32Value()));
      field->SetOffset(*field_offset);
      *field_offset = MemberOffset(field_offset->Uint32Value() + n);
    }
    ++(*current_field_idx);
    }
    }
ClassLinker::ClassLinker(InternTable* intern_table)
    : dex_lock_("ClassLinker dex lock", kDefaultMutexLevel),
      dex_cache_image_class_lookup_required_(false),
      failed_dex_cache_class_lookups_(0),
      class_roots_(nullptr),
      array_iftable_(nullptr),
      find_array_class_cache_next_victim_(0),
      init_done_(false),
      log_new_dex_caches_roots_(false),
      log_new_class_table_roots_(false),
      intern_table_(intern_table),
      quick_resolution_trampoline_(nullptr),
      quick_imt_conflict_trampoline_(nullptr),
      quick_generic_jni_trampoline_(nullptr),
      quick_to_interpreter_bridge_trampoline_(nullptr),
      image_pointer_size_(sizeof(void*)) {
  CHECK(intern_table_ != nullptr);
  for (auto& root : find_array_class_cache_) {
    root = GcRoot<mirror::Class>(nullptr);
  }
}
void ClassLinker::InitWithoutImage(std::vector<std::unique_ptr<const DexFile>> boot_class_path) {
  VLOG(startup) << "ClassLinker::Init";
  Thread* const self = Thread::Current();
  Runtime* const runtime = Runtime::Current();
  gc::Heap* const heap = runtime->GetHeap();
  CHECK(!heap->HasImageSpace()) << "Runtime has image. We should use it.";
  CHECK(!init_done_);
  image_pointer_size_ = InstructionSetPointerSize(runtime->GetInstructionSet());
  heap->IncrementDisableMovingGC(self);
  StackHandleScope<64> hs(self);
  auto class_class_size = mirror::Class::ClassClassSize(image_pointer_size_);
  Handle<mirror::Class> java_lang_Class(hs.NewHandle(down_cast<mirror::Class*>(
      heap->AllocNonMovableObject<true>(self, nullptr, class_class_size, VoidFunctor()))));
  CHECK(java_lang_Class.Get() != nullptr);
  mirror::Class::SetClassClass(java_lang_Class.Get());
  java_lang_Class->SetClass(java_lang_Class.Get());
  if (kUseBakerOrBrooksReadBarrier) {
    java_lang_Class->AssertReadBarrierPointer();
  }
  java_lang_Class->SetClassSize(class_class_size);
  java_lang_Class->SetPrimitiveType(Primitive::kPrimNot);
  heap->DecrementDisableMovingGC(self);
  auto class_array_class_size = mirror::ObjectArray<mirror::Class>::ClassSize(image_pointer_size_);
  Handle<mirror::Class> class_array_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), class_array_class_size)));
  class_array_class->SetComponentType(java_lang_Class.Get());
  Handle<mirror::Class> java_lang_Object(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Object::ClassSize(image_pointer_size_))));
  CHECK(java_lang_Object.Get() != nullptr);
  java_lang_Class->SetSuperClass(java_lang_Object.Get());
  mirror::Class::SetStatus(java_lang_Object, mirror::Class::kStatusLoaded, self);
  Handle<mirror::Class> object_array_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(),
                 mirror::ObjectArray<mirror::Object>::ClassSize(image_pointer_size_))));
  object_array_class->SetComponentType(java_lang_Object.Get());
  Handle<mirror::Class> char_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(),
                 mirror::Class::PrimitiveClassSize(image_pointer_size_))));
  char_class->SetPrimitiveType(Primitive::kPrimChar);
  Handle<mirror::Class> char_array_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Array::ClassSize(image_pointer_size_))));
  char_array_class->SetComponentType(char_class.Get());
  mirror::CharArray::SetArrayClass(char_array_class.Get());
  Handle<mirror::Class> java_lang_String(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::String::ClassSize(image_pointer_size_))));
  mirror::String::SetClass(java_lang_String.Get());
  mirror::Class::SetStatus(java_lang_String, mirror::Class::kStatusResolved, self);
  java_lang_String->SetStringClass();
  Handle<mirror::Class> java_lang_ref_Reference(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Reference::ClassSize(image_pointer_size_))));
  mirror::Reference::SetClass(java_lang_ref_Reference.Get());
  java_lang_ref_Reference->SetObjectSize(mirror::Reference::InstanceSize());
  mirror::Class::SetStatus(java_lang_ref_Reference, mirror::Class::kStatusResolved, self);
  class_roots_ = GcRoot<mirror::ObjectArray<mirror::Class>>(
      mirror::ObjectArray<mirror::Class>::Alloc(self, object_array_class.Get(),
                                                kClassRootsMax));
  CHECK(!class_roots_.IsNull());
  SetClassRoot(kJavaLangClass, java_lang_Class.Get());
  SetClassRoot(kJavaLangObject, java_lang_Object.Get());
  SetClassRoot(kClassArrayClass, class_array_class.Get());
  SetClassRoot(kObjectArrayClass, object_array_class.Get());
  SetClassRoot(kCharArrayClass, char_array_class.Get());
  SetClassRoot(kJavaLangString, java_lang_String.Get());
  SetClassRoot(kJavaLangRefReference, java_lang_ref_Reference.Get());
  SetClassRoot(kPrimitiveBoolean, CreatePrimitiveClass(self, Primitive::kPrimBoolean));
  SetClassRoot(kPrimitiveByte, CreatePrimitiveClass(self, Primitive::kPrimByte));
  SetClassRoot(kPrimitiveShort, CreatePrimitiveClass(self, Primitive::kPrimShort));
  SetClassRoot(kPrimitiveInt, CreatePrimitiveClass(self, Primitive::kPrimInt));
  SetClassRoot(kPrimitiveLong, CreatePrimitiveClass(self, Primitive::kPrimLong));
  SetClassRoot(kPrimitiveFloat, CreatePrimitiveClass(self, Primitive::kPrimFloat));
  SetClassRoot(kPrimitiveDouble, CreatePrimitiveClass(self, Primitive::kPrimDouble));
  SetClassRoot(kPrimitiveVoid, CreatePrimitiveClass(self, Primitive::kPrimVoid));
  array_iftable_ = GcRoot<mirror::IfTable>(AllocIfTable(self, 2));
  Handle<mirror::Class> int_array_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Array::ClassSize(image_pointer_size_))));
  int_array_class->SetComponentType(GetClassRoot(kPrimitiveInt));
  mirror::IntArray::SetArrayClass(int_array_class.Get());
  SetClassRoot(kIntArrayClass, int_array_class.Get());
  Handle<mirror::Class> long_array_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Array::ClassSize(image_pointer_size_))));
  long_array_class->SetComponentType(GetClassRoot(kPrimitiveLong));
  mirror::LongArray::SetArrayClass(long_array_class.Get());
  SetClassRoot(kLongArrayClass, long_array_class.Get());
  Handle<mirror::Class> java_lang_DexCache(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::DexCache::ClassSize(image_pointer_size_))));
  SetClassRoot(kJavaLangDexCache, java_lang_DexCache.Get());
  java_lang_DexCache->SetObjectSize(mirror::DexCache::InstanceSize());
  mirror::Class::SetStatus(java_lang_DexCache, mirror::Class::kStatusResolved, self);
  Handle<mirror::Class> object_array_string(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(),
                 mirror::ObjectArray<mirror::String>::ClassSize(image_pointer_size_))));
  object_array_string->SetComponentType(java_lang_String.Get());
  SetClassRoot(kJavaLangStringArrayClass, object_array_string.Get());
  runtime->SetResolutionMethod(runtime->CreateResolutionMethod());
  runtime->SetImtConflictMethod(runtime->CreateImtConflictMethod());
  runtime->SetImtUnimplementedMethod(runtime->CreateImtConflictMethod());
  CHECK_NE(0U, boot_class_path.size());
  for (auto& dex_file : boot_class_path) {
    CHECK(dex_file.get() != nullptr);
    AppendToBootClassPath(self, *dex_file);
    opened_dex_files_.push_back(std::move(dex_file));
  }
  InitializePrimitiveClass(char_class.Get(), Primitive::kPrimChar);
  SetClassRoot(kPrimitiveChar, char_class.Get());
  quick_generic_jni_trampoline_ = GetQuickGenericJniStub();
  if (!runtime->IsAotCompiler()) {
    quick_resolution_trampoline_ = GetQuickResolutionStub();
    quick_imt_conflict_trampoline_ = GetQuickImtConflictStub();
    quick_to_interpreter_bridge_trampoline_ = GetQuickToInterpreterBridge();
  }
  mirror::Class::SetStatus(java_lang_Object, mirror::Class::kStatusNotReady, self);
  CHECK_EQ(java_lang_Object.Get(), FindSystemClass(self, "Ljava/lang/Object;"));
  CHECK_EQ(java_lang_Object->GetObjectSize(), mirror::Object::InstanceSize());
  mirror::Class::SetStatus(java_lang_String, mirror::Class::kStatusNotReady, self);
  mirror::Class* String_class = FindSystemClass(self, "Ljava/lang/String;");
  if (java_lang_String.Get() != String_class) {
    std::ostringstream os1, os2;
    java_lang_String->DumpClass(os1, mirror::Class::kDumpClassFullDetail);
    String_class->DumpClass(os2, mirror::Class::kDumpClassFullDetail);
    LOG(FATAL) << os1.str() << "\n\n" << os2.str();
  }
  mirror::Class::SetStatus(java_lang_DexCache, mirror::Class::kStatusNotReady, self);
  CHECK_EQ(java_lang_DexCache.Get(), FindSystemClass(self, "Ljava/lang/DexCache;"));
  CHECK_EQ(java_lang_DexCache->GetObjectSize(), mirror::DexCache::InstanceSize());
  SetClassRoot(kBooleanArrayClass, FindSystemClass(self, "[Z"));
  mirror::BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));
  SetClassRoot(kByteArrayClass, FindSystemClass(self, "[B"));
  mirror::ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));
  CHECK_EQ(char_array_class.Get(), FindSystemClass(self, "[C"));
  SetClassRoot(kShortArrayClass, FindSystemClass(self, "[S"));
  mirror::ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));
  CHECK_EQ(int_array_class.Get(), FindSystemClass(self, "[I"));
  CHECK_EQ(long_array_class.Get(), FindSystemClass(self, "[J"));
  SetClassRoot(kFloatArrayClass, FindSystemClass(self, "[F"));
  mirror::FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));
  SetClassRoot(kDoubleArrayClass, FindSystemClass(self, "[D"));
  mirror::DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));
  CHECK_EQ(class_array_class.Get(), FindSystemClass(self, "[Ljava/lang/Class;"));
  CHECK_EQ(object_array_class.Get(), FindSystemClass(self, "[Ljava/lang/Object;"));
  auto java_lang_Cloneable = hs.NewHandle(FindSystemClass(self, "Ljava/lang/Cloneable;"));
  CHECK(java_lang_Cloneable.Get() != nullptr);
  auto java_io_Serializable = hs.NewHandle(FindSystemClass(self, "Ljava/io/Serializable;"));
  CHECK(java_io_Serializable.Get() != nullptr);
  array_iftable_.Read()->SetInterface(0, java_lang_Cloneable.Get());
  array_iftable_.Read()->SetInterface(1, java_io_Serializable.Get());
  CHECK_EQ(java_lang_Cloneable.Get(),
           mirror::Class::GetDirectInterface(self, class_array_class, 0));
  CHECK_EQ(java_io_Serializable.Get(),
           mirror::Class::GetDirectInterface(self, class_array_class, 1));
  CHECK_EQ(java_lang_Cloneable.Get(),
           mirror::Class::GetDirectInterface(self, object_array_class, 0));
  CHECK_EQ(java_io_Serializable.Get(),
           mirror::Class::GetDirectInterface(self, object_array_class, 1));
  CHECK_EQ(java_lang_Class.Get(), FindSystemClass(self, "Ljava/lang/Class;"));
  CHECK_EQ(object_array_string.Get(),
           FindSystemClass(self, GetClassRootDescriptor(kJavaLangStringArrayClass)));
  SetClassRoot(kJavaLangReflectProxy, FindSystemClass(self, "Ljava/lang/reflect/Proxy;"));
  auto* class_root = FindSystemClass(self, "Ljava/lang/reflect/Field;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectField, class_root);
  mirror::Field::SetClass(class_root);
  class_root = FindSystemClass(self, "[Ljava/lang/reflect/Field;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectFieldArrayClass, class_root);
  mirror::Field::SetArrayClass(class_root);
  class_root = FindSystemClass(self, "Ljava/lang/reflect/Constructor;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectConstructor, class_root);
  mirror::Constructor::SetClass(class_root);
  class_root = FindSystemClass(self, "[Ljava/lang/reflect/Constructor;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectConstructorArrayClass, class_root);
  mirror::Constructor::SetArrayClass(class_root);
  class_root = FindSystemClass(self, "Ljava/lang/reflect/Method;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectMethod, class_root);
  mirror::Method::SetClass(class_root);
  class_root = FindSystemClass(self, "[Ljava/lang/reflect/Method;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectMethodArrayClass, class_root);
  mirror::Method::SetArrayClass(class_root);
  mirror::Class::SetStatus(java_lang_ref_Reference, mirror::Class::kStatusNotReady, self);
  CHECK_EQ(java_lang_ref_Reference.Get(), FindSystemClass(self, "Ljava/lang/ref/Reference;"));
  CHECK_EQ(java_lang_ref_Reference->GetObjectSize(), mirror::Reference::InstanceSize());
  CHECK_EQ(java_lang_ref_Reference->GetClassSize(),
           mirror::Reference::ClassSize(image_pointer_size_));
  class_root = FindSystemClass(self, "Ljava/lang/ref/FinalizerReference;");
  class_root->SetAccessFlags(class_root->GetAccessFlags() |
                             kAccClassIsReference | kAccClassIsFinalizerReference);
  class_root = FindSystemClass(self, "Ljava/lang/ref/PhantomReference;");
  class_root->SetAccessFlags(class_root->GetAccessFlags() | kAccClassIsReference |
                             kAccClassIsPhantomReference);
  class_root = FindSystemClass(self, "Ljava/lang/ref/SoftReference;");
  class_root->SetAccessFlags(class_root->GetAccessFlags() | kAccClassIsReference);
  class_root = FindSystemClass(self, "Ljava/lang/ref/WeakReference;");
  class_root->SetAccessFlags(class_root->GetAccessFlags() | kAccClassIsReference |
                             kAccClassIsWeakReference);
  class_root = FindSystemClass(self, "Ljava/lang/ClassLoader;");
  CHECK_EQ(class_root->GetObjectSize(), mirror::ClassLoader::InstanceSize());
  SetClassRoot(kJavaLangClassLoader, class_root);
  SetClassRoot(kJavaLangThrowable, FindSystemClass(self, "Ljava/lang/Throwable;"));
  mirror::Throwable::SetClass(GetClassRoot(kJavaLangThrowable));
  SetClassRoot(kJavaLangClassNotFoundException,
               FindSystemClass(self, "Ljava/lang/ClassNotFoundException;"));
  SetClassRoot(kJavaLangStackTraceElement, FindSystemClass(self, "Ljava/lang/StackTraceElement;"));
  SetClassRoot(kJavaLangStackTraceElementArrayClass,
               FindSystemClass(self, "[Ljava/lang/StackTraceElement;"));
  mirror::StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));
  {
    const DexFile& dex_file = java_lang_Object->GetDexFile();
    const DexFile::StringId* void_string_id = dex_file.FindStringId("V");
    CHECK(void_string_id != nullptr);
    uint32_t void_string_index = dex_file.GetIndexForStringId(*void_string_id);
    const DexFile::TypeId* void_type_id = dex_file.FindTypeId(void_string_index);
    CHECK(void_type_id != nullptr);
    uint16_t void_type_idx = dex_file.GetIndexForTypeId(*void_type_id);
    mirror::Class* resolved_type = ResolveType(dex_file, void_type_idx, java_lang_Object.Get());
    CHECK_EQ(resolved_type, GetClassRoot(kPrimitiveVoid));
    self->AssertNoPendingException();
  }
  FinishInit(self);
  VLOG(startup) << "ClassLinker::InitFromCompiler exiting";
}
void ClassLinker::FinishInit(Thread* self) {
  VLOG(startup) << "ClassLinker::FinishInit entering";
  mirror::Class* java_lang_ref_Reference = GetClassRoot(kJavaLangRefReference);
  mirror::Class* java_lang_ref_FinalizerReference =
      FindSystemClass(self, "Ljava/lang/ref/FinalizerReference;");
  ArtField* pendingNext = java_lang_ref_Reference->GetInstanceField(0);
  CHECK_STREQ(pendingNext->GetName(), "pendingNext");
  CHECK_STREQ(pendingNext->GetTypeDescriptor(), "Ljava/lang/ref/Reference;");
  ArtField* queue = java_lang_ref_Reference->GetInstanceField(1);
  CHECK_STREQ(queue->GetName(), "queue");
  CHECK_STREQ(queue->GetTypeDescriptor(), "Ljava/lang/ref/ReferenceQueue;");
  ArtField* queueNext = java_lang_ref_Reference->GetInstanceField(2);
  CHECK_STREQ(queueNext->GetName(), "queueNext");
  CHECK_STREQ(queueNext->GetTypeDescriptor(), "Ljava/lang/ref/Reference;");
  ArtField* referent = java_lang_ref_Reference->GetInstanceField(3);
  CHECK_STREQ(referent->GetName(), "referent");
  CHECK_STREQ(referent->GetTypeDescriptor(), "Ljava/lang/Object;");
  ArtField* zombie = java_lang_ref_FinalizerReference->GetInstanceField(2);
  CHECK_STREQ(zombie->GetName(), "zombie");
  CHECK_STREQ(zombie->GetTypeDescriptor(), "Ljava/lang/Object;");
  for (size_t i = 0; i < kClassRootsMax; i++) {
    ClassRoot class_root = static_cast<ClassRoot>(i);
    mirror::Class* klass = GetClassRoot(class_root);
    CHECK(klass != nullptr);
    DCHECK(klass->IsArrayClass() || klass->IsPrimitive() || klass->GetDexCache() != nullptr);
  }
  CHECK(!array_iftable_.IsNull());
  init_done_ = true;
  VLOG(startup) << "ClassLinker::FinishInit exiting";
}
void ClassLinker::RunRootClinits() {
  Thread* self = Thread::Current();
  for (size_t i = 0; i < ClassLinker::kClassRootsMax; ++i) {
    mirror::Class* c = GetClassRoot(ClassRoot(i));
    if (!c->IsArrayClass() && !c->IsPrimitive()) {
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(GetClassRoot(ClassRoot(i))));
      EnsureInitialized(self, h_class, true, true);
      self->AssertNoPendingException();
    }
  }
}
const OatFile* ClassLinker::RegisterOatFile(const OatFile* oat_file) {
  WriterMutexLock mu(Thread::Current(), dex_lock_);
  if (kIsDebugBuild) {
    for (size_t i = 0; i < oat_files_.size(); ++i) {
      CHECK_NE(oat_file, oat_files_[i]) << oat_file->GetLocation();
    }
  }
  VLOG(class_linker) << "Registering " << oat_file->GetLocation();
  oat_files_.push_back(oat_file);
  return oat_file;
}
OatFile& ClassLinker::GetImageOatFile(gc::space::ImageSpace* space) {
  VLOG(startup) << "ClassLinker::GetImageOatFile entering";
  OatFile* oat_file = space->ReleaseOatFile();
  CHECK_EQ(RegisterOatFile(oat_file), oat_file);
  VLOG(startup) << "ClassLinker::GetImageOatFile exiting";
  return *oat_file;
}
class DexFileAndClassPair : ValueObject {
public:
  DexFileAndClassPair(const DexFile* dex_file, size_t current_class_index, bool from_loaded_oat)
     : cached_descriptor_(GetClassDescriptor(dex_file, current_class_index)),
       dex_file_(dex_file),
       current_class_index_(current_class_index),
       from_loaded_oat_(from_loaded_oat) {}
  DexFileAndClassPair(const DexFileAndClassPair&)
  DexFileAndClassPair& operator=(const DexFileAndClassPair& rhs) {
    cached_descriptor_ = rhs.cached_descriptor_;
    dex_file_ = rhs.dex_file_;
    current_class_index_ = rhs.current_class_index_;
    from_loaded_oat_ = rhs.from_loaded_oat_;
    return *this;
  }
  const char* GetCachedDescriptor() const {
    return cached_descriptor_;
  }
  bool operator<(const DexFileAndClassPair& rhs) const {
    const char* lhsDescriptor = cached_descriptor_;
    const char* rhsDescriptor = rhs.cached_descriptor_;
    int cmp = strcmp(lhsDescriptor, rhsDescriptor);
    if (cmp != 0) {
      return cmp > 0;
    }
    return dex_file_ < rhs.dex_file_;
  }
  bool DexFileHasMoreClasses() const {
    return current_class_index_ + 1 < dex_file_->NumClassDefs();
  }
  DexFileAndClassPair GetNext() const {
    return DexFileAndClassPair(dex_file_, current_class_index_ + 1, from_loaded_oat_);
  }
  size_t GetCurrentClassIndex() const {
    return current_class_index_;
  }
  bool FromLoadedOat() const {
    return from_loaded_oat_;
  }
  const DexFile* GetDexFile() const {
    return dex_file_;
  }
  void DeleteDexFile() {
    delete dex_file_;
    dex_file_ = nullptr;
  }
private:
  static const char* GetClassDescriptor(const DexFile* dex_file, size_t index) {
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(static_cast<uint16_t>(index));
    return dex_file->StringByTypeIdx(class_def.class_idx_);
  }
  const char* cached_descriptor_;
  const DexFile* dex_file_;
  size_t current_class_index_;
bool from_loaded_oat_;
};
static void AddDexFilesFromOat(const OatFile* oat_file, bool already_loaded,
                               std::priority_queue<DexFileAndClassPair>* heap) {
  const std::vector<const OatDexFile*>& oat_dex_files = oat_file->GetOatDexFiles();
  for (const OatDexFile* oat_dex_file : oat_dex_files) {
    std::string error;
    std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error);
    if (dex_file.get() == nullptr) {
      LOG(WARNING) << "Could not create dex file from oat file: " << error;
    } else {
      if (dex_file->NumClassDefs() > 0U) {
        heap->emplace(dex_file.release(), 0U, already_loaded);
      }
    }
  }
}
static void AddNext(DexFileAndClassPair* original,
                    std::priority_queue<DexFileAndClassPair>* heap) {
  if (original->DexFileHasMoreClasses()) {
    heap->push(original->GetNext());
  } else {
    original->DeleteDexFile();
  }
}
static void FreeDexFilesInHeap(std::priority_queue<DexFileAndClassPair>* heap) {
  while (!heap->empty()) {
    delete heap->top().GetDexFile();
    heap->pop();
  }
}
const OatFile* ClassLinker::GetBootOatFile() {
  if (boot_class_path_.empty()) {
    return nullptr;
  }
  const DexFile* boot_dex_file = boot_class_path_[0];
  if (boot_dex_file->GetOatDexFile() != nullptr) {
    return boot_dex_file->GetOatDexFile()->GetOatFile();
  }
  return nullptr;
}
const OatFile* ClassLinker::GetPrimaryOatFile() {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  const OatFile* boot_oat_file = GetBootOatFile();
  if (boot_oat_file != nullptr) {
    for (const OatFile* oat_file : oat_files_) {
      if (oat_file != boot_oat_file) {
        return oat_file;
      }
    }
  }
  return nullptr;
}
bool ClassLinker::HasCollisions(const OatFile* oat_file, std::string* error_msg) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  std::priority_queue<DexFileAndClassPair> queue;
  {
    const OatFile* boot_oat = GetBootOatFile();
    for (const OatFile* loaded_oat_file : oat_files_) {
      if (loaded_oat_file == boot_oat) {
        continue;
      }
      AddDexFilesFromOat(loaded_oat_file, true, &queue);
    }
  }
  if (queue.empty()) {
    return false;
  }
  AddDexFilesFromOat(oat_file, false, &queue);
  while (!queue.empty()) {
    DexFileAndClassPair compare_pop = queue.top();
    queue.pop();
    while (!queue.empty()) {
      DexFileAndClassPair top = queue.top();
      if (strcmp(compare_pop.GetCachedDescriptor(), top.GetCachedDescriptor()) == 0) {
        if (compare_pop.FromLoadedOat() != top.FromLoadedOat()) {
          *error_msg =
              StringPrintf("Found duplicated class when checking oat files: '%s' in %s and %s",
                           compare_pop.GetCachedDescriptor(),
                           compare_pop.GetDexFile()->GetLocation().c_str(),
                           top.GetDexFile()->GetLocation().c_str());
          FreeDexFilesInHeap(&queue);
          return true;
        }
        queue.pop();
        AddNext(&top, &queue);
      } else {
        break;
      }
    }
    AddNext(&compare_pop, &queue);
  }
  return false;
}
std::vector<std::unique_ptr<const DexFile>> ClassLinker::OpenDexFilesFromOat(
    const char* dex_location, const char* oat_location,
    std::vector<std::string>* error_msgs) {
  CHECK(error_msgs != nullptr);
  Locks::mutator_lock_->AssertNotHeld(Thread::Current());
  OatFileAssistant oat_file_assistant(dex_location, oat_location, kRuntimeISA,
     !Runtime::Current()->IsAotCompiler());
  std::string error_msg;
  if (!oat_file_assistant.Lock(&error_msg)) {
    VLOG(class_linker) << "OatFileAssistant::Lock: " << error_msg;
  }
  const OatFile* source_oat_file = nullptr;
  {
    ReaderMutexLock mu(Thread::Current(), dex_lock_);
    for (const OatFile* oat_file : oat_files_) {
      CHECK(oat_file != nullptr);
      if (oat_file_assistant.GivenOatFileIsUpToDate(*oat_file)) {
        source_oat_file = oat_file;
        break;
      }
    }
  }
  if (source_oat_file == nullptr) {
    if (!oat_file_assistant.MakeUpToDate(&error_msg)) {
      LOG(WARNING) << error_msg;
    }
    std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
    if (oat_file.get() != nullptr) {
      bool accept_oat_file = !HasCollisions(oat_file.get(), &error_msg);
      if (!accept_oat_file) {
        if (Runtime::Current()->IsDexFileFallbackEnabled()) {
          LOG(WARNING) << "Found duplicate classes, falling back to interpreter mode for "
                       << dex_location;
        } else {
          LOG(WARNING) << "Found duplicate classes, dex-file-fallback disabled, will be failing to "
                          " load classes for " << dex_location;
        }
        LOG(WARNING) << error_msg;
        if (!DexFile::MaybeDex(dex_location)) {
          accept_oat_file = true;
          LOG(WARNING) << "Dex location " << dex_location << " does not seem to include dex file. "
                       << "Allow oat file use. This is potentially dangerous.";
        }
      }
      if (accept_oat_file) {
        source_oat_file = oat_file.release();
        RegisterOatFile(source_oat_file);
      }
    }
  }
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  if (source_oat_file != nullptr) {
    dex_files = oat_file_assistant.LoadDexFiles(*source_oat_file, dex_location);
    if (dex_files.empty()) {
      error_msgs->push_back("Failed to open dex files from "
          + source_oat_file->GetLocation());
    }
  }
  if (dex_files.empty()) {
    if (Runtime::Current()->IsDexFileFallbackEnabled()) {
      if (!DexFile::Open(dex_location, dex_location, &error_msg, &dex_files)) {
        LOG(WARNING) << error_msg;
        error_msgs->push_back("Failed to open dex files from " + std::string(dex_location));
      }
    } else {
      error_msgs->push_back("Fallback mode disabled, skipping dex files.");
    }
  }
  return dex_files;
}
const OatFile* ClassLinker::FindOpenedOatFileFromOatLocation(const std::string& oat_location) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  for (size_t i = 0; i < oat_files_.size(); i++) {
    const OatFile* oat_file = oat_files_[i];
    DCHECK(oat_file != nullptr);
    if (oat_file->GetLocation() == oat_location) {
      return oat_file;
    }
  }
  return nullptr;
}
static void SanityCheckArtMethod(ArtMethod* m, mirror::Class* expected_class,
                                 gc::space::ImageSpace* space)
static void SanityCheckObjectsCallback(mirror::Object* obj, void* arg ATTRIBUTE_UNUSED)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(obj != nullptr);
    CHECK(obj->GetClass() != nullptr) << "Null class " << obj;
    CHECK(obj->GetClass()->GetClass() != nullptr) << "Null class class " << obj;
    if (obj->IsClass()) {
    auto klass = obj->AsClass();
    ArtField* fields[2] = { klass->GetSFields(), klass->GetIFields() };
    size_t num_fields[2] = { klass->NumStaticFields(), klass->NumInstanceFields() };
    for (size_t i = 0; i < 2; ++i) {
      for (size_t j = 0; j < num_fields[i]; ++j) {
        CHECK_EQ(fields[i][j].GetDeclaringClass(), klass);
      }
    }
    auto* runtime = Runtime::Current();
    auto* image_space = runtime->GetHeap()->GetImageSpace();
    auto pointer_size = runtime->GetClassLinker()->GetImagePointerSize();
    for (auto& m : klass->GetDirectMethods(pointer_size)) {
      SanityCheckArtMethod(&m, klass, image_space);
    }
    for (auto& m : klass->GetVirtualMethods(pointer_size)) {
      SanityCheckArtMethod(&m, klass, image_space);
    }
    auto* vtable = klass->GetVTable();
    if (vtable != nullptr) {
      SanityCheckArtMethodPointerArray(vtable, nullptr, pointer_size, image_space);
    }
    if (klass->ShouldHaveEmbeddedImtAndVTable()) {
      for (size_t i = 0; i < mirror::Class::kImtSize; ++i) {
        SanityCheckArtMethod(klass->GetEmbeddedImTableEntry(i, pointer_size), nullptr, image_space);
      }
      for (int32_t i = 0; i < klass->GetEmbeddedVTableLength(); ++i) {
        SanityCheckArtMethod(klass->GetEmbeddedVTableEntry(i, pointer_size), nullptr, image_space);
      }
    }
    auto* iftable = klass->GetIfTable();
    if (iftable != nullptr) {
      for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
        if (iftable->GetMethodArrayCount(i) > 0) {
          SanityCheckArtMethodPointerArray(iftable->GetMethodArray(i), nullptr, pointer_size,
                                           image_space);
        }
      }
    }
    }
    }
void ClassLinker::InitFromImage() {
  VLOG(startup) << "ClassLinker::InitFromImage entering";
  CHECK(!init_done_);
  Runtime* const runtime = Runtime::Current();
  Thread* const self = Thread::Current();
  gc::Heap* const heap = runtime->GetHeap();
  gc::space::ImageSpace* const space = heap->GetImageSpace();
  CHECK(space != nullptr);
  image_pointer_size_ = space->GetImageHeader().GetPointerSize();
  dex_cache_image_class_lookup_required_ = true;
  OatFile& oat_file = GetImageOatFile(space);
  CHECK_EQ(oat_file.GetOatHeader().GetImageFileLocationOatChecksum(), 0U);
  CHECK_EQ(oat_file.GetOatHeader().GetImageFileLocationOatDataBegin(), 0U);
  const char* image_file_location = oat_file.GetOatHeader().
      GetStoreValueByKey(OatHeader::kImageLocationKey);
  CHECK(image_file_location == nullptr || *image_file_location == 0);
  quick_resolution_trampoline_ = oat_file.GetOatHeader().GetQuickResolutionTrampoline();
  quick_imt_conflict_trampoline_ = oat_file.GetOatHeader().GetQuickImtConflictTrampoline();
  quick_generic_jni_trampoline_ = oat_file.GetOatHeader().GetQuickGenericJniTrampoline();
  quick_to_interpreter_bridge_trampoline_ = oat_file.GetOatHeader().GetQuickToInterpreterBridge();
  mirror::Object* dex_caches_object = space->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
  mirror::ObjectArray<mirror::DexCache>* dex_caches =
      dex_caches_object->AsObjectArray<mirror::DexCache>();
  StackHandleScope<1> hs(self);
  Handle<mirror::ObjectArray<mirror::Class>> class_roots(hs.NewHandle(
          space->GetImageHeader().GetImageRoot(ImageHeader::kClassRoots)->
          AsObjectArray<mirror::Class>()));
  class_roots_ = GcRoot<mirror::ObjectArray<mirror::Class>>(class_roots.Get());
  mirror::String::SetClass(GetClassRoot(kJavaLangString));
  CHECK_EQ(oat_file.GetOatHeader().GetDexFileCount(),
           static_cast<uint32_t>(dex_caches->GetLength()));
  for (int32_t i = 0; i < dex_caches->GetLength(); i++) {
    StackHandleScope<1> hs2(self);
    Handle<mirror::DexCache> dex_cache(hs2.NewHandle(dex_caches->Get(i)));
    const std::string& dex_file_location(dex_cache->GetLocation()->ToModifiedUtf8());
    const OatFile::OatDexFile* oat_dex_file = oat_file.GetOatDexFile(dex_file_location.c_str(),
                                                                     nullptr);
    CHECK(oat_dex_file != nullptr) << oat_file.GetLocation() << " " << dex_file_location;
    std::string error_msg;
    std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error_msg);
    if (dex_file.get() == nullptr) {
      LOG(FATAL) << "Failed to open dex file " << dex_file_location
                 << " from within oat file " << oat_file.GetLocation()
                 << " error '" << error_msg << "'";
      UNREACHABLE();
    }
    if (kSanityCheckObjects) {
      SanityCheckArtMethodPointerArray(dex_cache->GetResolvedMethods(), nullptr,
                                       image_pointer_size_, space);
    }
    CHECK_EQ(dex_file->GetLocationChecksum(), oat_dex_file->GetDexFileLocationChecksum());
    AppendToBootClassPath(*dex_file.get(), dex_cache);
    opened_dex_files_.push_back(std::move(dex_file));
  }
  CHECK(ValidPointerSize(image_pointer_size_)) << image_pointer_size_;
  if (!runtime->IsAotCompiler()) {
    CHECK_EQ(image_pointer_size_, sizeof(void*));
  }
  if (kSanityCheckObjects) {
    for (int32_t i = 0; i < dex_caches->GetLength(); i++) {
      auto* dex_cache = dex_caches->Get(i);
      for (size_t j = 0; j < dex_cache->NumResolvedFields(); ++j) {
        auto* field = dex_cache->GetResolvedField(j, image_pointer_size_);
        if (field != nullptr) {
          CHECK(field->GetDeclaringClass()->GetClass() != nullptr);
        }
      }
    }
    heap->VisitObjects(SanityCheckObjectsCallback, nullptr);
  }
  if (!runtime->IsAotCompiler() && runtime->GetInstrumentation()->InterpretOnly()) {
    const auto& header = space->GetImageHeader();
    const auto& methods = header.GetMethodsSection();
    const auto art_method_size = ArtMethod::ObjectSize(image_pointer_size_);
    for (uintptr_t pos = 0; pos < methods.Size(); pos += art_method_size) {
      auto* method = reinterpret_cast<ArtMethod*>(space->Begin() + pos + methods.Offset());
      if (kIsDebugBuild && !method->IsRuntimeMethod()) {
        CHECK(method->GetDeclaringClass() != nullptr);
      }
      if (!method->IsNative()) {
        method->SetEntryPointFromInterpreterPtrSize(
            artInterpreterToInterpreterBridge, image_pointer_size_);
        if (!method->IsRuntimeMethod() && method != runtime->GetResolutionMethod()) {
          method->SetEntryPointFromQuickCompiledCodePtrSize(GetQuickToInterpreterBridge(),
                                                            image_pointer_size_);
        }
      }
    }
  }
  mirror::Class::SetClassClass(class_roots->Get(kJavaLangClass));
  class_roots_ = GcRoot<mirror::ObjectArray<mirror::Class>>(class_roots.Get());
  array_iftable_ = GcRoot<mirror::IfTable>(GetClassRoot(kObjectArrayClass)->GetIfTable());
  DCHECK_EQ(array_iftable_.Read(), GetClassRoot(kBooleanArrayClass)->GetIfTable());
  mirror::Field::SetClass(GetClassRoot(kJavaLangReflectField));
  mirror::Field::SetArrayClass(GetClassRoot(kJavaLangReflectFieldArrayClass));
  mirror::Constructor::SetClass(GetClassRoot(kJavaLangReflectConstructor));
  mirror::Constructor::SetArrayClass(GetClassRoot(kJavaLangReflectConstructorArrayClass));
  mirror::Method::SetClass(GetClassRoot(kJavaLangReflectMethod));
  mirror::Method::SetArrayClass(GetClassRoot(kJavaLangReflectMethodArrayClass));
  mirror::Reference::SetClass(GetClassRoot(kJavaLangRefReference));
  mirror::BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));
  mirror::ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));
  mirror::CharArray::SetArrayClass(GetClassRoot(kCharArrayClass));
  mirror::DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));
  mirror::FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));
  mirror::IntArray::SetArrayClass(GetClassRoot(kIntArrayClass));
  mirror::LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));
  mirror::ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));
  mirror::Throwable::SetClass(GetClassRoot(kJavaLangThrowable));
  mirror::StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));
  FinishInit(self);
  VLOG(startup) << "ClassLinker::InitFromImage exiting";
}
bool ClassLinker::ClassInClassTable(mirror::Class* klass) {
  ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  auto it = class_table_.Find(GcRoot<mirror::Class>(klass));
  if (it == class_table_.end()) {
    return false;
  }
  return it->Read() == klass;
}
void ClassLinker::VisitClassRoots(RootVisitor* visitor, VisitRootFlags flags) {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(
      visitor, RootInfo(kRootStickyClass));
  if ((flags & kVisitRootFlagAllRoots) != 0) {
    for (GcRoot<mirror::Class>& root : class_table_) {
      buffered_visitor.VisitRoot(root);
      root.Read()->VisitNativeRoots(buffered_visitor, image_pointer_size_);
    }
    for (GcRoot<mirror::Class>& root : pre_zygote_class_table_) {
      buffered_visitor.VisitRoot(root);
      root.Read()->VisitNativeRoots(buffered_visitor, image_pointer_size_);
    }
  } else if ((flags & kVisitRootFlagNewRoots) != 0) {
    for (auto& root : new_class_roots_) {
      mirror::Class* old_ref = root.Read<kWithoutReadBarrier>();
      old_ref->VisitNativeRoots(buffered_visitor, image_pointer_size_);
      root.VisitRoot(visitor, RootInfo(kRootStickyClass));
      mirror::Class* new_ref = root.Read<kWithoutReadBarrier>();
      if (UNLIKELY(new_ref != old_ref)) {
        auto it = class_table_.Find(GcRoot<mirror::Class>(old_ref));
        DCHECK(it != class_table_.end());
        *it = GcRoot<mirror::Class>(new_ref);
      }
    }
  }
  buffered_visitor.Flush();
  if ((flags & kVisitRootFlagClearRootLog) != 0) {
    new_class_roots_.clear();
  }
  if ((flags & kVisitRootFlagStartLoggingNewRoots) != 0) {
    log_new_class_table_roots_ = true;
  } else if ((flags & kVisitRootFlagStopLoggingNewRoots) != 0) {
    log_new_class_table_roots_ = false;
  }
}
void ClassLinker::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) {
  class_roots_.VisitRoot(visitor, RootInfo(kRootVMInternal));
  Thread* const self = Thread::Current();
  {
    ReaderMutexLock mu(self, dex_lock_);
    if ((flags & kVisitRootFlagAllRoots) != 0) {
      for (GcRoot<mirror::DexCache>& dex_cache : dex_caches_) {
        dex_cache.VisitRoot(visitor, RootInfo(kRootVMInternal));
      }
    } else if ((flags & kVisitRootFlagNewRoots) != 0) {
      for (size_t index : new_dex_cache_roots_) {
        dex_caches_[index].VisitRoot(visitor, RootInfo(kRootVMInternal));
      }
    }
    if ((flags & kVisitRootFlagClearRootLog) != 0) {
      new_dex_cache_roots_.clear();
    }
    if ((flags & kVisitRootFlagStartLoggingNewRoots) != 0) {
      log_new_dex_caches_roots_ = true;
    } else if ((flags & kVisitRootFlagStopLoggingNewRoots) != 0) {
      log_new_dex_caches_roots_ = false;
    }
  }
  VisitClassRoots(visitor, flags);
  array_iftable_.VisitRoot(visitor, RootInfo(kRootVMInternal));
  for (size_t i = 0; i < kFindArrayCacheSize; ++i) {
    find_array_class_cache_[i].VisitRootIfNonNull(visitor, RootInfo(kRootVMInternal));
  }
}
void ClassLinker::VisitClasses(ClassVisitor* visitor, void* arg) {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  for (GcRoot<mirror::Class>& root : class_table_) {
    if (!visitor(root.Read(), arg)) {
      return;
    }
  }
  for (GcRoot<mirror::Class>& root : pre_zygote_class_table_) {
    if (!visitor(root.Read(), arg)) {
      return;
    }
  }
}
static bool GetClassesVisitorSet(mirror::Class* c, void* arg) {
  std::set<mirror::Class*>* classes = reinterpret_cast<std::set<mirror::Class*>*>(arg);
  classes->insert(c);
  return true;
}
struct GetClassesVisitorArrayArg {
  Handle<mirror::ObjectArray<mirror::Class>>* classes;
  int32_t index;
  bool success;
};
static bool GetClassesVisitorArray(mirror::Class* c, void* varg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    GetClassesVisitorArrayArg* arg = reinterpret_cast<GetClassesVisitorArrayArg*>(varg);
    if (arg->index < (*arg->classes)->GetLength()) {
    (*arg->classes)->Set(arg->index, c);
    arg->index++;
    return true;
    } else {
    arg->success = false;
    return false;
    }
    }
void ClassLinker::VisitClassesWithoutClassesLock(ClassVisitor* visitor, void* arg) {
  if (!kMovingClasses) {
    std::set<mirror::Class*> classes;
    VisitClasses(GetClassesVisitorSet, &classes);
    for (mirror::Class* klass : classes) {
      if (!visitor(klass, arg)) {
        return;
      }
    }
  } else {
    Thread* self = Thread::Current();
    StackHandleScope<1> hs(self);
    MutableHandle<mirror::ObjectArray<mirror::Class>> classes =
        hs.NewHandle<mirror::ObjectArray<mirror::Class>>(nullptr);
    GetClassesVisitorArrayArg local_arg;
    local_arg.classes = &classes;
    local_arg.success = false;
    while (!local_arg.success) {
      size_t class_table_size;
      {
        ReaderMutexLock mu(self, *Locks::classlinker_classes_lock_);
        class_table_size = class_table_.Size() + pre_zygote_class_table_.Size();
      }
      mirror::Class* class_type = mirror::Class::GetJavaLangClass();
      mirror::Class* array_of_class = FindArrayClass(self, &class_type);
      classes.Assign(
          mirror::ObjectArray<mirror::Class>::Alloc(self, array_of_class, class_table_size));
      CHECK(classes.Get() != nullptr);
      local_arg.index = 0;
      local_arg.success = true;
      VisitClasses(GetClassesVisitorArray, &local_arg);
    }
    for (int32_t i = 0; i < classes->GetLength(); ++i) {
      mirror::Class* klass = classes->Get(i);
      if (klass != nullptr && !visitor(klass, arg)) {
        return;
      }
    }
  }
}
ClassLinker::~ClassLinker() {
  mirror::Class::ResetClass();
  mirror::Constructor::ResetClass();
  mirror::Field::ResetClass();
  mirror::Method::ResetClass();
  mirror::Reference::ResetClass();
  mirror::StackTraceElement::ResetClass();
  mirror::String::ResetClass();
  mirror::Throwable::ResetClass();
  mirror::BooleanArray::ResetArrayClass();
  mirror::ByteArray::ResetArrayClass();
  mirror::CharArray::ResetArrayClass();
  mirror::Constructor::ResetArrayClass();
  mirror::DoubleArray::ResetArrayClass();
  mirror::Field::ResetArrayClass();
  mirror::FloatArray::ResetArrayClass();
  mirror::Method::ResetArrayClass();
  mirror::IntArray::ResetArrayClass();
  mirror::LongArray::ResetArrayClass();
  mirror::ShortArray::ResetArrayClass();
  STLDeleteElements(&oat_files_);
}
mirror::PointerArray* ClassLinker::AllocPointerArray(Thread* self, size_t length) {
  return down_cast<mirror::PointerArray*>(image_pointer_size_ == 8u ?
      static_cast<mirror::Array*>(mirror::LongArray::Alloc(self, length)) :
      static_cast<mirror::Array*>(mirror::IntArray::Alloc(self, length)));
}
mirror::DexCache* ClassLinker::AllocDexCache(Thread* self, const DexFile& dex_file) {
  StackHandleScope<6> hs(self);
  auto dex_cache(hs.NewHandle(down_cast<mirror::DexCache*>(
      GetClassRoot(kJavaLangDexCache)->AllocObject(self))));
  if (dex_cache.Get() == nullptr) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  auto location(hs.NewHandle(intern_table_->InternStrong(dex_file.GetLocation().c_str())));
  if (location.Get() == nullptr) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  auto strings(hs.NewHandle(AllocStringArray(self, dex_file.NumStringIds())));
  if (strings.Get() == nullptr) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  auto types(hs.NewHandle(AllocClassArray(self, dex_file.NumTypeIds())));
  if (types.Get() == nullptr) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  auto methods(hs.NewHandle(AllocPointerArray(self, dex_file.NumMethodIds())));
  if (methods.Get() == nullptr) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  auto fields(hs.NewHandle(AllocPointerArray(self, dex_file.NumFieldIds())));
  if (fields.Get() == nullptr) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  dex_cache->Init(&dex_file, location.Get(), strings.Get(), types.Get(), methods.Get(),
                  fields.Get(), image_pointer_size_);
  return dex_cache.Get();
}
mirror::Class* ClassLinker::AllocClass(Thread* self, mirror::Class* java_lang_Class,
                                       uint32_t class_size) {
  DCHECK_GE(class_size, sizeof(mirror::Class));
  gc::Heap* heap = Runtime::Current()->GetHeap();
  mirror::Class::InitializeClassVisitor visitor(class_size);
  mirror::Object* k = kMovingClasses ?
      heap->AllocObject<true>(self, java_lang_Class, class_size, visitor) :
      heap->AllocNonMovableObject<true>(self, java_lang_Class, class_size, visitor);
  if (UNLIKELY(k == nullptr)) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  return k->AsClass();
}
mirror::Class* ClassLinker::AllocClass(Thread* self, uint32_t class_size) {
  return AllocClass(self, GetClassRoot(kJavaLangClass), class_size);
}
mirror::ObjectArray<mirror::StackTraceElement>* ClassLinker::AllocStackTraceElementArray(
    Thread* self, size_t length) {
  return mirror::ObjectArray<mirror::StackTraceElement>::Alloc(
      self, GetClassRoot(kJavaLangStackTraceElementArrayClass), length);
}
mirror::Class* ClassLinker::EnsureResolved(Thread* self, const char* descriptor,
                                           mirror::Class* klass) {
  DCHECK(klass != nullptr);
  if (init_done_ && klass->IsTemp()) {
    CHECK(!klass->IsResolved());
    if (klass->IsErroneous()) {
      ThrowEarlierClassFailure(klass);
      return nullptr;
    }
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(klass));
    ObjectLock<mirror::Class> lock(self, h_class);
    while (!h_class->IsRetired() && !h_class->IsErroneous()) {
      lock.WaitIgnoringInterrupts();
    }
    if (h_class->IsErroneous()) {
      ThrowEarlierClassFailure(h_class.Get());
      return nullptr;
    }
    CHECK(h_class->IsRetired());
    klass = LookupClass(self, descriptor, ComputeModifiedUtf8Hash(descriptor),
                        h_class.Get()->GetClassLoader());
  }
  if (!klass->IsResolved() && !klass->IsErroneous()) {
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Class> h_class(hs.NewHandleWrapper(&klass));
    ObjectLock<mirror::Class> lock(self, h_class);
    if (!h_class->IsResolved() && h_class->GetClinitThreadId() == self->GetTid()) {
      ThrowClassCircularityError(h_class.Get());
      mirror::Class::SetStatus(h_class, mirror::Class::kStatusError, self);
      return nullptr;
    }
    while (!h_class->IsResolved() && !h_class->IsErroneous()) {
      lock.WaitIgnoringInterrupts();
    }
  }
  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass);
    return nullptr;
  }
  CHECK(klass->IsResolved()) << PrettyClass(klass);
  self->AssertNoPendingException();
  return klass;
}
typedef std::pair<const DexFile*, const DexFile::ClassDef*> ClassPathEntry;
ClassPathEntry FindInClassPath(const char* descriptor,
                               size_t hash, const std::vector<const DexFile*>& class_path) {
  for (const DexFile* dex_file : class_path) {
    const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor, hash);
    if (dex_class_def != nullptr) {
      return ClassPathEntry(dex_file, dex_class_def);
    }
  }
  return ClassPathEntry(nullptr, nullptr);
}
static bool IsBootClassLoader(ScopedObjectAccessAlreadyRunnable& soa,
                              mirror::ClassLoader* class_loader)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return class_loader == nullptr ||
      class_loader->GetClass() ==
          soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_BootClassLoader);
    }
bool ClassLinker::FindClassInPathClassLoader(ScopedObjectAccessAlreadyRunnable& soa,
                                             Thread* self, const char* descriptor,
                                             size_t hash,
                                             Handle<mirror::ClassLoader> class_loader,
                                             mirror::Class** result) {
  if (IsBootClassLoader(soa, class_loader.Get())) {
    ClassPathEntry pair = FindInClassPath(descriptor, hash, boot_class_path_);
    if (pair.second != nullptr) {
      mirror::Class* klass = LookupClass(self, descriptor, hash, nullptr);
      if (klass != nullptr) {
        *result = EnsureResolved(self, descriptor, klass);
      } else {
        *result = DefineClass(self, descriptor, hash, NullHandle<mirror::ClassLoader>(),
                              *pair.first, *pair.second);
      }
      if (*result == nullptr) {
        CHECK(self->IsExceptionPending()) << descriptor;
        self->ClearException();
      }
    } else {
      *result = nullptr;
    }
    return true;
  }
  if (class_loader->GetClass() !=
      soa.Decode<mirror::Class*>(WellKnownClasses::dalvik_system_PathClassLoader)) {
    *result = nullptr;
    return false;
  }
  StackHandleScope<4> hs(self);
  Handle<mirror::ClassLoader> h_parent(hs.NewHandle(class_loader->GetParent()));
  bool recursive_result = FindClassInPathClassLoader(soa, self, descriptor, hash, h_parent, result);
  if (!recursive_result) {
    return false;
  }
  if (*result != nullptr) {
    return true;
  }
  ArtField* const cookie_field = soa.DecodeField(WellKnownClasses::dalvik_system_DexFile_cookie);
  ArtField* const dex_file_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList__Element_dexFile);
  mirror::Object* dex_path_list =
      soa.DecodeField(WellKnownClasses::dalvik_system_PathClassLoader_pathList)->
      GetObject(class_loader.Get());
  if (dex_path_list != nullptr && dex_file_field != nullptr && cookie_field != nullptr) {
    mirror::Object* dex_elements_obj =
        soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList_dexElements)->
        GetObject(dex_path_list);
    if (dex_elements_obj != nullptr) {
      Handle<mirror::ObjectArray<mirror::Object>> dex_elements =
          hs.NewHandle(dex_elements_obj->AsObjectArray<mirror::Object>());
      for (int32_t i = 0; i < dex_elements->GetLength(); ++i) {
        mirror::Object* element = dex_elements->GetWithoutChecks(i);
        if (element == nullptr) {
          break;
        }
        mirror::Object* dex_file = dex_file_field->GetObject(element);
        if (dex_file != nullptr) {
          mirror::LongArray* long_array = cookie_field->GetObject(dex_file)->AsLongArray();
          if (long_array == nullptr) {
            LOG(WARNING) << "Null DexFile::mCookie for " << descriptor;
            break;
          }
          int32_t long_array_size = long_array->GetLength();
          for (int32_t j = 0; j < long_array_size; ++j) {
            const DexFile* cp_dex_file = reinterpret_cast<const DexFile*>(static_cast<uintptr_t>(
                long_array->GetWithoutChecks(j)));
            const DexFile::ClassDef* dex_class_def = cp_dex_file->FindClassDef(descriptor, hash);
            if (dex_class_def != nullptr) {
              RegisterDexFile(*cp_dex_file);
              mirror::Class* klass = DefineClass(self, descriptor, hash, class_loader,
                                                 *cp_dex_file, *dex_class_def);
              if (klass == nullptr) {
                CHECK(self->IsExceptionPending()) << descriptor;
                self->ClearException();
                return true;
              }
              *result = klass;
              return true;
            }
          }
        }
      }
    }
    self->AssertNoPendingException();
  }
  return true;
}
mirror::Class* ClassLinker::FindClass(Thread* self, const char* descriptor,
                                      Handle<mirror::ClassLoader> class_loader) {
  DCHECK_NE(*descriptor, '\0') << "descriptor is empty string";
  DCHECK(self != nullptr);
  self->AssertNoPendingException();
  if (descriptor[1] == '\0') {
    return FindPrimitiveClass(descriptor[0]);
  }
  const size_t hash = ComputeModifiedUtf8Hash(descriptor);
  mirror::Class* klass = LookupClass(self, descriptor, hash, class_loader.Get());
  if (klass != nullptr) {
    return EnsureResolved(self, descriptor, klass);
  }
  if (descriptor[0] == '[') {
    return CreateArrayClass(self, descriptor, hash, class_loader);
  } else if (class_loader.Get() == nullptr) {
    ClassPathEntry pair = FindInClassPath(descriptor, hash, boot_class_path_);
    if (pair.second != nullptr) {
      return DefineClass(self, descriptor, hash, NullHandle<mirror::ClassLoader>(), *pair.first,
                         *pair.second);
    } else {
      mirror::Throwable* pre_allocated = Runtime::Current()->GetPreAllocatedNoClassDefFoundError();
      self->SetException(pre_allocated);
      return nullptr;
    }
  } else {
    ScopedObjectAccessUnchecked soa(self);
    mirror::Class* cp_klass;
    if (FindClassInPathClassLoader(soa, self, descriptor, hash, class_loader, &cp_klass)) {
      if (cp_klass != nullptr) {
        return cp_klass;
      }
    }
    if (Runtime::Current()->IsAotCompiler()) {
      mirror::Throwable* pre_allocated = Runtime::Current()->GetPreAllocatedNoClassDefFoundError();
      self->SetException(pre_allocated);
      return nullptr;
    }
    ScopedLocalRef<jobject> class_loader_object(soa.Env(),
                                                soa.AddLocalReference<jobject>(class_loader.Get()));
    std::string class_name_string(DescriptorToDot(descriptor));
    ScopedLocalRef<jobject> result(soa.Env(), nullptr);
    {
      ScopedThreadStateChange tsc(self, kNative);
      ScopedLocalRef<jobject> class_name_object(soa.Env(),
                                                soa.Env()->NewStringUTF(class_name_string.c_str()));
      if (class_name_object.get() == nullptr) {
        DCHECK(self->IsExceptionPending());
        return nullptr;
      }
      CHECK(class_loader_object.get() != nullptr);
      result.reset(soa.Env()->CallObjectMethod(class_loader_object.get(),
                                               WellKnownClasses::java_lang_ClassLoader_loadClass,
                                               class_name_object.get()));
    }
    if (self->IsExceptionPending()) {
      return nullptr;
    } else if (result.get() == nullptr) {
      ThrowNullPointerException(StringPrintf("ClassLoader.loadClass returned null for %s",
                                             class_name_string.c_str()).c_str());
      return nullptr;
    } else {
      return soa.Decode<mirror::Class*>(result.get());
    }
  }
  UNREACHABLE();
}
mirror::Class* ClassLinker::DefineClass(Thread* self, const char* descriptor, size_t hash,
                                        Handle<mirror::ClassLoader> class_loader,
                                        const DexFile& dex_file,
                                        const DexFile::ClassDef& dex_class_def) {
  StackHandleScope<3> hs(self);
  auto klass = hs.NewHandle<mirror::Class>(nullptr);
  if (UNLIKELY(!init_done_)) {
    if (strcmp(descriptor, "Ljava/lang/Object;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangObject));
    } else if (strcmp(descriptor, "Ljava/lang/Class;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangClass));
    } else if (strcmp(descriptor, "Ljava/lang/String;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangString));
    } else if (strcmp(descriptor, "Ljava/lang/ref/Reference;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangRefReference));
    } else if (strcmp(descriptor, "Ljava/lang/DexCache;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangDexCache));
    }
  }
  if (klass.Get() == nullptr) {
    klass.Assign(AllocClass(self, SizeOfClassWithoutEmbeddedTables(dex_file, dex_class_def)));
  }
  if (UNLIKELY(klass.Get() == nullptr)) {
    CHECK(self->IsExceptionPending());
    return nullptr;
  }
  klass->SetDexCache(FindDexCache(dex_file));
  SetupClass(dex_file, dex_class_def, klass, class_loader.Get());
  if (UNLIKELY(!init_done_)) {
    if (strcmp(descriptor, "Ljava/lang/String;") == 0) {
      klass->SetStringClass();
    }
  }
  ObjectLock<mirror::Class> lock(self, klass);
  klass->SetClinitThreadId(self->GetTid());
  mirror::Class* existing = InsertClass(descriptor, klass.Get(), hash);
  if (existing != nullptr) {
    return EnsureResolved(self, descriptor, existing);
  }
  LoadClass(self, dex_file, dex_class_def, klass);
  if (self->IsExceptionPending()) {
    if (!klass->IsErroneous()) {
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
    }
    return nullptr;
  }
  CHECK(!klass->IsLoaded());
  if (!LoadSuperAndInterfaces(klass, dex_file)) {
    if (!klass->IsErroneous()) {
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
    }
    return nullptr;
  }
  CHECK(klass->IsLoaded());
  CHECK(!klass->IsResolved());
  auto interfaces = hs.NewHandle<mirror::ObjectArray<mirror::Class>>(nullptr);
  MutableHandle<mirror::Class> h_new_class = hs.NewHandle<mirror::Class>(nullptr);
  if (!LinkClass(self, descriptor, klass, interfaces, &h_new_class)) {
    if (!klass->IsErroneous()) {
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
    }
    return nullptr;
  }
  self->AssertNoPendingException();
  CHECK(h_new_class.Get() != nullptr) << descriptor;
  CHECK(h_new_class->IsResolved()) << descriptor;
  if (Runtime::Current()->GetInstrumentation()->AreExitStubsInstalled()) {
    DCHECK_EQ(self->GetState(), kRunnable);
    Runtime::Current()->GetInstrumentation()->InstallStubsForClass(h_new_class.Get());
  }
  Dbg::PostClassPrepare(h_new_class.Get());
  return h_new_class.Get();
}
uint32_t ClassLinker::SizeOfClassWithoutEmbeddedTables(const DexFile& dex_file,
                                                       const DexFile::ClassDef& dex_class_def) {
  const uint8_t* class_data = dex_file.GetClassData(dex_class_def);
  size_t num_ref = 0;
  size_t num_8 = 0;
  size_t num_16 = 0;
  size_t num_32 = 0;
  size_t num_64 = 0;
  if (class_data != nullptr) {
    for (ClassDataItemIterator it(dex_file, class_data); it.HasNextStaticField(); it.Next()) {
      const DexFile::FieldId& field_id = dex_file.GetFieldId(it.GetMemberIndex());
      const char* descriptor = dex_file.GetFieldTypeDescriptor(field_id);
      char c = descriptor[0];
      switch (c) {
        case 'L':
        case '[':
          num_ref++;
          break;
        case 'J':
        case 'D':
          num_64++;
          break;
        case 'I':
        case 'F':
          num_32++;
          break;
        case 'S':
        case 'C':
          num_16++;
          break;
        case 'B':
        case 'Z':
          num_8++;
          break;
        default:
          LOG(FATAL) << "Unknown descriptor: " << c;
          UNREACHABLE();
      }
    }
  }
  return mirror::Class::ComputeClassSize(false, 0, num_8, num_16, num_32, num_64, num_ref,
                                         image_pointer_size_);
}
OatFile::OatClass ClassLinker::FindOatClass(const DexFile& dex_file, uint16_t class_def_idx,
                                            bool* found) {
  DCHECK_NE(class_def_idx, DexFile::kDexNoIndex16);
  const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  if (oat_dex_file == nullptr) {
    *found = false;
    return OatFile::OatClass::Invalid();
  }
  *found = true;
  return oat_dex_file->GetOatClass(class_def_idx);
}
static uint32_t GetOatMethodIndexFromMethodIndex(const DexFile& dex_file, uint16_t class_def_idx,
                                                 uint32_t method_idx) {
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_idx);
  const uint8_t* class_data = dex_file.GetClassData(class_def);
  CHECK(class_data != nullptr);
  ClassDataItemIterator it(dex_file, class_data);
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  size_t class_def_method_index = 0;
  while (it.HasNextDirectMethod()) {
    if (it.GetMemberIndex() == method_idx) {
      return class_def_method_index;
    }
    class_def_method_index++;
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    if (it.GetMemberIndex() == method_idx) {
      return class_def_method_index;
    }
    class_def_method_index++;
    it.Next();
  }
  DCHECK(!it.HasNext());
  LOG(FATAL) << "Failed to find method index " << method_idx << " in " << dex_file.GetLocation();
  UNREACHABLE();
}
const OatFile::OatMethod ClassLinker::FindOatMethodFor(ArtMethod* method, bool* found) {
  mirror::Class* declaring_class = method->GetDeclaringClass();
  size_t oat_method_index;
  if (method->IsStatic() || method->IsDirect()) {
    oat_method_index = method->GetMethodIndex();
  } else {
    oat_method_index = declaring_class->NumDirectMethods();
    size_t end = declaring_class->NumVirtualMethods();
    bool found_virtual = false;
    for (size_t i = 0; i < end; i++) {
      if (method->GetDexMethodIndex() ==
          declaring_class->GetVirtualMethod(i, image_pointer_size_)->GetDexMethodIndex()) {
        found_virtual = true;
        break;
      }
      oat_method_index++;
    }
    CHECK(found_virtual) << "Didn't find oat method index for virtual method: "
                         << PrettyMethod(method);
  }
  DCHECK_EQ(oat_method_index,
            GetOatMethodIndexFromMethodIndex(*declaring_class->GetDexCache()->GetDexFile(),
                                             method->GetDeclaringClass()->GetDexClassDefIndex(),
                                             method->GetDexMethodIndex()));
  OatFile::OatClass oat_class = FindOatClass(*declaring_class->GetDexCache()->GetDexFile(),
                                             declaring_class->GetDexClassDefIndex(),
                                             found);
  if (!(*found)) {
    return OatFile::OatMethod::Invalid();
  }
  return oat_class.GetOatMethod(oat_method_index);
}
const void* ClassLinker::GetQuickOatCodeFor(const DexFile& dex_file, uint16_t class_def_idx,
                                            uint32_t method_idx) {
  bool found;
  OatFile::OatClass oat_class = FindOatClass(dex_file, class_def_idx, &found);
  if (!found) {
    return nullptr;
  }
  uint32_t oat_method_idx = GetOatMethodIndexFromMethodIndex(dex_file, class_def_idx, method_idx);
  return oat_class.GetOatMethod(oat_method_idx).GetQuickCode();
}
static bool NeedsInterpreter(ArtMethod* method, const void* quick_code)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    if (quick_code == nullptr) {
    return true;
    }
    return Runtime::Current()->GetInstrumentation()->InterpretOnly() &&
         !method->IsNative() && !method->IsProxyMethod();
    }
void ClassLinker::FixupStaticTrampolines(mirror::Class* klass) {
  DCHECK(klass->IsInitialized()) << PrettyDescriptor(klass);
  if (klass->NumDirectMethods() == 0) {
    return;
  }
  Runtime* runtime = Runtime::Current();
  if (!runtime->IsStarted()) {
    if (runtime->IsAotCompiler() || runtime->GetHeap()->HasImageSpace()) {
      return;
    }
  }
  const DexFile& dex_file = klass->GetDexFile();
  const DexFile::ClassDef* dex_class_def = klass->GetClassDef();
  CHECK(dex_class_def != nullptr);
  const uint8_t* class_data = dex_file.GetClassData(*dex_class_def);
  CHECK(class_data != nullptr) << PrettyDescriptor(klass);
  ClassDataItemIterator it(dex_file, class_data);
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  bool has_oat_class;
  OatFile::OatClass oat_class = FindOatClass(dex_file, klass->GetDexClassDefIndex(),
                                             &has_oat_class);
  for (size_t method_index = 0; it.HasNextDirectMethod(); ++method_index, it.Next()) {
    ArtMethod* method = klass->GetDirectMethod(method_index, image_pointer_size_);
    if (!method->IsStatic()) {
      continue;
    }
    const void* quick_code = nullptr;
    if (has_oat_class) {
      OatFile::OatMethod oat_method = oat_class.GetOatMethod(method_index);
      quick_code = oat_method.GetQuickCode();
    }
    const bool enter_interpreter = NeedsInterpreter(method, quick_code);
    if (enter_interpreter) {
      if (quick_code == nullptr && method->IsNative()) {
        quick_code = GetQuickGenericJniStub();
      } else {
        quick_code = GetQuickToInterpreterBridge();
      }
    }
    runtime->GetInstrumentation()->UpdateMethodsCode(method, quick_code);
  }
}
void ClassLinker::LinkCode(ArtMethod* method, const OatFile::OatClass* oat_class, uint32_t class_def_method_index) {
  Runtime* const runtime = Runtime::Current();
  if (runtime->IsAotCompiler()) {
    return;
  }
  DCHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr);
  if (oat_class != nullptr) {
    const OatFile::OatMethod oat_method = oat_class->GetOatMethod(class_def_method_index);
    oat_method.LinkMethod(method);
  }
  bool enter_interpreter = NeedsInterpreter(method, method->GetEntryPointFromQuickCompiledCode());
  if (enter_interpreter && !method->IsNative()) {
    method->SetEntryPointFromInterpreter(artInterpreterToInterpreterBridge);
  } else {
    method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
  }
  if (method->IsAbstract()) {
    method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
    return;
  }
  if (method->IsStatic() && !method->IsConstructor()) {
    method->SetEntryPointFromQuickCompiledCode(GetQuickResolutionStub());
  } else if (enter_interpreter) {
    if (!method->IsNative()) {
      method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
    } else {
      method->SetEntryPointFromQuickCompiledCode(GetQuickGenericJniStub());
    }
  }
  if (method->IsNative()) {
    method->UnregisterNative();
    if (enter_interpreter) {
      const void* entry_point = method->GetEntryPointFromQuickCompiledCode();
      DCHECK(IsQuickGenericJniStub(entry_point) || IsQuickResolutionStub(entry_point));
    }
  }
}
void ClassLinker::SetupClass(const DexFile& dex_file, const DexFile::ClassDef& dex_class_def,
                             Handle<mirror::Class> klass, mirror::ClassLoader* class_loader) {
  CHECK(klass.Get() != nullptr);
  CHECK(klass->GetDexCache() != nullptr);
  CHECK_EQ(mirror::Class::kStatusNotReady, klass->GetStatus());
  const char* descriptor = dex_file.GetClassDescriptor(dex_class_def);
  CHECK(descriptor != nullptr);
  klass->SetClass(GetClassRoot(kJavaLangClass));
  uint32_t access_flags = dex_class_def.GetJavaAccessFlags();
  CHECK_EQ(access_flags & ~kAccJavaFlagsMask, 0U);
  klass->SetAccessFlags(access_flags);
  klass->SetClassLoader(class_loader);
  DCHECK_EQ(klass->GetPrimitiveType(), Primitive::kPrimNot);
  mirror::Class::SetStatus(klass, mirror::Class::kStatusIdx, nullptr);
  klass->SetDexClassDefIndex(dex_file.GetIndexForClassDef(dex_class_def));
  klass->SetDexTypeIndex(dex_class_def.class_idx_);
  CHECK(klass->GetDexCacheStrings() != nullptr);
}
void ClassLinker::LoadClass(Thread* self, const DexFile& dex_file,
                            const DexFile::ClassDef& dex_class_def,
                            Handle<mirror::Class> klass) {
  const uint8_t* class_data = dex_file.GetClassData(dex_class_def);
  if (class_data == nullptr) {
    return;
  }
  bool has_oat_class = false;
  if (Runtime::Current()->IsStarted() && !Runtime::Current()->IsAotCompiler()) {
    OatFile::OatClass oat_class = FindOatClass(dex_file, klass->GetDexClassDefIndex(),
                                               &has_oat_class);
    if (has_oat_class) {
      LoadClassMembers(self, dex_file, class_data, klass, &oat_class);
    }
  }
  if (!has_oat_class) {
    LoadClassMembers(self, dex_file, class_data, klass, nullptr);
  }
}
ArtField* ClassLinker::AllocArtFieldArray(Thread* self, size_t length) {
  auto* const la = Runtime::Current()->GetLinearAlloc();
  auto* ptr = reinterpret_cast<ArtField*>(la->AllocArray<ArtField>(self, length));
  CHECK(ptr!= nullptr);
  std::uninitialized_fill_n(ptr, length, ArtField());
  return ptr;
}
ArtMethod* ClassLinker::AllocArtMethodArray(Thread* self, size_t length) {
  const size_t method_size = ArtMethod::ObjectSize(image_pointer_size_);
  uintptr_t ptr = reinterpret_cast<uintptr_t>(
      Runtime::Current()->GetLinearAlloc()->Alloc(self, method_size * length));
  CHECK_NE(ptr, 0u);
  for (size_t i = 0; i < length; ++i) {
    new(reinterpret_cast<void*>(ptr + i * method_size)) ArtMethod;
  }
  return reinterpret_cast<ArtMethod*>(ptr);
}
void ClassLinker::LoadClassMembers(Thread* self, const DexFile& dex_file,
                                   const uint8_t* class_data,
                                   Handle<mirror::Class> klass,
                                   const OatFile::OatClass* oat_class) {
  {
    ScopedAssertNoThreadSuspension nts(self, __FUNCTION__);
    ClassDataItemIterator it(dex_file, class_data);
    const size_t num_sfields = it.NumStaticFields();
    ArtField* sfields = num_sfields != 0 ? AllocArtFieldArray(self, num_sfields) : nullptr;
    for (size_t i = 0; it.HasNextStaticField(); i++, it.Next()) {
      CHECK_LT(i, num_sfields);
      LoadField(it, klass, &sfields[i]);
    }
    klass->SetSFields(sfields);
    klass->SetNumStaticFields(num_sfields);
    DCHECK_EQ(klass->NumStaticFields(), num_sfields);
    const size_t num_ifields = it.NumInstanceFields();
    ArtField* ifields = num_ifields != 0 ? AllocArtFieldArray(self, num_ifields) : nullptr;
    for (size_t i = 0; it.HasNextInstanceField(); i++, it.Next()) {
      CHECK_LT(i, num_ifields);
      LoadField(it, klass, &ifields[i]);
    }
    klass->SetIFields(ifields);
    klass->SetNumInstanceFields(num_ifields);
    DCHECK_EQ(klass->NumInstanceFields(), num_ifields);
    if (it.NumDirectMethods() != 0) {
      klass->SetDirectMethodsPtr(AllocArtMethodArray(self, it.NumDirectMethods()));
    }
    klass->SetNumDirectMethods(it.NumDirectMethods());
    if (it.NumVirtualMethods() != 0) {
      klass->SetVirtualMethodsPtr(AllocArtMethodArray(self, it.NumVirtualMethods()));
    }
    klass->SetNumVirtualMethods(it.NumVirtualMethods());
    size_t class_def_method_index = 0;
    uint32_t last_dex_method_index = DexFile::kDexNoIndex;
    size_t last_class_def_method_index = 0;
    for (size_t i = 0; it.HasNextDirectMethod(); i++, it.Next()) {
      ArtMethod* method = klass->GetDirectMethodUnchecked(i, image_pointer_size_);
      LoadMethod(self, dex_file, it, klass, method);
      LinkCode(method, oat_class, class_def_method_index);
      uint32_t it_method_index = it.GetMemberIndex();
      if (last_dex_method_index == it_method_index) {
        method->SetMethodIndex(last_class_def_method_index);
      } else {
        method->SetMethodIndex(class_def_method_index);
        last_dex_method_index = it_method_index;
        last_class_def_method_index = class_def_method_index;
      }
      class_def_method_index++;
    }
    for (size_t i = 0; it.HasNextVirtualMethod(); i++, it.Next()) {
      ArtMethod* method = klass->GetVirtualMethodUnchecked(i, image_pointer_size_);
      LoadMethod(self, dex_file, it, klass, method);
      DCHECK_EQ(class_def_method_index, it.NumDirectMethods() + i);
      LinkCode(method, oat_class, class_def_method_index);
      class_def_method_index++;
    }
    DCHECK(!it.HasNext());
  }
  self->AllowThreadSuspension();
}
void ClassLinker::LoadField(const ClassDataItemIterator& it, Handle<mirror::Class> klass,
                            ArtField* dst) {
  const uint32_t field_idx = it.GetMemberIndex();
  dst->SetDexFieldIndex(field_idx);
  dst->SetDeclaringClass(klass.Get());
  dst->SetAccessFlags(it.GetFieldAccessFlags());
}
void ClassLinker::LoadMethod(Thread* self, const DexFile& dex_file, const ClassDataItemIterator& it, Handle<mirror::Class> klass, ArtMethod* dst) {
  uint32_t dex_method_idx = it.GetMemberIndex();
  const DexFile::MethodId& method_id = dex_file.GetMethodId(dex_method_idx);
  const char* method_name = dex_file.StringDataByIdx(method_id.name_idx_);
  ScopedAssertNoThreadSuspension ants(self, "LoadMethod");
  dst->SetDexMethodIndex(dex_method_idx);
  dst->SetDeclaringClass(klass.Get());
  dst->SetCodeItemOffset(it.GetMethodCodeItemOffset());
  dst->SetDexCacheResolvedMethods(klass->GetDexCache()->GetResolvedMethods());
  dst->SetDexCacheResolvedTypes(klass->GetDexCache()->GetResolvedTypes());
  uint32_t access_flags = it.GetMethodAccessFlags();
  if (UNLIKELY(strcmp("finalize", method_name) == 0)) {
    if (strcmp("V", dex_file.GetShorty(method_id.proto_idx_)) == 0) {
      if (klass->GetClassLoader() != nullptr) {
        klass->SetFinalizable();
      } else {
        std::string temp;
        const char* klass_descriptor = klass->GetDescriptor(&temp);
        if (strcmp(klass_descriptor, "Ljava/lang/Object;") != 0 &&
            strcmp(klass_descriptor, "Ljava/lang/Enum;") != 0) {
          klass->SetFinalizable();
        }
      }
    }
  } else if (method_name[0] == '<') {
    bool is_init = (strcmp("<init>", method_name) == 0);
    bool is_clinit = !is_init && (strcmp("<clinit>", method_name) == 0);
    if (UNLIKELY(!is_init && !is_clinit)) {
      LOG(WARNING) << "Unexpected '<' at start of method name " << method_name;
    } else {
      if (UNLIKELY((access_flags & kAccConstructor) == 0)) {
        LOG(WARNING) << method_name << " didn't have expected constructor access flag in class "
            << PrettyDescriptor(klass.Get()) << " in dex file " << dex_file.GetLocation();
        access_flags |= kAccConstructor;
      }
    }
  }
  dst->SetAccessFlags(access_flags);
}
void ClassLinker::AppendToBootClassPath(Thread* self, const DexFile& dex_file) {
  StackHandleScope<1> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(AllocDexCache(self, dex_file)));
  CHECK(dex_cache.Get() != nullptr) << "Failed to allocate dex cache for "
                                    << dex_file.GetLocation();
  AppendToBootClassPath(dex_file, dex_cache);
}
void ClassLinker::AppendToBootClassPath(const DexFile& dex_file,
                                        Handle<mirror::DexCache> dex_cache) {
  CHECK(dex_cache.Get() != nullptr) << dex_file.GetLocation();
  boot_class_path_.push_back(&dex_file);
  RegisterDexFile(dex_file, dex_cache);
}
bool ClassLinker::IsDexFileRegisteredLocked(const DexFile& dex_file) {
  dex_lock_.AssertSharedHeld(Thread::Current());
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    mirror::DexCache* dex_cache = GetDexCache(i);
    if (dex_cache->GetDexFile() == &dex_file) {
      return true;
    }
  }
  return false;
}
bool ClassLinker::IsDexFileRegistered(const DexFile& dex_file) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  return IsDexFileRegisteredLocked(dex_file);
}
void ClassLinker::RegisterDexFileLocked(const DexFile& dex_file,
                                        Handle<mirror::DexCache> dex_cache) {
  dex_lock_.AssertExclusiveHeld(Thread::Current());
  CHECK(dex_cache.Get() != nullptr) << dex_file.GetLocation();
  CHECK(dex_cache->GetLocation()->Equals(dex_file.GetLocation()))
      << dex_cache->GetLocation()->ToModifiedUtf8() << " " << dex_file.GetLocation();
  dex_caches_.push_back(GcRoot<mirror::DexCache>(dex_cache.Get()));
  dex_cache->SetDexFile(&dex_file);
  if (log_new_dex_caches_roots_) {
    new_dex_cache_roots_.push_back(dex_caches_.size() - 1);
  }
}
void ClassLinker::RegisterDexFile(const DexFile& dex_file) {
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(AllocDexCache(self, dex_file)));
  CHECK(dex_cache.Get() != nullptr) << "Failed to allocate dex cache for "
                                    << dex_file.GetLocation();
  {
    WriterMutexLock mu(self, dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
    RegisterDexFileLocked(dex_file, dex_cache);
  }
}
void ClassLinker::RegisterDexFile(const DexFile& dex_file,
                                  Handle<mirror::DexCache> dex_cache) {
  WriterMutexLock mu(Thread::Current(), dex_lock_);
  RegisterDexFileLocked(dex_file, dex_cache);
}
mirror::DexCache* ClassLinker::FindDexCache(const DexFile& dex_file) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    mirror::DexCache* dex_cache = GetDexCache(i);
    if (dex_cache->GetDexFile() == &dex_file) {
      return dex_cache;
    }
  }
  std::string location(dex_file.GetLocation());
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    mirror::DexCache* dex_cache = GetDexCache(i);
    if (dex_cache->GetDexFile()->GetLocation() == location) {
      return dex_cache;
    }
  }
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    mirror::DexCache* dex_cache = GetDexCache(i);
    LOG(ERROR) << "Registered dex file " << i << " = " << dex_cache->GetDexFile()->GetLocation();
  }
  LOG(FATAL) << "Failed to find DexCache for DexFile " << location;
  UNREACHABLE();
}
void ClassLinker::FixupDexCaches(ArtMethod* resolution_method) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  for (auto& dex_cache : dex_caches_) {
    dex_cache.Read()->Fixup(resolution_method, image_pointer_size_);
  }
}
mirror::Class* ClassLinker::CreatePrimitiveClass(Thread* self, Primitive::Type type) {
  mirror::Class* klass = AllocClass(self, mirror::Class::PrimitiveClassSize(image_pointer_size_));
  if (UNLIKELY(klass == nullptr)) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  return InitializePrimitiveClass(klass, type);
}
mirror::Class* ClassLinker::InitializePrimitiveClass(mirror::Class* primitive_class,
                                                     Primitive::Type type) {
  CHECK(primitive_class != nullptr);
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> h_class(hs.NewHandle(primitive_class));
  ObjectLock<mirror::Class> lock(self, h_class);
  h_class->SetAccessFlags(kAccPublic | kAccFinal | kAccAbstract);
  h_class->SetPrimitiveType(type);
  mirror::Class::SetStatus(h_class, mirror::Class::kStatusInitialized, self);
  const char* descriptor = Primitive::Descriptor(type);
  mirror::Class* existing = InsertClass(descriptor, h_class.Get(),
                                        ComputeModifiedUtf8Hash(descriptor));
  CHECK(existing == nullptr) << "InitPrimitiveClass(" << type << ") failed";
  return h_class.Get();
}
mirror::Class* ClassLinker::CreateArrayClass(Thread* self, const char* descriptor, size_t hash,
                                             Handle<mirror::ClassLoader> class_loader) {
  CHECK_EQ('[', descriptor[0]);
  StackHandleScope<2> hs(self);
  MutableHandle<mirror::Class> component_type(hs.NewHandle(FindClass(self, descriptor + 1,
                                                                     class_loader)));
  if (component_type.Get() == nullptr) {
    DCHECK(self->IsExceptionPending());
    const size_t component_hash = ComputeModifiedUtf8Hash(descriptor + 1);
    component_type.Assign(LookupClass(self, descriptor + 1, component_hash, class_loader.Get()));
    if (component_type.Get() == nullptr) {
      DCHECK(self->IsExceptionPending());
      return nullptr;
    } else {
      self->ClearException();
    }
  }
  if (UNLIKELY(component_type->IsPrimitiveVoid())) {
    ThrowNoClassDefFoundError("Attempt to create array of void primitive type");
    return nullptr;
  }
  if (class_loader.Get() != component_type->GetClassLoader()) {
    mirror::Class* new_class = LookupClass(self, descriptor, hash, component_type->GetClassLoader());
    if (new_class != nullptr) {
      return new_class;
    }
  }
  auto new_class = hs.NewHandle<mirror::Class>(nullptr);
  if (UNLIKELY(!init_done_)) {
    if (strcmp(descriptor, "[Ljava/lang/Class;") == 0) {
      new_class.Assign(GetClassRoot(kClassArrayClass));
    } else if (strcmp(descriptor, "[Ljava/lang/Object;") == 0) {
      new_class.Assign(GetClassRoot(kObjectArrayClass));
    } else if (strcmp(descriptor, GetClassRootDescriptor(kJavaLangStringArrayClass)) == 0) {
      new_class.Assign(GetClassRoot(kJavaLangStringArrayClass));
    } else if (strcmp(descriptor, "[C") == 0) {
      new_class.Assign(GetClassRoot(kCharArrayClass));
    } else if (strcmp(descriptor, "[I") == 0) {
      new_class.Assign(GetClassRoot(kIntArrayClass));
    } else if (strcmp(descriptor, "[J") == 0) {
      new_class.Assign(GetClassRoot(kLongArrayClass));
    }
  }
  if (new_class.Get() == nullptr) {
    new_class.Assign(AllocClass(self, mirror::Array::ClassSize(image_pointer_size_)));
    if (new_class.Get() == nullptr) {
      self->AssertPendingOOMException();
      return nullptr;
    }
    new_class->SetComponentType(component_type.Get());
  }
  ObjectLock<mirror::Class> lock(self, new_class);
  DCHECK(new_class->GetComponentType() != nullptr);
  mirror::Class* java_lang_Object = GetClassRoot(kJavaLangObject);
  new_class->SetSuperClass(java_lang_Object);
  new_class->SetVTable(java_lang_Object->GetVTable());
  new_class->SetPrimitiveType(Primitive::kPrimNot);
  new_class->SetClassLoader(component_type->GetClassLoader());
  mirror::Class::SetStatus(new_class, mirror::Class::kStatusLoaded, self);
  {
    ArtMethod* imt[mirror::Class::kImtSize];
    std::fill_n(imt, arraysize(imt), Runtime::Current()->GetImtUnimplementedMethod());
    new_class->PopulateEmbeddedImtAndVTable(imt, image_pointer_size_);
  }
  mirror::Class::SetStatus(new_class, mirror::Class::kStatusInitialized, self);
  {
    mirror::IfTable* array_iftable = array_iftable_.Read();
    CHECK(array_iftable != nullptr);
    new_class->SetIfTable(array_iftable);
  }
  int access_flags = new_class->GetComponentType()->GetAccessFlags();
  access_flags &= kAccJavaFlagsMask;
  access_flags |= kAccAbstract | kAccFinal;
  access_flags &= ~kAccInterface;
  new_class->SetAccessFlags(access_flags);
  mirror::Class* existing = InsertClass(descriptor, new_class.Get(), hash);
  if (existing == nullptr) {
    return new_class.Get();
  }
  return existing;
}
mirror::Class* ClassLinker::FindPrimitiveClass(char type) {
  switch (type) {
    case 'B':
      return GetClassRoot(kPrimitiveByte);
    case 'C':
      return GetClassRoot(kPrimitiveChar);
    case 'D':
      return GetClassRoot(kPrimitiveDouble);
    case 'F':
      return GetClassRoot(kPrimitiveFloat);
    case 'I':
      return GetClassRoot(kPrimitiveInt);
    case 'J':
      return GetClassRoot(kPrimitiveLong);
    case 'S':
      return GetClassRoot(kPrimitiveShort);
    case 'Z':
      return GetClassRoot(kPrimitiveBoolean);
    case 'V':
      return GetClassRoot(kPrimitiveVoid);
    default:
      break;
  }
  std::string printable_type(PrintableChar(type));
  ThrowNoClassDefFoundError("Not a primitive type: %s", printable_type.c_str());
  return nullptr;
}
mirror::Class* ClassLinker::InsertClass(const char* descriptor, mirror::Class* klass,
                                        size_t hash) {
  if (VLOG_IS_ON(class_linker)) {
    mirror::DexCache* dex_cache = klass->GetDexCache();
    std::string source;
    if (dex_cache != nullptr) {
      source += " from ";
      source += dex_cache->GetLocation()->ToModifiedUtf8();
    }
    LOG(INFO) << "Loaded class " << descriptor << source;
  }
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  mirror::Class* existing = LookupClassFromTableLocked(descriptor, klass->GetClassLoader(), hash);
  if (existing != nullptr) {
    return existing;
  }
  if (kIsDebugBuild && !klass->IsTemp() && klass->GetClassLoader() == nullptr &&
      dex_cache_image_class_lookup_required_) {
    existing = LookupClassFromImage(descriptor);
    if (existing != nullptr) {
      CHECK_EQ(klass, existing);
    }
  }
  VerifyObject(klass);
  class_table_.InsertWithHash(GcRoot<mirror::Class>(klass), hash);
  if (log_new_class_table_roots_) {
    new_class_roots_.push_back(GcRoot<mirror::Class>(klass));
  }
  return nullptr;
}
void ClassLinker::UpdateClassVirtualMethods(mirror::Class* klass, ArtMethod* new_methods, size_t new_num_methods) {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  klass->SetNumVirtualMethods(new_num_methods);
  klass->SetVirtualMethodsPtr(new_methods);
  if (log_new_class_table_roots_) {
    new_class_roots_.push_back(GcRoot<mirror::Class>(klass));
  }
}
mirror::Class* ClassLinker::UpdateClass(const char* descriptor, mirror::Class* klass,
                                        size_t hash) {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  auto existing_it = class_table_.FindWithHash(std::make_pair(descriptor, klass->GetClassLoader()),
                                               hash);
  CHECK(existing_it != class_table_.end());
  mirror::Class* existing = existing_it->Read();
  CHECK_NE(existing, klass) << descriptor;
  CHECK(!existing->IsResolved()) << descriptor;
  CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusResolving) << descriptor;
  CHECK(!klass->IsTemp()) << descriptor;
  if (kIsDebugBuild && klass->GetClassLoader() == nullptr &&
      dex_cache_image_class_lookup_required_) {
    existing = LookupClassFromImage(descriptor);
    if (existing != nullptr) {
      CHECK_EQ(klass, existing) << descriptor;
    }
  }
  VerifyObject(klass);
  *existing_it = GcRoot<mirror::Class>(klass);
  if (log_new_class_table_roots_) {
    new_class_roots_.push_back(GcRoot<mirror::Class>(klass));
  }
  return existing;
}
bool ClassLinker::RemoveClass(const char* descriptor, mirror::ClassLoader* class_loader) {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  auto pair = std::make_pair(descriptor, class_loader);
  auto it = class_table_.Find(pair);
  if (it != class_table_.end()) {
    class_table_.Erase(it);
    return true;
  }
  it = pre_zygote_class_table_.Find(pair);
  if (it != pre_zygote_class_table_.end()) {
    pre_zygote_class_table_.Erase(it);
    return true;
  }
  return false;
}
mirror::Class* ClassLinker::LookupClass(Thread* self, const char* descriptor, size_t hash,
                                        mirror::ClassLoader* class_loader) {
  {
    ReaderMutexLock mu(self, *Locks::classlinker_classes_lock_);
    mirror::Class* result = LookupClassFromTableLocked(descriptor, class_loader, hash);
    if (result != nullptr) {
      return result;
    }
  }
  if (class_loader != nullptr || !dex_cache_image_class_lookup_required_) {
    return nullptr;
  } else {
    mirror::Class* result = LookupClassFromImage(descriptor);
    if (result != nullptr) {
      InsertClass(descriptor, result, hash);
    } else {
      constexpr uint32_t kMaxFailedDexCacheLookups = 1000;
      if (++failed_dex_cache_class_lookups_ > kMaxFailedDexCacheLookups) {
        MoveImageClassesToClassTable();
      }
    }
    return result;
  }
}
mirror::Class* ClassLinker::LookupClassFromTableLocked(const char* descriptor,
                                                       mirror::ClassLoader* class_loader,
                                                       size_t hash) {
  auto descriptor_pair = std::make_pair(descriptor, class_loader);
  auto it = pre_zygote_class_table_.FindWithHash(descriptor_pair, hash);
  if (it == pre_zygote_class_table_.end()) {
    it = class_table_.FindWithHash(descriptor_pair, hash);
    if (it == class_table_.end()) {
      return nullptr;
    }
  }
  return it->Read();
}
static mirror::ObjectArray<mirror::DexCache>* GetImageDexCaches()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    gc::space::ImageSpace* image = Runtime::Current()->GetHeap()->GetImageSpace();
    CHECK(image != nullptr);
    mirror::Object* root = image->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
    return root->AsObjectArray<mirror::DexCache>();
    }
void ClassLinker::MoveImageClassesToClassTable() {
  Thread* self = Thread::Current();
  WriterMutexLock mu(self, *Locks::classlinker_classes_lock_);
  if (!dex_cache_image_class_lookup_required_) {
    return;
  }
  ScopedAssertNoThreadSuspension ants(self, "Moving image classes to class table");
  mirror::ObjectArray<mirror::DexCache>* dex_caches = GetImageDexCaches();
  std::string temp;
  for (int32_t i = 0; i < dex_caches->GetLength(); i++) {
    mirror::DexCache* dex_cache = dex_caches->Get(i);
    mirror::ObjectArray<mirror::Class>* types = dex_cache->GetResolvedTypes();
    for (int32_t j = 0; j < types->GetLength(); j++) {
      mirror::Class* klass = types->Get(j);
      if (klass != nullptr) {
        DCHECK(klass->GetClassLoader() == nullptr);
        const char* descriptor = klass->GetDescriptor(&temp);
        size_t hash = ComputeModifiedUtf8Hash(descriptor);
        mirror::Class* existing = LookupClassFromTableLocked(descriptor, nullptr, hash);
        if (existing != nullptr) {
          CHECK_EQ(existing, klass) << PrettyClassAndClassLoader(existing) << " != "
              << PrettyClassAndClassLoader(klass);
        } else {
          class_table_.Insert(GcRoot<mirror::Class>(klass));
          if (log_new_class_table_roots_) {
            new_class_roots_.push_back(GcRoot<mirror::Class>(klass));
          }
        }
      }
    }
  }
  dex_cache_image_class_lookup_required_ = false;
}
void ClassLinker::MoveClassTableToPreZygote() {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  DCHECK(pre_zygote_class_table_.Empty());
  pre_zygote_class_table_ = std::move(class_table_);
  class_table_.Clear();
}
mirror::Class* ClassLinker::LookupClassFromImage(const char* descriptor) {
  ScopedAssertNoThreadSuspension ants(Thread::Current(), "Image class lookup");
  mirror::ObjectArray<mirror::DexCache>* dex_caches = GetImageDexCaches();
  for (int32_t i = 0; i < dex_caches->GetLength(); ++i) {
    mirror::DexCache* dex_cache = dex_caches->Get(i);
    const DexFile* dex_file = dex_cache->GetDexFile();
    const DexFile::StringId* string_id = dex_file->FindStringId(descriptor);
    if (string_id != nullptr) {
      const DexFile::TypeId* type_id =
          dex_file->FindTypeId(dex_file->GetIndexForStringId(*string_id));
      if (type_id != nullptr) {
        uint16_t type_idx = dex_file->GetIndexForTypeId(*type_id);
        mirror::Class* klass = dex_cache->GetResolvedType(type_idx);
        if (klass != nullptr) {
          return klass;
        }
      }
    }
  }
  return nullptr;
}
void ClassLinker::LookupClasses(const char* descriptor, std::vector<mirror::Class*>& result) {
  result.clear();
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  while (true) {
    auto it = class_table_.Find(descriptor);
    if (it == class_table_.end()) {
      break;
    }
    result.push_back(it->Read());
    class_table_.Erase(it);
  }
  for (mirror::Class* k : result) {
    class_table_.Insert(GcRoot<mirror::Class>(k));
  }
  size_t pre_zygote_start = result.size();
  while (true) {
    auto it = pre_zygote_class_table_.Find(descriptor);
    if (it == pre_zygote_class_table_.end()) {
      break;
    }
    result.push_back(it->Read());
    pre_zygote_class_table_.Erase(it);
  }
  for (size_t i = pre_zygote_start; i < result.size(); ++i) {
    pre_zygote_class_table_.Insert(GcRoot<mirror::Class>(result[i]));
  }
}
void ClassLinker::VerifyClass(Thread* self, Handle<mirror::Class> klass) {
  ObjectLock<mirror::Class> lock(self, klass);
  if (klass->IsVerified()) {
    EnsurePreverifiedMethods(klass);
    return;
  }
  if (klass->IsCompileTimeVerified() && Runtime::Current()->IsAotCompiler()) {
    return;
  }
  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass.Get());
    return;
  }
  if (klass->GetStatus() == mirror::Class::kStatusResolved) {
    mirror::Class::SetStatus(klass, mirror::Class::kStatusVerifying, self);
  } else {
    CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime)
        << PrettyClass(klass.Get());
    CHECK(!Runtime::Current()->IsAotCompiler());
    mirror::Class::SetStatus(klass, mirror::Class::kStatusVerifyingAtRuntime, self);
  }
  if (!Runtime::Current()->IsVerificationEnabled()) {
    mirror::Class::SetStatus(klass, mirror::Class::kStatusVerified, self);
    EnsurePreverifiedMethods(klass);
    return;
  }
  StackHandleScope<2> hs(self);
  Handle<mirror::Class> super(hs.NewHandle(klass->GetSuperClass()));
  if (super.Get() != nullptr) {
    ObjectLock<mirror::Class> super_lock(self, super);
    if (!super->IsVerified() && !super->IsErroneous()) {
      VerifyClass(self, super);
    }
    if (!super->IsCompileTimeVerified()) {
      std::string error_msg(
          StringPrintf("Rejecting class %s that attempts to sub-class erroneous class %s",
                       PrettyDescriptor(klass.Get()).c_str(),
                       PrettyDescriptor(super.Get()).c_str()));
      LOG(WARNING) << error_msg << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8();
      Handle<mirror::Throwable> cause(hs.NewHandle(self->GetException()));
      if (cause.Get() != nullptr) {
        self->ClearException();
      }
      ThrowVerifyError(klass.Get(), "%s", error_msg.c_str());
      if (cause.Get() != nullptr) {
        self->GetException()->SetCause(cause.Get());
      }
      ClassReference ref(klass->GetDexCache()->GetDexFile(), klass->GetDexClassDefIndex());
      if (Runtime::Current()->IsAotCompiler()) {
        Runtime::Current()->GetCompilerCallbacks()->ClassRejected(ref);
      }
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      return;
    }
  }
  const DexFile& dex_file = *klass->GetDexCache()->GetDexFile();
  mirror::Class::Status oat_file_class_status(mirror::Class::kStatusNotReady);
  bool preverified = VerifyClassUsingOatFile(dex_file, klass.Get(), oat_file_class_status);
  if (oat_file_class_status == mirror::Class::kStatusError) {
    VLOG(class_linker) << "Skipping runtime verification of erroneous class "
        << PrettyDescriptor(klass.Get()) << " in "
        << klass->GetDexCache()->GetLocation()->ToModifiedUtf8();
    ThrowVerifyError(klass.Get(), "Rejecting class %s because it failed compile-time verification",
                     PrettyDescriptor(klass.Get()).c_str());
    mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
    return;
  }
  verifier::MethodVerifier::FailureKind verifier_failure = verifier::MethodVerifier::kNoFailure;
  std::string error_msg;
  if (!preverified) {
    verifier_failure = verifier::MethodVerifier::VerifyClass(self, klass.Get(),
                                                             Runtime::Current()->IsAotCompiler(),
                                                             &error_msg);
  }
  if (preverified || verifier_failure != verifier::MethodVerifier::kHardFailure) {
    if (!preverified && verifier_failure != verifier::MethodVerifier::kNoFailure) {
      VLOG(class_linker) << "Soft verification failure in class " << PrettyDescriptor(klass.Get())
          << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8()
          << " because: " << error_msg;
    }
    self->AssertNoPendingException();
    ResolveClassExceptionHandlerTypes(dex_file, klass);
    if (verifier_failure == verifier::MethodVerifier::kNoFailure) {
      if (super.Get() == nullptr || super->IsVerified()) {
        mirror::Class::SetStatus(klass, mirror::Class::kStatusVerified, self);
      } else {
        CHECK_EQ(super->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime);
        mirror::Class::SetStatus(klass, mirror::Class::kStatusRetryVerificationAtRuntime, self);
        verifier_failure = verifier::MethodVerifier::kSoftFailure;
      }
    } else {
      CHECK_EQ(verifier_failure, verifier::MethodVerifier::kSoftFailure);
      if (Runtime::Current()->IsAotCompiler()) {
        mirror::Class::SetStatus(klass, mirror::Class::kStatusRetryVerificationAtRuntime, self);
      } else {
        mirror::Class::SetStatus(klass, mirror::Class::kStatusVerified, self);
        klass->SetPreverified();
      }
    }
  } else {
    LOG(WARNING) << "Verification failed on class " << PrettyDescriptor(klass.Get())
        << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8()
        << " because: " << error_msg;
    self->AssertNoPendingException();
    ThrowVerifyError(klass.Get(), "%s", error_msg.c_str());
    mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
  }
  if (preverified || verifier_failure == verifier::MethodVerifier::kNoFailure) {
    EnsurePreverifiedMethods(klass);
  }
}
void ClassLinker::EnsurePreverifiedMethods(Handle<mirror::Class> klass) {
  if (!klass->IsPreverified()) {
    klass->SetPreverifiedFlagOnAllMethods(image_pointer_size_);
    klass->SetPreverified();
  }
}
bool ClassLinker::VerifyClassUsingOatFile(const DexFile& dex_file, mirror::Class* klass,
                                          mirror::Class::Status& oat_file_class_status) {
  if (Runtime::Current()->IsAotCompiler()) {
    if (Runtime::Current()->GetCompilerCallbacks()->IsBootImage()) {
      return false;
    }
    if (klass->GetClassLoader() != nullptr) {
      return false;
    }
  }
  const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  if (oat_dex_file == nullptr) {
    return false;
  }
  if (!Runtime::Current()->IsAotCompiler() &&
      !Runtime::Current()->GetHeap()->HasImageSpace() &&
      klass->GetClassLoader() != nullptr) {
    return false;
  }
  uint16_t class_def_index = klass->GetDexClassDefIndex();
  oat_file_class_status = oat_dex_file->GetOatClass(class_def_index).GetStatus();
  if (oat_file_class_status == mirror::Class::kStatusVerified ||
      oat_file_class_status == mirror::Class::kStatusInitialized) {
      return true;
  }
  if (oat_file_class_status == mirror::Class::kStatusRetryVerificationAtRuntime) {
    return false;
  }
  if (oat_file_class_status == mirror::Class::kStatusError) {
    return false;
  }
  if (oat_file_class_status == mirror::Class::kStatusNotReady) {
    return false;
  }
  std::string temp;
  LOG(FATAL) << "Unexpected class status: " << oat_file_class_status
             << " " << dex_file.GetLocation() << " " << PrettyClass(klass) << " "
             << klass->GetDescriptor(&temp);
  UNREACHABLE();
}
void ClassLinker::ResolveClassExceptionHandlerTypes(const DexFile& dex_file,
                                                    Handle<mirror::Class> klass) {
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    ResolveMethodExceptionHandlerTypes(dex_file, klass->GetDirectMethod(i, image_pointer_size_));
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    ResolveMethodExceptionHandlerTypes(dex_file, klass->GetVirtualMethod(i, image_pointer_size_));
  }
}
void ClassLinker::ResolveMethodExceptionHandlerTypes(const DexFile& dex_file,
                                                     ArtMethod* method) {
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());
  if (code_item == nullptr) {
    return;
  }
  if (code_item->tries_size_ == 0) {
    return;
  }
  const uint8_t* handlers_ptr = DexFile::GetCatchHandlerData(*code_item, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      if (iterator.GetHandlerTypeIndex() != DexFile::kDexNoIndex16) {
        mirror::Class* exception_type = linker->ResolveType(iterator.GetHandlerTypeIndex(), method);
        if (exception_type == nullptr) {
          DCHECK(Thread::Current()->IsExceptionPending());
          Thread::Current()->ClearException();
        }
      }
    }
    handlers_ptr = iterator.EndDataPointer();
  }
}
mirror::Class* ClassLinker::CreateProxyClass(ScopedObjectAccessAlreadyRunnable& soa, jstring name,
                                             jobjectArray interfaces, jobject loader,
                                             jobjectArray methods, jobjectArray throws) {
  Thread* self = soa.Self();
  StackHandleScope<10> hs(self);
  MutableHandle<mirror::Class> klass(hs.NewHandle(
      AllocClass(self, GetClassRoot(kJavaLangClass), sizeof(mirror::Class))));
  if (klass.Get() == nullptr) {
    CHECK(self->IsExceptionPending());
    return nullptr;
  }
  DCHECK(klass->GetClass() != nullptr);
  klass->SetObjectSize(sizeof(mirror::Proxy));
  klass->SetAccessFlags(kAccClassIsProxy | kAccPublic | kAccFinal | kAccPreverified);
  klass->SetClassLoader(soa.Decode<mirror::ClassLoader*>(loader));
  DCHECK_EQ(klass->GetPrimitiveType(), Primitive::kPrimNot);
  klass->SetName(soa.Decode<mirror::String*>(name));
  klass->SetDexCache(GetClassRoot(kJavaLangReflectProxy)->GetDexCache());
  mirror::Class::SetStatus(klass, mirror::Class::kStatusIdx, self);
  std::string descriptor(GetDescriptorForProxy(klass.Get()));
  size_t hash = ComputeModifiedUtf8Hash(descriptor.c_str());
  mirror::Class* existing = InsertClass(descriptor.c_str(), klass.Get(), hash);
  CHECK(existing == nullptr);
  const size_t num_fields = 2;
  ArtField* sfields = AllocArtFieldArray(self, num_fields);
  klass->SetSFields(sfields);
  klass->SetNumStaticFields(num_fields);
  ArtField* interfaces_sfield = &sfields[0];
  interfaces_sfield->SetDexFieldIndex(0);
  interfaces_sfield->SetDeclaringClass(klass.Get());
  interfaces_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);
  ArtField* throws_sfield = &sfields[1];
  throws_sfield->SetDexFieldIndex(1);
  throws_sfield->SetDeclaringClass(klass.Get());
  throws_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);
  auto* directs = AllocArtMethodArray(self, 1);
  if (UNLIKELY(directs == nullptr)) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  klass->SetDirectMethodsPtr(directs);
  klass->SetNumDirectMethods(1u);
  CreateProxyConstructor(klass, klass->GetDirectMethodUnchecked(0, image_pointer_size_));
  auto h_methods = hs.NewHandle(soa.Decode<mirror::ObjectArray<mirror::Method>*>(methods));
  DCHECK_EQ(h_methods->GetClass(), mirror::Method::ArrayClass())
    << PrettyClass(h_methods->GetClass());
  const size_t num_virtual_methods = h_methods->GetLength();
  auto* virtuals = AllocArtMethodArray(self, num_virtual_methods);
  if (UNLIKELY(virtuals == nullptr)) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  klass->SetVirtualMethodsPtr(virtuals);
  klass->SetNumVirtualMethods(num_virtual_methods);
  for (size_t i = 0; i < num_virtual_methods; ++i) {
    auto* virtual_method = klass->GetVirtualMethodUnchecked(i, image_pointer_size_);
    auto* prototype = h_methods->Get(i)->GetArtMethod();
    CreateProxyMethod(klass, prototype, virtual_method);
    DCHECK(virtual_method->GetDeclaringClass() != nullptr);
    DCHECK(prototype->GetDeclaringClass() != nullptr);
  }
  klass->SetSuperClass(GetClassRoot(kJavaLangReflectProxy));
  mirror::Class::SetStatus(klass, mirror::Class::kStatusLoaded, self);
  self->AssertNoPendingException();
  MutableHandle<mirror::Class> new_class = hs.NewHandle<mirror::Class>(nullptr);
  {
    ObjectLock<mirror::Class> resolution_lock(self, klass);
    Handle<mirror::ObjectArray<mirror::Class>> h_interfaces(
        hs.NewHandle(soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces)));
    if (!LinkClass(self, descriptor.c_str(), klass, h_interfaces, &new_class)) {
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      return nullptr;
    }
  }
  CHECK(klass->IsRetired());
  CHECK_NE(klass.Get(), new_class.Get());
  klass.Assign(new_class.Get());
  CHECK_EQ(interfaces_sfield->GetDeclaringClass(), klass.Get());
  interfaces_sfield->SetObject<false>(klass.Get(),
                                      soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces));
  CHECK_EQ(throws_sfield->GetDeclaringClass(), klass.Get());
  throws_sfield->SetObject<false>(klass.Get(),
      soa.Decode<mirror::ObjectArray<mirror::ObjectArray<mirror::Class> >*>(throws));
  {
    ObjectLock<mirror::Class> initialization_lock(self, klass);
    mirror::Class::SetStatus(klass, mirror::Class::kStatusInitialized, self);
  }
  if (kIsDebugBuild) {
    CHECK(klass->GetIFields() == nullptr);
    CheckProxyConstructor(klass->GetDirectMethod(0, image_pointer_size_));
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      auto* virtual_method = klass->GetVirtualMethodUnchecked(i, image_pointer_size_);
      auto* prototype = h_methods->Get(i++)->GetArtMethod();
      CheckProxyMethod(virtual_method, prototype);
    }
    StackHandleScope<1> hs2(self);
    Handle<mirror::String> decoded_name = hs2.NewHandle(soa.Decode<mirror::String*>(name));
    std::string interfaces_field_name(StringPrintf("java.lang.Class[] %s.interfaces",
                                                   decoded_name->ToModifiedUtf8().c_str()));
    CHECK_EQ(PrettyField(klass->GetStaticField(0)), interfaces_field_name);
    std::string throws_field_name(StringPrintf("java.lang.Class[][] %s.throws",
                                               decoded_name->ToModifiedUtf8().c_str()));
    CHECK_EQ(PrettyField(klass->GetStaticField(1)), throws_field_name);
    CHECK_EQ(klass.Get()->GetInterfaces(),
             soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces));
    CHECK_EQ(klass.Get()->GetThrows(),
             soa.Decode<mirror::ObjectArray<mirror::ObjectArray<mirror::Class>>*>(throws));
  }
  return klass.Get();
}
std::string ClassLinker::GetDescriptorForProxy(mirror::Class* proxy_class) {
  DCHECK(proxy_class->IsProxyClass());
  mirror::String* name = proxy_class->GetName();
  DCHECK(name != nullptr);
  return DotToDescriptor(name->ToModifiedUtf8().c_str());
}
ArtMethod* ClassLinker::FindMethodForProxy(mirror::Class* proxy_class,
                                                   ArtMethod* proxy_method) {
  DCHECK(proxy_class->IsProxyClass());
  DCHECK(proxy_method->IsProxyMethod());
  {
    ReaderMutexLock mu(Thread::Current(), dex_lock_);
    for (const GcRoot<mirror::DexCache>& root : dex_caches_) {
      auto* dex_cache = root.Read();
      if (proxy_method->HasSameDexCacheResolvedTypes(dex_cache->GetResolvedTypes())) {
        ArtMethod* resolved_method = dex_cache->GetResolvedMethod(
            proxy_method->GetDexMethodIndex(), image_pointer_size_);
        CHECK(resolved_method != nullptr);
        return resolved_method;
      }
    }
  }
  LOG(FATAL) << "Didn't find dex cache for " << PrettyClass(proxy_class) << " "
      << PrettyMethod(proxy_method);
  UNREACHABLE();
}
bool ClassLinker::InitializeClass(Thread* self, Handle<mirror::Class> klass,
                                  bool can_init_statics, bool can_init_parents) {
  if (klass->IsInitialized()) {
    return true;
  }
  if (!CanWeInitializeClass(klass.Get(), can_init_statics, can_init_parents)) {
    return false;
  }
  self->AllowThreadSuspension();
  uint64_t t0;
  {
    ObjectLock<mirror::Class> lock(self, klass);
    if (klass->IsInitialized()) {
      return true;
    }
    if (klass->IsErroneous()) {
      ThrowEarlierClassFailure(klass.Get());
      VlogClassInitializationFailure(klass);
      return false;
    }
    CHECK(klass->IsResolved()) << PrettyClass(klass.Get()) << ": state=" << klass->GetStatus();
    if (!klass->IsVerified()) {
      VerifyClass(self, klass);
      if (!klass->IsVerified()) {
        if (klass->IsErroneous()) {
          CHECK(self->IsExceptionPending());
          VlogClassInitializationFailure(klass);
        } else {
          CHECK(Runtime::Current()->IsAotCompiler());
          CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime);
        }
        return false;
      } else {
        self->AssertNoPendingException();
      }
    }
    if (klass->GetStatus() == mirror::Class::kStatusInitializing) {
      if (self->IsExceptionPending()) {
        VlogClassInitializationFailure(klass);
        return false;
      }
      if (klass->GetClinitThreadId() == self->GetTid()) {
        return true;
      }
      return WaitForInitializeClass(klass, self, lock);
    }
    if (!ValidateSuperClassDescriptors(klass)) {
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      return false;
    }
    self->AllowThreadSuspension();
    CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusVerified) << PrettyClass(klass.Get());
    klass->SetClinitThreadId(self->GetTid());
    mirror::Class::SetStatus(klass, mirror::Class::kStatusInitializing, self);
    t0 = NanoTime();
  }
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    mirror::Class* super_class = klass->GetSuperClass();
    if (!super_class->IsInitialized()) {
      CHECK(!super_class->IsInterface());
      CHECK(can_init_parents);
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> handle_scope_super(hs.NewHandle(super_class));
      bool super_initialized = InitializeClass(self, handle_scope_super, can_init_statics, true);
      if (!super_initialized) {
        CHECK(handle_scope_super->IsErroneous() && self->IsExceptionPending())
            << "Super class initialization failed for "
            << PrettyDescriptor(handle_scope_super.Get())
            << " that has unexpected status " << handle_scope_super->GetStatus()
            << "\nPending exception:\n"
            << (self->GetException() != nullptr ? self->GetException()->Dump() : "");
        ObjectLock<mirror::Class> lock(self, klass);
        mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
        return false;
      }
    }
  }
  const size_t num_static_fields = klass->NumStaticFields();
  if (num_static_fields > 0) {
    const DexFile::ClassDef* dex_class_def = klass->GetClassDef();
    CHECK(dex_class_def != nullptr);
    const DexFile& dex_file = klass->GetDexFile();
    StackHandleScope<3> hs(self);
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(klass->GetClassLoader()));
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(klass->GetDexCache()));
    for (size_t i = 0; i < num_static_fields; ++i) {
      ArtField* field = klass->GetStaticField(i);
      const uint32_t field_idx = field->GetDexFieldIndex();
      ArtField* resolved_field = dex_cache->GetResolvedField(field_idx, image_pointer_size_);
      if (resolved_field == nullptr) {
        dex_cache->SetResolvedField(field_idx, field, image_pointer_size_);
      } else {
        DCHECK_EQ(field, resolved_field);
      }
    }
    EncodedStaticFieldValueIterator value_it(dex_file, &dex_cache, &class_loader,
                                             this, *dex_class_def);
    const uint8_t* class_data = dex_file.GetClassData(*dex_class_def);
    ClassDataItemIterator field_it(dex_file, class_data);
    if (value_it.HasNext()) {
      DCHECK(field_it.HasNextStaticField());
      CHECK(can_init_statics);
      for ( ; value_it.HasNext(); value_it.Next(), field_it.Next()) {
        ArtField* field = ResolveField(
            dex_file, field_it.GetMemberIndex(), dex_cache, class_loader, true);
        if (Runtime::Current()->IsActiveTransaction()) {
          value_it.ReadValueToField<true>(field);
        } else {
          value_it.ReadValueToField<false>(field);
        }
        DCHECK(!value_it.HasNext() || field_it.HasNextStaticField());
      }
    }
  }
  ArtMethod* clinit = klass->FindClassInitializer(image_pointer_size_);
  if (clinit != nullptr) {
    CHECK(can_init_statics);
    JValue result;
    clinit->Invoke(self, nullptr, 0, &result, "V");
  }
  self->AllowThreadSuspension();
  uint64_t t1 = NanoTime();
  bool success = true;
  {
    ObjectLock<mirror::Class> lock(self, klass);
    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer(klass);
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      success = false;
    } else if (Runtime::Current()->IsTransactionAborted()) {
      VLOG(compiler) << "Return from class initializer of " << PrettyDescriptor(klass.Get())
                     << " without exception while transaction was aborted: re-throw it now.";
      Runtime::Current()->ThrowTransactionAbortError(self);
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      success = false;
    } else {
      RuntimeStats* global_stats = Runtime::Current()->GetStats();
      RuntimeStats* thread_stats = self->GetStats();
      ++global_stats->class_init_count;
      ++thread_stats->class_init_count;
      global_stats->class_init_time_ns += (t1 - t0);
      thread_stats->class_init_time_ns += (t1 - t0);
      mirror::Class::SetStatus(klass, mirror::Class::kStatusInitialized, self);
      if (VLOG_IS_ON(class_linker)) {
        std::string temp;
        LOG(INFO) << "Initialized class " << klass->GetDescriptor(&temp) << " from " <<
            klass->GetLocation();
      }
      FixupStaticTrampolines(klass.Get());
    }
  }
  return success;
}
bool ClassLinker::WaitForInitializeClass(Handle<mirror::Class> klass, Thread* self,
                                         ObjectLock<mirror::Class>& lock)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    while (true) {
    self->AssertNoPendingException();
    CHECK(!klass->IsInitialized());
    lock.WaitIgnoringInterrupts();
    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer(klass);
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      return false;
    }
    if (klass->GetStatus() == mirror::Class::kStatusInitializing) {
      continue;
    }
    if (klass->GetStatus() == mirror::Class::kStatusVerified &&
        Runtime::Current()->IsAotCompiler()) {
      return false;
    }
    if (klass->IsErroneous()) {
      ThrowNoClassDefFoundError("<clinit> failed for class %s; see exception in other thread",
                                PrettyDescriptor(klass.Get()).c_str());
      VlogClassInitializationFailure(klass);
      return false;
    }
    if (klass->IsInitialized()) {
      return true;
    }
    LOG(FATAL) << "Unexpected class status. " << PrettyClass(klass.Get()) << " is "
        << klass->GetStatus();
    }
    UNREACHABLE();
    }
static void ThrowSignatureCheckResolveReturnTypeException(Handle<mirror::Class> klass,
                                                          Handle<mirror::Class> super_klass,
                                                          ArtMethod* method,
                                                          ArtMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    DCHECK(Thread::Current()->IsExceptionPending());
    DCHECK(!m->IsProxyMethod());
    const DexFile* dex_file = m->GetDexFile();
    const DexFile::MethodId& method_id = dex_file->GetMethodId(m->GetDexMethodIndex());
    const DexFile::ProtoId& proto_id = dex_file->GetMethodPrototype(method_id);
    uint16_t return_type_idx = proto_id.return_type_idx_;
    std::string return_type = PrettyType(return_type_idx, *dex_file);
    std::string class_loader = PrettyTypeOf(m->GetDeclaringClass()->GetClassLoader());
    ThrowWrappedLinkageError(klass.Get(),
                           "While checking class %s method %s signature against %s %s: "
                           "Failed to resolve return type %s with %s",
                           PrettyDescriptor(klass.Get()).c_str(),
                           PrettyMethod(method).c_str(),
                           super_klass->IsInterface() ? "interface" : "superclass",
                           PrettyDescriptor(super_klass.Get()).c_str(),
                           return_type.c_str(), class_loader.c_str());
    }
static void ThrowSignatureCheckResolveArgException(Handle<mirror::Class> klass,
                                                   Handle<mirror::Class> super_klass,
                                                   ArtMethod* method,
                                                   ArtMethod* m,
                                                   uint32_t index, uint32_t arg_type_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    DCHECK(Thread::Current()->IsExceptionPending());
    DCHECK(!m->IsProxyMethod());
    const DexFile* dex_file = m->GetDexFile();
    std::string arg_type = PrettyType(arg_type_idx, *dex_file);
    std::string class_loader = PrettyTypeOf(m->GetDeclaringClass()->GetClassLoader());
    ThrowWrappedLinkageError(klass.Get(),
                           "While checking class %s method %s signature against %s %s: "
                           "Failed to resolve arg %u type %s with %s",
                           PrettyDescriptor(klass.Get()).c_str(),
                           PrettyMethod(method).c_str(),
                           super_klass->IsInterface() ? "interface" : "superclass",
                           PrettyDescriptor(super_klass.Get()).c_str(),
                           index, arg_type.c_str(), class_loader.c_str());
    }
static void ThrowSignatureMismatch(Handle<mirror::Class> klass,
                                   Handle<mirror::Class> super_klass,
                                   ArtMethod* method,
                                   const std::string& error_msg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    ThrowLinkageError(klass.Get(),
                    "Class %s method %s resolves differently in %s %s: %s",
                    PrettyDescriptor(klass.Get()).c_str(),
                    PrettyMethod(method).c_str(),
                    super_klass->IsInterface() ? "interface" : "superclass",
                    PrettyDescriptor(super_klass.Get()).c_str(),
                    error_msg.c_str());
    }
static bool HasSameSignatureWithDifferentClassLoaders(Thread* self,
                                                      Handle<mirror::Class> klass,
                                                      Handle<mirror::Class> super_klass,
                                                      ArtMethod* method1,
                                                      ArtMethod* method2)
static void ThrowSignatureCheckResolveReturnTypeException(Handle<mirror::Class> klass,
                                                          Handle<mirror::Class> super_klass,
                                                          Handle<mirror::ArtMethod> method,
                                                          mirror::ArtMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    DCHECK(Thread::Current()->IsExceptionPending());
    DCHECK(!m->IsProxyMethod());
    const DexFile* dex_file = m->GetDexFile();
    const DexFile::MethodId& method_id = dex_file->GetMethodId(m->GetDexMethodIndex());
    const DexFile::ProtoId& proto_id = dex_file->GetMethodPrototype(method_id);
    uint16_t return_type_idx = proto_id.return_type_idx_;
    std::string return_type = PrettyType(return_type_idx, *dex_file);
    std::string class_loader = PrettyTypeOf(m->GetDeclaringClass()->GetClassLoader());
    ThrowWrappedLinkageError(klass.Get(),
                           "While checking class %s method %s signature against %s %s: "
                           "Failed to resolve return type %s with %s",
                           PrettyDescriptor(klass.Get()).c_str(),
                           PrettyMethod(method.Get()).c_str(),
                           super_klass->IsInterface() ? "interface" : "superclass",
                           PrettyDescriptor(super_klass.Get()).c_str(),
                           return_type.c_str(), class_loader.c_str());
    }
static void ThrowSignatureCheckResolveArgException(Handle<mirror::Class> klass,
                                                   Handle<mirror::Class> super_klass,
                                                   Handle<mirror::ArtMethod> method,
                                                   mirror::ArtMethod* m,
                                                   uint32_t index, uint32_t arg_type_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    DCHECK(Thread::Current()->IsExceptionPending());
    DCHECK(!m->IsProxyMethod());
    const DexFile* dex_file = m->GetDexFile();
    std::string arg_type = PrettyType(arg_type_idx, *dex_file);
    std::string class_loader = PrettyTypeOf(m->GetDeclaringClass()->GetClassLoader());
    ThrowWrappedLinkageError(klass.Get(),
                           "While checking class %s method %s signature against %s %s: "
                           "Failed to resolve arg %u type %s with %s",
                           PrettyDescriptor(klass.Get()).c_str(),
                           PrettyMethod(method.Get()).c_str(),
                           super_klass->IsInterface() ? "interface" : "superclass",
                           PrettyDescriptor(super_klass.Get()).c_str(),
                           index, arg_type.c_str(), class_loader.c_str());
    }
static void ThrowSignatureMismatch(Handle<mirror::Class> klass,
                                   Handle<mirror::Class> super_klass,
                                   Handle<mirror::ArtMethod> method,
                                   const std::string& error_msg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    ThrowLinkageError(klass.Get(),
                    "Class %s method %s resolves differently in %s %s: %s",
                    PrettyDescriptor(klass.Get()).c_str(),
                    PrettyMethod(method.Get()).c_str(),
                    super_klass->IsInterface() ? "interface" : "superclass",
                    PrettyDescriptor(super_klass.Get()).c_str(),
                    error_msg.c_str());
    }
static bool HasSameSignatureWithDifferentClassLoaders(Thread* self,
                                                      Handle<mirror::Class> klass,
                                                      Handle<mirror::Class> super_klass,
                                                      Handle<mirror::ArtMethod> method1,
                                                      Handle<mirror::ArtMethod> method2)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> return_type(hs.NewHandle(method1->GetReturnType()));
<<<<<<< HEAD
    if (UNLIKELY(return_type.Get() == nullptr)) {
      ThrowSignatureCheckResolveReturnTypeException(klass, super_klass, method1, method1);
      return false;
    }
||||||| c9d450554e
=======
    if (UNLIKELY(return_type.Get() == nullptr)) {
      ThrowSignatureCheckResolveReturnTypeException(klass, super_klass, method1, method1.Get());
      return false;
    }
>>>>>>> a2ea7405
    mirror::Class* other_return_type = method2->GetReturnType();
<<<<<<< HEAD
    if (UNLIKELY(other_return_type == nullptr)) {
      ThrowSignatureCheckResolveReturnTypeException(klass, super_klass, method1, method2);
      return false;
    }
||||||| c9d450554e
=======
    if (UNLIKELY(other_return_type == nullptr)) {
      ThrowSignatureCheckResolveReturnTypeException(klass, super_klass, method1, method2.Get());
      return false;
    }
>>>>>>> a2ea7405
    if (UNLIKELY(other_return_type != return_type.Get())) {
      ThrowSignatureMismatch(klass, super_klass, method1,
                             StringPrintf("Return types mismatch: %s(%p) vs %s(%p)",
                                          PrettyClassAndClassLoader(return_type.Get()).c_str(),
                                          return_type.Get(),
                                          PrettyClassAndClassLoader(other_return_type).c_str(),
                                          other_return_type));
      return false;
    }
    }
    const DexFile::TypeList* types1 = method1->GetParameterTypeList();
    const DexFile::TypeList* types2 = method2->GetParameterTypeList();
    if (types1 == nullptr) {
    if (types2 != nullptr && types2->Size() != 0) {
<<<<<<< HEAD
      ThrowSignatureMismatch(klass, super_klass, method1,
                             StringPrintf("Type list mismatch with %s",
                                          PrettyMethod(method2, true).c_str()));
||||||| c9d450554e
      *error_msg = StringPrintf("Type list mismatch with %s",
                                PrettyMethod(method2.Get(), true).c_str());
=======
      ThrowSignatureMismatch(klass, super_klass, method1,
                             StringPrintf("Type list mismatch with %s",
                                          PrettyMethod(method2.Get(), true).c_str()));
>>>>>>> a2ea7405
      return false;
    }
    return true;
    } else if (UNLIKELY(types2 == nullptr)) {
    if (types1->Size() != 0) {
<<<<<<< HEAD
      ThrowSignatureMismatch(klass, super_klass, method1,
                             StringPrintf("Type list mismatch with %s",
                                          PrettyMethod(method2, true).c_str()));
||||||| c9d450554e
      *error_msg = StringPrintf("Type list mismatch with %s",
                                PrettyMethod(method2.Get(), true).c_str());
=======
      ThrowSignatureMismatch(klass, super_klass, method1,
                             StringPrintf("Type list mismatch with %s",
                                          PrettyMethod(method2.Get(), true).c_str()));
>>>>>>> a2ea7405
      return false;
    }
    return true;
    }
    uint32_t num_types = types1->Size();
    if (UNLIKELY(num_types != types2->Size())) {
<<<<<<< HEAD
    ThrowSignatureMismatch(klass, super_klass, method1,
                           StringPrintf("Type list mismatch with %s",
                                        PrettyMethod(method2, true).c_str()));
||||||| c9d450554e
    *error_msg = StringPrintf("Type list mismatch with %s",
                              PrettyMethod(method2.Get(), true).c_str());
=======
    ThrowSignatureMismatch(klass, super_klass, method1,
                           StringPrintf("Type list mismatch with %s",
                                        PrettyMethod(method2.Get(), true).c_str()));
>>>>>>> a2ea7405
    return false;
    }
    for (uint32_t i = 0; i < num_types; ++i) {
    StackHandleScope<1> hs(self);
    uint32_t param_type_idx = types1->GetTypeItem(i).type_idx_;
    Handle<mirror::Class> param_type(hs.NewHandle(
<<<<<<< HEAD
        method1->GetClassFromTypeIndex(param_type_idx, true)));
    if (UNLIKELY(param_type.Get() == nullptr)) {
      ThrowSignatureCheckResolveArgException(klass, super_klass, method1,
                                             method1, i, param_type_idx);
      return false;
    }
    uint32_t other_param_type_idx = types2->GetTypeItem(i).type_idx_;
||||||| c9d450554e
        method1->GetClassFromTypeIndex(types1->GetTypeItem(i).type_idx_, true)));
=======
        method1->GetClassFromTypeIndex(param_type_idx, true)));
    if (UNLIKELY(param_type.Get() == nullptr)) {
      ThrowSignatureCheckResolveArgException(klass, super_klass, method1,
                                             method1.Get(), i, param_type_idx);
      return false;
    }
    uint32_t other_param_type_idx = types2->GetTypeItem(i).type_idx_;
>>>>>>> a2ea7405
    mirror::Class* other_param_type =
<<<<<<< HEAD
        method2->GetClassFromTypeIndex(other_param_type_idx, true);
    if (UNLIKELY(other_param_type == nullptr)) {
      ThrowSignatureCheckResolveArgException(klass, super_klass, method1,
                                             method2, i, other_param_type_idx);
      return false;
    }
||||||| c9d450554e
        method2->GetClassFromTypeIndex(types2->GetTypeItem(i).type_idx_, true);
=======
        method2->GetClassFromTypeIndex(other_param_type_idx, true);
    if (UNLIKELY(other_param_type == nullptr)) {
      ThrowSignatureCheckResolveArgException(klass, super_klass, method1,
                                             method2.Get(), i, other_param_type_idx);
      return false;
    }
>>>>>>> a2ea7405
    if (UNLIKELY(param_type.Get() != other_param_type)) {
      ThrowSignatureMismatch(klass, super_klass, method1,
                             StringPrintf("Parameter %u type mismatch: %s(%p) vs %s(%p)",
                                          i,
                                          PrettyClassAndClassLoader(param_type.Get()).c_str(),
                                          param_type.Get(),
                                          PrettyClassAndClassLoader(other_param_type).c_str(),
                                          other_param_type));
      return false;
    }
    }
    return true;
    }
bool ClassLinker::ValidateSuperClassDescriptors(Handle<mirror::Class> klass) {
  if (klass->IsInterface()) {
    return true;
  }
  Thread* self = Thread::Current();
<<<<<<< HEAD
  StackHandleScope<1> hs(self);
  MutableHandle<mirror::Class> super_klass(hs.NewHandle<mirror::Class>(nullptr));
||||||| c9d450554e
  StackHandleScope<2> hs(self);
  MutableHandle<mirror::ArtMethod> h_m(hs.NewHandle<mirror::ArtMethod>(nullptr));
  MutableHandle<mirror::ArtMethod> super_h_m(hs.NewHandle<mirror::ArtMethod>(nullptr));
=======
  StackHandleScope<3> hs(self);
  MutableHandle<mirror::Class> super_klass(hs.NewHandle<mirror::Class>(nullptr));
  MutableHandle<mirror::ArtMethod> h_m(hs.NewHandle<mirror::ArtMethod>(nullptr));
  MutableHandle<mirror::ArtMethod> super_h_m(hs.NewHandle<mirror::ArtMethod>(nullptr));
>>>>>>> a2ea7405
  if (klass->HasSuperClass() &&
      klass->GetClassLoader() != klass->GetSuperClass()->GetClassLoader()) {
    super_klass.Assign(klass->GetSuperClass());
    for (int i = klass->GetSuperClass()->GetVTableLength() - 1; i >= 0; --i) {
<<<<<<< HEAD
      auto* m = klass->GetVTableEntry(i, image_pointer_size_);
      auto* super_m = klass->GetSuperClass()->GetVTableEntry(i, image_pointer_size_);
      if (m != super_m) {
        if (UNLIKELY(!HasSameSignatureWithDifferentClassLoaders(self, klass, super_klass,
                                                                m, super_m))) {
          self->AssertPendingException();
||||||| c9d450554e
      h_m.Assign(klass->GetVTableEntry(i));
      super_h_m.Assign(klass->GetSuperClass()->GetVTableEntry(i));
      if (h_m.Get() != super_h_m.Get()) {
        std::string error_msg;
        if (!HasSameSignatureWithDifferentClassLoaders(self, h_m, super_h_m, &error_msg)) {
          ThrowLinkageError(klass.Get(),
                            "Class %s method %s resolves differently in superclass %s: %s",
                            PrettyDescriptor(klass.Get()).c_str(),
                            PrettyMethod(h_m.Get()).c_str(),
                            PrettyDescriptor(klass->GetSuperClass()).c_str(),
                            error_msg.c_str());
=======
      h_m.Assign(klass->GetVTableEntry(i));
      super_h_m.Assign(klass->GetSuperClass()->GetVTableEntry(i));
      if (h_m.Get() != super_h_m.Get()) {
        if (UNLIKELY(!HasSameSignatureWithDifferentClassLoaders(self, klass, super_klass,
                                                                h_m, super_h_m))) {
          self->AssertPendingException();
>>>>>>> a2ea7405
          return false;
        }
      }
    }
  }
  for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
    super_klass.Assign(klass->GetIfTable()->GetInterface(i));
    if (klass->GetClassLoader() != super_klass->GetClassLoader()) {
      uint32_t num_methods = super_klass->NumVirtualMethods();
      for (uint32_t j = 0; j < num_methods; ++j) {
<<<<<<< HEAD
        auto* m = klass->GetIfTable()->GetMethodArray(i)->GetElementPtrSize<ArtMethod*>(
            j, image_pointer_size_);
        auto* super_m = super_klass->GetVirtualMethod(j, image_pointer_size_);
        if (m != super_m) {
          if (UNLIKELY(!HasSameSignatureWithDifferentClassLoaders(self, klass, super_klass,
                                                                  m, super_m))) {
            self->AssertPendingException();
||||||| c9d450554e
        h_m.Assign(klass->GetIfTable()->GetMethodArray(i)->GetWithoutChecks(j));
        super_h_m.Assign(klass->GetIfTable()->GetInterface(i)->GetVirtualMethod(j));
        if (h_m.Get() != super_h_m.Get()) {
          std::string error_msg;
          if (!HasSameSignatureWithDifferentClassLoaders(self, h_m, super_h_m, &error_msg)) {
            ThrowLinkageError(klass.Get(),
                              "Class %s method %s resolves differently in interface %s: %s",
                              PrettyDescriptor(klass.Get()).c_str(),
                              PrettyMethod(h_m.Get()).c_str(),
                              PrettyDescriptor(klass->GetIfTable()->GetInterface(i)).c_str(),
                              error_msg.c_str());
=======
        h_m.Assign(klass->GetIfTable()->GetMethodArray(i)->GetWithoutChecks(j));
        super_h_m.Assign(super_klass->GetVirtualMethod(j));
        if (h_m.Get() != super_h_m.Get()) {
          if (UNLIKELY(!HasSameSignatureWithDifferentClassLoaders(self, klass, super_klass,
                                                                  h_m, super_h_m))) {
            self->AssertPendingException();
>>>>>>> a2ea7405
            return false;
          }
        }
      }
    }
  }
  return true;
}
bool ClassLinker::EnsureInitialized(Thread* self, Handle<mirror::Class> c, bool can_init_fields,
                                    bool can_init_parents) {
  DCHECK(c.Get() != nullptr);
  if (c->IsInitialized()) {
    EnsurePreverifiedMethods(c);
    return true;
  }
  const bool success = InitializeClass(self, c, can_init_fields, can_init_parents);
  if (!success) {
    if (can_init_fields && can_init_parents) {
      CHECK(self->IsExceptionPending()) << PrettyClass(c.Get());
    }
  } else {
    self->AssertNoPendingException();
  }
  return success;
}
void ClassLinker::FixupTemporaryDeclaringClass(mirror::Class* temp_class,
                                               mirror::Class* new_class) {
  ArtField* fields = new_class->GetIFields();
  DCHECK_EQ(temp_class->NumInstanceFields(), new_class->NumInstanceFields());
  for (size_t i = 0, count = new_class->NumInstanceFields(); i < count; i++) {
    if (fields[i].GetDeclaringClass() == temp_class) {
      fields[i].SetDeclaringClass(new_class);
    }
  }
  fields = new_class->GetSFields();
  DCHECK_EQ(temp_class->NumStaticFields(), new_class->NumStaticFields());
  for (size_t i = 0, count = new_class->NumStaticFields(); i < count; i++) {
    if (fields[i].GetDeclaringClass() == temp_class) {
      fields[i].SetDeclaringClass(new_class);
    }
  }
  DCHECK_EQ(temp_class->NumDirectMethods(), new_class->NumDirectMethods());
  for (auto& method : new_class->GetDirectMethods(image_pointer_size_)) {
    if (method.GetDeclaringClass() == temp_class) {
      method.SetDeclaringClass(new_class);
    }
  }
  DCHECK_EQ(temp_class->NumVirtualMethods(), new_class->NumVirtualMethods());
  for (auto& method : new_class->GetVirtualMethods(image_pointer_size_)) {
    if (method.GetDeclaringClass() == temp_class) {
      method.SetDeclaringClass(new_class);
    }
  }
}
bool ClassLinker::LinkClass(Thread* self, const char* descriptor, Handle<mirror::Class> klass,
                            Handle<mirror::ObjectArray<mirror::Class>> interfaces,
                            MutableHandle<mirror::Class>* h_new_class_out) {
  CHECK_EQ(mirror::Class::kStatusLoaded, klass->GetStatus());
  if (!LinkSuperClass(klass)) {
    return false;
  }
  ArtMethod* imt[mirror::Class::kImtSize];
  std::fill_n(imt, arraysize(imt), Runtime::Current()->GetImtUnimplementedMethod());
  if (!LinkMethods(self, klass, interfaces, imt)) {
    return false;
  }
  if (!LinkInstanceFields(self, klass)) {
    return false;
  }
  size_t class_size;
  if (!LinkStaticFields(self, klass, &class_size)) {
    return false;
  }
  CreateReferenceInstanceOffsets(klass);
  CHECK_EQ(mirror::Class::kStatusLoaded, klass->GetStatus());
  if (!klass->IsTemp() || (!init_done_ && klass->GetClassSize() == class_size)) {
    CHECK_EQ(klass->GetClassSize(), class_size) << PrettyDescriptor(klass.Get());
    if (klass->ShouldHaveEmbeddedImtAndVTable()) {
      klass->PopulateEmbeddedImtAndVTable(imt, image_pointer_size_);
    }
    mirror::Class::SetStatus(klass, mirror::Class::kStatusResolved, self);
    h_new_class_out->Assign(klass.Get());
  } else {
    CHECK(!klass->IsResolved());
    StackHandleScope<1> hs(self);
    auto h_new_class = hs.NewHandle(klass->CopyOf(self, class_size, imt, image_pointer_size_));
    if (UNLIKELY(h_new_class.Get() == nullptr)) {
      self->AssertPendingOOMException();
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      return false;
    }
    CHECK_EQ(h_new_class->GetClassSize(), class_size);
    ObjectLock<mirror::Class> lock(self, h_new_class);
    FixupTemporaryDeclaringClass(klass.Get(), h_new_class.Get());
    mirror::Class* existing = UpdateClass(descriptor, h_new_class.Get(),
                                          ComputeModifiedUtf8Hash(descriptor));
    CHECK(existing == nullptr || existing == klass.Get());
    mirror::Class::SetStatus(klass, mirror::Class::kStatusRetired, self);
    CHECK_EQ(h_new_class->GetStatus(), mirror::Class::kStatusResolving);
    mirror::Class::SetStatus(h_new_class, mirror::Class::kStatusResolved, self);
    h_new_class_out->Assign(h_new_class.Get());
  }
  return true;
}
static void CountMethodsAndFields(ClassDataItemIterator& dex_data,
                                  size_t* virtual_methods,
                                  size_t* direct_methods,
                                  size_t* static_fields,
                                  size_t* instance_fields) {
  *virtual_methods = *direct_methods = *static_fields = *instance_fields = 0;
  while (dex_data.HasNextStaticField()) {
    dex_data.Next();
    (*static_fields)++;
  }
  while (dex_data.HasNextInstanceField()) {
    dex_data.Next();
    (*instance_fields)++;
  }
  while (dex_data.HasNextDirectMethod()) {
    (*direct_methods)++;
    dex_data.Next();
  }
  while (dex_data.HasNextVirtualMethod()) {
    (*virtual_methods)++;
    dex_data.Next();
  }
  DCHECK(!dex_data.HasNext());
}
static void DumpClass(std::ostream& os,
                      const DexFile& dex_file, const DexFile::ClassDef& dex_class_def,
                      const char* suffix) {
  ClassDataItemIterator dex_data(dex_file, dex_file.GetClassData(dex_class_def));
  os << dex_file.GetClassDescriptor(dex_class_def) << suffix << ":\n";
  os << " Static fields:\n";
  while (dex_data.HasNextStaticField()) {
    const DexFile::FieldId& id = dex_file.GetFieldId(dex_data.GetMemberIndex());
    os << "  " << dex_file.GetFieldTypeDescriptor(id) << " " << dex_file.GetFieldName(id) << "\n";
    dex_data.Next();
  }
  os << " Instance fields:\n";
  while (dex_data.HasNextInstanceField()) {
    const DexFile::FieldId& id = dex_file.GetFieldId(dex_data.GetMemberIndex());
    os << "  " << dex_file.GetFieldTypeDescriptor(id) << " " << dex_file.GetFieldName(id) << "\n";
    dex_data.Next();
  }
  os << " Direct methods:\n";
  while (dex_data.HasNextDirectMethod()) {
    const DexFile::MethodId& id = dex_file.GetMethodId(dex_data.GetMemberIndex());
    os << "  " << dex_file.GetMethodName(id) << dex_file.GetMethodSignature(id).ToString() << "\n";
    dex_data.Next();
  }
  os << " Virtual methods:\n";
  while (dex_data.HasNextVirtualMethod()) {
    const DexFile::MethodId& id = dex_file.GetMethodId(dex_data.GetMemberIndex());
    os << "  " << dex_file.GetMethodName(id) << dex_file.GetMethodSignature(id).ToString() << "\n";
    dex_data.Next();
  }
}
static std::string DumpClasses(const DexFile& dex_file1, const DexFile::ClassDef& dex_class_def1,
                               const DexFile& dex_file2, const DexFile::ClassDef& dex_class_def2) {
  std::ostringstream os;
  DumpClass(os, dex_file1, dex_class_def1, " (Compile time)");
  DumpClass(os, dex_file2, dex_class_def2, " (Runtime)");
  return os.str();
}
static bool SimpleStructuralCheck(const DexFile& dex_file1, const DexFile::ClassDef& dex_class_def1,
                                  const DexFile& dex_file2, const DexFile::ClassDef& dex_class_def2,
                                  std::string* error_msg) {
  ClassDataItemIterator dex_data1(dex_file1, dex_file1.GetClassData(dex_class_def1));
  ClassDataItemIterator dex_data2(dex_file2, dex_file2.GetClassData(dex_class_def2));
  size_t dex_virtual_methods1, dex_direct_methods1, dex_static_fields1, dex_instance_fields1;
  CountMethodsAndFields(dex_data1, &dex_virtual_methods1, &dex_direct_methods1, &dex_static_fields1,
                        &dex_instance_fields1);
  size_t dex_virtual_methods2, dex_direct_methods2, dex_static_fields2, dex_instance_fields2;
  CountMethodsAndFields(dex_data2, &dex_virtual_methods2, &dex_direct_methods2, &dex_static_fields2,
                        &dex_instance_fields2);
  if (dex_virtual_methods1 != dex_virtual_methods2) {
    std::string class_dump = DumpClasses(dex_file1, dex_class_def1, dex_file2, dex_class_def2);
    *error_msg = StringPrintf("Virtual method count off: %zu vs %zu\n%s", dex_virtual_methods1,
                              dex_virtual_methods2, class_dump.c_str());
    return false;
  }
  if (dex_direct_methods1 != dex_direct_methods2) {
    std::string class_dump = DumpClasses(dex_file1, dex_class_def1, dex_file2, dex_class_def2);
    *error_msg = StringPrintf("Direct method count off: %zu vs %zu\n%s", dex_direct_methods1,
                              dex_direct_methods2, class_dump.c_str());
    return false;
  }
  if (dex_static_fields1 != dex_static_fields2) {
    std::string class_dump = DumpClasses(dex_file1, dex_class_def1, dex_file2, dex_class_def2);
    *error_msg = StringPrintf("Static field count off: %zu vs %zu\n%s", dex_static_fields1,
                              dex_static_fields2, class_dump.c_str());
    return false;
  }
  if (dex_instance_fields1 != dex_instance_fields2) {
    std::string class_dump = DumpClasses(dex_file1, dex_class_def1, dex_file2, dex_class_def2);
    *error_msg = StringPrintf("Instance field count off: %zu vs %zu\n%s", dex_instance_fields1,
                              dex_instance_fields2, class_dump.c_str());
    return false;
  }
  return true;
}
static bool CheckSuperClassChange(Handle<mirror::Class> klass,
                                  const DexFile& dex_file,
                                  const DexFile::ClassDef& class_def,
                                  mirror::Class* super_class)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (super_class->GetClassLoader() != nullptr &&
      klass->GetDexCache() != super_class->GetDexCache()) {
    const OatDexFile* class_oat_dex_file = dex_file.GetOatDexFile();
    const OatFile* class_oat_file = nullptr;
    if (class_oat_dex_file != nullptr) {
      class_oat_file = class_oat_dex_file->GetOatFile();
    }
    if (class_oat_file != nullptr) {
      const OatDexFile* loaded_super_oat_dex_file = super_class->GetDexFile().GetOatDexFile();
      const OatFile* loaded_super_oat_file = nullptr;
      if (loaded_super_oat_dex_file != nullptr) {
        loaded_super_oat_file = loaded_super_oat_dex_file->GetOatFile();
      }
      if (loaded_super_oat_file != nullptr && class_oat_file != loaded_super_oat_file) {
        const DexFile::ClassDef* super_class_def = dex_file.FindClassDef(class_def.superclass_idx_);
        if (super_class_def != nullptr) {
          std::string error_msg;
          if (!SimpleStructuralCheck(dex_file, *super_class_def,
                                     super_class->GetDexFile(), *super_class->GetClassDef(),
                                     &error_msg)) {
            LOG(WARNING) << "Incompatible structural change detected: " <<
                StringPrintf(
                    "Structural change of %s is hazardous (%s at compile time, %s at runtime): %s",
                    PrettyType(super_class_def->class_idx_, dex_file).c_str(),
                    class_oat_file->GetLocation().c_str(),
                    loaded_super_oat_file->GetLocation().c_str(),
                    error_msg.c_str());
            ThrowIncompatibleClassChangeError(klass.Get(),
                "Structural change of %s is hazardous (%s at compile time, %s at runtime): %s",
                PrettyType(super_class_def->class_idx_, dex_file).c_str(),
                class_oat_file->GetLocation().c_str(),
                loaded_super_oat_file->GetLocation().c_str(),
                error_msg.c_str());
            return false;
          }
        }
      }
    }
    }
    return true;
    }
bool ClassLinker::LoadSuperAndInterfaces(Handle<mirror::Class> klass, const DexFile& dex_file) {
  CHECK_EQ(mirror::Class::kStatusIdx, klass->GetStatus());
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(klass->GetDexClassDefIndex());
  uint16_t super_class_idx = class_def.superclass_idx_;
  if (super_class_idx != DexFile::kDexNoIndex16) {
    mirror::Class* super_class = ResolveType(dex_file, super_class_idx, klass.Get());
    if (super_class == nullptr) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return false;
    }
    if (!klass->CanAccess(super_class)) {
      ThrowIllegalAccessError(klass.Get(), "Class %s extended by class %s is inaccessible",
                              PrettyDescriptor(super_class).c_str(),
                              PrettyDescriptor(klass.Get()).c_str());
      return false;
    }
    CHECK(super_class->IsResolved());
    klass->SetSuperClass(super_class);
    if (!CheckSuperClassChange(klass, dex_file, class_def, super_class)) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return false;
    }
  }
  const DexFile::TypeList* interfaces = dex_file.GetInterfacesList(class_def);
  if (interfaces != nullptr) {
    for (size_t i = 0; i < interfaces->Size(); i++) {
      uint16_t idx = interfaces->GetTypeItem(i).type_idx_;
      mirror::Class* interface = ResolveType(dex_file, idx, klass.Get());
      if (interface == nullptr) {
        DCHECK(Thread::Current()->IsExceptionPending());
        return false;
      }
      if (!klass->CanAccess(interface)) {
        ThrowIllegalAccessError(klass.Get(), "Interface %s implemented by class %s is inaccessible",
                                PrettyDescriptor(interface).c_str(),
                                PrettyDescriptor(klass.Get()).c_str());
        return false;
      }
    }
  }
  mirror::Class::SetStatus(klass, mirror::Class::kStatusLoaded, nullptr);
  return true;
}
bool ClassLinker::LinkSuperClass(Handle<mirror::Class> klass) {
  CHECK(!klass->IsPrimitive());
  mirror::Class* super = klass->GetSuperClass();
  if (klass.Get() == GetClassRoot(kJavaLangObject)) {
    if (super != nullptr) {
      ThrowClassFormatError(klass.Get(), "java.lang.Object must not have a superclass");
      return false;
    }
    return true;
  }
  if (super == nullptr) {
    ThrowLinkageError(klass.Get(), "No superclass defined for class %s",
                      PrettyDescriptor(klass.Get()).c_str());
    return false;
  }
  if (super->IsFinal() || super->IsInterface()) {
    ThrowIncompatibleClassChangeError(klass.Get(), "Superclass %s of %s is %s",
                                      PrettyDescriptor(super).c_str(),
                                      PrettyDescriptor(klass.Get()).c_str(),
                                      super->IsFinal() ? "declared final" : "an interface");
    return false;
  }
  if (!klass->CanAccess(super)) {
    ThrowIllegalAccessError(klass.Get(), "Superclass %s is inaccessible to class %s",
                            PrettyDescriptor(super).c_str(),
                            PrettyDescriptor(klass.Get()).c_str());
    return false;
  }
  if (super->IsFinalizable()) {
    klass->SetFinalizable();
  }
  int reference_flags = (super->GetAccessFlags() & kAccReferenceFlagsMask);
  if (reference_flags != 0) {
    klass->SetAccessFlags(klass->GetAccessFlags() | reference_flags);
  }
  if (init_done_ && super == GetClassRoot(kJavaLangRefReference)) {
    ThrowLinkageError(klass.Get(),
                      "Class %s attempts to subclass java.lang.ref.Reference, which is not allowed",
                      PrettyDescriptor(klass.Get()).c_str());
    return false;
  }
  if (kIsDebugBuild) {
    while (super != nullptr) {
      CHECK(super->IsResolved());
      super = super->GetSuperClass();
    }
  }
  return true;
}
bool ClassLinker::LinkMethods(Thread* self, Handle<mirror::Class> klass,
                              Handle<mirror::ObjectArray<mirror::Class>> interfaces,
                              ArtMethod** out_imt) {
  self->AllowThreadSuspension();
  if (klass->IsInterface()) {
    size_t count = klass->NumVirtualMethods();
    if (!IsUint<16>(count)) {
      ThrowClassFormatError(klass.Get(), "Too many methods on interface: %zd", count);
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      klass->GetVirtualMethodDuringLinking(i, image_pointer_size_)->SetMethodIndex(i);
    }
  } else if (!LinkVirtualMethods(self, klass)) {
    return false;
  }
  return LinkInterfaceMethods(self, klass, interfaces, out_imt);
}
{
 public:
  explicit MethodNameAndSignatureComparator(ArtMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) :
      dex_file_(method->GetDexFile()), mid_(&dex_file_->GetMethodId(method->GetDexMethodIndex())),
      name_(nullptr), name_len_(0) {
    DCHECK(!method->IsProxyMethod()) << PrettyMethod(method);
  }
  const char* GetName() {
    if (name_ == nullptr) {
      name_ = dex_file_->StringDataAndUtf16LengthByIdx(mid_->name_idx_, &name_len_);
    }
    return name_;
  }
  bool HasSameNameAndSignature(ArtMethod* other)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(!other->IsProxyMethod()) << PrettyMethod(other);
    const DexFile* other_dex_file = other->GetDexFile();
    const DexFile::MethodId& other_mid = other_dex_file->GetMethodId(other->GetDexMethodIndex());
    if (dex_file_ == other_dex_file) {
      return mid_->name_idx_ == other_mid.name_idx_ && mid_->proto_idx_ == other_mid.proto_idx_;
    }
    GetName();
    uint32_t other_name_len;
    const char* other_name = other_dex_file->StringDataAndUtf16LengthByIdx(other_mid.name_idx_,
                                                                           &other_name_len);
    if (name_len_ != other_name_len || strcmp(name_, other_name) != 0) {
      return false;
    }
    return dex_file_->GetMethodSignature(*mid_) == other_dex_file->GetMethodSignature(other_mid);
  }
 private:
  const DexFile* const dex_file_;
  const DexFile::MethodId* const mid_;
  const char* name_;
  uint32_t name_len_;
}class LinkVirtualHashTable {
public:
  LinkVirtualHashTable(Handle<mirror::Class> klass, size_t hash_size, uint32_t* hash_table,
                       size_t image_pointer_size)
     : klass_(klass), hash_size_(hash_size), hash_table_(hash_table),
       image_pointer_size_(image_pointer_size) {
    std::fill(hash_table_, hash_table_ + hash_size_, invalid_index_);
  }
void Add(uint32_t virtual_method_index) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
                                          ArtMethod* local_method = klass_->GetVirtualMethodDuringLinking(
                                          virtual_method_index, image_pointer_size_);
                                          const char* name = local_method->GetInterfaceMethodIfProxy(image_pointer_size_)->GetName();
                                          uint32_t hash = ComputeModifiedUtf8Hash(name);
                                          uint32_t index = hash % hash_size_;
                                          while (hash_table_[index] != invalid_index_) {
                                          if (++index == hash_size_) {
                                          index = 0;
                                          }
                                          }
                                          hash_table_[index] = virtual_method_index;
                                          }
  uint32_t FindAndRemove(MethodNameAndSignatureComparator* comparator)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      const char* name = comparator->GetName();
      uint32_t hash = ComputeModifiedUtf8Hash(name);
      size_t index = hash % hash_size_;
      while (true) {
      const uint32_t value = hash_table_[index];
      if (value == invalid_index_) {
        break;
      }
      if (value != removed_index_) {
        ArtMethod* virtual_method =
            klass_->GetVirtualMethodDuringLinking(value, image_pointer_size_);
        if (comparator->HasSameNameAndSignature(
            virtual_method->GetInterfaceMethodIfProxy(image_pointer_size_))) {
          hash_table_[index] = removed_index_;
          return value;
        }
      }
      if (++index == hash_size_) {
        index = 0;
      }
      }
      return GetNotFoundIndex();
      }
  static uint32_t GetNotFoundIndex() {
    return invalid_index_;
  }
private:
  static const uint32_t invalid_index_;
  static const uint32_t removed_index_;
  Handle<mirror::Class> klass_;
  const size_t hash_size_;
  uint32_t* const hash_table_;
  const size_t image_pointer_size_;
};
const uint32_t LinkVirtualHashTable::removed_index_ = std::numeric_limits<uint32_t>::max() - 1;
const uint32_t LinkVirtualHashTable::removed_index_ = std::numeric_limits<uint32_t>::max() - 1;
bool ClassLinker::LinkVirtualMethods(Thread* self, Handle<mirror::Class> klass) {
  const size_t num_virtual_methods = klass->NumVirtualMethods();
  if (klass->HasSuperClass()) {
    const size_t super_vtable_length = klass->GetSuperClass()->GetVTableLength();
    const size_t max_count = num_virtual_methods + super_vtable_length;
    StackHandleScope<2> hs(self);
    Handle<mirror::Class> super_class(hs.NewHandle(klass->GetSuperClass()));
    MutableHandle<mirror::PointerArray> vtable;
    if (super_class->ShouldHaveEmbeddedImtAndVTable()) {
      vtable = hs.NewHandle(AllocPointerArray(self, max_count));
      if (UNLIKELY(vtable.Get() == nullptr)) {
        self->AssertPendingOOMException();
        return false;
      }
      for (size_t i = 0; i < super_vtable_length; i++) {
        vtable->SetElementPtrSize(
            i, super_class->GetEmbeddedVTableEntry(i, image_pointer_size_), image_pointer_size_);
      }
      if (num_virtual_methods == 0) {
        klass->SetVTable(vtable.Get());
        return true;
      }
    } else {
      auto* super_vtable = super_class->GetVTable();
      CHECK(super_vtable != nullptr) << PrettyClass(super_class.Get());
      if (num_virtual_methods == 0) {
        klass->SetVTable(super_vtable);
        return true;
      }
      vtable = hs.NewHandle(down_cast<mirror::PointerArray*>(
          super_vtable->CopyOf(self, max_count)));
      if (UNLIKELY(vtable.Get() == nullptr)) {
        self->AssertPendingOOMException();
        return false;
      }
    }
    static constexpr size_t kMaxStackHash = 250;
    const size_t hash_table_size = num_virtual_methods * 3;
    uint32_t* hash_table_ptr;
    std::unique_ptr<uint32_t[]> hash_heap_storage;
    if (hash_table_size <= kMaxStackHash) {
      hash_table_ptr = reinterpret_cast<uint32_t*>(
          alloca(hash_table_size * sizeof(*hash_table_ptr)));
    } else {
      hash_heap_storage.reset(new uint32_t[hash_table_size]);
      hash_table_ptr = hash_heap_storage.get();
    }
    LinkVirtualHashTable hash_table(klass, hash_table_size, hash_table_ptr, image_pointer_size_);
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      DCHECK(klass->GetVirtualMethodDuringLinking(
          i, image_pointer_size_)->GetDeclaringClass() != nullptr);
      hash_table.Add(i);
    }
    for (size_t j = 0; j < super_vtable_length; ++j) {
      ArtMethod* super_method = vtable->GetElementPtrSize<ArtMethod*>(j, image_pointer_size_);
      MethodNameAndSignatureComparator super_method_name_comparator(
          super_method->GetInterfaceMethodIfProxy(image_pointer_size_));
      uint32_t hash_index = hash_table.FindAndRemove(&super_method_name_comparator);
      if (hash_index != hash_table.GetNotFoundIndex()) {
        ArtMethod* virtual_method = klass->GetVirtualMethodDuringLinking(
            hash_index, image_pointer_size_);
        if (klass->CanAccessMember(super_method->GetDeclaringClass(),
                                   super_method->GetAccessFlags())) {
          if (super_method->IsFinal()) {
            ThrowLinkageError(klass.Get(), "Method %s overrides final method in class %s",
                              PrettyMethod(virtual_method).c_str(),
                              super_method->GetDeclaringClassDescriptor());
            return false;
          }
          vtable->SetElementPtrSize(j, virtual_method, image_pointer_size_);
          virtual_method->SetMethodIndex(j);
        } else {
          LOG(WARNING) << "Before Android 4.1, method " << PrettyMethod(virtual_method)
                       << " would have incorrectly overridden the package-private method in "
                       << PrettyDescriptor(super_method->GetDeclaringClassDescriptor());
        }
      }
    }
    size_t actual_count = super_vtable_length;
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      ArtMethod* local_method = klass->GetVirtualMethodDuringLinking(i, image_pointer_size_);
      size_t method_idx = local_method->GetMethodIndexDuringLinking();
      if (method_idx < super_vtable_length &&
          local_method == vtable->GetElementPtrSize<ArtMethod*>(method_idx, image_pointer_size_)) {
        continue;
      }
      vtable->SetElementPtrSize(actual_count, local_method, image_pointer_size_);
      local_method->SetMethodIndex(actual_count);
      ++actual_count;
    }
    if (!IsUint<16>(actual_count)) {
      ThrowClassFormatError(klass.Get(), "Too many methods defined on class: %zd", actual_count);
      return false;
    }
    CHECK_LE(actual_count, max_count);
    if (actual_count < max_count) {
      vtable.Assign(down_cast<mirror::PointerArray*>(vtable->CopyOf(self, actual_count)));
      if (UNLIKELY(vtable.Get() == nullptr)) {
        self->AssertPendingOOMException();
        return false;
      }
    }
    klass->SetVTable(vtable.Get());
  } else {
    CHECK_EQ(klass.Get(), GetClassRoot(kJavaLangObject));
    if (!IsUint<16>(num_virtual_methods)) {
      ThrowClassFormatError(klass.Get(), "Too many methods: %d",
                            static_cast<int>(num_virtual_methods));
      return false;
    }
    auto* vtable = AllocPointerArray(self, num_virtual_methods);
    if (UNLIKELY(vtable == nullptr)) {
      self->AssertPendingOOMException();
      return false;
    }
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      ArtMethod* virtual_method = klass->GetVirtualMethodDuringLinking(i, image_pointer_size_);
      vtable->SetElementPtrSize(i, virtual_method, image_pointer_size_);
      virtual_method->SetMethodIndex(i & 0xFFFF);
    }
    klass->SetVTable(vtable);
  }
  return true;
}
bool ClassLinker::LinkInterfaceMethods(Thread* self, Handle<mirror::Class> klass,
                                       Handle<mirror::ObjectArray<mirror::Class>> interfaces,
                                       ArtMethod** out_imt) {
  StackHandleScope<3> hs(self);
  Runtime* const runtime = Runtime::Current();
  const bool has_superclass = klass->HasSuperClass();
  const size_t super_ifcount = has_superclass ? klass->GetSuperClass()->GetIfTableCount() : 0U;
  const bool have_interfaces = interfaces.Get() != nullptr;
  const size_t num_interfaces =
      have_interfaces ? interfaces->GetLength() : klass->NumDirectInterfaces();
  const size_t method_size = ArtMethod::ObjectSize(image_pointer_size_);
  if (num_interfaces == 0) {
    if (super_ifcount == 0) {
      DCHECK_EQ(klass->GetIfTableCount(), 0);
      DCHECK(klass->GetIfTable() == nullptr);
      return true;
    }
    bool has_non_marker_interface = false;
    mirror::IfTable* super_iftable = klass->GetSuperClass()->GetIfTable();
    for (size_t i = 0; i < super_ifcount; ++i) {
      if (super_iftable->GetMethodArrayCount(i) > 0) {
        has_non_marker_interface = true;
        break;
      }
    }
    if (!has_non_marker_interface) {
      klass->SetIfTable(super_iftable);
      return true;
    }
  }
  size_t ifcount = super_ifcount + num_interfaces;
  for (size_t i = 0; i < num_interfaces; i++) {
    mirror::Class* interface = have_interfaces ?
        interfaces->GetWithoutChecks(i) : mirror::Class::GetDirectInterface(self, klass, i);
    DCHECK(interface != nullptr);
    if (UNLIKELY(!interface->IsInterface())) {
      std::string temp;
      ThrowIncompatibleClassChangeError(klass.Get(), "Class %s implements non-interface class %s",
                                        PrettyDescriptor(klass.Get()).c_str(),
                                        PrettyDescriptor(interface->GetDescriptor(&temp)).c_str());
      return false;
    }
    ifcount += interface->GetIfTableCount();
  }
  MutableHandle<mirror::IfTable> iftable(hs.NewHandle(AllocIfTable(self, ifcount)));
  if (UNLIKELY(iftable.Get() == nullptr)) {
    self->AssertPendingOOMException();
    return false;
  }
  if (super_ifcount != 0) {
    mirror::IfTable* super_iftable = klass->GetSuperClass()->GetIfTable();
    for (size_t i = 0; i < super_ifcount; i++) {
      mirror::Class* super_interface = super_iftable->GetInterface(i);
      iftable->SetInterface(i, super_interface);
    }
  }
  self->AllowThreadSuspension();
  size_t idx = super_ifcount;
  for (size_t i = 0; i < num_interfaces; i++) {
    mirror::Class* interface = have_interfaces ? interfaces->Get(i) :
        mirror::Class::GetDirectInterface(self, klass, i);
    bool duplicate = false;
    for (size_t j = 0; j < idx; j++) {
      mirror::Class* existing_interface = iftable->GetInterface(j);
      if (existing_interface == interface) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      iftable->SetInterface(idx++, interface);
      for (int32_t j = 0; j < interface->GetIfTableCount(); j++) {
        mirror::Class* super_interface = interface->GetIfTable()->GetInterface(j);
        bool super_duplicate = false;
        for (size_t k = 0; k < idx; k++) {
          mirror::Class* existing_interface = iftable->GetInterface(k);
          if (existing_interface == super_interface) {
            super_duplicate = true;
            break;
          }
        }
        if (!super_duplicate) {
          iftable->SetInterface(idx++, super_interface);
        }
      }
    }
  }
  self->AllowThreadSuspension();
  if (idx < ifcount) {
    DCHECK_NE(num_interfaces, 0U);
    iftable.Assign(down_cast<mirror::IfTable*>(
        iftable->CopyOf(self, idx * mirror::IfTable::kMax)));
    if (UNLIKELY(iftable.Get() == nullptr)) {
      self->AssertPendingOOMException();
      return false;
    }
    ifcount = idx;
  } else {
    DCHECK_EQ(idx, ifcount);
  }
  klass->SetIfTable(iftable.Get());
  if (klass->IsInterface()) {
    return true;
  }
  ArenaStack stack(runtime->GetLinearAlloc()->GetArenaPool());
  ScopedArenaAllocator allocator(&stack);
  ScopedArenaVector<ArtMethod*> miranda_methods(allocator.Adapter());
  MutableHandle<mirror::PointerArray> vtable(hs.NewHandle(klass->GetVTableDuringLinking()));
  ArtMethod* const unimplemented_method = runtime->GetImtUnimplementedMethod();
  ArtMethod* const conflict_method = runtime->GetImtConflictMethod();
  bool extend_super_iftable = false;
  if (has_superclass) {
    mirror::Class* super_class = klass->GetSuperClass();
    extend_super_iftable = true;
    if (super_class->ShouldHaveEmbeddedImtAndVTable()) {
      for (size_t i = 0; i < mirror::Class::kImtSize; ++i) {
        out_imt[i] = super_class->GetEmbeddedImTableEntry(i, image_pointer_size_);
      }
    } else {
      mirror::IfTable* if_table = super_class->GetIfTable();
      const size_t length = super_class->GetIfTableCount();
      for (size_t i = 0; i < length; ++i) {
        mirror::Class* interface = iftable->GetInterface(i);
        const size_t num_virtuals = interface->NumVirtualMethods();
        const size_t method_array_count = if_table->GetMethodArrayCount(i);
        DCHECK_EQ(num_virtuals, method_array_count);
        if (method_array_count == 0) {
          continue;
        }
        auto* method_array = if_table->GetMethodArray(i);
        for (size_t j = 0; j < num_virtuals; ++j) {
          auto method = method_array->GetElementPtrSize<ArtMethod*>(j, image_pointer_size_);
          DCHECK(method != nullptr) << PrettyClass(super_class);
          if (method->IsMiranda()) {
            continue;
          }
          ArtMethod* interface_method = interface->GetVirtualMethod(j, image_pointer_size_);
          uint32_t imt_index = interface_method->GetDexMethodIndex() % mirror::Class::kImtSize;
          auto*& imt_ref = out_imt[imt_index];
          if (imt_ref == unimplemented_method) {
            imt_ref = method;
          } else if (imt_ref != conflict_method) {
            imt_ref = conflict_method;
          }
        }
      }
    }
  }
  for (size_t i = 0; i < ifcount; ++i) {
    size_t num_methods = iftable->GetInterface(i)->NumVirtualMethods();
    if (num_methods > 0) {
      const bool is_super = i < super_ifcount;
      const bool super_interface = is_super && extend_super_iftable;
      mirror::PointerArray* method_array;
      if (super_interface) {
        mirror::IfTable* if_table = klass->GetSuperClass()->GetIfTable();
        DCHECK(if_table != nullptr);
        DCHECK(if_table->GetMethodArray(i) != nullptr);
        method_array = down_cast<mirror::PointerArray*>(if_table->GetMethodArray(i)->Clone(self));
      } else {
        method_array = AllocPointerArray(self, num_methods);
      }
      if (UNLIKELY(method_array == nullptr)) {
        self->AssertPendingOOMException();
        return false;
      }
      iftable->SetMethodArray(i, method_array);
    }
  }
  auto* old_cause = self->StartAssertNoThreadSuspension(
      "Copying ArtMethods for LinkInterfaceMethods");
  for (size_t i = 0; i < ifcount; ++i) {
    size_t num_methods = iftable->GetInterface(i)->NumVirtualMethods();
    if (num_methods > 0) {
      StackHandleScope<2> hs2(self);
      const bool is_super = i < super_ifcount;
      const bool super_interface = is_super && extend_super_iftable;
      auto method_array(hs2.NewHandle(iftable->GetMethodArray(i)));
      ArtMethod* input_virtual_methods = nullptr;
      Handle<mirror::PointerArray> input_vtable_array = NullHandle<mirror::PointerArray>();
      int32_t input_array_length = 0;
      if (super_interface) {
        input_virtual_methods = klass->GetVirtualMethodsPtr();
        input_array_length = klass->NumVirtualMethods();
      } else {
        input_vtable_array = vtable;
        input_array_length = input_vtable_array->GetLength();
      }
      if (input_array_length == 0) {
        DCHECK(super_interface);
        continue;
      }
      for (size_t j = 0; j < num_methods; ++j) {
        auto* interface_method = iftable->GetInterface(i)->GetVirtualMethod(
            j, image_pointer_size_);
        MethodNameAndSignatureComparator interface_name_comparator(
            interface_method->GetInterfaceMethodIfProxy(image_pointer_size_));
        int32_t k;
        for (k = input_array_length - 1; k >= 0; --k) {
          ArtMethod* vtable_method = input_virtual_methods != nullptr ?
              reinterpret_cast<ArtMethod*>(
                  reinterpret_cast<uintptr_t>(input_virtual_methods) + method_size * k) :
              input_vtable_array->GetElementPtrSize<ArtMethod*>(k, image_pointer_size_);
          ArtMethod* vtable_method_for_name_comparison =
              vtable_method->GetInterfaceMethodIfProxy(image_pointer_size_);
          if (interface_name_comparator.HasSameNameAndSignature(
              vtable_method_for_name_comparison)) {
            if (!vtable_method->IsAbstract() && !vtable_method->IsPublic()) {
              ThrowIllegalAccessError(klass.Get(),
                  "Method '%s' implementing interface method '%s' is not public",
                  PrettyMethod(vtable_method).c_str(), PrettyMethod(interface_method).c_str());
              return false;
            }
            method_array->SetElementPtrSize(j, vtable_method, image_pointer_size_);
            uint32_t imt_index = interface_method->GetDexMethodIndex() % mirror::Class::kImtSize;
            auto** imt_ref = &out_imt[imt_index];
            if (*imt_ref == unimplemented_method) {
              *imt_ref = vtable_method;
            } else if (*imt_ref != conflict_method) {
              MethodNameAndSignatureComparator imt_comparator(
                  (*imt_ref)->GetInterfaceMethodIfProxy(image_pointer_size_));
              *imt_ref = imt_comparator.HasSameNameAndSignature(vtable_method_for_name_comparison) ?
                  vtable_method : conflict_method;
            }
            break;
          }
        }
        if (k < 0 && !super_interface) {
          ArtMethod* miranda_method = nullptr;
          for (auto& mir_method : miranda_methods) {
            if (interface_name_comparator.HasSameNameAndSignature(mir_method)) {
              miranda_method = mir_method;
              break;
            }
          }
          if (miranda_method == nullptr) {
            size_t size = ArtMethod::ObjectSize(image_pointer_size_);
            miranda_method = reinterpret_cast<ArtMethod*>(allocator.Alloc(size));
            CHECK(miranda_method != nullptr);
            new(miranda_method) ArtMethod(*interface_method, image_pointer_size_);
            miranda_methods.push_back(miranda_method);
          }
          method_array->SetElementPtrSize(j, miranda_method, image_pointer_size_);
        }
      }
    }
  }
  if (!miranda_methods.empty()) {
    const size_t old_method_count = klass->NumVirtualMethods();
    const size_t new_method_count = old_method_count + miranda_methods.size();
    ArtMethod* old_virtuals = klass->GetVirtualMethodsPtr();
    auto* virtuals = reinterpret_cast<ArtMethod*>(runtime->GetLinearAlloc()->Realloc(
        self, old_virtuals, old_method_count * method_size, new_method_count * method_size));
    if (UNLIKELY(virtuals == nullptr)) {
      self->AssertPendingOOMException();
      return false;
    }
    ScopedArenaUnorderedMap<ArtMethod*, ArtMethod*> move_table(allocator.Adapter());
    if (virtuals != old_virtuals) {
      StrideIterator<ArtMethod> out(reinterpret_cast<uintptr_t>(virtuals), method_size);
      for (auto& m : klass->GetVirtualMethods(image_pointer_size_)) {
        move_table.emplace(&m, &*out);
        out->CopyFrom(&m, image_pointer_size_);
        ++out;
      }
    }
    UpdateClassVirtualMethods(klass.Get(), virtuals, new_method_count);
    self->EndAssertNoThreadSuspension(old_cause);
    size_t old_vtable_count = vtable->GetLength();
    const size_t new_vtable_count = old_vtable_count + miranda_methods.size();
    vtable.Assign(down_cast<mirror::PointerArray*>(vtable->CopyOf(self, new_vtable_count)));
    if (UNLIKELY(vtable.Get() == nullptr)) {
      self->AssertPendingOOMException();
      return false;
    }
    StrideIterator<ArtMethod> out(
        reinterpret_cast<uintptr_t>(virtuals) + old_method_count * method_size, method_size);
    for (auto* mir_method : miranda_methods) {
      ArtMethod* out_method = &*out;
      out->CopyFrom(mir_method, image_pointer_size_);
      out_method->SetAccessFlags(out_method->GetAccessFlags() | kAccMiranda);
      out_method->SetMethodIndex(0xFFFF & old_vtable_count);
      vtable->SetElementPtrSize(old_vtable_count, out_method, image_pointer_size_);
      move_table.emplace(mir_method, out_method);
      ++out;
      ++old_vtable_count;
    }
    for (size_t i = 0; i < old_vtable_count - miranda_methods.size(); ++i) {
      auto* m = vtable->GetElementPtrSize<ArtMethod*>(i, image_pointer_size_);
      DCHECK(m != nullptr) << PrettyClass(klass.Get());
      auto it = move_table.find(m);
      if (it != move_table.end()) {
        auto* new_m = it->second;
        DCHECK(new_m != nullptr) << PrettyClass(klass.Get());
        vtable->SetElementPtrSize(i, new_m, image_pointer_size_);
      }
    }
    klass->SetVTable(vtable.Get());
    CHECK_EQ(old_vtable_count, new_vtable_count);
    for (size_t i = 0; i < ifcount; ++i) {
      for (size_t j = 0, count = iftable->GetMethodArrayCount(i); j < count; ++j) {
        auto* method_array = iftable->GetMethodArray(i);
        auto* m = method_array->GetElementPtrSize<ArtMethod*>(j, image_pointer_size_);
        DCHECK(m != nullptr) << PrettyClass(klass.Get());
        auto it = move_table.find(m);
        if (it != move_table.end()) {
          auto* new_m = it->second;
          DCHECK(new_m != nullptr) << PrettyClass(klass.Get());
          method_array->SetElementPtrSize(j, new_m, image_pointer_size_);
        }
      }
    }
    if (kIsDebugBuild) {
      auto* resolved_methods = klass->GetDexCache()->GetResolvedMethods();
      for (size_t i = 0, count = resolved_methods->GetLength(); i < count; ++i) {
        auto* m = resolved_methods->GetElementPtrSize<ArtMethod*>(i, image_pointer_size_);
        CHECK(move_table.find(m) == move_table.end()) << PrettyMethod(m);
      }
    }
    if (virtuals != old_virtuals) {
      memset(old_virtuals, 0xFEu, ArtMethod::ObjectSize(image_pointer_size_) * old_method_count);
    }
  } else {
    self->EndAssertNoThreadSuspension(old_cause);
  }
  if (kIsDebugBuild) {
    auto* check_vtable = klass->GetVTableDuringLinking();
    for (int i = 0; i < check_vtable->GetLength(); ++i) {
      CHECK(check_vtable->GetElementPtrSize<ArtMethod*>(i, image_pointer_size_) != nullptr);
    }
  }
  return true;
}
bool ClassLinker::LinkInstanceFields(Thread* self, Handle<mirror::Class> klass) {
  CHECK(klass.Get() != nullptr);
  return LinkFields(self, klass, false, nullptr);
}
bool ClassLinker::LinkStaticFields(Thread* self, Handle<mirror::Class> klass, size_t* class_size) {
  CHECK(klass.Get() != nullptr);
  return LinkFields(self, klass, true, class_size);
}
struct LinkFieldsComparator {
explicit LinkFieldsComparator() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
                                  }
  bool operator()(ArtField* field1, ArtField* field2)
      NO_THREAD_SAFETY_ANALYSIS {
    Primitive::Type type1 = field1->GetTypeAsPrimitiveType();
    Primitive::Type type2 = field2->GetTypeAsPrimitiveType();
    if (type1 != type2) {
      if (type1 == Primitive::kPrimNot) {
        return true;
      }
      if (type2 == Primitive::kPrimNot) {
        return false;
      }
      size_t size1 = Primitive::ComponentSize(type1);
      size_t size2 = Primitive::ComponentSize(type2);
      if (size1 != size2) {
        return size1 > size2;
      }
      return type1 < type2;
    }
    return field1->GetDexFieldIndex() < field2->GetDexFieldIndex();
  }
                                  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
                                  }
};
bool ClassLinker::LinkFields(Thread* self, Handle<mirror::Class> klass, bool is_static,
                             size_t* class_size) {
  self->AllowThreadSuspension();
  const size_t num_fields = is_static ? klass->NumStaticFields() : klass->NumInstanceFields();
  ArtField* const fields = is_static ? klass->GetSFields() : klass->GetIFields();
  MemberOffset field_offset(0);
  if (is_static) {
    field_offset = klass->GetFirstReferenceStaticFieldOffsetDuringLinking(image_pointer_size_);
  } else {
    mirror::Class* super_class = klass->GetSuperClass();
    if (super_class != nullptr) {
      CHECK(super_class->IsResolved())
          << PrettyClass(klass.Get()) << " " << PrettyClass(super_class);
      field_offset = MemberOffset(super_class->GetObjectSize());
    }
  }
  CHECK_EQ(num_fields == 0, fields == nullptr) << PrettyClass(klass.Get());
  std::deque<ArtField*> grouped_and_sorted_fields;
  const char* old_no_suspend_cause = self->StartAssertNoThreadSuspension(
      "Naked ArtField references in deque");
  for (size_t i = 0; i < num_fields; i++) {
    grouped_and_sorted_fields.push_back(&fields[i]);
  }
  std::sort(grouped_and_sorted_fields.begin(), grouped_and_sorted_fields.end(),
            LinkFieldsComparator());
  size_t current_field = 0;
  size_t num_reference_fields = 0;
  FieldGaps gaps;
  for (; current_field < num_fields; current_field++) {
    ArtField* field = grouped_and_sorted_fields.front();
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    bool isPrimitive = type != Primitive::kPrimNot;
    if (isPrimitive) {
      break;
    }
    if (UNLIKELY(!IsAligned<sizeof(mirror::HeapReference<mirror::Object>)>(
        field_offset.Uint32Value()))) {
      MemberOffset old_offset = field_offset;
      field_offset = MemberOffset(RoundUp(field_offset.Uint32Value(), 4));
      AddFieldGap(old_offset.Uint32Value(), field_offset.Uint32Value(), &gaps);
    }
    DCHECK(IsAligned<sizeof(mirror::HeapReference<mirror::Object>)>(field_offset.Uint32Value()));
    grouped_and_sorted_fields.pop_front();
    num_reference_fields++;
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() +
                                sizeof(mirror::HeapReference<mirror::Object>));
  }
  ShuffleForward<8>(&current_field, &field_offset, &grouped_and_sorted_fields, &gaps);
  ShuffleForward<4>(&current_field, &field_offset, &grouped_and_sorted_fields, &gaps);
  ShuffleForward<2>(&current_field, &field_offset, &grouped_and_sorted_fields, &gaps);
  ShuffleForward<1>(&current_field, &field_offset, &grouped_and_sorted_fields, &gaps);
  CHECK(grouped_and_sorted_fields.empty()) << "Missed " << grouped_and_sorted_fields.size() <<
      " fields.";
  self->EndAssertNoThreadSuspension(old_no_suspend_cause);
  if (!is_static && klass->DescriptorEquals("Ljava/lang/ref/Reference;")) {
    CHECK_EQ(num_reference_fields, num_fields) << PrettyClass(klass.Get());
    CHECK_STREQ(fields[num_fields - 1].GetName(), "referent") << PrettyClass(klass.Get());
    --num_reference_fields;
  }
  size_t size = field_offset.Uint32Value();
  if (is_static) {
    klass->SetNumReferenceStaticFields(num_reference_fields);
    *class_size = size;
  } else {
    klass->SetNumReferenceInstanceFields(num_reference_fields);
    if (!klass->IsVariableSize()) {
      std::string temp;
      DCHECK_GE(size, sizeof(mirror::Object)) << klass->GetDescriptor(&temp);
      size_t previous_size = klass->GetObjectSize();
      if (previous_size != 0) {
        CHECK_EQ(previous_size, size) << klass->GetDescriptor(&temp);
      }
      klass->SetObjectSize(size);
    }
  }
  if (kIsDebugBuild) {
    MemberOffset start_ref_offset = is_static
        ? klass->GetFirstReferenceStaticFieldOffsetDuringLinking(image_pointer_size_)
        : klass->GetFirstReferenceInstanceFieldOffset();
    MemberOffset end_ref_offset(start_ref_offset.Uint32Value() +
                                num_reference_fields *
                                    sizeof(mirror::HeapReference<mirror::Object>));
    MemberOffset current_ref_offset = start_ref_offset;
    for (size_t i = 0; i < num_fields; i++) {
      ArtField* field = &fields[i];
      VLOG(class_linker) << "LinkFields: " << (is_static ? "static" : "instance")
          << " class=" << PrettyClass(klass.Get()) << " field=" << PrettyField(field) << " offset="
          << field->GetOffset();
      if (i != 0) {
        ArtField* const prev_field = &fields[i - 1];
        CHECK_LE(strcmp(prev_field->GetName(), field->GetName()), 0);
      }
      Primitive::Type type = field->GetTypeAsPrimitiveType();
      bool is_primitive = type != Primitive::kPrimNot;
      if (klass->DescriptorEquals("Ljava/lang/ref/Reference;") &&
          strcmp("referent", field->GetName()) == 0) {
        is_primitive = true;
      }
      MemberOffset offset = field->GetOffsetDuringLinking();
      if (is_primitive) {
        if (offset.Uint32Value() < end_ref_offset.Uint32Value()) {
          size_t type_size = Primitive::ComponentSize(type);
          CHECK_LT(type_size, sizeof(mirror::HeapReference<mirror::Object>));
          CHECK_LT(offset.Uint32Value(), start_ref_offset.Uint32Value());
          CHECK_LE(offset.Uint32Value() + type_size, start_ref_offset.Uint32Value());
          CHECK(!IsAligned<sizeof(mirror::HeapReference<mirror::Object>)>(offset.Uint32Value()));
        }
      } else {
        CHECK_EQ(current_ref_offset.Uint32Value(), offset.Uint32Value());
        current_ref_offset = MemberOffset(current_ref_offset.Uint32Value() +
                                          sizeof(mirror::HeapReference<mirror::Object>));
      }
    }
    CHECK_EQ(current_ref_offset.Uint32Value(), end_ref_offset.Uint32Value());
  }
  return true;
}
void ClassLinker::CreateReferenceInstanceOffsets(Handle<mirror::Class> klass) {
  uint32_t reference_offsets = 0;
  mirror::Class* super_class = klass->GetSuperClass();
  if (super_class != nullptr) {
    reference_offsets = super_class->GetReferenceInstanceOffsets();
    if (reference_offsets != mirror::Class::kClassWalkSuper) {
      size_t num_reference_fields = klass->NumReferenceInstanceFieldsDuringLinking();
      if (num_reference_fields != 0u) {
        uint32_t start_offset = RoundUp(super_class->GetObjectSize(),
                                        sizeof(mirror::HeapReference<mirror::Object>));
        uint32_t start_bit = (start_offset - mirror::kObjectHeaderSize) /
            sizeof(mirror::HeapReference<mirror::Object>);
        if (start_bit + num_reference_fields > 32) {
          reference_offsets = mirror::Class::kClassWalkSuper;
        } else {
          reference_offsets |= (0xffffffffu << start_bit) &
                               (0xffffffffu >> (32 - (start_bit + num_reference_fields)));
        }
      }
    }
  }
  klass->SetReferenceInstanceOffsets(reference_offsets);
}
mirror::String* ClassLinker::ResolveString(const DexFile& dex_file, uint32_t string_idx,
                                           Handle<mirror::DexCache> dex_cache) {
  DCHECK(dex_cache.Get() != nullptr);
  mirror::String* resolved = dex_cache->GetResolvedString(string_idx);
  if (resolved != nullptr) {
    return resolved;
  }
  uint32_t utf16_length;
  const char* utf8_data = dex_file.StringDataAndUtf16LengthByIdx(string_idx, &utf16_length);
  mirror::String* string = intern_table_->InternStrong(utf16_length, utf8_data);
  dex_cache->SetResolvedString(string_idx, string);
  return string;
}
mirror::Class* ClassLinker::ResolveType(const DexFile& dex_file, uint16_t type_idx,
                                        Handle<mirror::DexCache> dex_cache,
                                        Handle<mirror::ClassLoader> class_loader) {
  DCHECK(dex_cache.Get() != nullptr);
  mirror::Class* resolved = dex_cache->GetResolvedType(type_idx);
  if (resolved == nullptr) {
    Thread* self = Thread::Current();
    const char* descriptor = dex_file.StringByTypeIdx(type_idx);
    resolved = FindClass(self, descriptor, class_loader);
    if (resolved != nullptr) {
      dex_cache->SetResolvedType(type_idx, resolved);
    } else {
      CHECK(self->IsExceptionPending())
          << "Expected pending exception for failed resolution of: " << descriptor;
      StackHandleScope<1> hs(self);
      Handle<mirror::Throwable> cause(hs.NewHandle(self->GetException()));
      if (cause->InstanceOf(GetClassRoot(kJavaLangClassNotFoundException))) {
        DCHECK(resolved == nullptr);
        self->ClearException();
        ThrowNoClassDefFoundError("Failed resolution of: %s", descriptor);
        self->GetException()->SetCause(cause.Get());
      }
    }
  }
  DCHECK((resolved == nullptr) || resolved->IsResolved() || resolved->IsErroneous())
          << PrettyDescriptor(resolved) << " " << resolved->GetStatus();
  return resolved;
}
mirror::Class* ClassLinker::ResolveType(const DexFile& dex_file, uint16_t type_idx,
                                        Handle<mirror::DexCache> dex_cache,
                                        Handle<mirror::ClassLoader> class_loader) {
  DCHECK(dex_cache.Get() != nullptr);
  mirror::Class* resolved = dex_cache->GetResolvedType(type_idx);
  if (resolved == nullptr) {
    Thread* self = Thread::Current();
    const char* descriptor = dex_file.StringByTypeIdx(type_idx);
    resolved = FindClass(self, descriptor, class_loader);
    if (resolved != nullptr) {
      dex_cache->SetResolvedType(type_idx, resolved);
    } else {
      CHECK(self->IsExceptionPending())
          << "Expected pending exception for failed resolution of: " << descriptor;
      StackHandleScope<1> hs(self);
      Handle<mirror::Throwable> cause(hs.NewHandle(self->GetException()));
      if (cause->InstanceOf(GetClassRoot(kJavaLangClassNotFoundException))) {
        DCHECK(resolved == nullptr);
        self->ClearException();
        ThrowNoClassDefFoundError("Failed resolution of: %s", descriptor);
        self->GetException()->SetCause(cause.Get());
      }
    }
  }
  DCHECK((resolved == nullptr) || resolved->IsResolved() || resolved->IsErroneous())
          << PrettyDescriptor(resolved) << " " << resolved->GetStatus();
  return resolved;
}
ArtMethod* ClassLinker::ResolveMethod(const DexFile& dex_file, uint32_t method_idx,
                                      Handle<mirror::DexCache> dex_cache,
                                      Handle<mirror::ClassLoader> class_loader,
                                      ArtMethod* referrer, InvokeType type) {
  DCHECK(dex_cache.Get() != nullptr);
  ArtMethod* resolved = dex_cache->GetResolvedMethod(method_idx, image_pointer_size_);
  if (resolved != nullptr && !resolved->IsRuntimeMethod()) {
    DCHECK(resolved->GetDeclaringClassUnchecked() != nullptr) << resolved->GetDexMethodIndex();
    return resolved;
  }
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  mirror::Class* klass = ResolveType(dex_file, method_id.class_idx_, dex_cache, class_loader);
  if (klass == nullptr) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }
  switch (type) {
    case kDirect:
    case kStatic:
      resolved = klass->FindDirectMethod(dex_cache.Get(), method_idx, image_pointer_size_);
      DCHECK(resolved == nullptr || resolved->GetDeclaringClassUnchecked() != nullptr);
      break;
    case kInterface:
      resolved = klass->FindInterfaceMethod(dex_cache.Get(), method_idx, image_pointer_size_);
      DCHECK(resolved == nullptr || resolved->GetDeclaringClass()->IsInterface());
      break;
    case kSuper:
    case kVirtual:
      resolved = klass->FindVirtualMethod(dex_cache.Get(), method_idx, image_pointer_size_);
      break;
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << type;
      UNREACHABLE();
  }
  if (resolved == nullptr) {
    const char* name = dex_file.StringDataByIdx(method_id.name_idx_);
    const Signature signature = dex_file.GetMethodSignature(method_id);
    switch (type) {
      case kDirect:
      case kStatic:
        resolved = klass->FindDirectMethod(name, signature, image_pointer_size_);
        DCHECK(resolved == nullptr || resolved->GetDeclaringClassUnchecked() != nullptr);
        break;
      case kInterface:
        resolved = klass->FindInterfaceMethod(name, signature, image_pointer_size_);
        DCHECK(resolved == nullptr || resolved->GetDeclaringClass()->IsInterface());
        break;
      case kSuper:
      case kVirtual:
        resolved = klass->FindVirtualMethod(name, signature, image_pointer_size_);
        break;
    }
  }
  if (LIKELY(resolved != nullptr && !resolved->CheckIncompatibleClassChange(type))) {
    dex_cache->SetResolvedMethod(method_idx, resolved, image_pointer_size_);
    return resolved;
  } else {
    if (resolved != nullptr) {
      ThrowIncompatibleClassChangeError(type, resolved->GetInvokeType(), resolved, referrer);
    } else {
      const char* name = dex_file.StringDataByIdx(method_id.name_idx_);
      const Signature signature = dex_file.GetMethodSignature(method_id);
      switch (type) {
        case kDirect:
        case kStatic:
          resolved = klass->FindVirtualMethod(name, signature, image_pointer_size_);
          break;
        case kInterface:
        case kVirtual:
        case kSuper:
          resolved = klass->FindDirectMethod(name, signature, image_pointer_size_);
          break;
      }
      bool exception_generated = false;
      if (resolved != nullptr && referrer != nullptr) {
        mirror::Class* methods_class = resolved->GetDeclaringClass();
        mirror::Class* referring_class = referrer->GetDeclaringClass();
        if (!referring_class->CanAccess(methods_class)) {
          ThrowIllegalAccessErrorClassForMethodDispatch(referring_class, methods_class, resolved,
                                                        type);
          exception_generated = true;
        } else if (!referring_class->CanAccessMember(methods_class, resolved->GetAccessFlags())) {
          ThrowIllegalAccessErrorMethod(referring_class, resolved);
          exception_generated = true;
        }
      }
      if (!exception_generated) {
        switch (type) {
          case kDirect:
          case kStatic:
            if (resolved != nullptr) {
              ThrowIncompatibleClassChangeError(type, kVirtual, resolved, referrer);
            } else {
              resolved = klass->FindInterfaceMethod(name, signature, image_pointer_size_);
              if (resolved != nullptr) {
                ThrowIncompatibleClassChangeError(type, kInterface, resolved, referrer);
              } else {
                ThrowNoSuchMethodError(type, klass, name, signature);
              }
            }
            break;
          case kInterface:
            if (resolved != nullptr) {
              ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer);
            } else {
              resolved = klass->FindVirtualMethod(name, signature, image_pointer_size_);
              if (resolved != nullptr) {
                ThrowIncompatibleClassChangeError(type, kVirtual, resolved, referrer);
              } else {
                ThrowNoSuchMethodError(type, klass, name, signature);
              }
            }
            break;
          case kSuper:
            if (resolved != nullptr) {
              ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer);
            } else {
              ThrowNoSuchMethodError(type, klass, name, signature);
            }
            break;
          case kVirtual:
            if (resolved != nullptr) {
              ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer);
            } else {
              resolved = klass->FindInterfaceMethod(name, signature, image_pointer_size_);
              if (resolved != nullptr) {
                ThrowIncompatibleClassChangeError(type, kInterface, resolved, referrer);
              } else {
                ThrowNoSuchMethodError(type, klass, name, signature);
              }
            }
            break;
        }
      }
    }
    Thread::Current()->AssertPendingException();
    return nullptr;
  }
}
ArtField* ClassLinker::ResolveField(const DexFile& dex_file, uint32_t field_idx,
                                    Handle<mirror::DexCache> dex_cache,
                                    Handle<mirror::ClassLoader> class_loader, bool is_static) {
  DCHECK(dex_cache.Get() != nullptr);
  ArtField* resolved = dex_cache->GetResolvedField(field_idx, image_pointer_size_);
  if (resolved != nullptr) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  Thread* const self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(
      hs.NewHandle(ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader)));
  if (klass.Get() == nullptr) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }
  if (is_static) {
    resolved = mirror::Class::FindStaticField(self, klass, dex_cache.Get(), field_idx);
  } else {
    resolved = klass->FindInstanceField(dex_cache.Get(), field_idx);
  }
  if (resolved == nullptr) {
    const char* name = dex_file.GetFieldName(field_id);
    const char* type = dex_file.GetFieldTypeDescriptor(field_id);
    if (is_static) {
      resolved = mirror::Class::FindStaticField(self, klass, name, type);
    } else {
      resolved = klass->FindInstanceField(name, type);
    }
    if (resolved == nullptr) {
      ThrowNoSuchFieldError(is_static ? "static " : "instance ", klass.Get(), type, name);
      return nullptr;
    }
  }
  dex_cache->SetResolvedField(field_idx, resolved, image_pointer_size_);
  return resolved;
}
ArtField* ClassLinker::ResolveFieldJLS(const DexFile& dex_file, uint32_t field_idx,
                                       Handle<mirror::DexCache> dex_cache,
                                       Handle<mirror::ClassLoader> class_loader) {
  DCHECK(dex_cache.Get() != nullptr);
  ArtField* resolved = dex_cache->GetResolvedField(field_idx, image_pointer_size_);
  if (resolved != nullptr) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(
      hs.NewHandle(ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader)));
  if (klass.Get() == nullptr) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }
  StringPiece name(dex_file.StringDataByIdx(field_id.name_idx_));
  StringPiece type(dex_file.StringDataByIdx(
      dex_file.GetTypeId(field_id.type_idx_).descriptor_idx_));
  resolved = mirror::Class::FindField(self, klass, name, type);
  if (resolved != nullptr) {
    dex_cache->SetResolvedField(field_idx, resolved, image_pointer_size_);
  } else {
    ThrowNoSuchFieldError("", klass.Get(), type, name);
  }
  return resolved;
}
const char* ClassLinker::MethodShorty(uint32_t method_idx, ArtMethod* referrer,
                                      uint32_t* length) {
  mirror::Class* declaring_class = referrer->GetDeclaringClass();
  mirror::DexCache* dex_cache = declaring_class->GetDexCache();
  const DexFile& dex_file = *dex_cache->GetDexFile();
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  return dex_file.GetMethodShorty(method_id, length);
}
void ClassLinker::DumpAllClasses(int flags) {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  std::vector<mirror::Class*> all_classes;
  {
    ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
    for (GcRoot<mirror::Class>& it : class_table_) {
      all_classes.push_back(it.Read());
    }
  }
  for (size_t i = 0; i < all_classes.size(); ++i) {
    all_classes[i]->DumpClass(std::cerr, flags);
  }
}
static OatFile::OatMethod CreateOatMethod(const void* code) {
  CHECK(code != nullptr);
  const uint8_t* base = reinterpret_cast<const uint8_t*>(code);
  base -= sizeof(void*);
  const uint32_t code_offset = sizeof(void*);
  return OatFile::OatMethod(base, code_offset);
}
bool ClassLinker::IsQuickResolutionStub(const void* entry_point) const {
  return (entry_point == GetQuickResolutionStub()) ||
      (quick_resolution_trampoline_ == entry_point);
}
bool ClassLinker::IsQuickToInterpreterBridge(const void* entry_point) const {
  return (entry_point == GetQuickToInterpreterBridge()) ||
      (quick_to_interpreter_bridge_trampoline_ == entry_point);
}
bool ClassLinker::IsQuickGenericJniStub(const void* entry_point) const {
  return (entry_point == GetQuickGenericJniStub()) ||
      (quick_generic_jni_trampoline_ == entry_point);
}
const void* ClassLinker::GetRuntimeQuickGenericJniStub() const {
  return GetQuickGenericJniStub();
}
void ClassLinker::SetEntryPointsToCompiledCode(ArtMethod* method,
                                               const void* method_code) const {
  OatFile::OatMethod oat_method = CreateOatMethod(method_code);
  oat_method.LinkMethod(method);
  method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
}
void ClassLinker::SetEntryPointsToInterpreter(ArtMethod* method) const {
  if (!method->IsNative()) {
    method->SetEntryPointFromInterpreter(artInterpreterToInterpreterBridge);
    method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
  } else {
    const void* quick_method_code = GetQuickGenericJniStub();
    OatFile::OatMethod oat_method = CreateOatMethod(quick_method_code);
    oat_method.LinkMethod(method);
    method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
  }
}
void ClassLinker::DumpForSigQuit(std::ostream& os) {
  Thread* self = Thread::Current();
  if (dex_cache_image_class_lookup_required_) {
    ScopedObjectAccess soa(self);
    MoveImageClassesToClassTable();
  }
  ReaderMutexLock mu(self, *Locks::classlinker_classes_lock_);
  os << "Zygote loaded classes=" << pre_zygote_class_table_.Size() << " post zygote classes="
     << class_table_.Size() << "\n";
}
size_t ClassLinker::NumLoadedClasses() {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  return class_table_.Size();
}
pid_t ClassLinker::GetClassesLockOwner() {
  return Locks::classlinker_classes_lock_->GetExclusiveOwnerTid();
}
pid_t ClassLinker::GetDexLockOwner() {
  return dex_lock_.GetExclusiveOwnerTid();
}
void ClassLinker::SetClassRoot(ClassRoot class_root, mirror::Class* klass) {
  DCHECK(!init_done_);
  DCHECK(klass != nullptr);
  DCHECK(klass->GetClassLoader() == nullptr);
  mirror::ObjectArray<mirror::Class>* class_roots = class_roots_.Read();
  DCHECK(class_roots != nullptr);
  DCHECK(class_roots->Get(class_root) == nullptr);
  class_roots->Set<false>(class_root, klass);
}
const char* ClassLinker::GetClassRootDescriptor(ClassRoot class_root) {
  static const char* class_roots_descriptors[] = {
    "Ljava/lang/Class;",
    "Ljava/lang/Object;",
    "[Ljava/lang/Class;",
    "[Ljava/lang/Object;",
    "Ljava/lang/String;",
    "Ljava/lang/DexCache;",
    "Ljava/lang/ref/Reference;",
    "Ljava/lang/reflect/Constructor;",
    "Ljava/lang/reflect/Field;",
    "Ljava/lang/reflect/Method;",
    "Ljava/lang/reflect/Proxy;",
    "[Ljava/lang/String;",
    "[Ljava/lang/reflect/Constructor;",
    "[Ljava/lang/reflect/Field;",
    "[Ljava/lang/reflect/Method;",
    "Ljava/lang/ClassLoader;",
    "Ljava/lang/Throwable;",
    "Ljava/lang/ClassNotFoundException;",
    "Ljava/lang/StackTraceElement;",
    "Z",
    "B",
    "C",
    "D",
    "F",
    "I",
    "J",
    "S",
    "V",
    "[Z",
    "[B",
    "[C",
    "[D",
    "[F",
    "[I",
    "[J",
    "[S",
    "[Ljava/lang/StackTraceElement;",
  };
  static_assert(arraysize(class_roots_descriptors) == size_t(kClassRootsMax),
                "Mismatch between class descriptors and class-root enum");
  const char* descriptor = class_roots_descriptors[class_root];
  CHECK(descriptor != nullptr);
  return descriptor;
}
std::size_t ClassLinker::ClassDescriptorHashEquals::operator()(const GcRoot<mirror::Class>& root)
    const {
  std::string temp;
  return ComputeModifiedUtf8Hash(root.Read()->GetDescriptor(&temp));
}
bool ClassLinker::ClassDescriptorHashEquals::operator()(const GcRoot<mirror::Class>& a,
                                                        const GcRoot<mirror::Class>& b) const {
  if (a.Read()->GetClassLoader() != b.Read()->GetClassLoader()) {
    return false;
  }
  std::string temp;
  return a.Read()->DescriptorEquals(b.Read()->GetDescriptor(&temp));
}
std::size_t ClassLinker::ClassDescriptorHashEquals::operator()(
    const std::pair<const char*, mirror::ClassLoader*>& element) const {
  return ComputeModifiedUtf8Hash(element.first);
}
bool ClassLinker::ClassDescriptorHashEquals::operator()(
    const GcRoot<mirror::Class>& a, const std::pair<const char*, mirror::ClassLoader*>& b) const {
  if (a.Read()->GetClassLoader() != b.second) {
    return false;
  }
  return a.Read()->DescriptorEquals(b.first);
}
bool ClassLinker::ClassDescriptorHashEquals::operator()(const GcRoot<mirror::Class>& a,
                                                        const char* descriptor) const {
  return a.Read()->DescriptorEquals(descriptor);
}
std::size_t ClassLinker::ClassDescriptorHashEquals::operator()(const char* descriptor) const {
  return ComputeModifiedUtf8Hash(descriptor);
}
bool ClassLinker::MayBeCalledWithDirectCodePointer(ArtMethod* m) {
  if (Runtime::Current()->UseJit()) {
    return true;
  }
  if (!m->GetDeclaringClass()->IsBootStrapClassLoaded()) {
    return false;
  }
  if (m->IsPrivate()) {
    const DexFile& dex_file = m->GetDeclaringClass()->GetDexFile();
    const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
    if (oat_dex_file == nullptr) {
      return false;
    }
    const OatFile* oat_file = oat_dex_file->GetOatFile();
    return oat_file != nullptr && !oat_file->IsPic();
  } else {
    ReaderMutexLock mu(Thread::Current(), dex_lock_);
    for (const OatFile* oat_file : oat_files_) {
      if (!oat_file->IsPic()) {
        return true;
      }
    }
    return false;
  }
}
jobject ClassLinker::CreatePathClassLoader(Thread* self, std::vector<const DexFile*>& dex_files) {
  ScopedObjectAccessUnchecked soa(self);
  for (const DexFile* dex_file : dex_files) {
    RegisterDexFile(*dex_file);
  }
  StackHandleScope<10> hs(self);
  ArtField* dex_elements_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList_dexElements);
  mirror::Class* dex_elements_class = dex_elements_field->GetType<true>();
  DCHECK(dex_elements_class != nullptr);
  DCHECK(dex_elements_class->IsArrayClass());
  Handle<mirror::ObjectArray<mirror::Object>> h_dex_elements(hs.NewHandle(
      mirror::ObjectArray<mirror::Object>::Alloc(self, dex_elements_class, dex_files.size())));
  Handle<mirror::Class> h_dex_element_class =
      hs.NewHandle(dex_elements_class->GetComponentType());
  ArtField* element_file_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList__Element_dexFile);
  DCHECK_EQ(h_dex_element_class.Get(), element_file_field->GetDeclaringClass());
  ArtField* cookie_field = soa.DecodeField(WellKnownClasses::dalvik_system_DexFile_cookie);
  DCHECK_EQ(cookie_field->GetDeclaringClass(), element_file_field->GetType<false>());
  int32_t index = 0;
  for (const DexFile* dex_file : dex_files) {
    StackHandleScope<3> hs2(self);
    Handle<mirror::LongArray> h_long_array = hs2.NewHandle(mirror::LongArray::Alloc(self, 1));
    DCHECK(h_long_array.Get() != nullptr);
    h_long_array->Set(0, reinterpret_cast<intptr_t>(dex_file));
    Handle<mirror::Object> h_dex_file = hs2.NewHandle(
        cookie_field->GetDeclaringClass()->AllocObject(self));
    DCHECK(h_dex_file.Get() != nullptr);
    cookie_field->SetObject<false>(h_dex_file.Get(), h_long_array.Get());
    Handle<mirror::Object> h_element = hs2.NewHandle(h_dex_element_class->AllocObject(self));
    DCHECK(h_element.Get() != nullptr);
    element_file_field->SetObject<false>(h_element.Get(), h_dex_file.Get());
    h_dex_elements->Set(index, h_element.Get());
    index++;
  }
  DCHECK_EQ(index, h_dex_elements->GetLength());
  Handle<mirror::Object> h_dex_path_list = hs.NewHandle(
      dex_elements_field->GetDeclaringClass()->AllocObject(self));
  DCHECK(h_dex_path_list.Get() != nullptr);
  dex_elements_field->SetObject<false>(h_dex_path_list.Get(), h_dex_elements.Get());
  Handle<mirror::Class> h_path_class_class = hs.NewHandle(
      soa.Decode<mirror::Class*>(WellKnownClasses::dalvik_system_PathClassLoader));
  Handle<mirror::Object> h_path_class_loader = hs.NewHandle(
      h_path_class_class->AllocObject(self));
  DCHECK(h_path_class_loader.Get() != nullptr);
  ArtField* path_list_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_PathClassLoader_pathList);
  DCHECK(path_list_field != nullptr);
  path_list_field->SetObject<false>(h_path_class_loader.Get(), h_dex_path_list.Get());
  ArtField* const parent_field =
      mirror::Class::FindField(self, hs.NewHandle(h_path_class_loader->GetClass()), "parent",
                               "Ljava/lang/ClassLoader;");
  DCHECK(parent_field != nullptr);
  mirror::Object* boot_cl =
      soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_BootClassLoader)->AllocObject(self);
  parent_field->SetObject<false>(h_path_class_loader.Get(), boot_cl);
  ScopedLocalRef<jobject> local_ref(
      soa.Env(), soa.Env()->AddLocalReference<jobject>(h_path_class_loader.Get()));
  return soa.Env()->NewGlobalRef(local_ref.get());
}
ArtMethod* ClassLinker::CreateRuntimeMethod() {
  ArtMethod* method = AllocArtMethodArray(Thread::Current(), 1);
  CHECK(method != nullptr);
  method->SetDexMethodIndex(DexFile::kDexNoIndex);
  CHECK(method->IsRuntimeMethod());
  return method;
}
static void SanityCheckArtMethodPointerArray(
    mirror::PointerArray* arr, mirror::Class* expected_class, size_t pointer_size,
    gc::space::ImageSpace* space)
const void* ClassLinker::GetQuickOatCodeFor(ArtMethod* method) {
  CHECK(!method->IsAbstract()) << PrettyMethod(method);
  if (method->IsProxyMethod()) {
    return GetQuickProxyInvokeHandler();
  }
  bool found;
  OatFile::OatMethod oat_method = FindOatMethodFor(method, &found);
  if (found) {
    auto* code = oat_method.GetQuickCode();
    if (code != nullptr) {
      return code;
    }
  }
  jit::Jit* const jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    auto* code = jit->GetCodeCache()->GetCodeFor(method);
    if (code != nullptr) {
      return code;
    }
  }
  if (method->IsNative()) {
    return GetQuickGenericJniStub();
  }
  return GetQuickToInterpreterBridge();
}
const void* ClassLinker::GetOatMethodQuickCodeFor(ArtMethod* method) {
  if (method->IsNative() || method->IsAbstract() || method->IsProxyMethod()) {
    return nullptr;
  }
  bool found;
  OatFile::OatMethod oat_method = FindOatMethodFor(method, &found);
  if (found) {
    return oat_method.GetQuickCode();
  }
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    auto* code = jit->GetCodeCache()->GetCodeFor(method);
    if (code != nullptr) {
      return code;
    }
  }
  return nullptr;
}
void ClassLinker::CreateProxyConstructor(Handle<mirror::Class> klass, ArtMethod* out) {
  CHECK_EQ(GetClassRoot(kJavaLangReflectProxy)->NumDirectMethods(), 16u);
  ArtMethod* proxy_constructor = GetClassRoot(kJavaLangReflectProxy)->GetDirectMethodUnchecked(
      2, image_pointer_size_);
  GetClassRoot(kJavaLangReflectProxy)->GetDexCache()->SetResolvedMethod(
      proxy_constructor->GetDexMethodIndex(), proxy_constructor, image_pointer_size_);
  DCHECK(out != nullptr);
  out->CopyFrom(proxy_constructor, image_pointer_size_);
  out->SetAccessFlags((out->GetAccessFlags() & ~kAccProtected) | kAccPublic);
  out->SetDeclaringClass(klass.Get());
}
void ClassLinker::CheckProxyConstructor(ArtMethod* constructor) const {
  CHECK(constructor->IsConstructor());
  auto* np = constructor->GetInterfaceMethodIfProxy(image_pointer_size_);
  CHECK_STREQ(np->GetName(), "<init>");
  CHECK_STREQ(np->GetSignature().ToString().c_str(), "(Ljava/lang/reflect/InvocationHandler;)V");
  DCHECK(constructor->IsPublic());
}
void ClassLinker::CreateProxyMethod(Handle<mirror::Class> klass, ArtMethod* prototype, ArtMethod* out) {
  auto* dex_cache = prototype->GetDeclaringClass()->GetDexCache();
  if (dex_cache->GetResolvedMethod(prototype->GetDexMethodIndex(), image_pointer_size_) !=
      prototype) {
    dex_cache->SetResolvedMethod(
        prototype->GetDexMethodIndex(), prototype, image_pointer_size_);
  }
  DCHECK(out != nullptr);
  out->CopyFrom(prototype, image_pointer_size_);
  out->SetDeclaringClass(klass.Get());
  out->SetAccessFlags((out->GetAccessFlags() & ~kAccAbstract) | kAccFinal);
  out->SetEntryPointFromQuickCompiledCode(GetQuickProxyInvokeHandler());
  out->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
}
void ClassLinker::CheckProxyMethod(ArtMethod* method, ArtMethod* prototype) const {
  CHECK(!prototype->IsFinal());
  CHECK(method->IsFinal());
  CHECK(!method->IsAbstract());
  CHECK(prototype->HasSameDexCacheResolvedMethods(method));
  CHECK(prototype->HasSameDexCacheResolvedTypes(method));
  auto* np = method->GetInterfaceMethodIfProxy(image_pointer_size_);
  CHECK_EQ(prototype->GetDeclaringClass()->GetDexCache(), np->GetDexCache());
  CHECK_EQ(prototype->GetDexMethodIndex(), method->GetDexMethodIndex());
  CHECK_STREQ(np->GetName(), prototype->GetName());
  CHECK_STREQ(np->GetShorty(), prototype->GetShorty());
  CHECK_EQ(np->GetReturnType(), prototype->GetReturnType());
}
bool ClassLinker::CanWeInitializeClass(mirror::Class* klass, bool can_init_statics, bool can_init_parents) {
  if (can_init_statics && can_init_parents) {
    return true;
  }
  if (!can_init_statics) {
    ArtMethod* clinit = klass->FindClassInitializer(image_pointer_size_);
    if (clinit != nullptr) {
      return false;
    }
    if (klass->NumStaticFields() != 0) {
      const DexFile::ClassDef* dex_class_def = klass->GetClassDef();
      DCHECK(dex_class_def != nullptr);
      if (dex_class_def->static_values_off_ != 0) {
        return false;
      }
    }
  }
  if (klass->IsInterface() || !klass->HasSuperClass()) {
    return true;
  }
  mirror::Class* super_class = klass->GetSuperClass();
  if (!can_init_parents && !super_class->IsInitialized()) {
    return false;
  }
  return CanWeInitializeClass(super_class, can_init_statics, can_init_parents);
}
}
