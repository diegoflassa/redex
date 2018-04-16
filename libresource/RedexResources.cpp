/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <boost/regex.hpp>
#include <sstream>
#include <string>

#ifdef _MSC_VER
#include <mman/sys/mman.h>
#else Q_OS_LINUX
#include <sys/mman.h>
#endif

#include <sys/stat.h>
#include <unordered_set>
#include <vector>

#ifdef _MSC_VER
#include "CompatWindows.h"
#endif

#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/Serialize.h"
#include "utils/TypeHelpers.h"

#include "StringUtil.h"

constexpr size_t MIN_CLASSNAME_LENGTH = 10;
constexpr size_t MAX_CLASSNAME_LENGTH = 500;

const uint32_t PACKAGE_RESID_START = 0x7f000000;

using path_t = boost::filesystem::path;
using dir_iterator = boost::filesystem::directory_iterator;
using rdir_iterator = boost::filesystem::recursive_directory_iterator;

std::string convert_from_string16(const android::String16& string16) {
  android::String8 string8(string16);
  std::string converted(string8.string());
  return converted;
}

// Returns the attribute with the given name for the current XML element
std::string get_string_attribute_value(
    const android::ResXMLTree& parser,
    const android::String16& attribute_name) {

  const size_t attr_count = parser.getAttributeCount();

  for (size_t i = 0; i < attr_count; ++i) {
    size_t len;
    android::String16 key(parser.getAttributeName(i, &len));
    if (key == attribute_name) {
      const char16_t* p = parser.getAttributeStringValue(i, &len);
      if (p != nullptr) {
        android::String16 target(p, len);
        std::string converted = convert_from_string16(target);
        return converted;
      }
    }
  }
  return std::string("");
}

bool has_raw_attribute_value(
    const android::ResXMLTree& parser,
    const android::String16& attribute_name,
    android::Res_value& outValue
  ) {
  const size_t attr_count = parser.getAttributeCount();

  for (size_t i = 0; i < attr_count; ++i) {
    size_t len;
    android::String16 key(parser.getAttributeName(i, &len));
    if (key == attribute_name) {
      parser.getAttributeValue(i, &outValue);
      return true;
    }
  }

  return false;
}

std::string dotname_to_dexname(const std::string& classname) {
  std::string dexname;
  dexname.reserve(classname.size() + 2);
  dexname += 'L';
  dexname += classname;
  dexname += ';';
  std::replace(dexname.begin(), dexname.end(), '.', '/');
  return dexname;
}

void extract_by_pattern(
    const std::string& string_to_search,
    boost::regex regex,
    std::unordered_set<std::string>& result) {
  boost::smatch m;
  std::string s = string_to_search;
  while (boost::regex_search (s, m, regex)) {
    if (m.size() > 1) {
        result.insert(m[1].str());
    }
    s = m.suffix().str();
  }
}

void extract_js_sounds(
    const std::string& file_contents,
    std::unordered_set<std::string>& result) {
  static boost::regex sound_regex("\"([^\\\"]+)\\.(m4a|ogg)\"");
  extract_by_pattern(file_contents, sound_regex, result);
}

void extract_js_uris(
    const std::string& file_contents,
    std::unordered_set<std::string>& result) {
  static boost::regex uri_regex("\\buri:\\s*\"([^\\\"]+)\"");
  extract_by_pattern(file_contents, uri_regex, result);
}

void extract_js_asset_registrations(
    const std::string& file_contents,
    std::unordered_set<std::string>& result) {
  static boost::regex register_regex("registerAsset\\((.+?)\\)");
  static boost::regex name_regex("name:\\\"(.+?)\\\"");
  static boost::regex location_regex("httpServerLocation:\\\"/assets/(.+?)\\\"");
  static boost::regex special_char_regex("[^a-z0-9_]");
  std::unordered_set<std::string> registrations;
  extract_by_pattern(file_contents, register_regex, registrations);
  for (std::string registration : registrations) {
    boost::smatch m;
    if (!boost::regex_search (registration, m, location_regex) || m.size() == 0) {
      continue;
    }
    std::stringstream asset_path;
    asset_path << m[1].str() << '/'; // location
    if (!boost::regex_search (registration, m, name_regex) || m.size() == 0) {
      continue;
    }
    asset_path << m[1].str(); // name
    std::string full_path = asset_path.str();
    boost::replace_all(full_path, "/", "_");;
    boost::algorithm::to_lower(full_path);

    std::stringstream stripped_asset_path;
    std::ostream_iterator<char, char> oi(stripped_asset_path);
    boost::regex_replace(oi, full_path.begin(), full_path.end(),
      special_char_regex, "", boost::match_default | boost::format_all);

    result.emplace(stripped_asset_path.str());
  }
}

std::unordered_set<std::string> extract_js_resources(const std::string& file_contents) {
  std::unordered_set<std::string> result;
  extract_js_sounds(file_contents, result);
  extract_js_uris(file_contents, result);
  extract_js_asset_registrations(file_contents, result);
  return result;
}

std::unordered_set<uint32_t> extract_xml_reference_attributes(
    const std::string& file_contents,
    const std::string& filename) {
  android::ResXMLTree parser;
  parser.setTo(file_contents.data(), file_contents.size());
  std::unordered_set<uint32_t> result;
  if (parser.getError() != android::NO_ERROR) {
    throw std::runtime_error("Unable to read file: " + filename);
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      const size_t attr_count = parser.getAttributeCount();
      for (size_t i = 0; i < attr_count; ++i) {
        if (parser.getAttributeDataType(i) == android::Res_value::TYPE_REFERENCE ||
            parser.getAttributeDataType(i) == android::Res_value::TYPE_ATTRIBUTE) {
          android::Res_value outValue;
          parser.getAttributeValue(i, &outValue);
          if (outValue.data > PACKAGE_RESID_START) {
            result.emplace(outValue.data);
          }
        }
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);

  return result;
}

/**
 * Follows the reference links for a resource for all configurations.
 * Returns all the nodes visited, as well as all the string values seen.
 */
void walk_references_for_resource(
    uint32_t resID,
    std::unordered_set<uint32_t>& nodes_visited,
    std::unordered_set<std::string>& leaf_string_values,
    android::ResTable* table) {
  if (nodes_visited.find(resID) != nodes_visited.end()) {
    return;
  }
  nodes_visited.emplace(resID);

  ssize_t pkg_index = table->getResourcePackageIndex(resID);

  android::Vector<android::Res_value> initial_values;
  table->getAllValuesForResource(resID, initial_values);

  std::stack<android::Res_value> nodes_to_explore;
  for (size_t index = 0; index < initial_values.size(); ++index) {
    nodes_to_explore.push(initial_values[index]);
  }

  while (!nodes_to_explore.empty()) {
    android::Res_value r = nodes_to_explore.top();
    nodes_to_explore.pop();

    if (r.dataType == android::Res_value::TYPE_STRING) {
      android::String8 str = table->getString8FromIndex(pkg_index, r.data);
      leaf_string_values.insert(std::string(str.string()));
      continue;
    }

    // Skip any non-references or already visited nodes
    if ((r.dataType != android::Res_value::TYPE_REFERENCE &&
          r.dataType != android::Res_value::TYPE_ATTRIBUTE)
        || r.data <= PACKAGE_RESID_START
        || nodes_visited.find(r.data) != nodes_visited.end()) {
      continue;
    }

    nodes_visited.insert(r.data);
    android::Vector<android::Res_value> inner_values;
    table->getAllValuesForResource(r.data, inner_values);
    for (size_t index = 0; index < inner_values.size(); ++index) {
      nodes_to_explore.push(inner_values[index]);
    }
  }
}

/*
 * Parse AndroidManifest from buffer, return a list of class names that are
 * referenced
 */
std::unordered_set<std::string> extract_classes_from_manifest(const std::string& manifest_contents) {

  // Tags
  android::String16 activity("activity");
  android::String16 activity_alias("activity-alias");
  android::String16 application("application");
  android::String16 provider("provider");
  android::String16 receiver("receiver");
  android::String16 service("service");
  android::String16 instrumentation("instrumentation");

  // Attributes
  android::String16 authorities("authorities");
  android::String16 name("name");
  android::String16 target_activity("targetActivity");

  android::ResXMLTree parser;
  parser.setTo(manifest_contents.data(), manifest_contents.size());

  std::unordered_set<std::string> result;

  if (parser.getError() != android::NO_ERROR) {
    return result;
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      size_t len;
      android::String16 tag(parser.getElementName(&len));
      if (tag == activity ||
          tag == application ||
          tag == provider ||
          tag == receiver ||
          tag == service ||
          tag == instrumentation) {

        std::string classname = get_string_attribute_value(parser, name);
        if (classname.size()) {
          result.insert(dotname_to_dexname(classname));
        }

        if (tag == provider) {
          std::string text = get_string_attribute_value(parser, authorities);
          size_t start = 0;
          size_t end = 0;
          while ((end = text.find(';', start)) != std::string::npos) {
            result.insert(dotname_to_dexname(text.substr(start, end - start)));
            start = end + 1;
          }
          result.insert(dotname_to_dexname(text.substr(start)));
        }
      } else if (tag == activity_alias) {
        std::string classname = get_string_attribute_value(parser, target_activity);
        if (classname.size()) {
          result.insert(dotname_to_dexname(classname));
        }
        classname = get_string_attribute_value(parser, name);
        if (classname.size()) {
          result.insert(dotname_to_dexname(classname));
        }
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);
  return result;
}

std::unordered_set<std::string> extract_classes_from_layout(const std::string& layout_contents) {

  android::ResXMLTree parser;
  parser.setTo(layout_contents.data(), layout_contents.size());

  std::unordered_set<std::string> result;

  android::String16 name("name");
  android::String16 klazz("class");

  if (parser.getError() != android::NO_ERROR) {
    return result;
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      size_t len;
      android::String16 tag(parser.getElementName(&len));
      std::string classname = convert_from_string16(tag);
      if (!strcmp(classname.c_str(), "fragment") || !strcmp(classname.c_str(), "view")) {
        classname = get_string_attribute_value(parser, klazz);
        if (classname.empty()) {
          classname = get_string_attribute_value(parser, name);
        }
      }
      std::string converted = std::string("L") + classname + std::string(";");

      bool is_classname = converted.find('.') != std::string::npos;
      if (is_classname) {
        std::replace(converted.begin(), converted.end(), '.', '/');
        result.insert(converted);
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);
  return result;
}

/*
 * Returns all strings that look like java class names from a native library.
 *
 * Return values will be formatted the way that the dex spec formats class names:
 *
 *   "Ljava/lang/String;"
 *
 */
std::unordered_set<std::string> extract_classes_from_native_lib(const std::string& lib_contents) {
  std::unordered_set<std::string> classes;
  char buffer[MAX_CLASSNAME_LENGTH + 2]; // +2 for the trailing ";\0"
  const char* inptr = lib_contents.data();
  char* outptr = buffer;
  const char* end = inptr + lib_contents.size();

  size_t length = 0;

  while (inptr < end) {
    outptr = buffer;
    length = 0;
    // All classnames start with a package, which starts with a lowercase
    // letter. Some of them are preceded by an 'L' and followed by a ';' in
    // native libraries while others are not.
    if ((*inptr >= 'a' && *inptr <= 'z') || *inptr == 'L') {

      if (*inptr != 'L') {
        *outptr++ = 'L';
        length++;
      }

      while (( // This loop is safe since lib_contents.data() ends with a \0
                 (*inptr >= 'a' && *inptr <= 'z') ||
                 (*inptr >= 'A' && *inptr <= 'Z') ||
                 (*inptr >= '0' && *inptr <= '9') ||
                 *inptr == '/' || *inptr == '_' || *inptr == '$')
             && length < MAX_CLASSNAME_LENGTH) {

        *outptr++ = *inptr++;
        length++;
      }
      if (length >= MIN_CLASSNAME_LENGTH) {
        *outptr++ = ';';
        *outptr = '\0';
        classes.insert(std::string(buffer));
      }
    }
    inptr++;
  }
  return classes;
}

/*
 * Reads an entire file into a std::string. Returns an empty string if
 * anything went wrong (e.g. file not found).
 */
std::string read_entire_file(const std::string& filename) {
  std::ifstream in(filename, std::ios::in | std::ios::binary);
  std::stringstream sstr;
  sstr << in.rdbuf();
  return sstr.str();
}

void write_entire_file(
    const std::string& filename,
    const std::string& contents) {
  std::ofstream out(filename, std::ofstream::binary);
  out << contents;
}

std::unordered_set<std::string> get_manifest_classes(const std::string& filename) {
  std::string manifest = read_entire_file(filename);
  std::unordered_set<std::string> classes;
  if (manifest.size()) {
    classes = extract_classes_from_manifest(manifest);
  } else {
    fprintf(stderr, "Unable to read manifest file: %s\n", filename.data());
  }
  return classes;
}

std::unordered_set<std::string> get_files_by_suffix(
    const std::string& directory,
    const std::string& suffix) {
  std::unordered_set<std::string> files;
  path_t dir(directory);

  if (exists(dir) && is_directory(dir)) {
    for (auto it = dir_iterator(dir); it != dir_iterator(); ++it) {
      auto const& entry = *it;
      path_t entry_path = entry.path();

      if (is_regular_file(entry_path) &&
          ends_with(entry_path.string().c_str(), suffix.c_str())) {
        files.emplace(entry_path.string());
      }

      if (is_directory(entry_path)) {
        std::unordered_set<std::string> sub_files = get_files_by_suffix(
          entry_path.string(),
          suffix);

        files.insert(sub_files.begin(), sub_files.end());
      }
    }
  }
  return files;
}

std::unordered_set<std::string> get_xml_files(const std::string& directory) {
  return get_files_by_suffix(directory, ".xml");
}

std::unordered_set<std::string> get_js_files(const std::string& directory) {
  return get_files_by_suffix(directory, ".js");
}

std::unordered_set<std::string> get_candidate_js_resources(
    const std::string& filename) {
  std::string file_contents = read_entire_file(filename);
  std::unordered_set<std::string> js_candidate_resources;
  if (file_contents.size()) {
    js_candidate_resources = extract_js_resources(file_contents);
  } else {
    fprintf(stderr, "Unable to read file: %s\n", filename.data());
  }
  return js_candidate_resources;
}


// Parses the content of all .js files and extracts all resources referenced.
std::unordered_set<uint32_t> get_js_resources_by_parsing(
    const std::string& directory,
    std::map<std::string, std::vector<uint32_t>> name_to_ids) {
  std::unordered_set<std::string> js_candidate_resources;
  std::unordered_set<uint32_t> js_resources;

  for (auto& f : get_js_files(directory)) {
    auto c = get_candidate_js_resources(f);
    js_candidate_resources.insert(c.begin(), c.end());
  }

  // The actual resources are the intersection of the real resources and the
  // candidate resources (since our current javascript processing produces
  // a few potential resource names that are not actually valid).
  // Look through the smaller set and compare it to the larger to efficiently
  // compute the intersection.
  if (name_to_ids.size() < js_candidate_resources.size()) {
    for (auto& p : name_to_ids) {
      if (js_candidate_resources.find(p.first) != js_candidate_resources.end()) {
        js_resources.insert(p.second.begin(), p.second.end());
      }
    }
  } else {
    for (auto& name : js_candidate_resources) {
      if (name_to_ids.find(name) != name_to_ids.end()) {
        js_resources.insert(name_to_ids[name].begin(), name_to_ids[name].end());
      }
    }
  }

  return js_resources;
}

std::unordered_set<uint32_t> get_resources_by_name_prefix(
    std::vector<std::string> prefixes,
    std::map<std::string, std::vector<uint32_t>> name_to_ids) {
  std::unordered_set<uint32_t> found_resources;

  for (auto& pair : name_to_ids) {
    for (auto& prefix : prefixes) {
      if (boost::algorithm::starts_with(pair.first, prefix)) {
        found_resources.insert(pair.second.begin(), pair.second.end());
      }
    }
  }

  return found_resources;
}

void ensure_file_contents(
    const std::string& file_contents,
    const std::string& filename) {
  if (!file_contents.size()) {
    fprintf(stderr, "Unable to read file: %s\n", filename.data());
    throw std::runtime_error("Unable to read file: " + filename);
  }
}

std::unordered_set<uint32_t> get_xml_reference_attributes(
    const std::string& filename) {
  std::string file_contents = read_entire_file(filename);
  ensure_file_contents(file_contents, filename);
  return extract_xml_reference_attributes(file_contents, filename);
}

bool is_drawable_attribute(
    android::ResXMLTree& parser,
    size_t attr_index) {
  size_t name_size;
  const char* attr_name_8 = parser.getAttributeName8(attr_index, &name_size);
  if (attr_name_8 != nullptr) {
    std::string name_str = std::string(attr_name_8, name_size);
    if (name_str.compare("drawable") == 0) {
      return true;
    }
  }

  const char16_t* attr_name_16 = parser.getAttributeName(attr_index, &name_size);
  if (attr_name_16 != nullptr) {
    android::String8 name_str_8 = android::String8(attr_name_16, name_size);
    std::string name_str = std::string(name_str_8.string(), name_size);
    if (name_str.compare("drawable") == 0) {
      return true;
    }
  }

  return false;
}

int inline_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, android::Res_value>& id_to_inline_value) {
  int num_values_inlined = 0;
  std::string file_contents = read_entire_file(filename);
  ensure_file_contents(file_contents, filename);
  bool made_change = false;

  android::ResXMLTree parser;
  parser.setTo(file_contents.data(), file_contents.size());
  if (parser.getError() != android::NO_ERROR) {
    throw std::runtime_error("Unable to read file: " + filename);
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      const size_t attr_count = parser.getAttributeCount();
      for (size_t i = 0; i < attr_count; ++i) {
        // Older versions of Android (below V5) do not allow inlining into
        // android:drawable attributes.
        if (is_drawable_attribute(parser, i)) {
          continue;
        }

        if (parser.getAttributeDataType(i) == android::Res_value::TYPE_REFERENCE) {
          android::Res_value outValue;
          parser.getAttributeValue(i, &outValue);
          if (outValue.data <= PACKAGE_RESID_START) {
            continue;
          }

          auto p = id_to_inline_value.find(outValue.data);
          if (p != id_to_inline_value.end()) {
            android::Res_value new_value = p->second;
            parser.setAttribute(i, new_value);
            ++num_values_inlined;
            made_change = true;
          }
        }
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);

  if (made_change) {
    write_entire_file(filename, file_contents);
  }

  return num_values_inlined;
}

void remap_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) {
  std::string file_contents = read_entire_file(filename);
  ensure_file_contents(file_contents, filename);
  bool made_change = false;

  android::ResXMLTree parser;
  parser.setTo(file_contents.data(), file_contents.size());
  if (parser.getError() != android::NO_ERROR) {
    throw std::runtime_error("Unable to read file: " + filename);
  }

  // Update embedded resource ID array
  size_t resIdCount = 0;
  uint32_t* resourceIds = parser.getResourceIds(&resIdCount);
  for (size_t i = 0; i < resIdCount; ++i) {
    auto id_search = kept_to_remapped_ids.find(resourceIds[i]);
    if (id_search != kept_to_remapped_ids.end()) {
      resourceIds[i] = id_search->second;
      made_change = true;
    }
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      const size_t attr_count = parser.getAttributeCount();
      for (size_t i = 0; i < attr_count; ++i) {
        if (parser.getAttributeDataType(i) == android::Res_value::TYPE_REFERENCE ||
            parser.getAttributeDataType(i) == android::Res_value::TYPE_ATTRIBUTE) {
          android::Res_value outValue;
          parser.getAttributeValue(i, &outValue);
          if (outValue.data > PACKAGE_RESID_START &&
              kept_to_remapped_ids.count(outValue.data)) {
            uint32_t new_value = kept_to_remapped_ids.at(outValue.data);
            if (new_value != outValue.data) {
              parser.setAttributeData(i, new_value);
              made_change = true;
            }
          }
        }
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);

  if (made_change) {
    write_entire_file(filename, file_contents);
  }
}

std::vector<std::string> find_layout_files(const std::string& apk_directory) {

  std::vector<std::string> layout_files;

  std::string root = apk_directory + std::string("/res");
  path_t res(root);

  if (exists(res) && is_directory(res)) {
    for (auto it = dir_iterator(res); it != dir_iterator(); ++it) {
      auto const& entry = *it;
      path_t entry_path = entry.path();

      if (is_directory(entry_path) &&
          starts_with(entry_path.filename().string().c_str(), "layout")) {
        for (auto lit = dir_iterator(entry_path); lit != dir_iterator(); ++lit) {
          auto const& layout_entry = *lit;
          path_t layout_path = layout_entry.path();
          if (is_regular_file(layout_path)) {
            layout_files.push_back(layout_path.string());
          }
        }
      }
    }
  }
  return layout_files;
}

std::unordered_set<std::string> get_layout_classes(const std::string& apk_directory) {
  std::vector<std::string> tmp = find_layout_files(apk_directory);
  std::unordered_set<std::string> all_classes;
  for (auto layout_file : tmp) {
    std::string contents = read_entire_file(layout_file);
    std::unordered_set<std::string> classes_from_layout = extract_classes_from_layout(contents);
    all_classes.insert(classes_from_layout.begin(), classes_from_layout.end());
  }
  return all_classes;
}

/**
 * Return a list of all the .so files in /lib
 */
std::vector<std::string> find_native_library_files(const std::string& apk_directory) {
  std::vector<std::string> native_library_files;
  std::string lib_root = apk_directory + std::string("/lib");
  std::string library_extension(".so");

  path_t lib(lib_root);

  if (exists(lib) && is_directory(lib)) {
    for (auto it = rdir_iterator(lib); it != rdir_iterator(); ++it) {
      auto const& entry = *it;
      path_t entry_path = entry.path();
      if (is_regular_file(entry_path) &&
          ends_with(entry_path.filename().string().c_str(),
                    library_extension.c_str())) {
        native_library_files.push_back(entry_path.string());
      }
    }
  }
  return native_library_files;
}

/**
 * Return all potential java class names located in native libraries.
 */
std::unordered_set<std::string> get_native_classes(const std::string& apk_directory) {
  std::vector<std::string> native_libs = find_native_library_files(apk_directory);
  std::unordered_set<std::string> all_classes;
  for (auto native_lib : native_libs) {
    std::string contents = read_entire_file(native_lib);
    std::unordered_set<std::string> classes_from_layout = extract_classes_from_native_lib(contents);
    all_classes.insert(classes_from_layout.begin(), classes_from_layout.end());
  }
  return all_classes;
}

void* map_file(
  const char* path,
  int& file_descriptor,
  size_t& length,
  const bool mode_write) {
  file_descriptor = open(path, mode_write ? O_RDWR : O_RDONLY);
  if (file_descriptor <= 0) {
    throw std::runtime_error("Failed to open arsc file");
  }
  struct stat st = {};
  if (fstat(file_descriptor, &st) == -1) {
    close(file_descriptor);
    throw std::runtime_error("Failed to get file length");
  }
  length = (size_t)st.st_size;
  int flags = PROT_READ;
  if (mode_write) {
    flags |= PROT_WRITE;
  }
  void* fp = mmap(nullptr, length, flags, MAP_SHARED, file_descriptor, 0);
  if (fp == MAP_FAILED) {
		close(file_descriptor);
		throw std::runtime_error("Failed to mmap file");
	}
  return fp;
}

void unmap_and_close(int file_descriptor, void* file_pointer, size_t length) {
  munmap(file_pointer, length);
  close(file_descriptor);
}

size_t write_serialized_data(
  const android::Vector<char>& cVec,
  int file_descriptor,
  void* file_pointer,
  const size_t& length) {
  size_t vec_size = cVec.size();
  if (vec_size > 0) {
    memcpy(file_pointer, &(cVec[0]), vec_size);
  }

  munmap(file_pointer, length);
  ftruncate(file_descriptor, cVec.size());
  close(file_descriptor);
  return vec_size > 0 ? vec_size : length;
}

int replace_in_xml_string_pool(
  const void* data,
  const size_t len,
  const std::map<std::string, std::string>& shortened_names,
  android::Vector<char>* out_data,
  size_t* out_num_renamed) {
  const auto chunk_size = sizeof(android::ResChunk_header);
  const auto pool_header_size =
    (uint16_t) sizeof(android::ResStringPool_header);

  // Validate the given bytes.
  if (len < chunk_size + pool_header_size) {
    return android::NOT_ENOUGH_DATA;
  }

  // Layout XMLs will have a ResChunk_header, followed by ResStringPool
  // representing each XML tag and attribute string.
  auto chunk = (android::ResChunk_header*) data;
  LOG_FATAL_IF(dtohl(chunk->size) != len, "Can't read header size");

  auto pool_ptr = (android::ResStringPool_header*) ((char*) data + chunk_size);
  if (dtohs(pool_ptr->header.type) != android::RES_STRING_POOL_TYPE) {
    return android::BAD_TYPE;
  }

  size_t num_replaced = 0;
  android::ResStringPool pool(pool_ptr, dtohl(pool_ptr->header.size));

  // Straight copy of everything after the string pool.
  android::Vector<char> serialized_nodes;
  auto start = chunk_size + pool_ptr->header.size;
  auto remaining = len - start;
  serialized_nodes.resize(remaining);
  void* start_ptr = ((char*) data) + start;
  memcpy((void*) &serialized_nodes[0], start_ptr, remaining);

  // Rewrite the strings
  android::Vector<char> serialized_pool;
  auto num_strings = pool_ptr->stringCount;

  // Make an empty pool.
  auto new_pool_header = android::ResStringPool_header {
    {
      htods(android::RES_STRING_POOL_TYPE),
      htods(pool_header_size),
      htodl(pool_header_size)
    },
    0,
    0,
    htodl(android::ResStringPool_header::UTF8_FLAG),
    0,
    0
  };
  android::ResStringPool new_pool(
    &new_pool_header,
    pool_header_size);

  for (size_t i = 0; i < num_strings; i++) {
    // Public accessors for strings are a bit of a foot gun. string8ObjectAt
    // does not reliably return lengths with chars outside the BMP. Work around
    // to get a proper String8.
    size_t u16_len;
    auto wide_chars = pool.stringAt(i, &u16_len);
    android::String16 s16(wide_chars, u16_len);
    android::String8 string8(s16);
    std::string existing_str(string8.string());

    auto replacement = shortened_names.find(existing_str);
    if (replacement == shortened_names.end()) {
      new_pool.appendString(string8);
    } else {
      android::String8 replacement8(replacement->second.c_str());
      new_pool.appendString(replacement8);
      num_replaced++;
    }
  }

  new_pool.serialize(serialized_pool);

  // Assemble
  push_short(*out_data, android::RES_XML_TYPE);
  push_short(*out_data, chunk_size);
  auto total_size =
    chunk_size + serialized_nodes.size() + serialized_pool.size();
  push_long(*out_data, total_size);

  out_data->appendVector(serialized_pool);
  out_data->appendVector(serialized_nodes);

  LOG_FATAL_IF(
    num_replaced == 0 && out_data->size() != len,
    "No strings replaced, but length mismatch!");
  *out_num_renamed = num_replaced;
  return android::OK;
}

int rename_classes_in_layout(
  const std::string& file_path,
  const std::map<std::string, std::string>& shortened_names,
  size_t* out_num_renamed,
  ssize_t* out_size_delta) {

  int file_desc;
  size_t len;
  auto fp = map_file(file_path.c_str(), file_desc, len, true);

  android::Vector<char> serialized;
  auto status = replace_in_xml_string_pool(
    fp,
    len,
    shortened_names,
    &serialized,
    out_num_renamed);

  if (status != android::OK) {
    unmap_and_close(file_desc, fp, len);
    return status;
  }

  write_serialized_data(serialized, file_desc, fp, len);
  *out_size_delta = serialized.size() - len;
  return android::OK;
}
