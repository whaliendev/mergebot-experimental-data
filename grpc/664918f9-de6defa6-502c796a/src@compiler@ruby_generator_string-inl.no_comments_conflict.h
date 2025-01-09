#ifndef GRPC_INTERNAL_COMPILER_RUBY_GENERATOR_STRING_INL_H
#define GRPC_INTERNAL_COMPILER_RUBY_GENERATOR_STRING_INL_H 
#include "src/compiler/config.h"
#include <algorithm>
#include <sstream>
#include <vector>
using std::getline;
using std::transform;
namespace grpc_ruby_generator {
inline std::vector<std::string>& Split(const std::string& s, char delim,
                                       std::vector<std::string>* elems) {
  std::stringstream ss(s);
  std::string item;
  while (getline(ss, item, delim)) {
    elems->push_back(item);
  }
  return *elems;
}
inline std::vector<std::string> Split(const std::string& s, char delim) {
  std::vector<std::string> elems;
  Split(s, delim, &elems);
  return elems;
}
inline std::string Replace(std::string s, const std::string& from,
                           const std::string& to) {
  size_t start_pos = s.find(from);
  if (start_pos == std::string::npos) {
    return s;
  }
  s.replace(start_pos, from.length(), to);
  return s;
}
inline std::string ReplaceAll(std::string s, const std::string& search,
                              const std::string& replace) {
  size_t pos = 0;
  while ((pos = s.find(search, pos)) != std::string::npos) {
    s.replace(pos, search.length(), replace);
    pos += replace.length();
  }
  return s;
}
inline bool ReplacePrefix(std::string* s, const std::string& from,
                          const std::string& to) {
  size_t start_pos = s->find(from);
  if (start_pos == std::string::npos || start_pos != 0) {
    return false;
  }
  s->replace(start_pos, from.length(), to);
  return true;
}
inline std::string Modularize(std::string s) {
  if (s.empty()) {
    return s;
  }
  std::string new_string = "";
  bool was_last_underscore = false;
  new_string.append(1, ::toupper(s[0]));
  for (std::string::size_type i = 1; i < s.size(); ++i) {
    if (was_last_underscore && s[i] != '_') {
      new_string.append(1, ::toupper(s[i]));
    } else if (s[i] != '_') {
      new_string.append(1, s[i]);
    }
    was_last_underscore = s[i] == '_';
  }
  return new_string;
}
inline std::string RubyPackage(const grpc::protobuf::FileDescriptor* file) {
  std::string package_name = file->package();
  if (file->options().has_ruby_package()) {
    package_name = file->options().ruby_package();
    package_name = ReplaceAll(package_name, "::", ".");
  }
  return package_name;
}
<<<<<<< HEAD
inline std::string RubyTypeOf(const grpc::protobuf::Descriptor* descriptor) {
||||||| 502c796a94
inline grpc::string RubyTypeOf(const grpc::protobuf::Descriptor* descriptor,
                               const grpc::string& package) {
=======
inline grpc::string RubyTypeOf(const grpc::protobuf::Descriptor* descriptor) {
>>>>>>> de6defa6
  std::string proto_type = descriptor->full_name();
  if (descriptor->file()->options().has_ruby_package()) {
    ReplacePrefix(&proto_type, descriptor->file()->package(), "");
    ReplacePrefix(&proto_type, ".", "");
    proto_type = RubyPackage(descriptor->file()) + "." + proto_type;
  }
  std::string res(proto_type);
  if (res.find('.') == std::string::npos) {
    return res;
  } else {
    std::vector<std::string> prefixes_and_type = Split(res, '.');
    res.clear();
    for (unsigned int i = 0; i < prefixes_and_type.size(); ++i) {
      if (i != 0) {
        res += "::";
      }
      if (i < prefixes_and_type.size() - 1) {
        res += Modularize(prefixes_and_type[i]);
      } else {
        res += prefixes_and_type[i];
      }
    }
    return res;
  }
}
}
#endif
