#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ps1::source_selection {

// These are caller-established alternatives, NOT candidates inferred here.
// Source IDs, not coordinates, cameras, visibility or quality scores, determine
// execution. The game adapter owns the evidence authorizing each rule.
struct Rule {
  std::vector<uint32_t> sources;
  std::vector<uint32_t> targets;
};

struct Program {
  // Zero means selected; otherwise one plus the rule index. Indexed access is
  // shared by all draw lanes. All replacement targets are terminal/selected.
  std::vector<uint32_t> owner;
  std::vector<std::vector<uint32_t>> terminals;
  std::vector<uint32_t> selectedSources;
  uint32_t chainedRules = 0;
};

// Compile an immutable resource's explicit replacement DAG once, upstream of
// draw construction. A -> B -> C excludes BOTH A and B, never abandoning A
// because B also has a replacement. No recursive stack or unchecked expansion.
// Malformed/ambiguous/cyclic rules reject the complete program atomically.
inline std::optional<Program> compile(uint32_t sourceCount,
                                      std::span<const Rule> rules,
                                      uint32_t maxSources = 65536u,
                                      uint32_t maxTerminals = 1048576u) {
  if (sourceCount > maxSources || rules.size() > sourceCount)
    return std::nullopt;
  Program result;
  result.owner.resize(sourceCount);
  result.terminals.resize(rules.size());
  uint64_t inputTargets = 0;
  for (uint32_t i = 0; i < rules.size(); ++i) {
    const auto &rule = rules[i];
    inputTargets += rule.targets.size();
    if (rule.sources.empty() || rule.targets.empty() ||
        inputTargets > maxTerminals)
      return std::nullopt;
    for (uint32_t source : rule.sources) {
      if (source >= sourceCount || result.owner[source] != 0)
        return std::nullopt;
      result.owner[source] = i + 1;
    }
    for (uint32_t target : rule.targets)
      if (target >= sourceCount)
        return std::nullopt;
  }
  std::vector<std::vector<uint32_t>> dependencies(rules.size());
  std::vector<std::vector<uint32_t>> consumers(rules.size());
  std::vector<uint32_t> unresolved(rules.size()), ready;
  for (uint32_t i = 0; i < rules.size(); ++i) {
    for (uint32_t target : rules[i].targets)
      if (const auto owner = result.owner[target])
        dependencies[i].push_back(owner - 1);
    auto &deps = dependencies[i];
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    unresolved[i] = deps.size();
    result.chainedRules += !deps.empty();
    if (deps.empty())
      ready.push_back(i);
    for (uint32_t dependency : deps)
      consumers[dependency].push_back(i);
  }
  std::vector<uint8_t> included(sourceCount);
  uint64_t terminalCount = 0;
  uint32_t completed = 0;
  while (!ready.empty()) {
    const uint32_t i = ready.back();
    ready.pop_back();
    auto &targets = result.terminals[i];
    const auto add = [&](uint32_t target) {
      if (included[target])
        return true;
      if (++terminalCount > maxTerminals)
        return false;
      included[target] = 1;
      targets.push_back(target);
      return true;
    };
    for (uint32_t target : rules[i].targets) {
      if (const auto owner = result.owner[target]) {
        for (uint32_t terminal : result.terminals[owner - 1])
          if (!add(terminal))
            return std::nullopt;
      } else if (!add(target))
        return std::nullopt;
    }
    if (targets.empty())
      return std::nullopt;
    std::sort(targets.begin(), targets.end());
    for (uint32_t target : targets)
      included[target] = 0;
    ++completed;
    for (uint32_t consumer : consumers[i])
      if (--unresolved[consumer] == 0)
        ready.push_back(consumer);
  }
  if (completed != rules.size())
    return std::nullopt; // Cycle, including a direct self replacement.
  for (uint32_t source = 0; source < sourceCount; ++source)
    if (result.owner[source] == 0)
      result.selectedSources.push_back(source);
  return result;
}

} // namespace ps1::source_selection
