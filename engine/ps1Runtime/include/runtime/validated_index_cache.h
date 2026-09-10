#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

namespace ps1 {

// A cheap lookup for changing owned source collections. Retains indices, not
// pointers: validate the cached index AND identity against the current span,
// including on the fallback path. Callers own synchronization and identity
// uniqueness within the supplied scope. Resolve outer ownership scopes first.
// The returned pointer is borrowed only until the collection next mutates.
class ValidatedIndexCache {
public:
  template <typename T, typename Matches>
  T *find(std::span<T> values, Matches &&matches) {
    if (index_ < values.size() && matches(values[index_]))
      return &values[index_];
    const auto found = std::find_if(values.begin(), values.end(),
                                    std::forward<Matches>(matches));
    if (found == values.end())
      return nullptr;
    index_ = static_cast<std::size_t>(found - values.begin());
    return &*found;
  }

private:
  std::size_t index_ = 0;
};

} // namespace ps1
