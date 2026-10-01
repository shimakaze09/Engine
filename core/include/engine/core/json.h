// Declares json types and APIs for the Engine core engine.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine::core {

/// Parsed JSON node handle; navigate it through JsonParser accessors.
struct JsonValue final {
  /// Enumerates type values used by the engine.
  enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  const char *begin = nullptr;
  const char *end = nullptr;
};

/// How a JsonWriter lays out its document.
enum class JsonLayout : std::uint8_t {
  /// No whitespace: in-memory snapshots and messages no person reads.
  Compact,
  /// One object member per line, indented two spaces a level, ending in a
  /// newline. An array of scalars (a vector, a colour) stays on its
  /// member's line; an array of objects puts each on its own lines. For
  /// authored documents people diff and merge, as Unity's text scenes put
  /// one property per line.
  Lines,
};

/// Appends JSON into a fixed buffer; failure is sticky (check ok()).
class JsonWriter final {
public:
  static constexpr std::size_t kBufferBytes = 256U * 1024U;
  static constexpr std::size_t kMaxBufferBytes = 16U * 1024U * 1024U;

  /// A writer that lays its document out as `layout` says, across resets.
  explicit JsonWriter(JsonLayout layout = JsonLayout::Compact) noexcept;
  ~JsonWriter() noexcept;

  JsonWriter(const JsonWriter &) = delete;
  JsonWriter &operator=(const JsonWriter &) = delete;
  JsonWriter(JsonWriter &&) = delete;
  JsonWriter &operator=(JsonWriter &&) = delete;

  /// Resets this object back to its reusable empty state.
  void reset() noexcept;

  /// Begins the requested operation or profiling range for object.
  void begin_object() noexcept;
  /// Ends the requested operation or profiling range for object.
  void end_object() noexcept;

  /// Begins the requested operation or profiling range for array.
  void begin_array(const char *key) noexcept;
  /// Begins the requested operation or profiling range for array.
  void begin_array() noexcept;
  /// Ends the requested operation or profiling range for array.
  void end_array() noexcept;

  /// Writes key data.
  void write_key(const char *key) noexcept;
  /// Writes one object member verbatim: `escapedKey` is a key as it is
  /// written between the quotes of a JSON document, `rawValue` a complete
  /// JSON value's source text. A writer uses it to carry, unchanged, a
  /// member a reader did not know (json_visit_unread_top_level_members).
  void write_raw_member(const char *escapedKey, std::size_t keyLength,
                        const char *rawValue, std::size_t valueLength) noexcept;
  /// Writes float data.
  void write_float(const char *key, float value) noexcept;
  /// Writes a double at round-trip precision (%.17g); non-finite fails.
  void write_double(const char *key, double value) noexcept;
  /// Writes a signed 64-bit integer.
  void write_int64(const char *key, std::int64_t value) noexcept;
  /// Writes uint data.
  void write_uint(const char *key, std::uint32_t value) noexcept;
  /// Writes uint64 data.
  void write_uint64(const char *key, std::uint64_t value) noexcept;
  /// Writes bool data.
  void write_bool(const char *key, bool value) noexcept;
  /// Writes string data.
  void write_string(const char *key, const char *value) noexcept;
  /// Writes a null value under `key`.
  void write_null(const char *key) noexcept;

  /// Writes float value data.
  void write_float_value(float value) noexcept;
  /// Writes uint value data.
  void write_uint_value(std::uint32_t value) noexcept;
  /// Writes uint64 value data.
  void write_uint64_value(std::uint64_t value) noexcept;
  /// Writes bool value data.
  void write_bool_value(bool value) noexcept;
  /// Writes string value data.
  void write_string_value(const char *value) noexcept;

  /// True once any append overflowed or was malformed (sticky).
  bool failed() const noexcept;
  /// True when the document is complete and no append failed.
  bool ok() const noexcept;
  /// Null-terminated JSON text (valid until reset).
  const char *result() const noexcept;
  /// Byte length of result(), excluding the terminator.
  std::size_t result_size() const noexcept;

private:
  /// Enumerates container kind values used by the engine.
  enum class ContainerKind : std::uint8_t { Object, Array };

  /// Tracks one open object/array while writing (comma placement).
  struct ContainerState final {
    ContainerKind kind = ContainerKind::Object;
    bool firstElement = true;
    bool expectingValue = false;
    /// An array holding an object or array closes on its own line.
    bool hasContainerChild = false;
  };

  /// Places the next value: its separator and, in the Lines layout, the
  /// line break a `container` element of an array starts with.
  bool begin_value(bool container = false) noexcept;
  /// In the Lines layout, starts a new line indented `depth` levels.
  bool append_line_break(std::size_t depth) noexcept;
  /// Grows usage bookkeeping; false (sticky failure) on overflow.
  bool ensure_capacity(std::size_t additionalBytes) noexcept;
  /// Appends one raw character.
  bool append_char(char value) noexcept;
  /// Appends `size` raw bytes.
  bool append_bytes(const char *value, std::size_t size) noexcept;
  /// Appends a raw null-terminated string (no escaping).
  bool append_cstr(const char *value) noexcept;
  /// Appends a string with JSON escaping applied.
  bool append_escaped(const char *value) noexcept;
  /// Appends a float as the fewest significant digits that read back as
  /// the same float, so an authored 0.15 is written as 0.15.
  bool append_float(float value) noexcept;
  /// Appends a double in round-trip-stable decimal form.
  bool append_double(double value) noexcept;
  /// Appends a signed 64-bit integer.
  bool append_int64(std::int64_t value) noexcept;
  /// Appends an unsigned 32-bit integer.
  bool append_uint(std::uint32_t value) noexcept;
  /// Appends an unsigned 64-bit integer.
  bool append_uint64(std::uint64_t value) noexcept;
  /// Pushes an item onto the owning stack or queue for container.
  bool push_container(ContainerKind kind) noexcept;
  /// Pops an item from the owning stack or queue for container.
  bool pop_container(ContainerKind kind) noexcept;

  std::unique_ptr<char[]> m_buffer{};
  std::size_t m_capacity = 0U;
  std::array<ContainerState, 32U> m_stack{};
  std::size_t m_pos = 0U;
  std::size_t m_depth = 0U;
  bool m_failed = false;
  JsonLayout m_layout = JsonLayout::Compact;
};

/// Replaces (or inserts) the value of one top-level field in a JSON
/// object document while preserving every other byte — unknown and
/// forward-compatible fields, ordering, and formatting all survive.
/// The document and `valueText` are both validated with
/// JsonParser before any splice; documents whose top-level keys contain
/// escape sequences are refused (keys match as raw bytes, and a decoded
/// alias of `fieldName` could otherwise duplicate), and `fieldName` is
/// spliced verbatim, so it must be non-empty and free of quotes,
/// backslashes, and control bytes. False on malformed input, non-object
/// roots, or insufficient output capacity; the output buffer is
/// null-terminated on success.
bool json_replace_top_level_field(const char *documentText,
                                  std::size_t documentLength,
                                  const char *fieldName,
                                  const char *valueText, char *outBuffer,
                                  std::size_t outCapacity,
                                  std::size_t *outLength) noexcept;

/// Remembers which object members a reader looked up, so a loader can name
/// the keys it never read: a misspelt component, a field from a newer
/// build, a key a codec forgot. It holds one bit per byte of the document,
/// keyed by where each member's value starts, so recording costs O(1) and
/// can never run out of room however often a field is read.
class JsonReadTracker final {
public:
  /// Sizes the tracker for the document `text` and clears it. False, with
  /// the tracker unarmed, when the memory is unavailable or the text empty.
  bool reset_for(const char *text, std::size_t length) noexcept;
  /// True once reset_for succeeded.
  bool armed() const noexcept { return m_bits != nullptr; }
  /// Marks the value starting at `valueBegin` as read; a pointer outside
  /// the document is ignored.
  void record(const char *valueBegin) noexcept;
  /// True when record() saw `valueBegin`.
  bool was_read(const char *valueBegin) const noexcept;

private:
  std::unique_ptr<std::uint8_t[]> m_bits{};
  const char *m_text = nullptr;
  std::size_t m_length = 0U;
};

/// Called once per unread member with its path, keys joined by '.' and
/// array elements as [i]: "entities[3].components.Colider".
using JsonUnreadVisitor = void (*)(const char *path, void *userData) noexcept;

/// Visits every object member under `root` (an object) that no
/// get_object_field call looked up while `tracker` was set on the parser.
/// It descends into the objects of members that were read and into the
/// object elements of arrays that were read; an unread member is visited
/// once and not entered. Paths longer than 255 bytes are cut. Returns the
/// number of unread members, or 0 with nothing visited when the tracker
/// is unarmed.
std::size_t json_visit_unread_members(const JsonValue &root,
                                      const JsonReadTracker &tracker,
                                      JsonUnreadVisitor visit,
                                      void *userData) noexcept;

/// Logs one Warning per member under `root` that no lookup read
/// (json_visit_unread_members), worded "<document>: key '<path>' is not
/// read by this build; <consequence>". For a reader whose document has no
/// ValidationReport. Returns the number logged.
std::size_t json_log_unread_members(const JsonValue &root,
                                    const JsonReadTracker &tracker,
                                    const char *channel, const char *document,
                                    const char *consequence) noexcept;

class JsonParser;

/// Called once per carried member with its key as written between the
/// quotes and its value's exact source text (a string with its quotes).
using JsonRawMemberVisitor = void (*)(const char *key, std::size_t keyLength,
                                      const char *value,
                                      std::size_t valueLength,
                                      void *userData) noexcept;

/// Visits each top-level member of `object` that no get_object_field call
/// read while `tracker` was set on `parser`, skipping one whose key a read
/// member shares (a repeated key the reader already resolved). A writer
/// re-emits these verbatim, so an older build rewriting a document keeps
/// the members a newer build added. Returns the number visited; 0 when the
/// tracker is unarmed.
std::size_t json_visit_unread_top_level_members(const JsonParser &parser,
                                                const JsonValue &object,
                                                const JsonReadTracker &tracker,
                                                JsonRawMemberVisitor visit,
                                                void *userData) noexcept;

/// Parses JSON into fixed storage; query values via JsonValue handles.
class JsonParser final {
public:
  /// Parses text into the engine representation.
  bool parse(const char *input, std::size_t length) noexcept;
  /// Root value of the last successful parse, or nullptr.
  const JsonValue *root() const noexcept;
  /// Byte offset where the last failed parse() stopped, so a refusal can
  /// say where a document is broken; 0 after a successful parse.
  std::size_t error_offset() const noexcept { return m_errorOffset; }

  // Pointer-returning navigation helpers are transient: do not keep returned
  // pointers across additional pointer-returning navigation calls. They
  // draw on a fixed scratch ring of kScratchSlots values that parse()
  // resets; past it they return nullptr like a missing field, so an
  // unbounded walk (every element of an authored array) uses the by-value
  // overloads, which never touch the ring.
  static constexpr std::size_t kScratchSlots = 1024U;
  const JsonValue *get_object_field(const JsonValue &object,
                                    const char *fieldName) const noexcept;
  /// Finds a field by name in an object; false when missing.
  bool get_object_field(const JsonValue &object, const char *fieldName,
                        JsonValue *outValue) const noexcept;
  /// Records every member get_object_field finds in `tracker` from now on
  /// (nullptr stops). The tracker must be sized for the parsed text.
  void set_read_tracker(JsonReadTracker *tracker) noexcept {
    m_readTracker = tracker;
  }

  /// Element at index, or nullptr out of range.
  const JsonValue *get_array_element(const JsonValue &array,
                                     std::size_t index) const noexcept;
  /// Copies the element at index; false out of range.
  bool get_array_element(const JsonValue &array, std::size_t index,
                         JsonValue *outValue) const noexcept;

  /// Number of elements in an array value (0 for non-arrays).
  std::size_t array_size(const JsonValue &array) const noexcept;
  /// Number of members of an object value (0 for non-objects), duplicate
  /// keys counted each time they appear.
  std::size_t object_size(const JsonValue &object) const noexcept;
  /// Copies the member at `index`, in document order: its key as a String
  /// value (copy_string decodes it) and its value. False out of range. A
  /// strict reader walks these to refuse keys it does not know and keys
  /// that appear twice, which get_object_field cannot see.
  bool get_object_member(const JsonValue &object, std::size_t index,
                         JsonValue *outKey, JsonValue *outValue) const noexcept;
  /// Elements parsed by get_array_element to reach requested indices since
  /// parse(); an ascending walk over N elements, nested walks included,
  /// costs O(N) of these. The unit tests pin the scaling with it.
  std::size_t array_element_scans() const noexcept {
    return m_arrayElementScans;
  }
  /// True once a pointer-returning navigation call since parse() found
  /// the scratch ring full and returned nullptr; the first such call also
  /// logs a warning, once per parse.
  bool scratch_exhausted() const noexcept { return m_scratchExhausted; }

  /// Numeric value as float; false for non-numbers.
  bool as_float(const JsonValue &value, float *outValue) const noexcept;
  /// Numeric value as double; false for non-numbers or non-finite results.
  bool as_double(const JsonValue &value, double *outValue) const noexcept;
  /// Numeric value as int64; false unless the token is an integer literal
  /// (digits with an optional leading minus, no fraction or exponent) in
  /// range, so an integer written by write_int64 reads back exactly.
  bool as_int64(const JsonValue &value, std::int64_t *outValue) const noexcept;
  /// Reads exactly expectedCount floats from a JSON array; the element
  /// count must match exactly and every element must be a number.
  bool as_float_array(const JsonValue &value, float *outValues,
                      std::size_t expectedCount) const noexcept;
  /// Numeric value as uint32; false for non-numbers or out of range.
  bool as_uint(const JsonValue &value, std::uint32_t *outValue) const noexcept;
  /// Numeric value as uint64; false for non-numbers or out of range.
  bool as_uint64(const JsonValue &value,
                 std::uint64_t *outValue) const noexcept;
  /// Boolean value; false for non-booleans.
  bool as_bool(const JsonValue &value, bool *outValue) const noexcept;
  /// Unescaped string view (begin + length); false for non-strings.
  bool as_string(const JsonValue &value, const char **outBegin,
                 std::size_t *outLength) const noexcept;
  /// Copies a decoded string value into a null-terminated output buffer,
  /// truncating on overflow; outRequiredLength reports the full decoded length.
  bool copy_string(const JsonValue &value, char *out, std::size_t outCapacity,
                   std::size_t *outRequiredLength = nullptr) const noexcept;
  /// Copies a decoded string value; fails with a cleared buffer instead of
  /// truncating when the decoded string does not fit outCapacity.
  bool copy_string_strict(const JsonValue &value, char *out,
                          std::size_t outCapacity) const noexcept;

private:
  /// Pushes an item onto the owning stack or queue for scratch.
  const JsonValue *push_scratch(const JsonValue &value) const noexcept;

  const char *m_input = nullptr;
  JsonReadTracker *m_readTracker = nullptr;
  std::size_t m_errorOffset = 0U;
  std::size_t m_length = 0U;
  JsonValue m_root{};
  bool m_hasRoot = false;
  mutable std::array<JsonValue, kScratchSlots> m_scratch =
      std::array<JsonValue, kScratchSlots>();
  mutable std::size_t m_scratchCursor = 0U;
  mutable bool m_scratchExhausted = false;
  // Sequential-access memos for get_array_element: the lazy representation
  // rescans an array from its opening bracket, which made per-index walks
  // quadratic; resuming from the last returned element makes ascending
  // walks amortized O(1).
  // One entry per recently walked array, because a single entry was
  // evicted by every nested walk — each Transform's position array — which
  // made the outer entity loop quadratic again for every real scene.
  // Keyed by the array's byte range; a full table evicts the
  // entry spanning the fewest bytes, never the enclosing array.
  // Single-threaded like the scratch ring; invalidated by parse().
  struct ArrayMemo final {
    const char *begin = nullptr;
    const char *end = nullptr;
    const char *cursor = nullptr;
    std::size_t index = 0U;
  };
  static constexpr std::size_t kArrayMemoEntries = 8U;
  mutable std::array<ArrayMemo, kArrayMemoEntries> m_arrayMemos{};
  mutable std::size_t m_arrayElementScans = 0U;
};

} // namespace engine::core
