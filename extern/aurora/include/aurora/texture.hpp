#ifndef AURORA_TEXTURE_HPP
#define AURORA_TEXTURE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace aurora::texture {

/// Wildcard hash values used by the replacement filename convention ("$" fields).
inline constexpr uint64_t kWildcardTextureHash = 0xFFFFFFFFFFFFFFFFull;
inline constexpr uint64_t kWildcardTlutHash = 0xFFFFFFFFFFFFFFFEull;

struct TextureSourceKey {
  uint64_t textureHash = 0;
  uint64_t tlutHash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  bool hasTlut = false;

  bool operator==(const TextureSourceKey&) const = default;
};

struct TexturePointerKey {
  const void* data = nullptr;

  bool operator==(const TexturePointerKey&) const = default;
};

using ReplacementKey = std::variant<TexturePointerKey, TextureSourceKey>;

struct RawTextureReplacement {
  std::span<const uint8_t> bytes;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 1;
  uint32_t gxFormat = 0;
  std::string_view label = {};
};

struct ReplacementOptions {
  int32_t priority = 0;
};

struct ReplacementRegistration {
  uint64_t id = 0;
  ReplacementKey key;
};

struct ReplacementGroup {
  std::vector<ReplacementRegistration> registrations;
};

/// Parses a replacement filename of the form "tex1_{w}x{h}[_m]_{texhash}[_{tluthash}]_{fmt}[_arb].dds|.png"
/// into the source key it addresses. Hash fields may be "$" (see the wildcard constants above).
/// Returns nullopt if the filename is invalid.
std::optional<TextureSourceKey> parse_replacement_filename(std::string_view filename) noexcept;

/// Callback source for replacement files that do not live on the filesystem.
///
/// read() loads the entire encoded file at `path` into outBytes, returning false if the file is
/// unavailable. It must be thread safe: it is invoked lazily from Aurora worker threads without
/// internal registry locks held. `userData` must remain valid until unregistration returns;
/// unregistration may wait for one in-flight read callback to finish.
///
/// The callback must not call back into aurora::texture.
struct VirtualFileSource {
  bool (*read)(void* userData, const char* path, std::vector<uint8_t>& outBytes) = nullptr;
  void* userData = nullptr;
};

/// Registers an encoded (.dds/.png) replacement served through callbacks instead of the filesystem.
/// `path` is a relative '/'-separated virtual path whose final component follows the replacement
/// filename convention and determines the key. Mip sidecars are probed lazily by deriving
/// "{stem}_mipN{ext}" from `path` and calling source.read for each level until it returns false.
///
/// Returns ID 0 (invalid) if the filename does not parse or source.read is null.
ReplacementRegistration register_virtual_replacement(std::string_view path, VirtualFileSource source,
                                                     ReplacementOptions options = {});

ReplacementRegistration register_replacement(ReplacementKey key, RawTextureReplacement replacement,
                                             ReplacementOptions options = {});
void unregister_replacement(const ReplacementRegistration& registration);
void unregister_replacements(std::span<const ReplacementRegistration> registrations);
void unregister_replacements(const ReplacementGroup& group);
void unregister_replacements(const ReplacementKey& key);
void clear_replacements();
// Registrations with a priority below `floor` are ignored until it is lowered again (default
// INT32_MIN: none), without unregistering or dropping prefetched data: e.g. texture packs (priority
// 0) switched off for a moment by a hotkey while higher-priority replacements stay.
void set_replacement_priority_floor(int32_t floor);
// RL-NX (Stage 12): a file replacement larger than maxSize in either dimension drops its top mip levels until it
// fits (0: no limit, the default). Applies to textures loaded after the call.
void set_replacement_max_size(uint32_t maxSize);

ReplacementGroup load_replacement_directory(const std::filesystem::path& root, ReplacementOptions options = {});
void reload_replacement_directory(const std::filesystem::path& root, ReplacementGroup& group,
                                  ReplacementOptions options = {});

bool has_replacement(const GXTexObj* obj, const GXTlutObj* tlut = nullptr);

// Prefetch (Dolphin's "Prefetch Custom Textures"): every registered file texture is read and
// parsed into memory on the texture workers, up to budgetBytes; a texture used for the first
// time (or again after leaving the GPU cache) then gets its full GPU texture at once, without a
// low-resolution preview or a disk read first. Registrations made later are not prefetched.
struct PrefetchStatus {
  uint64_t total = 0;    // textures queued
  uint64_t done = 0;     // loaded, failed or skipped
  uint64_t bytes = 0;    // held in memory
  bool budgetFull = false;
};
void prefetch_replacements(uint64_t budgetBytes);
PrefetchStatus prefetch_status();
// Frees every prefetched texture (e.g. after unregistering a pack).
void clear_prefetched_replacements();

} // namespace aurora::texture

#endif
