#include "runtime/input/input_script.h"

#include "runtime/input/input.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace ps1::input {

namespace {

uint16_t buttonFromName(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static const std::unordered_map<std::string, uint16_t> buttons{
      {"select", BTN_SELECT},     {"l3", BTN_L3},
      {"r3", BTN_R3},             {"start", BTN_START},
      {"up", BTN_UP},             {"right", BTN_RIGHT},
      {"down", BTN_DOWN},         {"left", BTN_LEFT},
      {"l2", BTN_L2},             {"r2", BTN_R2},
      {"l1", BTN_L1},             {"r1", BTN_R1},
      {"triangle", BTN_TRIANGLE}, {"circle", BTN_CIRCLE},
      {"cross", BTN_CROSS},       {"square", BTN_SQUARE},
  };
  const auto found = buttons.find(name);
  return found == buttons.end() ? 0 : found->second;
}

bool fail(std::string *error, std::size_t line, const std::string &message) {
  if (error)
    *error = "line " + std::to_string(line) + ": " + message;
  return false;
}

} // namespace

bool InputScript::parse(std::string_view text, std::string *error) {
  std::vector<Interval> parsed;
  std::istringstream source{std::string(text)};
  std::string line;
  std::size_t lineNumber = 0;
  while (std::getline(source, line)) {
    ++lineNumber;
    const auto comment = line.find('#');
    if (comment != std::string::npos)
      line.resize(comment);

    std::istringstream fields(line);
    Interval interval;
    std::string buttonName;
    std::string first, last, portText, trailing;
    if (!(fields >> first))
      continue;
    const auto number = [](const std::string &token, auto &value) {
      const auto result =
          std::from_chars(token.data(), token.data() + token.size(), value);
      return result.ec == std::errc{} &&
             result.ptr == token.data() + token.size();
    };
    if (!(fields >> last >> buttonName) ||
        !number(first, interval.firstVblank) ||
        !number(last, interval.lastVblank))
      return fail(error, lineNumber,
                  "expected: first_vblank last_vblank button [port]");
    if (fields >> portText) {
      if (!number(portText, interval.port) || interval.port > 1)
        return fail(error, lineNumber, "controller port must be 0 or 1");
    }
    if (fields >> trailing)
      return fail(error, lineNumber, "unexpected trailing field");
    if (interval.lastVblank < interval.firstVblank)
      return fail(error, lineNumber, "interval ends before it starts");
    interval.button = buttonFromName(buttonName);
    if (interval.button == 0)
      return fail(error, lineNumber, "unknown PS1 button '" + buttonName + "'");
    parsed.push_back(interval);
  }

  intervals_ = std::move(parsed);
  if (error)
    error->clear();
  return true;
}

bool InputScript::loadFile(const std::filesystem::path &path,
                           std::string *error) {
  std::ifstream source(path);
  if (!source) {
    if (error)
      *error = "cannot open '" + path.string() + "'";
    return false;
  }
  std::ostringstream contents;
  contents << source.rdbuf();
  return parse(contents.str(), error);
}

void InputScript::apply(uint64_t vblank, InputController &input) const {
  uint16_t masks[2]{};
  for (const Interval &interval : intervals_) {
    if (vblank >= interval.firstVblank && vblank <= interval.lastVblank)
      masks[interval.port] |= interval.button;
  }
  input.setScriptedButtons(masks[0], 0);
  input.setScriptedButtons(masks[1], 1);
}

} // namespace ps1::input
