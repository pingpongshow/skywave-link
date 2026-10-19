// LZH compression in the Winlink / FBB "B2" LZHUF format.
//
// The algorithm is LZHUF (LZSS by Haruhiko Okumura, adaptive Huffman coding
// by Haruyasu Yoshizaki, "use, distribute, and modify this program freely")
// with the FBB parameters (2048-byte ring buffer, 60-byte look-ahead) and the
// B2 header (CRC16 + 32-bit length). The encoder's input order (pre-filling
// the look-ahead while inserting tree nodes) follows la5nta/wl2k-go's
// lzhuf.Writer (MIT licence, Copyright 2015-2016 Martin Hebnes Pedersen),
// which reproduces the reference Winlink/JNOS output byte for byte; the
// known-answer tests in tests/test_lzh.cpp check that compatibility.

#include "skywave_link/lzh.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace skylink {

namespace {

constexpr int N = LZH_N;
constexpr int F = LZH_F;
constexpr int THRESHOLD = LZH_THRESHOLD;
constexpr int NIL = N;                        // leaf of the search tree
constexpr int N_CHAR = 256 - THRESHOLD + F;   // kinds of symbols
constexpr int T = N_CHAR * 2 - 1;             // size of the Huffman table
constexpr int R = T - 1;                      // position of the root
constexpr uint32_t MAX_FREQ = 0x8000;         // rebuild the tree at this count

// Upper six bits of a match position: code length and code (classic
// lzhuf.c p_len / p_code tables).
constexpr uint8_t kPosLen[64] = {
    3, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
};
constexpr uint8_t kPosCode[64] = {
    0x00, 0x20, 0x30, 0x40, 0x50, 0x58, 0x60, 0x68,
    0x70, 0x78, 0x80, 0x88, 0x90, 0x94, 0x98, 0x9C,
    0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC,
    0xC0, 0xC2, 0xC4, 0xC6, 0xC8, 0xCA, 0xCC, 0xCE,
    0xD0, 0xD2, 0xD4, 0xD6, 0xD8, 0xDA, 0xDC, 0xDE,
    0xE0, 0xE2, 0xE4, 0xE6, 0xE8, 0xEA, 0xEC, 0xEE,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

// Decoding tables (lzhuf.c d_code / d_len), derived from the encoding
// tables: every byte whose leading kPosLen[j] bits equal code j decodes to j.
struct DecodeTables {
    std::array<uint8_t, 256> code{};
    std::array<uint8_t, 256> len{};
    DecodeTables() {
        for (int j = 0; j < 64; ++j) {
            const int span = 1 << (8 - kPosLen[j]);
            for (int k = 0; k < span; ++k) {
                code[kPosCode[j] + k] = static_cast<uint8_t>(j);
                len[kPosCode[j] + k] = kPosLen[j];
            }
        }
    }
};

const DecodeTables& decodeTables() {
    static const DecodeTables tables;
    return tables;
}

// CRC-16/XMODEM table (poly 0x1021). wl2k-go computes the same CRC in its
// "augmented" form (message followed by two zero bytes).
struct Crc16Table {
    std::array<uint16_t, 256> t{};
    Crc16Table() {
        for (int i = 0; i < 256; ++i) {
            uint16_t crc = static_cast<uint16_t>(i << 8);
            for (int b = 0; b < 8; ++b) {
                crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                     : static_cast<uint16_t>(crc << 1);
            }
            t[static_cast<size_t>(i)] = crc;
        }
    }
};

}  // namespace

uint16_t lzhCrc16(const uint8_t* data, size_t len) {
    static const Crc16Table table;
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc = static_cast<uint16_t>((crc << 8) ^ table.t[((crc >> 8) ^ data[i]) & 0xFF]);
    }
    return crc;
}

struct LZHCodec::State {
    // Huffman tree
    uint32_t freq[T + 1];
    int prnt[T + N_CHAR];
    int son[T];

    // LZSS search tree and ring buffer
    int dad[N + 1];
    int lson[N + 1];
    int rson[N + 257];
    uint8_t text[N + F - 1];
    int match_length = 0;
    int match_position = 0;

    void initHuffman() {
        for (int i = 0; i < N_CHAR; ++i) {
            freq[i] = 1;
            son[i] = i + T;
            prnt[i + T] = i;
        }
        for (int i = 0, j = N_CHAR; j <= R; i += 2, ++j) {
            freq[j] = freq[i] + freq[i + 1];
            son[j] = i;
            prnt[i] = prnt[i + 1] = j;
        }
        freq[T] = 0xFFFF;
        prnt[R] = 0;
    }

    void initTree() {
        for (int i = N + 1; i <= N + 256; ++i) rson[i] = NIL;
        for (int i = 0; i < N; ++i) dad[i] = NIL;
    }

    // Inserts string text[r..r+F-1] into the tree and records the longest
    // match (nearest one on ties) as a distance code, as lzhuf.c does.
    void insertNode(int r) {
        int cmp = 1;
        const uint8_t* key = &text[r];
        int p = N + 1 + key[0];
        rson[r] = lson[r] = NIL;
        match_length = 0;
        for (;;) {
            if (cmp >= 0) {
                if (rson[p] != NIL) {
                    p = rson[p];
                } else {
                    rson[p] = r;
                    dad[r] = p;
                    return;
                }
            } else {
                if (lson[p] != NIL) {
                    p = lson[p];
                } else {
                    lson[p] = r;
                    dad[r] = p;
                    return;
                }
            }
            int i = 1;
            for (; i < F; ++i) {
                cmp = static_cast<int>(key[i]) - static_cast<int>(text[p + i]);
                if (cmp != 0) break;
            }
            if (i > THRESHOLD) {
                if (i > match_length) {
                    match_position = ((r - p) & (N - 1)) - 1;
                    match_length = i;
                    if (match_length >= F) break;
                }
                if (i == match_length) {
                    const int c = ((r - p) & (N - 1)) - 1;
                    if (c < match_position) match_position = c;
                }
            }
        }
        dad[r] = dad[p];
        lson[r] = lson[p];
        rson[r] = rson[p];
        dad[lson[p]] = r;
        dad[rson[p]] = r;
        if (rson[dad[p]] == p) {
            rson[dad[p]] = r;
        } else {
            lson[dad[p]] = r;
        }
        dad[p] = NIL;
    }

    void deleteNode(int p) {
        if (dad[p] == NIL) return;
        int q;
        if (rson[p] == NIL) {
            q = lson[p];
        } else if (lson[p] == NIL) {
            q = rson[p];
        } else {
            q = lson[p];
            if (rson[q] != NIL) {
                do {
                    q = rson[q];
                } while (rson[q] != NIL);
                rson[dad[q]] = lson[q];
                dad[lson[q]] = dad[q];
                lson[q] = lson[p];
                dad[lson[p]] = q;
            }
            rson[q] = rson[p];
            dad[rson[p]] = q;
        }
        dad[q] = dad[p];
        if (rson[dad[p]] == p) {
            rson[dad[p]] = q;
        } else {
            lson[dad[p]] = q;
        }
        dad[p] = NIL;
    }

    void reconstruct() {
        // Collect leaves, halving their counts.
        int j = 0;
        for (int i = 0; i < T; ++i) {
            if (son[i] >= T) {
                freq[j] = (freq[i] + 1) / 2;
                son[j] = son[i];
                ++j;
            }
        }
        // Rebuild the tree by connecting sons.
        for (int i = 0, k = 0; j < T; i += 2, ++j) {  // j == N_CHAR here
            k = i + 1;
            const uint32_t f = freq[i] + freq[k];
            freq[j] = f;
            for (k = j - 1; f < freq[k]; --k) {
            }
            ++k;
            const size_t count = static_cast<size_t>(j - k);
            std::memmove(&freq[k + 1], &freq[k], count * sizeof(freq[0]));
            freq[k] = f;
            std::memmove(&son[k + 1], &son[k], count * sizeof(son[0]));
            son[k] = i;
        }
        for (int i = 0; i < T; ++i) {
            const int k = son[i];
            if (k >= T) {
                prnt[k] = i;
            } else {
                prnt[k] = prnt[k + 1] = i;
            }
        }
    }

    void update(int c) {
        if (freq[R] == MAX_FREQ) reconstruct();
        c = prnt[c + T];
        do {
            const uint32_t k = ++freq[c];
            int l = c + 1;
            if (k > freq[l]) {
                while (k > freq[++l]) {
                }
                --l;
                freq[c] = freq[l];
                freq[l] = k;

                const int i = son[c];
                prnt[i] = l;
                if (i < T) prnt[i + 1] = l;

                const int j = son[l];
                son[l] = i;

                prnt[j] = c;
                if (j < T) prnt[j + 1] = c;
                son[c] = j;

                c = l;
            }
            c = prnt[c];
        } while (c != 0);
    }
};

namespace {

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

    // Appends the top `l` bits of the 16-bit left-aligned code `c`.
    void put(int l, uint32_t c) {
        putbuf_ |= c >> putlen_;
        putlen_ += l;
        if (putlen_ < 8) return;
        out_.push_back(static_cast<uint8_t>(putbuf_ >> 8));
        putlen_ -= 8;
        if (putlen_ >= 8) {
            out_.push_back(static_cast<uint8_t>(putbuf_));
            putlen_ -= 8;
            putbuf_ = (c << (l - putlen_)) & 0xFFFF;
        } else {
            putbuf_ = (putbuf_ << 8) & 0xFFFF;
        }
    }

    void flush() {
        if (putlen_ > 0) out_.push_back(static_cast<uint8_t>(putbuf_ >> 8));
    }

private:
    std::vector<uint8_t>& out_;
    uint32_t putbuf_ = 0;
    int putlen_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // Returns false once the stream would read past its end.
    bool bits(int n, uint32_t* out) {
        while (avail_ < n) {
            if (pos_ >= size_) return false;
            acc_ = (acc_ << 8) | data_[pos_++];
            avail_ += 8;
        }
        *out = static_cast<uint32_t>((acc_ >> (avail_ - n)) & ((1u << n) - 1));
        avail_ -= n;
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint64_t acc_ = 0;
    int avail_ = 0;
};

void putLe32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

}  // namespace

LZHCodec::LZHCodec() : st_(std::make_unique<State>()) {}
LZHCodec::~LZHCodec() = default;

std::vector<uint8_t> LZHCodec::compress(const std::vector<uint8_t>& input) {
    State& z = *st_;
    z.initHuffman();
    z.initTree();
    std::memset(z.text, 0, sizeof(z.text));

    std::vector<uint8_t> bits;
    bits.reserve(input.size() / 2 + 16);
    BitWriter bw(bits);

    int r = N - F;
    int s = 0;
    int len = 0;
    int last_match_length = 0;
    bool prefilled = false;
    std::memset(z.text, ' ', static_cast<size_t>(r));

    auto encodeChar = [&](int c) {
        uint32_t code = 0;
        int code_len = 0;
        int k = z.prnt[c + T];
        do {
            code >>= 1;
            if (k & 1) code += 0x8000;
            ++code_len;
            k = z.prnt[k];
        } while (k != R);
        bw.put(code_len, code);
        z.update(c);
    };
    auto encodePosition = [&](int c) {
        const int i = c >> 6;
        bw.put(kPosLen[i], static_cast<uint32_t>(kPosCode[i]) << 8);
        bw.put(6, static_cast<uint32_t>(c & 0x3F) << 10);
    };
    auto encode = [&]() {
        if (len == 0) return;
        if (z.match_length > len) z.match_length = len;
        if (z.match_length <= THRESHOLD) {
            z.match_length = 1;
            encodeChar(z.text[r]);
        } else {
            encodeChar(255 - THRESHOLD + z.match_length);
            encodePosition(z.match_position);
        }
        last_match_length = z.match_length;
    };
    auto advance = [&](const uint8_t* c) {
        if (c != nullptr) {
            z.text[s] = *c;
            if (s < F - 1) z.text[s + N] = *c;
            ++len;
        }
        z.insertNode(r);
        if (--last_match_length == 0) encode();
        z.deleteNode(s);
        s = (s + 1) & (N - 1);
        r = (r + 1) & (N - 1);
        --len;
    };

    for (const uint8_t& byte : input) {
        if (!prefilled) {
            // Fill the look-ahead, inserting the preceding space strings.
            z.text[r + len] = byte;
            ++len;
            z.insertNode(r - len);
            last_match_length = 1;
            prefilled = (len == F);
            continue;
        }
        advance(&byte);
    }
    while (len > 0) advance(nullptr);
    encode();
    bw.flush();

    std::vector<uint8_t> out;
    out.reserve(LZH_HEADER_SIZE + bits.size());
    out.push_back(0);
    out.push_back(0);
    putLe32(out, static_cast<uint32_t>(input.size()));
    out.insert(out.end(), bits.begin(), bits.end());
    const uint16_t crc = lzhCrc16(out.data() + 2, out.size() - 2);
    out[0] = static_cast<uint8_t>(crc & 0xFF);
    out[1] = static_cast<uint8_t>(crc >> 8);
    return out;
}

bool LZHCodec::decompress(const std::vector<uint8_t>& input,
                          std::vector<uint8_t>& output,
                          std::string* error,
                          size_t max_output) {
    output.clear();
    auto fail = [&](const char* why) {
        output.clear();
        if (error) *error = why;
        return false;
    };

    if (input.size() < LZH_HEADER_SIZE) return fail("LZH data shorter than its header");

    const uint16_t crc = static_cast<uint16_t>(input[0] | (input[1] << 8));
    if (lzhCrc16(input.data() + 2, input.size() - 2) != crc) return fail("LZH CRC mismatch");

    const uint32_t textsize = static_cast<uint32_t>(input[2]) |
                              (static_cast<uint32_t>(input[3]) << 8) |
                              (static_cast<uint32_t>(input[4]) << 16) |
                              (static_cast<uint32_t>(input[5]) << 24);
    if (textsize > max_output) return fail("LZH length exceeds limit");
    if (textsize == 0) return true;

    State& z = *st_;
    z.initHuffman();
    std::memset(z.text, ' ', N - F);
    std::memset(z.text + (N - F), 0, sizeof(z.text) - (N - F));
    const DecodeTables& dt = decodeTables();

    BitReader br(input.data() + LZH_HEADER_SIZE, input.size() - LZH_HEADER_SIZE);
    output.reserve(textsize);

    int r = N - F;
    while (output.size() < textsize) {
        // Symbol: walk the Huffman tree from the root.
        int c = z.son[R];
        while (c < T) {
            uint32_t bit = 0;
            if (!br.bits(1, &bit)) return fail("LZH bit stream truncated");
            c = z.son[c + static_cast<int>(bit)];
        }
        c -= T;
        z.update(c);

        if (c < 256) {
            output.push_back(static_cast<uint8_t>(c));
            z.text[r] = static_cast<uint8_t>(c);
            r = (r + 1) & (N - 1);
            continue;
        }

        // Match: upper six position bits via the decode tables, then the rest.
        uint32_t first = 0;
        if (!br.bits(8, &first)) return fail("LZH bit stream truncated");
        const uint32_t upper = static_cast<uint32_t>(dt.code[first]) << 6;
        const int extra = dt.len[first] - 2;
        uint32_t low = first;
        for (int j = 0; j < extra; ++j) {
            uint32_t bit = 0;
            if (!br.bits(1, &bit)) return fail("LZH bit stream truncated");
            low = (low << 1) | bit;
        }
        const int position = static_cast<int>(upper | (low & 0x3F));
        const int start = (r - position - 1) & (N - 1);
        const int count = c - 255 + THRESHOLD;
        if (output.size() + static_cast<size_t>(count) > textsize) {
            return fail("LZH match runs past the announced length");
        }
        for (int k = 0; k < count; ++k) {
            const uint8_t ch = z.text[(start + k) & (N - 1)];
            output.push_back(ch);
            z.text[r] = ch;
            r = (r + 1) & (N - 1);
        }
    }
    return true;
}

std::vector<uint8_t> LZHCodec::decompress(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    if (!decompress(input, out, nullptr, LZH_DEFAULT_MAX_OUTPUT)) out.clear();
    return out;
}

} // namespace skylink
