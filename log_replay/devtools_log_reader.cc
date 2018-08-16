// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/test/chromedriver/log_replay/devtools_log_reader.h"

#include <iostream>
#include <string>

#include "base/logging.h"
#include "base/strings/pattern.h"

namespace {
// Parses the word (id=X) and just returns the id number
int GetId(std::istringstream& header_stream) {
  int id = 0;
  header_stream.ignore(5);  // ignore the " (id=" characters
  header_stream >> id;
  header_stream.ignore(1);  // ignore the final parenthesis
  return id;
}
}  // namespace

LogEntry::LogEntry(std::istringstream& header_stream) {
  error = false;
  std::string protocol_type_string;
  header_stream >> protocol_type_string;  // "HTTP" or "WebSocket"
  if (protocol_type_string == "HTTP") {
    protocol_type = HTTP;
  } else if (protocol_type_string == "WebSocket") {
    protocol_type = WebSocket;
  } else {
    error = true;
    LOG(ERROR) << "Could not read protocol from log entry header.";
    return;
  }

  std::string event_type_string;
  header_stream >> event_type_string;

  if (event_type_string == "Response:") {
    event_type = response;
  } else if (event_type_string == "Command:" ||
             event_type_string == "Request:") {
    event_type = request;
  } else if (event_type_string == "Event:") {
    event_type = event;
  } else {
    error = true;
    LOG(ERROR) << "Could not read event type from log entry header.";
    return;
  }

  if (!(protocol_type == HTTP && event_type == response)) {
    header_stream >> command_name;
    if (command_name == "") {
      error = true;
      LOG(ERROR) << "Could not read command name from log entry header";
      return;
    }
    if (protocol_type != HTTP) {
      id = GetId(header_stream);
      if (id == 0) {
        error = true;
        LOG(ERROR) << "Could not read sequential id from log entry header.";
        return;
      }
    }
  }
}

LogEntry::~LogEntry() {}

DevToolsLogReader::DevToolsLogReader(const base::FilePath& log_path)
    : log_file(log_path.value().c_str(), std::ios::in) {}

DevToolsLogReader::~DevToolsLogReader() {}

bool DevToolsLogReader::IsHeader(std::istringstream& header_stream) {
  std::string word;
  header_stream >> word;  // preamble
  if (!base::MatchPattern(word, "[??????????.???][DEBUG]:")) {
    return false;
  }
  header_stream >> word;  // "DevTools" for DevTools commands/responses/events
  bool result = word == "DevTools";
  return result;
}

std::unique_ptr<LogEntry> DevToolsLogReader::GetNext(
    LogEntry::Protocol protocol_type) {
  std::string next_line;
  while (true) {
    if (log_file.eof())
      return nullptr;
    std::getline(log_file, next_line);

    std::istringstream next_line_stream(next_line);
    if (IsHeader(next_line_stream)) {
      std::unique_ptr<LogEntry> log_entry =
          std::make_unique<LogEntry>(next_line_stream);
      if (log_entry->error) {
        return nullptr;  // helpful error message already logged
      }
      if (log_entry->protocol_type != protocol_type)
        continue;
      if (!(log_entry->event_type == LogEntry::EventType::request &&
            log_entry->protocol_type == LogEntry::Protocol::HTTP)) {
        log_entry->payload = GetJSONString(next_line_stream);
        if (log_entry->payload == "") {
          LOG(ERROR) << "Problem parsing JSON from log file";
          return nullptr;
        }
      }
      return log_entry;
    }
  }
}

std::string DevToolsLogReader::GetJSONString(
    std::istringstream& header_stream) {
  std::string next_line, json;

  int opening_char_count = 0;
  std::getline(header_stream, next_line);
  next_line = next_line.substr(1);
  char opening_char = next_line[0];
  char closing_char;
  switch (opening_char) {
    case '{':
      closing_char = '}';
      break;
    case '[':
      closing_char = ']';
      break;
    default:
      return "";
  }
  while (true) {
    json += next_line;
    opening_char_count += CountChar(next_line, opening_char, closing_char);
    if (opening_char_count == 0)
      break;
    if (log_file.eof())
      return "";
    getline(log_file, next_line);
  }
  return json;
}

int DevToolsLogReader::CountChar(const std::string& line,
                                 char opening_char,
                                 char closing_char) const {
  bool in_quote = false;
  int total = 0;
  for (size_t i = 0; i < line.length(); i++) {
    if (!in_quote && line[i] == opening_char)
      total++;
    if (!in_quote && line[i] == closing_char)
      total--;
    if (line[i] == '"' && (i == 0 || line[i - 1] != '\\'))
      in_quote = !in_quote;
  }
  return total;
}
