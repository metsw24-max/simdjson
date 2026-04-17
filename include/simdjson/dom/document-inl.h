#ifndef SIMDJSON_DOCUMENT_INL_H
#define SIMDJSON_DOCUMENT_INL_H

// Inline implementations go in here.

#include "simdjson/dom/base.h"
#include "simdjson/dom/document.h"
#include "simdjson/dom/element-inl.h"
#include "simdjson/internal/tape_ref-inl.h"
#include "simdjson/internal/jsonformatutils.h"

#include <cstring>
#include <limits>

namespace simdjson {
namespace dom {

//
// document inline implementation
//
inline element document::root() const noexcept {
  return element(internal::tape_ref(this, 1));
}
simdjson_warn_unused
inline size_t document::capacity() const noexcept {
  return allocated_capacity;
}

simdjson_warn_unused
inline error_code document::allocate(size_t capacity) noexcept {
  if (capacity == 0) {
    string_buf.reset();
    tape.reset();
    allocated_capacity = 0;
    return SUCCESS;
  }
  if (capacity > SIMDJSON_MAXSIZE_BYTES) {
    return CAPACITY;
  }

  auto add_checked = [](size_t lhs, size_t rhs, size_t &sum) noexcept -> bool {
    if (lhs > (std::numeric_limits<size_t>::max)() - rhs) {
      return false;
    }
    sum = lhs + rhs;
    return true;
  };

  auto roundup64_checked = [](size_t value, size_t &rounded) noexcept -> bool {
    constexpr size_t roundup_mask = 64 - 1;
    if (value > (std::numeric_limits<size_t>::max)() - roundup_mask) {
      return false;
    }
    rounded = SIMDJSON_ROUNDUP_N(value, 64);
    return true;
  };

  auto string_bytes_for_capacity = [](size_t input_capacity, size_t &string_bytes) noexcept -> bool {
    // Compute floor(5 * input_capacity / 3) without overflow.
    const size_t capacity_div3 = input_capacity / 3;
    const size_t capacity_mod3 = input_capacity % 3;
    if (capacity_div3 > (std::numeric_limits<size_t>::max)() / 5) {
      return false;
    }
    string_bytes = capacity_div3 * 5 + ((capacity_mod3 * 5) / 3);
    return true;
  };

  // a pathological input like "[[[[..." would generate capacity tape elements, so
  // need a capacity of at least capacity + 1, but it is also possible to do
  // worse with "[7,7,7,7,6,7,7,7,6,7,7,6,[7,7,7,7,6,7,7,7,6,7,7,6,7,7,7,7,7,7,6"
  //where capacity + 1 tape elements are
  // generated, see issue https://github.com/simdjson/simdjson/issues/345
  size_t tape_elements;
  if (!add_checked(capacity, size_t(3), tape_elements)) {
    return CAPACITY;
  }
  size_t tape_capacity;
  if (!roundup64_checked(tape_elements, tape_capacity)) {
    return CAPACITY;
  }

  // a document with only zero-length strings... could have capacity/3 string
  // and we would need capacity/3 * 5 bytes on the string buffer
  size_t string_bytes;
  if (!string_bytes_for_capacity(capacity, string_bytes)) {
    return CAPACITY;
  }

  size_t padded_string_bytes;
  if (!add_checked(string_bytes, size_t(SIMDJSON_PADDING), padded_string_bytes)) {
    return CAPACITY;
  }

  size_t string_capacity;
  if (!roundup64_checked(padded_string_bytes, string_capacity)) {
    return CAPACITY;
  }

  string_buf.reset( new (std::nothrow) uint8_t[string_capacity]);
  tape.reset(new (std::nothrow) uint64_t[tape_capacity]);
  if(!(string_buf && tape)) {
    allocated_capacity = 0;
    string_buf.reset();
    tape.reset();
    return MEMALLOC;
  }
  // Technically the allocated_capacity might be larger than capacity
  // so the next line is pessimistic.
  allocated_capacity = capacity;
  return SUCCESS;
}

inline bool document::dump_raw_tape(std::ostream &os) const noexcept {
  uint32_t string_length;
  size_t tape_idx = 0;
  uint64_t tape_val = tape[tape_idx];
  uint8_t type = uint8_t(tape_val >> 56);
  os << tape_idx << " : " << type;
  tape_idx++;
  size_t how_many = 0;
  if (type == 'r') {
    how_many = size_t(tape_val & internal::JSON_VALUE_MASK);
  } else {
    // Error: no starting root node?
    return false;
  }
  os << "\t// pointing to " << how_many << " (right after last node)\n";
  uint64_t payload;
  for (; tape_idx < how_many; tape_idx++) {
    os << tape_idx << " : ";
    tape_val = tape[tape_idx];
    payload = tape_val & internal::JSON_VALUE_MASK;
    type = uint8_t(tape_val >> 56);
    switch (type) {
    case '"': // we have a string
      os << "string \"";
      std::memcpy(&string_length, string_buf.get() + payload, sizeof(uint32_t));
      os << internal::escape_json_string(std::string_view(
        reinterpret_cast<const char *>(string_buf.get() + payload + sizeof(uint32_t)),
        string_length
      ));
      os << '"';
      os << '\n';
      break;
    case 'l': // we have a long int
      if (tape_idx + 1 >= how_many) {
        return false;
      }
      os << "integer " << static_cast<int64_t>(tape[++tape_idx]) << "\n";
      break;
    case 'u': // we have a long uint
      if (tape_idx + 1 >= how_many) {
        return false;
      }
      os << "unsigned integer " << tape[++tape_idx] << "\n";
      break;
    case 'd': // we have a double
      os << "float ";
      if (tape_idx + 1 >= how_many) {
        return false;
      }
      double answer;
      std::memcpy(&answer, &tape[++tape_idx], sizeof(answer));
      os << answer << '\n';
      break;
    case 'n': // we have a null
      os << "null\n";
      break;
    case 't': // we have a true
      os << "true\n";
      break;
    case 'f': // we have a false
      os << "false\n";
      break;
    case '{': // we have an object
      os << "{\t// pointing to next tape location " << uint32_t(payload)
         << " (first node after the scope), "
         << " saturated count "
         << ((payload >> 32) & internal::JSON_COUNT_MASK)<< "\n";
      break;    case '}': // we end an object
      os << "}\t// pointing to previous tape location " << uint32_t(payload)
         << " (start of the scope)\n";
      break;
    case '[': // we start an array
      os << "[\t// pointing to next tape location " << uint32_t(payload)
         << " (first node after the scope), "
         << " saturated count "
         << ((payload >> 32) & internal::JSON_COUNT_MASK)<< "\n";
      break;
    case ']': // we end an array
      os << "]\t// pointing to previous tape location " << uint32_t(payload)
         << " (start of the scope)\n";
      break;
    case 'r': // we start and end with the root node
      // should we be hitting the root node?
      return false;
    case 'Z': // we have a big integer
      os << "bigint ";
      std::memcpy(&string_length, string_buf.get() + payload, sizeof(uint32_t));
      os << std::string_view(
        reinterpret_cast<const char *>(string_buf.get() + payload + sizeof(uint32_t)),
        string_length
      );
      os << '\n';
      break;
    default:
      return false;
    }
  }
  tape_val = tape[tape_idx];
  payload = tape_val & internal::JSON_VALUE_MASK;
  type = uint8_t(tape_val >> 56);
  os << tape_idx << " : " << type << "\t// pointing to " << payload
     << " (start root)\n";
  return true;
}

} // namespace dom
} // namespace simdjson

#endif // SIMDJSON_DOCUMENT_INL_H
