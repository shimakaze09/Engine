// Encodes small RGB images as PNG files for tests that hand the engine a
// texture through its real loader. The image data is stored uncompressed
// (deflate "stored" blocks), so a test needs no compressor, and the
// encoding is exact: what a test writes is what the decoder reads back.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::tests {

namespace png_detail {

inline void put_u32(std::vector<unsigned char> *out,
                    std::uint32_t value) noexcept {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out->push_back(static_cast<unsigned char>((value >> shift) & 0xFFU));
  }
}

/// CRC-32 as PNG chunks use it, over a chunk's type and data.
inline std::uint32_t chunk_crc(const std::vector<unsigned char> &body) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const unsigned char byte : body) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

inline void put_chunk(std::vector<unsigned char> *png, const char *type,
                      const std::vector<unsigned char> &data) {
  put_u32(png, static_cast<std::uint32_t>(data.size()));
  std::vector<unsigned char> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  png->insert(png->end(), body.begin(), body.end());
  put_u32(png, chunk_crc(body));
}

} // namespace png_detail

/// A `width` x `height` 8-bit RGB PNG of `rgb`, three bytes per texel in
/// rows from the top. Empty when `rgb` does not hold exactly that many.
inline std::vector<unsigned char>
encode_rgb8_png(std::uint32_t width, std::uint32_t height,
                const std::vector<unsigned char> &rgb) {
  using png_detail::put_chunk;
  using png_detail::put_u32;
  const std::size_t rowBytes = static_cast<std::size_t>(width) * 3U;
  if (rgb.size() != (rowBytes * height)) {
    return {};
  }
  std::vector<unsigned char> raw{};
  raw.reserve((rowBytes + 1U) * height);
  for (std::uint32_t y = 0U; y < height; ++y) {
    raw.push_back(0U); // filter: none
    const auto row =
        rgb.begin() + static_cast<std::ptrdiff_t>(rowBytes * y);
    raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(rowBytes));
  }

  std::vector<unsigned char> zlib{0x78U, 0x01U};
  std::uint32_t a = 1U;
  std::uint32_t b = 0U;
  for (const unsigned char byte : raw) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  for (std::size_t offset = 0U; offset < raw.size(); offset += 65535U) {
    const std::size_t length =
        ((raw.size() - offset) < 65535U) ? (raw.size() - offset) : 65535U;
    const bool last = (offset + length) == raw.size();
    zlib.push_back(last ? 1U : 0U);
    zlib.push_back(static_cast<unsigned char>(length & 0xFFU));
    zlib.push_back(static_cast<unsigned char>((length >> 8U) & 0xFFU));
    zlib.push_back(static_cast<unsigned char>(~length & 0xFFU));
    zlib.push_back(static_cast<unsigned char>((~length >> 8U) & 0xFFU));
    zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
  }
  put_u32(&zlib, (b << 16U) | a);

  std::vector<unsigned char> png{0x89U, 'P',   'N',   'G',
                                 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  std::vector<unsigned char> header{};
  put_u32(&header, width);
  put_u32(&header, height);
  header.insert(header.end(), {8U, 2U, 0U, 0U, 0U}); // 8-bit RGB
  put_chunk(&png, "IHDR", header);
  put_chunk(&png, "IDAT", zlib);
  put_chunk(&png, "IEND", {});
  return png;
}

/// A `size` x `size` RGB PNG whose every channel of every texel is
/// `level`.
inline std::vector<unsigned char> uniform_rgb8_png(std::uint32_t size,
                                                   unsigned char level) {
  return encode_rgb8_png(
      size, size,
      std::vector<unsigned char>(static_cast<std::size_t>(size) * size * 3U,
                                 level));
}

} // namespace engine::tests
