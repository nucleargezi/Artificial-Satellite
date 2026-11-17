#include "codec/gzip.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace yrs::gzip {

namespace {

uint32_t crc32_ieee(std::string_view data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char b : data) {
    crc ^= b;
    for (int i = 0; i < 8; ++i) {
      uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

uint16_t read_le16(const unsigned char *p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_le32(const unsigned char *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

struct BitReader {
  const unsigned char *data;
  size_t pos;
  size_t end;
  uint32_t bit_buf = 0;
  int bit_cnt = 0;

  bool ensure(int n) {
    while (bit_cnt < n) {
      if (pos >= end) return false;
      bit_buf |= static_cast<uint32_t>(data[pos++]) << bit_cnt;
      bit_cnt += 8;
    }
    return true;
  }

  bool read_bits(int n, uint32_t &out) {
    if (n == 0) {
      out = 0;
      return true;
    }
    if (!ensure(n)) return false;
    out = bit_buf & ((1u << n) - 1u);
    bit_buf >>= n;
    bit_cnt -= n;
    return true;
  }

  void align_to_byte() {
    int discard = bit_cnt & 7;
    bit_buf >>= discard;
    bit_cnt -= discard;
    pos -= static_cast<size_t>(bit_cnt / 8);
    bit_buf = 0;
    bit_cnt = 0;
  }

  bool read_aligned_u16(uint16_t &out) {
    if (pos + 2 > end) return false;
    out = read_le16(data + pos);
    pos += 2;
    return true;
  }
};

struct HuffTree {
  std::array<uint16_t, 16> count{};
  std::vector<uint16_t> symbol;

  bool build(std::span<const uint8_t> lengths) {
    count.fill(0);
    for (uint8_t len : lengths) {
      if (len > 15) return false;
      ++count[len];
    }

    if (count[0] == lengths.size()) {
      symbol.clear();
      return true;
    }

    int left = 1;
    for (int len = 1; len <= 15; ++len) {
      left <<= 1;
      left -= count[len];
      if (left < 0) return false;
    }

    std::array<uint16_t, 16> offs{};
    for (int len = 1; len < 15; ++len) {
      offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
    }

    symbol.assign(offs[15] + count[15], 0);
    for (size_t sym = 0; sym < lengths.size(); ++sym) {
      uint8_t len = lengths[sym];
      if (len != 0) {
        symbol[offs[len]++] = static_cast<uint16_t>(sym);
      }
    }
    return true;
  }

  bool decode(BitReader &br, uint16_t &out_sym) const {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 15; ++len) {
      uint32_t bit = 0;
      if (!br.read_bits(1, bit)) return false;
      code |= static_cast<int>(bit);
      int cnt = count[len];
      if (code - cnt < first) {
        out_sym = symbol[static_cast<size_t>(index + (code - first))];
        return true;
      }
      index += cnt;
      first += cnt;
      first <<= 1;
      code <<= 1;
    }
    return false;
  }
};

Result<void> decode_huffman_block(BitReader &br, const HuffTree &lit_tree,
                                  const HuffTree &dist_tree, std::string &out) {
  static constexpr std::array<uint16_t, 29> LEN_BASE = {
      3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr std::array<uint8_t, 29> LEN_EXTRA = {
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr std::array<uint16_t, 30> DIST_BASE = {
      1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
      33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static constexpr std::array<uint8_t, 30> DIST_EXTRA = {
      0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  static constexpr size_t MAX_OUTPUT_BYTES = 128ULL * 1024ULL * 1024ULL;

  while (true) {
    uint16_t sym = 0;
    if (!lit_tree.decode(br, sym)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate literal symbol"));
    }
    if (sym < 256) {
      if (out.size() >= MAX_OUTPUT_BYTES) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "gzip output exceeds size limit"));
      }
      out.push_back(static_cast<char>(sym));
      continue;
    }
    if (sym == 256) return {};
    if (sym >= 286) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate length symbol"));
    }

    size_t lidx = sym - 257;
    uint32_t extra = 0;
    if (!br.read_bits(LEN_EXTRA[lidx], extra)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated deflate length extra bits"));
    }
    size_t match_len = LEN_BASE[lidx] + extra;

    uint16_t dsym = 0;
    if (!dist_tree.decode(br, dsym) || dsym >= 30) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate distance symbol"));
    }
    uint32_t dextra = 0;
    if (!br.read_bits(DIST_EXTRA[dsym], dextra)) {
      return std::unexpected(make_err(ErrKind::InvalidData,
                                      "truncated deflate distance extra bits"));
    }
    size_t dist = DIST_BASE[dsym] + dextra;
    if (dist == 0 || dist > out.size()) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate back-reference"));
    }
    if (out.size() + match_len > MAX_OUTPUT_BYTES) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "gzip output exceeds size limit"));
    }

    for (size_t i = 0; i < match_len; ++i) {
      out.push_back(out[out.size() - dist]);
    }
  }
}

Result<void> decode_fixed_block(BitReader &br, std::string &out) {
  std::array<uint8_t, 288> lit_lens{};
  for (size_t i = 0; i <= 143; ++i) lit_lens[i] = 8;
  for (size_t i = 144; i <= 255; ++i) lit_lens[i] = 9;
  for (size_t i = 256; i <= 279; ++i) lit_lens[i] = 7;
  for (size_t i = 280; i <= 287; ++i) lit_lens[i] = 8;

  std::array<uint8_t, 32> dist_lens{};
  dist_lens.fill(5);

  HuffTree lit_tree;
  HuffTree dist_tree;
  if (!lit_tree.build(lit_lens) || !dist_tree.build(dist_lens)) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "failed to build fixed huffman trees"));
  }
  return decode_huffman_block(br, lit_tree, dist_tree, out);
}

Result<void> decode_dynamic_block(BitReader &br, std::string &out) {
  static constexpr std::array<uint8_t, 19> CL_ORDER = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

  uint32_t hlit_raw = 0, hdist_raw = 0, hclen_raw = 0;
  if (!br.read_bits(5, hlit_raw) || !br.read_bits(5, hdist_raw) ||
      !br.read_bits(4, hclen_raw)) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "truncated dynamic deflate header"));
  }
  size_t hlit = hlit_raw + 257;
  size_t hdist = hdist_raw + 1;
  size_t hclen = hclen_raw + 4;
  if (hlit > 286 || hdist > 30) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid dynamic deflate code counts"));
  }

  std::array<uint8_t, 19> cl_lens{};
  for (size_t i = 0; i < hclen; ++i) {
    uint32_t v = 0;
    if (!br.read_bits(3, v)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated code length table"));
    }
    cl_lens[CL_ORDER[i]] = static_cast<uint8_t>(v);
  }

  HuffTree cl_tree;
  if (!cl_tree.build(cl_lens)) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid code length huffman tree"));
  }

  std::vector<uint8_t> lens(hlit + hdist, 0);
  size_t idx = 0;
  while (idx < hlit + hdist) {
    uint16_t sym = 0;
    if (!cl_tree.decode(br, sym)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid code length symbol"));
    }
    if (sym < 16) {
      lens[idx++] = static_cast<uint8_t>(sym);
      continue;
    }

    uint8_t rep_val = 0;
    size_t rep_cnt = 0;
    uint32_t extra = 0;
    if (sym == 16) {
      if (idx == 0 || !br.read_bits(2, extra)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid repeat length code"));
      }
      rep_val = lens[idx - 1];
      rep_cnt = 3 + extra;
    } else if (sym == 17) {
      if (!br.read_bits(3, extra)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid repeat zero code"));
      }
      rep_cnt = 3 + extra;
    } else if (sym == 18) {
      if (!br.read_bits(7, extra)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "invalid repeat long zero code"));
      }
      rep_cnt = 11 + extra;
    } else {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "unexpected code length symbol"));
    }

    if (idx + rep_cnt > hlit + hdist) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "code length table overflow"));
    }
    for (size_t k = 0; k < rep_cnt; ++k) {
      lens[idx++] = rep_val;
    }
  }

  if (lens[256] == 0) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "missing end-of-block symbol"));
  }

  HuffTree lit_tree;
  HuffTree dist_tree;
  if (!lit_tree.build(std::span<const uint8_t>(lens.data(), hlit)) ||
      !dist_tree.build(std::span<const uint8_t>(lens.data() + hlit, hdist))) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid dynamic huffman tree"));
  }
  return decode_huffman_block(br, lit_tree, dist_tree, out);
}

}  // namespace

Result<std::string> decompress(std::string_view input) {
  if (input.size() < 18) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "gzip input too short"));
  }
  const auto *bytes = reinterpret_cast<const unsigned char *>(input.data());
  if (bytes[0] != 0x1F || bytes[1] != 0x8B || bytes[2] != 8) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid gzip header"));
  }
  uint8_t flg = bytes[3];
  if ((flg & 0xE0u) != 0) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "invalid gzip flags"));
  }

  size_t pos = 10;
  auto skip_cstr = [&]() -> bool {
    while (pos < input.size() && bytes[pos] != 0) ++pos;
    if (pos >= input.size()) return false;
    ++pos;
    return true;
  };

  if (flg & 0x04u) {
    if (pos + 2 > input.size()) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated gzip FEXTRA"));
    }
    uint16_t xlen = read_le16(bytes + pos);
    pos += 2;
    if (xlen > input.size() - pos) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated gzip FEXTRA payload"));
    }
    pos += xlen;
  }
  if ((flg & 0x08u) && !skip_cstr()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "unterminated gzip FNAME"));
  }
  if ((flg & 0x10u) && !skip_cstr()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "unterminated gzip FCOMMENT"));
  }
  if (flg & 0x02u) {
    if (pos + 2 > input.size()) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated gzip FHCRC"));
    }
    pos += 2;
  }

  if (pos + 8 > input.size()) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "missing gzip trailer"));
  }

  size_t deflate_end = input.size() - 8;
  BitReader br{bytes, pos, deflate_end};
  std::string out;
  uint32_t expected_isize = read_le32(bytes + deflate_end + 4);
  if (expected_isize > 0 && expected_isize <= 16u * 1024u * 1024u) {
    out.reserve(expected_isize);
  }

  while (true) {
    uint32_t bfinal = 0, btype = 0;
    if (!br.read_bits(1, bfinal) || !br.read_bits(2, btype)) {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "truncated deflate block header"));
    }
    if (btype == 0) {
      br.align_to_byte();
      uint16_t len = 0, nlen = 0;
      if (!br.read_aligned_u16(len) || !br.read_aligned_u16(nlen)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "truncated stored deflate block"));
      }
      if (len != static_cast<uint16_t>(~nlen)) {
        return std::unexpected(
            make_err(ErrKind::InvalidData, "corrupt stored deflate block len"));
      }
      if (len > br.end - br.pos) {
        return std::unexpected(make_err(ErrKind::InvalidData,
                                        "truncated stored deflate payload"));
      }
      out.append(reinterpret_cast<const char *>(br.data + br.pos), len);
      br.pos += len;
    } else if (btype == 1) {
      auto res = decode_fixed_block(br, out);
      if (!res) return std::unexpected(res.error());
    } else if (btype == 2) {
      auto res = decode_dynamic_block(br, out);
      if (!res) return std::unexpected(res.error());
    } else {
      return std::unexpected(
          make_err(ErrKind::InvalidData, "invalid deflate block type"));
    }
    if (bfinal != 0) break;
  }

  br.align_to_byte();
  if (br.pos != deflate_end) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "unexpected trailing gzip data"));
  }

  uint32_t expected_crc = read_le32(bytes + deflate_end);
  uint32_t actual_crc = crc32_ieee(out);
  if (actual_crc != expected_crc) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "gzip CRC32 mismatch"));
  }
  uint32_t actual_isize = static_cast<uint32_t>(out.size());
  if (actual_isize != expected_isize) {
    return std::unexpected(
        make_err(ErrKind::InvalidData, "gzip ISIZE mismatch"));
  }
  return out;
}

std::string compress_stored(std::string_view input) {
  std::string out;
  size_t num_blocks =
      input.empty() ? 1 : ((input.size() + 65534u) / 65535u);
  out.reserve(10 + input.size() + num_blocks * 5 + 8);

  static constexpr unsigned char GZIP_HDR[10] = {
      0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
  out.append(reinterpret_cast<const char *>(GZIP_HDR), 10);

  if (input.empty()) {
    static constexpr unsigned char EMPTY_BLK[5] = {0x01, 0x00, 0x00, 0xFF, 0xFF};
    out.append(reinterpret_cast<const char *>(EMPTY_BLK), 5);
  } else {
    size_t offset = 0;
    while (offset < input.size()) {
      size_t chunk = std::min<size_t>(65535u, input.size() - offset);
      bool is_final = (offset + chunk == input.size());
      uint16_t len = static_cast<uint16_t>(chunk);
      uint16_t nlen = static_cast<uint16_t>(~len);
      unsigned char blk_hdr[5] = {
          static_cast<unsigned char>(is_final ? 0x01 : 0x00),
          static_cast<unsigned char>(len & 0xFFu),
          static_cast<unsigned char>((len >> 8) & 0xFFu),
          static_cast<unsigned char>(nlen & 0xFFu),
          static_cast<unsigned char>((nlen >> 8) & 0xFFu),
      };
      out.append(reinterpret_cast<const char *>(blk_hdr), 5);
      out.append(input.data() + offset, chunk);
      offset += chunk;
    }
  }

  uint32_t crc = crc32_ieee(input);
  uint32_t isize = static_cast<uint32_t>(input.size());
  unsigned char trailer[8] = {
      static_cast<unsigned char>(crc & 0xFFu),
      static_cast<unsigned char>((crc >> 8) & 0xFFu),
      static_cast<unsigned char>((crc >> 16) & 0xFFu),
      static_cast<unsigned char>((crc >> 24) & 0xFFu),
      static_cast<unsigned char>(isize & 0xFFu),
      static_cast<unsigned char>((isize >> 8) & 0xFFu),
      static_cast<unsigned char>((isize >> 16) & 0xFFu),
      static_cast<unsigned char>((isize >> 24) & 0xFFu),
  };
  out.append(reinterpret_cast<const char *>(trailer), 8);
  return out;
}

}  // namespace yrs::gzip
