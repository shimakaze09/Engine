// Implements the glTF accessor bound check shared by mesh and animation
// import.

#include "gltf_bounds.h"

#include <cstdio>
#include <limits>

bool accessor_count_is_cookable(const cgltf_accessor *accessor,
                                std::size_t elementsPerItem,
                                const char *what) {
  if (accessor == nullptr) {
    return false;
  }

  // An accessor with neither a view nor sparse data is unbounded: nothing
  // in the file constrains its count, and cgltf reads it as zeros.
  if ((accessor->buffer_view == nullptr) && (accessor->is_sparse == 0)) {
    std::fprintf(stderr,
                 "error: %s accessor has no bufferView and is not sparse; "
                 "its count cannot be bounded against any buffer\n",
                 what);
    return false;
  }

  const auto count = static_cast<unsigned long long>(accessor->count);
  if (count > static_cast<unsigned long long>(kMaxCookedElementCount)) {
    std::fprintf(stderr,
                 "error: %s accessor declares %llu elements, above the %llu "
                 "the cooked format stores\n",
                 what, count,
                 static_cast<unsigned long long>(kMaxCookedElementCount));
    return false;
  }

  if ((elementsPerItem != 0U) &&
      (static_cast<std::size_t>(accessor->count) >
       (std::numeric_limits<std::size_t>::max() / elementsPerItem))) {
    std::fprintf(stderr,
                 "error: %s accessor's %llu elements overflow the buffer "
                 "size they would be expanded into\n",
                 what, count);
    return false;
  }

  return true;
}
