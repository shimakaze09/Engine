<!-- Binding commenting standard for this repository. CLAUDE.md's comment rules reference it; tools/check_comment_quality.py enforces its mechanically checkable classes. -->

# Enterprise-Grade C++ and CMake Commenting Guidelines

## 1. Purpose

For a large-scale C++ project, comments are not merely explanations of individual lines of code. They are part of the project's engineering documentation, architecture, build orchestration, maintenance strategy, and developer onboarding process.

Large C++ systems often contain hundreds of source files distributed across multiple libraries, executables, platforms, and subsystems. They may also rely heavily on CMake for dependency management, feature configuration, platform abstraction, code generation, testing, packaging, and installation.

This guide defines a consistent commenting standard for production C++ and CMake projects.

The goals are to:

* explain why important code exists;
* document architectural assumptions and boundaries;
* describe public interfaces and contracts;
* record non-obvious constraints;
* explain complex algorithms and low-level behavior;
* document ownership, lifetime, threading, and synchronization rules;
* make CMake configuration understandable;
* reduce the time required for code review and onboarding;
* prevent outdated or misleading comments;
* keep comments useful without duplicating the source code.

The core principle is:

Code should explain what happens. Comments should explain why it happens, what assumptions it depends on, and what must remain true.

## 2. General Commenting Principles

### 2.1 Explain "Why", Not "What"

Avoid comments that simply translate C++ syntax into English.

Bad:

```cpp
// Increment the counter.
++counter;
```

Better:

```cpp
// Skip zero because it represents an uninitialized generation ID.
if (++generation == 0)
    ++generation;
```

The second comment explains the reason behind unusual behavior.

### 2.2 Prefer Clear Code Over Explanatory Comments

If code requires a large comment merely because the code itself is difficult to understand, consider improving the implementation first.

Bad:

```cpp
// Check whether x is inside the allowed range and y is valid.
if (x >= 0 && x < 64 && y >= 0 && y < 64)
{
    ...
}
```

Better:

```cpp
if (IsInsideBoard(position))
{
    ...
}
```

Then document `IsInsideBoard()` only if its behavior contains important assumptions.

Comments must not compensate for poor naming.

### 2.3 Comments Must Add Information

A useful comment should provide information that cannot be immediately obtained by reading the code.

Useful topics include:

* design rationale;
* architectural boundaries;
* performance requirements;
* lifetime assumptions;
* concurrency rules;
* invariants;
* data ownership;
* platform limitations;
* protocol requirements;
* algorithmic reasoning;
* unusual workarounds;
* compatibility requirements;
* security constraints;
* external specifications.

### 2.4 Keep Comments Close to the Code They Describe

Comments should normally appear immediately above the declaration, statement, or block they describe.

Bad:

```cpp
// Somewhere much earlier:
// Objects cannot be removed while iterating.

...

for (auto& object : objects)
{
    ...
}
```

Better:

```cpp
// Removal is deferred because erasing here would invalidate the iterator.
for (auto& object : objects)
{
    ...
}
```

Architecture-level information that affects many files should instead live in module documentation or dedicated design documentation.

### 2.5 Comments Are Part of the Code

Comments must be maintained with the same discipline as source code.

Whenever code changes, review nearby comments.

A stale comment is often worse than no comment because it provides incorrect information with apparent authority.

During code review, reviewers should ask:

* Is the comment still correct?
* Does it describe the current behavior?
* Is the stated reason still valid?
* Is the comment now redundant?
* Should the information be moved to higher-level documentation?

## 3. Comment Style

Use standard C++ comments consistently.

Preferred:

```cpp
// Single-line comment.
```

Use consecutive `//` lines for normal multi-line explanations:

```cpp
// The renderer keeps two frame contexts alive because GPU execution may
// still reference resources submitted during the previous frame.
```

Avoid excessive block comments for ordinary code:

```cpp
/*
 * Explanation...
 */
```

Block comments may still be appropriate for:

* copyright headers;
* generated documentation;
* temporarily commenting out large sections during debugging, although such code should never be committed;
* specialized documentation systems.

For normal source documentation, `//` is preferred.

## 4. File-Level Documentation

Important source files should begin with a short description when their responsibility is not obvious from the filename.

Example:

```cpp
// Implements asynchronous asset loading and the transition from CPU-side
// decoded assets to GPU-resident resources.
//
// AssetLoader owns worker-thread decoding but does not own GPU resources.
// GPU uploads are submitted through RenderDevice on the render thread.
```

A file header should explain:

* the file's responsibility;
* subsystem ownership;
* important relationships;
* unusual constraints.

Do not write long historical descriptions.

Bad:

```cpp
// This file was originally created in 2024 and later rewritten...
```

Version control already records history.

## 5. Namespace Documentation

Namespaces representing important modules may contain a short description.

```cpp
namespace engine::render
{

// Rendering subsystem interfaces and GPU resource abstractions.

}
```

Do not document obvious utility namespaces unnecessarily.

## 6. Class and Struct Documentation

Public or architecturally important classes should document their role.

Example:

```cpp
/// Manages the lifetime and lookup of loaded texture resources.
///
/// TextureManager owns Texture objects after successful creation.
/// Individual callers receive non-owning TextureHandle values.
///
/// Threading:
/// - LoadTexture() may be called from worker threads.
/// - GPU resource creation occurs on the render thread.
/// - DestroyAll() must be called after all worker threads have stopped.
class TextureManager
{
    ...
};
```

Class documentation should describe:

* responsibility;
* ownership;
* lifetime;
* important invariants;
* threading model;
* interaction with other subsystems;
* restrictions.

Avoid documenting implementation details that callers do not need to know.

## 7. Function Documentation

### 7.1 Public API Functions

Public APIs should normally document:

* purpose;
* parameters when their meaning is not obvious;
* return value;
* ownership;
* failure behavior;
* preconditions;
* postconditions;
* thread-safety;
* invalidation rules;
* important side effects.

Example:

```cpp
/// Loads an image from disk and creates a texture resource.
///
/// @param path Absolute or project-relative asset path.
/// @return A valid texture handle on success, or an invalid handle if the
///         asset cannot be loaded.
///
/// The returned handle does not own the underlying texture. The texture
/// remains valid until UnloadTexture() or Shutdown() is called.
///
/// Thread-safe.
TextureHandle LoadTexture(const std::filesystem::path& path);
```

### 7.2 Private Functions

Do not automatically document every private function.

Bad:

```cpp
// Updates velocity.
void UpdateVelocity();
```

Document private functions when they have:

* unusual side effects;
* complex assumptions;
* non-obvious algorithms;
* lifetime implications;
* ordering requirements;
* synchronization requirements.

Example:

```cpp
// Rebuilds the lookup table after entity compaction.
// Must run before any component queries are executed again.
void RebuildEntityLookup();
```

## 8. Parameter Documentation

Do not document parameters whose meaning is already obvious.

Unnecessary:

```cpp
/// @param width The width.
/// @param height The height.
void Resize(int width, int height);
```

Useful:

```cpp
/// @param timeout Maximum time to wait. A zero duration performs a
///        non-blocking poll.
bool WaitForCompletion(std::chrono::milliseconds timeout);
```

Parameter comments should explain semantics rather than repeat names.

## 9. Return Value Documentation

Document return values when:

* special values exist;
* failure is represented indirectly;
* returned objects have ownership implications;
* invalidation rules apply.

Example:

```cpp
/// @return Pointer to the component if present, otherwise nullptr.
///
/// The returned pointer is invalidated by any operation that causes the
/// component pool to reallocate.
Transform* FindTransform(Entity entity);
```

For obvious value-returning getters, documentation is usually unnecessary.

## 10. Ownership and Lifetime Comments

Ownership is one of the most important things to document in large C++ systems.

Explicitly document unusual ownership relationships.

Example:

```cpp
// Non-owning pointer. RenderDevice must outlive this object.
RenderDevice* device_;
```

Example:

```cpp
// Owns all registered systems. Destruction occurs in reverse registration
// order because later systems may depend on earlier ones.
std::vector<std::unique_ptr<System>> systems_;
```

Avoid repeatedly documenting ownership already made obvious by standard types.

Normally unnecessary:

```cpp
// Owns the texture.
std::unique_ptr<Texture> texture_;
```

The type already communicates ownership.

Comment only if additional semantics matter.

## 11. Invariant Documentation

Important invariants should be documented near the code enforcing them.

Example:

```cpp
// Invariant: activeCount_ is always <= slots_.size().
// Slots [0, activeCount_) contain live objects.
std::vector<Slot> slots_;
std::size_t activeCount_;
```

Assertions should reinforce invariants:

```cpp
assert(activeCount_ <= slots_.size());
```

Comments describe the rule; assertions verify it.

## 12. Preconditions and Postconditions

Document non-obvious requirements imposed on callers.

Example:

```cpp
/// Removes an entity from the world.
///
/// Preconditions:
/// - entity must belong to this World.
/// - no component iteration may currently be active.
///
/// Postcondition:
/// - all handles referencing the entity become invalid.
void DestroyEntity(Entity entity);
```

Prefer enforcing preconditions programmatically when possible.

Comments are not substitutes for validation.

## 13. Threading and Concurrency Comments

Concurrency assumptions should be documented explicitly.

Example:

```cpp
// Protected by queueMutex_.
std::queue<Job> pendingJobs_;
```

Example:

```cpp
// Written only by the simulation thread.
// Read by the rendering thread after frame synchronization.
TransformSnapshot snapshot_;
```

Example:

```cpp
/// Thread-safe.
///
/// Multiple callers may enqueue commands concurrently. Commands are consumed
/// exclusively by the render thread.
void Submit(RenderCommand command);
```

For complicated synchronization, explain the reason behind the locking strategy.

```cpp
// Acquire stateMutex_ before connectionMutex_.
// The reverse order is forbidden to prevent deadlocks with Disconnect().
```

Lock ordering rules should be documented centrally and near relevant locks.

## 14. Atomic Operations

Atomic variables often require semantic documentation.

Bad:

```cpp
std::atomic<bool> running_;
```

Better:

```cpp
// Set to false by the controlling thread to request worker termination.
// Workers observe the flag with acquire semantics before starting new jobs.
std::atomic<bool> running_;
```

If memory ordering is not obvious, explain why a particular ordering is correct.

```cpp
// Release publishes the completed frame data before exposing the new index.
frameIndex_.store(nextIndex, std::memory_order_release);
```

Do not explain basic atomic syntax.

Explain the synchronization relationship.

## 15. Algorithm Comments

Complex algorithms should contain a short overview before the implementation.

Example:

```cpp
// Broad-phase collision detection:
//
// 1. Insert dynamic objects into spatial grid cells.
// 2. Generate candidate pairs from objects sharing a cell.
// 3. Remove duplicate pairs.
// 4. Pass remaining candidates to narrow-phase collision detection.
//
// This reduces the expected candidate search from O(n²) to approximately
// O(n + k), where k is the number of spatially local candidate pairs.
```

Inside the algorithm, comment only important transitions.

Avoid narrating every line.

## 16. Mathematical Code

Mathematical implementations should document:

* equation being implemented;
* coordinate system;
* units;
* conventions;
* source of the formula where appropriate.

Example:

```cpp
// Perspective projection using a right-handed coordinate system.
// Depth is mapped to [0, 1] to match Vulkan's clip-space convention.
```

Example:

```cpp
// Exponential smoothing:
// y(t) = target + (previous - target) * exp(-lambda * dt)
const float decay = std::exp(-lambda * deltaTime);
```

## 17. Units

Always make units explicit when ambiguity is possible.

Prefer type systems when practical:

```cpp
std::chrono::milliseconds timeout;
```

If raw numerical types are required:

```cpp
// Simulation timestep in seconds.
float deltaTimeSeconds;
```

Avoid:

```cpp
float time;
```

For constants:

```cpp
// Maximum network inactivity before disconnecting the peer, in milliseconds.
constexpr std::uint32_t kConnectionTimeoutMs = 5000;
```

## 18. Coordinate Systems

Game engines, graphics systems, physics engines, and simulation software must document coordinate conventions.

Example:

```cpp
// World coordinate system:
// +X = right
// +Y = up
// +Z = forward
//
// Rotations follow the right-hand rule.
```

Put the canonical definition in one central location and reference it elsewhere instead of duplicating it.

## 19. Magic Numbers

Prefer named constants over comments.

Bad:

```cpp
if (retries > 5) // Maximum retry count
```

Better:

```cpp
constexpr int kMaxRetryCount = 5;

if (retries > kMaxRetryCount)
```

If the value itself requires explanation:

```cpp
// Three retries cover the expected transient network failure window while
// keeping worst-case connection latency below the 2-second requirement.
constexpr int kMaxRetryCount = 3;
```

## 20. Workarounds

Workarounds require strong documentation.

Use a structured comment:

```cpp
// WORKAROUND:
// AMD driver versions before 24.3 incorrectly reject this image layout
// transition when the source access mask is zero.
//
// Remove this path after support for those driver versions is dropped.
```

When possible, include a reference:

```cpp
// WORKAROUND: GCC 13 incorrectly diagnoses this expression under -Warray-bounds.
// See GCC PR 110123.
// Remove when GCC 13 support is dropped.
```

A workaround should explain:

1. what is wrong;
2. why the workaround exists;
3. when it can be removed.

## 21. Platform-Specific Code

Platform-specific behavior should explain why it differs.

```cpp
#ifdef _WIN32

// Windows requires explicit binary mode here; otherwise the CRT may translate
// line endings and corrupt binary asset data.

#endif
```

Do not comment obvious preprocessor syntax.

## 22. Security-Sensitive Code

Security-critical code should document assumptions and threat boundaries.

Example:

```cpp
// Do not trust packetLength. It originates from the remote peer and must be
// validated before allocating memory or reading payload data.
```

Example:

```cpp
// Authentication tokens must never be written to logs, including debug logs.
```

Comments should not reveal secrets, passwords, API keys, private credentials, or sensitive production information.

## 23. Error Handling Comments

Do not explain normal error handling unnecessarily.

Bad:

```cpp
// Return false if opening fails.
if (!file)
    return false;
```

Useful:

```cpp
// Missing configuration files are intentionally not treated as errors.
// The application will continue with built-in defaults.
if (!file)
    return {};
```

Document unusual failure semantics.

## 24. Exception Policy

Projects should clearly document their exception policy.

Example:

```cpp
// This subsystem does not use exceptions.
// Allocation and parsing failures are represented through Result<T>.
```

If exceptions are used, document important guarantees:

```cpp
/// Strong exception guarantee:
/// on failure, the original configuration remains unchanged.
void ApplyConfiguration(const Configuration& config);
```

## 25. Template Code

Templates may require additional explanation because errors often appear far from the implementation.

Example:

```cpp
/// Registers a component type.
///
/// T must:
/// - be move-constructible;
/// - have a unique ComponentTypeId specialization;
/// - not contain references to temporary world state.
template <typename T>
void RegisterComponent();
```

Where possible, encode constraints using C++ concepts:

```cpp
template <Component T>
void RegisterComponent();
```

Then document only semantics not expressed by the concept.

## 26. Concepts and Constraints

Document the semantic requirement of a concept.

```cpp
/// A resource that can be uploaded to a RenderDevice.
///
/// Resource types must provide immutable descriptor information after
/// creation because descriptors may be cached by the rendering backend.
template <typename T>
concept GpuResource = ...;
```

Do not merely restate the requires-expression.

## 27. Public Header Comments

Public headers deserve stricter documentation than implementation files.

Public API comments should answer:

* What does this type/function do?
* Who owns returned resources?
* Is it thread-safe?
* What invalidates returned references?
* What are valid parameter ranges?
* What happens on failure?
* Can the function block?
* Does it allocate?
* Does it modify global state?

Implementation details belong in `.cpp` files unless callers need to know them.

## 28. Implementation Comments

Implementation comments should explain internal decisions.

Header:

```cpp
/// Loads a resource from the asset database.
ResourceHandle LoadResource(ResourceId id);
```

Implementation:

```cpp
// Check the in-memory table first because database lookups may require disk
// access and can stall the calling worker thread.
auto existing = FindLoadedResource(id);
```

This separation prevents implementation details from leaking into the public API.

## 29. Member Variables

Do not comment every member variable.

Bad:

```cpp
int width_;   // Width
int height_;  // Height
```

Useful:

```cpp
// Number of frames that may still be referenced by submitted GPU commands.
std::uint32_t framesInFlight_;
```

Comments are appropriate when semantics are not obvious from the name and type.

## 30. Boolean Variables

Avoid vague booleans requiring explanatory comments.

Bad:

```cpp
bool flag;
```

Better:

```cpp
bool shutdownRequested;
```

If the state is complex, replace the boolean with an enum.

```cpp
enum class ConnectionState
{
    Disconnected,
    Connecting,
    Connected,
    Disconnecting
};
```

Good data modeling reduces comment requirements.

## 31. State Machines

State machines should document allowed transitions.

```cpp
// Connection state transitions:
//
// Disconnected -> Connecting -> Connected
//      ^              |             |
//      |              v             v
//      +---------- Failed <--- Disconnecting
//
// Direct Disconnected -> Connected transitions are not allowed.
```

Individual transition handlers should explain only unusual behavior.

## 32. ECS and Game Engine Systems

For Entity Component System architectures, document ownership and execution order.

Example:

```cpp
/// Updates character movement.
///
/// Requires:
/// - TransformComponent
/// - VelocityComponent
///
/// Runs after InputSystem and before CollisionSystem.
class MovementSystem final : public System
{
    ...
};
```

If ordering is enforced elsewhere, avoid duplicating the entire execution graph in every class.

## 33. Renderer Comments

Rendering code should clearly document:

* graphics API assumptions;
* resource ownership;
* synchronization;
* image layouts;
* pipeline state assumptions;
* coordinate conventions;
* frame lifetime.

Example:

```cpp
// This fence belongs to frame N and is waited before frame N's transient
// resources are reused.
VkFence frameFence;
```

## 34. Memory Allocators

Custom allocators require extensive documentation.

Document:

* ownership;
* alignment;
* lifetime;
* thread safety;
* reset behavior;
* invalidation rules;
* fallback behavior.

Example:

```cpp
/// Linear allocator for frame-local temporary memory.
///
/// All allocations become invalid when Reset() is called.
/// Individual allocations cannot be freed.
///
/// Not thread-safe.
class FrameAllocator
{
    ...
};
```

## 35. Low-Level and Undefined-Behavior-Sensitive Code

Code involving:

* pointer arithmetic;
* placement new;
* object lifetime manipulation;
* memory mapping;
* SIMD;
* alignment;
* strict aliasing;
* atomics;
* ABI behavior;

should document assumptions carefully.

Example:

```cpp
// The buffer was allocated with alignof(Node), so placement construction here
// satisfies Node's alignment requirement.
auto* node = new (storage) Node(...);
```

Avoid claims that are not guaranteed by the C++ standard unless a platform ABI explicitly guarantees them.

## 36. TODO Comments

TODO comments are permitted only for concrete, actionable work.

Preferred format:

```cpp
// TODO: Replace linear lookup with the resource hash table after the asset
// database API is available.
```

Better when integrated with issue tracking:

```cpp
// TODO(#184): Remove the compatibility path after legacy maps are migrated.
```

Bad:

```cpp
// TODO: Fix this.
```

Bad:

```cpp
// TODO: Improve.
```

A TODO should clearly explain what remains to be done.

## 37. FIXME Comments

`FIXME` indicates known incorrect behavior.

```cpp
// FIXME(#231): This calculation overflows for worlds larger than 32767 cells.
```

FIXME comments should normally have a corresponding tracked issue.

Do not use FIXME merely for code that could be cleaner.

## 38. HACK Comments

Use `HACK` sparingly.

```cpp
// HACK:
// The legacy serializer requires this field to remain first in the structure.
// Remove this constraint when version-1 save support is dropped.
```

A HACK comment must explain:

* why the implementation is intentionally abnormal;
* what external constraint requires it;
* how it can eventually be removed.

## 39. NOTE Comments

Use `NOTE` for information developers are likely to overlook.

```cpp
// NOTE: IDs are stable across vector reallocations but not across World::Reset().
```

Do not prefix every ordinary comment with `NOTE`.

## 40. WARNING Comments

Use `WARNING` for behavior that can cause serious errors.

```cpp
// WARNING:
// Do not call this function while command recording is active.
// It destroys descriptor pools referenced by the current command buffer.
```

Warnings should be rare and meaningful.

## 41. Commented-Out Code

Never commit commented-out implementation code.

Bad:

```cpp
// oldRenderer.Draw(mesh);
// legacyRenderer.Submit(mesh);
newRenderer.Draw(mesh);
```

Version control already stores previous versions.

Delete dead code.

Exceptions should be extremely rare and explicitly justified.

## 42. Historical Comments

Avoid source comments describing development history.

Bad:

```cpp
// John changed this in March.
// Previously we used std::map here.
```

Use Git history, pull requests, issue trackers, and architecture decision records for historical information.

Source comments should explain the current system.

## 43. Doxygen Documentation

For large projects, Doxygen-compatible syntax is recommended for public API documentation.

Example:

```cpp
/// Creates a rendering pipeline.
///
/// @param descriptor Immutable pipeline configuration.
/// @return Pipeline handle on success.
///
/// @note Pipeline creation may perform expensive shader compilation.
/// @thread_safety Must be called from the render thread.
PipelineHandle CreatePipeline(const PipelineDescriptor& descriptor);
```

Recommended commands include:

```text
@param
@return
@retval
@tparam
@note
@warning
@pre
@post
@see
@deprecated
```

Do not use Doxygen tags simply to produce larger comments.

## 44. Deprecated APIs

Deprecated APIs must explain what should replace them.

```cpp
/// @deprecated Use CreateTexture(TextureDescriptor) instead.
[[deprecated("Use CreateTexture(TextureDescriptor)")]]
Texture* CreateTexture(int width, int height);
```

If migration requires special behavior, document it.

## 45. CMake Commenting Guidelines

CMake is part of the project and should follow the same documentation discipline as C++.

Comments should explain:

* target organization;
* dependency decisions;
* build options;
* platform-specific behavior;
* generated files;
* compiler flags;
* installation rules;
* packaging;
* unusual workarounds.

## 46. CMake Target Documentation

Important targets may include a short comment describing their role.

```cmake
# Core engine library shared by runtime executables and tools.
add_library(EngineCore STATIC
    ...
)
```

Avoid obvious comments:

```cmake
# Create library.
add_library(EngineCore STATIC ...)
```

## 47. CMake Dependency Comments

Explain non-obvious dependency choices.

```cmake
# PRIVATE because Vulkan is an implementation detail of RenderBackend and must
# not propagate into targets that only consume the abstract rendering API.
target_link_libraries(RenderBackend
    PRIVATE
        Vulkan::Vulkan
)
```

This is especially useful when choosing between `PRIVATE`, `PUBLIC`, and `INTERFACE`.

## 48. CMake Build Options

Options should explain their effect.

```cmake
option(
    ENGINE_BUILD_EDITOR
    "Build the graphical editor application"
    ON
)
```

For complex options:

```cmake
# Enables expensive runtime validation intended for development builds.
# This option is independent of CMAKE_BUILD_TYPE because multi-config
# generators may build Debug and Release from the same configuration.
option(ENGINE_ENABLE_VALIDATION "Enable engine validation checks" ON)
```

## 49. Compiler Flags

Never add unusual compiler flags without explaining why.

```cmake
# MSVC reports external SDK headers under /W4 unless external warning levels
# are configured separately.
target_compile_options(EngineCore PRIVATE /external:W0)
```

Avoid:

```cmake
target_compile_options(EngineCore PRIVATE -Wno-something)
```

without justification.

Disabling warnings must be documented.

## 50. Platform-Specific CMake

Example:

```cmake
if(WIN32)
    # ws2_32 provides the Winsock APIs used by NetworkPlatformWin32.cpp.
    target_link_libraries(EngineNetwork PRIVATE ws2_32)
endif()
```

Do not explain what `WIN32` itself means.

## 51. Generated Files

Document generated source and configuration files.

```cmake
# Generate BuildConfig.hpp so runtime code can access the engine version and
# enabled build features without depending directly on CMake definitions.
configure_file(
    BuildConfig.hpp.in
    generated/BuildConfig.hpp
    @ONLY
)
```

## 52. CMake Workarounds

Use the same structured workaround style as C++.

```cmake
# WORKAROUND:
# CMake 3.28 incorrectly propagates this property through imported targets
# under Visual Studio generators.
#
# Remove after the minimum CMake version is raised to 3.29.
set_property(...)
```

## 53. CMake Section Headers

Large `CMakeLists.txt` files may use simple section separators.

```cmake
# -----------------------------------------------------------------------------
# EngineCore
# -----------------------------------------------------------------------------
```

Use them only when the file is sufficiently large.

Do not produce decorative banners such as:

```cmake
############################################################
################## AMAZING BUILD SYSTEM ####################
############################################################
```

## 54. CMake Function Documentation

Custom functions should describe their contract.

```cmake
# Adds a runtime executable using the project's standard warning, sanitizer,
# and installation configuration.
#
# Usage:
#   engine_add_runtime_target(
#       NAME EngineEditor
#       SOURCES ...
#   )
function(engine_add_runtime_target)
    ...
endfunction()
```

Complex functions should document accepted arguments.

## 55. Build-System Rationale

Important build decisions should be explained.

Example:

```cmake
# EngineCore remains STATIC rather than OBJECT because several tools link
# against it independently and must receive identical transitive usage
# requirements.
add_library(EngineCore STATIC ...)
```

Such comments can prevent future developers from "simplifying" intentional design.

## 56. Test Comments

Tests should primarily communicate intent through descriptive names.

Prefer:

```cpp
TEST(ResourceCache, ReturnsExistingResourceWhenPathAlreadyLoaded)
```

over:

```cpp
TEST(ResourceCache, Test1)
```

Comments in tests are useful for explaining unusual setup or regression conditions.

```cpp
// Regression test for #417.
// Reallocation previously invalidated the handle returned for entity A.
```

Do not narrate ordinary Arrange/Act/Assert code unless the test is unusually complicated.

## 57. Regression Tests

Regression tests should identify the behavior being protected.

```cpp
// Regression: destroying an entity while iterating previously skipped the
// component immediately following it in packed storage.
TEST(EntityWorld, DeferredDestroyDoesNotSkipComponents)
{
    ...
}
```

Issue references are encouraged when available.

## 58. Performance-Critical Code

Performance-sensitive code should explain the reason for unusual implementations.

```cpp
// Keep this loop branch-free. It runs once per visible particle and accounts
// for approximately 8% of frame time in particle-heavy scenes.
```

Avoid vague statements such as:

```cpp
// Faster.
```

Prefer measurable reasoning.

## 59. Performance Assumptions

If an algorithm is selected based on expected workload, record that expectation.

```cpp
// A linear search is intentional. This list normally contains fewer than
// eight entries, making a hash table more expensive in both memory and cache
// behavior.
```

This prevents inappropriate "optimization" later.

## 60. Logging Comments

Comments should explain unusual logging decisions.

```cpp
// Do not log every failed probe here. The function may execute thousands of
// times during asset discovery and would flood diagnostic logs.
```

Do not comment every logging statement.

## 61. Serialization

Serialization code should document:

* binary format expectations;
* compatibility rules;
* version behavior;
* field ordering requirements;
* endianness.

Example:

```cpp
// Serialized explicitly as little-endian regardless of host architecture.
writer.WriteLittleEndian(version);
```

## 62. Networking

Network code should document:

* framing;
* byte order;
* trust boundaries;
* protocol versions;
* timeout semantics;
* retry behavior.

Example:

```cpp
// Packet length is encoded in network byte order and excludes the four-byte
// length prefix itself.
```

## 63. External Specifications

When implementing behavior defined elsewhere, cite the authoritative specification.

```cpp
// SPIR-V 1.6 §2.16 requires this alignment for physical storage buffers.
```

Do not copy large portions of specifications into source comments.

Reference them.

## 64. Architecture Comments

Architecture comments should explain boundaries, not reproduce architecture documents.

Example:

```cpp
// RenderGraph may depend on RenderDevice abstractions but must not depend on
// VulkanBackend directly. This keeps graph compilation backend-independent.
```

If an explanation requires several paragraphs or diagrams, move it into:

```text
docs/architecture/
```

and reference the document.

## 65. Comments Versus Documentation

Use comments for information tightly coupled to code.

Use external documentation for:

* subsystem architecture;
* large data-flow diagrams;
* build instructions;
* onboarding;
* design proposals;
* architecture decision records;
* protocol specifications;
* long explanations;
* tutorials.

Example source reference:

```cpp
// See docs/architecture/render-graph.md for lifetime and scheduling rules.
```

Do not duplicate the document inside the code.

## 66. README and Module Documentation

Large modules should normally contain local documentation.

Example:

```text
src/
    render/
        README.md
    physics/
        README.md
    assets/
        README.md
```

A module README can explain:

* purpose;
* directory structure;
* dependency direction;
* major interfaces;
* threading model;
* lifetime model;
* extension points.

Source comments then focus on local implementation details.

## 67. Architecture Decision Records

Long-term architectural decisions should use ADRs rather than source comments.

Example:

```text
docs/adr/
    0001-use-vulkan.md
    0002-static-engine-libraries.md
    0003-ecs-storage-model.md
```

A small source comment can reference the ADR:

```cpp
// Resource handles intentionally use generation counters.
// See ADR-0017.
```

## 68. Grammar and Language

Project comments should use clear professional English.

Preferred:

```cpp
// Keep the previous frame alive until the GPU signals completion.
```

Avoid:

```cpp
// we need keep old frame cuz gpu maybe still using it
```

Comments do not need to sound academic.

Prefer simple, precise language.

## 69. Sentence Style

Full sentences are recommended for explanatory comments.

```cpp
// The connection remains registered until all pending callbacks complete.
```

Short labels may omit punctuation:

```cpp
// Network initialization
```

Be consistent.

## 70. Terminology

Use project terminology consistently.

If the project defines:

```text
Entity
Resource
Asset
FrameContext
RenderPass
```

do not casually alternate between:

```text
object
thing
data item
frame object
drawing stage
```

Consistent terminology makes search and documentation significantly easier.

## 71. Avoid Ambiguous Pronouns

Bad:

```cpp
// It needs to be updated before this runs.
```

Better:

```cpp
// The descriptor cache must be updated before command recording begins.
```

A comment should remain understandable even after surrounding code changes.

## 72. Avoid Temporal Language

Bad:

```cpp
// Currently we use Vulkan.
```

Better:

```cpp
// The rendering backend uses Vulkan.
```

Bad:

```cpp
// For now this is static.
```

Better:

```cpp
// Static storage is intentional because the registry exists for the process
// lifetime.
```

Comments should describe design, not temporary perception.

## 73. Avoid Developer-Specific Language

Do not write:

```cpp
// John's workaround.
```

Write:

```cpp
// WORKAROUND: Preserve the legacy field layout for save-file compatibility.
```

Comments should remain meaningful after team members change.

## 74. Avoid Emotional Comments

Never write:

```cpp
// This API is stupid.
```

or:

```cpp
// CMake is horrible so we have to do this.
```

Write:

```cpp
// The imported target does not expose the required include directory, so it
// must be added explicitly.
```

Comments must remain technical and professional.

## 75. Avoid Uncertain Comments

Bad:

```cpp
// I think this prevents a race condition.
```

If the author does not understand why the code is correct, the code is not ready for production.

Investigate and document the actual reason.

## 76. Comment Density

There is no target percentage of commented lines.

A healthy codebase may contain:

* heavily documented public APIs;
* sparsely commented straightforward implementation code;
* highly commented synchronization code;
* highly commented low-level memory code;
* minimally commented trivial utility functions.

Comment density should follow complexity, not a quota.

## 77. Example: Poorly Commented Code

```cpp
void Update()
{
    // Loop through objects.
    for (auto& object : objects_)
    {
        // Check active.
        if (object.active)
        {
            // Update object.
            object.Update();
        }
    }
}
```

Almost every comment is redundant.

## 78. Example: Improved Version

```cpp
void Update()
{
    // Inactive objects remain in the array until end-of-frame compaction so
    // handles remain stable during simulation updates.
    for (auto& object : objects_)
    {
        if (object.active)
            object.Update();
    }
}
```

Only the non-obvious architectural reason is documented.

## 79. Example: Complex Systems Code

```cpp
void JobQueue::Push(Job job)
{
    {
        std::lock_guard lock(mutex_);
        jobs_.push(std::move(job));
    }

    // Notify after releasing the mutex. Waking the worker while holding the
    // lock would immediately make it block on the same mutex.
    condition_.notify_one();
}
```

This comment explains a synchronization decision that may otherwise look arbitrary.

## 80. Example: GPU Resource Lifetime

```cpp
void Renderer::DestroyTexture(TextureHandle handle)
{
    // GPU commands submitted during earlier frames may still reference the
    // texture. Defer destruction until the corresponding frame fence signals.
    deferredDeletion_[currentFrame_].Push(handle);
}
```

This is an appropriate implementation comment because it explains why destruction is deferred.

## 81. Example: CMake Library Configuration

```cmake
# -----------------------------------------------------------------------------
# EngineRender
# -----------------------------------------------------------------------------

add_library(EngineRender STATIC)

target_sources(EngineRender
    PRIVATE
        Renderer.cpp
        RenderGraph.cpp
        TextureManager.cpp

    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${PROJECT_SOURCE_DIR}/include
        FILES
            ${PROJECT_SOURCE_DIR}/include/engine/render/Renderer.hpp
            ${PROJECT_SOURCE_DIR}/include/engine/render/RenderGraph.hpp
)

# Vulkan remains private because public EngineRender headers expose only the
# project's backend-independent rendering abstractions.
target_link_libraries(EngineRender
    PRIVATE
        Vulkan::Vulkan

    PUBLIC
        EngineCore
)

# Shader compiler support is required only by development tools. Shipping
# runtime builds consume precompiled shader binaries.
if(ENGINE_BUILD_TOOLS)
    target_link_libraries(EngineRender PRIVATE ShaderCompiler)
endif()
```

The comments describe architectural and build decisions rather than CMake syntax.

## 82. Recommended Standard Comment Markers

The project should standardize the following markers:

```cpp
// TODO(#issue): Actionable future work.

// FIXME(#issue): Known incorrect behavior.

// WORKAROUND: Required compatibility or external-system workaround.

// HACK: Intentionally abnormal implementation caused by a known constraint.

// NOTE: Important information that is easy to overlook.

// WARNING: Behavior that may cause serious failure if violated.
```

Do not invent many additional categories unless the project has a specific reason.

## 83. Required Documentation by Code Category

**Public API** — normally document: purpose; input semantics; output semantics; ownership; lifetime; thread safety; failure behavior.

**Internal Straightforward Code** — normally requires little or no commenting.

**Complex Algorithms** — document: algorithm; reasoning; complexity when important; unusual edge cases.

**Concurrent Code** — document: ownership; synchronization; lock rules; atomic relationships; thread restrictions.

**Low-Level Memory Code** — document: alignment; lifetime; aliasing assumptions; ownership; invalidation.

**Platform-Specific Code** — document: why the platform requires different behavior.

**CMake** — document: dependency decisions; unusual compiler flags; target relationships; generated sources; compatibility workarounds.

## 84. Comment Review Checklist

During code review, verify:

* Comments explain non-obvious reasoning rather than syntax.
* Public interfaces document important contracts.
* Ownership is clear.
* Lifetime and invalidation behavior are clear.
* Threading assumptions are documented.
* Complex algorithms have sufficient explanation.
* Platform-specific behavior has a reason.
* Workarounds explain when they can be removed.
* TODO/FIXME items are actionable.
* Commented-out code is absent.
* Historical explanations are kept in Git or design documents.
* CMake comments explain build decisions rather than commands.
* Comments match the current implementation.
* Terminology matches project terminology.
* Comments are concise enough to remain maintainable.

## 85. Rules for AI-Assisted Development

AI-generated code must follow the same commenting standard as human-written code.

Do not accept AI-generated comments such as:

```cpp
// Initialize variable.
int count = 0;

// Loop through vector.
for (const auto& item : items)
{
    ...
}
```

AI tools frequently over-comment straightforward code.

When generating or reviewing AI-assisted code:

1. Remove comments that simply narrate syntax.
2. Preserve comments explaining architectural intent.
3. Verify every technical claim.
4. Verify concurrency explanations.
5. Verify ownership explanations.
6. Verify C++ standard claims.
7. Remove speculative comments.
8. Do not allow the AI to invent issue numbers or external specifications.
9. Do not allow comments to claim guarantees the code does not enforce.
10. Treat generated comments as untrusted until reviewed.

A concise accurate comment is preferable to a detailed but speculative one.

## 86. Project-Level Commenting Policy

The following rules should be treated as the default project policy.

* **Rule 1** — Do not comment obvious code.
* **Rule 2** — Comment important reasoning.
* **Rule 3** — Document public contracts.
* **Rule 4** — Document ownership and lifetime when they are not obvious from the type system.
* **Rule 5** — Document concurrency rules explicitly.
* **Rule 6** — Document invariants and ordering constraints.
* **Rule 7** — Explain unusual CMake decisions.
* **Rule 8** — Use TODO/FIXME only for concrete actionable work.
* **Rule 9** — Never commit commented-out code.
* **Rule 10** — Do not use comments as substitutes for meaningful names or proper abstractions.
* **Rule 11** — Keep architecture documentation outside source files when the explanation becomes large.
* **Rule 12** — Update or delete comments when behavior changes.

## 87. Recommended Documentation Hierarchy

For a large project, documentation should be divided into layers:

```text
README.md
    High-level project introduction and build instructions

docs/
    architecture/
        System architecture and subsystem design

    adr/
        Architecture decision records

    build/
        Build configuration and toolchain documentation

    development/
        Coding and contribution standards

src/
    module/
        README.md
        Module-level architecture

include/
    Public API headers
        Doxygen/interface documentation

*.cpp
    Local implementation reasoning

CMakeLists.txt
    Build-system reasoning
```

This prevents source comments from becoming the only form of documentation.

## 88. Final Principle

The best comment is not necessarily the longest comment.

A good comment captures information that a competent developer could not reliably infer from the code alone.

Before writing a comment, ask:

What would a future developer need to know to safely modify this code?

If the answer is already clear from the code, do not add a comment.

If the answer involves:

* reasoning,
* ownership,
* lifetime,
* invariants,
* synchronization,
* compatibility,
* performance,
* architectural boundaries,
* external requirements,

then document it clearly.

The standard for a large C++ project should therefore be:

Write code that clearly expresses what the program does. Write comments that preserve the engineering knowledge required to understand why it does it that way.

