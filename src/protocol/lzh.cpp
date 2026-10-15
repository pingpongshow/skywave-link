// LZH Compression - LZSS + Adaptive Huffman (B2F compatible)
// Based on LZHUF algorithm used by FBB BBS systems

#include "skywave_link/lzh.hpp"
#include <cstring>
#include <algorithm>

namespace skylink {

LZHCodec::LZHCodec() {
    reset();
}

void LZHCodec::reset() {
    match_position_ = 0;
    match_length_ = 0;
    putbuf_ = 0;
    putlen_ = 0;
    getbuf_ = 0;
    getlen_ = 0;
    input_ = nullptr;
    input_pos_ = 0;
}

void LZHCodec::initTree() {
    for (int i = N + 1; i <= N + 256; i++) {
        rson_[i] = NIL;
    }
    for (int i = 0; i < N; i++) {
        dad_[i] = NIL;
    }
}

void LZHCodec::insertNode(int r) {
    int cmp = 1;
    uint8_t* key = &text_buf_[r];
    int p = N + 1 + key[0];

    rson_[r] = lson_[r] = NIL;
    match_length_ = 0;

    for (;;) {
        if (cmp >= 0) {
            if (rson_[p] != NIL) {
                p = rson_[p];
            } else {
                rson_[p] = r;
                dad_[r] = p;
                return;
            }
        } else {
            if (lson_[p] != NIL) {
                p = lson_[p];
            } else {
                lson_[p] = r;
                dad_[r] = p;
                return;
            }
        }

        int i;
        for (i = 1; i < F; i++) {
            cmp = key[i] - text_buf_[p + i];
            if (cmp != 0) break;
        }

        if (i > match_length_) {
            match_position_ = p;
            match_length_ = i;
            if (match_length_ >= F) break;
        }
    }

    dad_[r] = dad_[p];
    lson_[r] = lson_[p];
    rson_[r] = rson_[p];
    dad_[lson_[p]] = r;
    dad_[rson_[p]] = r;

    if (rson_[dad_[p]] == p) {
        rson_[dad_[p]] = r;
    } else {
        lson_[dad_[p]] = r;
    }
    dad_[p] = NIL;
}

void LZHCodec::deleteNode(int p) {
    if (dad_[p] == NIL) return;

    int q;
    if (rson_[p] == NIL) {
        q = lson_[p];
    } else if (lson_[p] == NIL) {
        q = rson_[p];
    } else {
        q = lson_[p];
        if (rson_[q] != NIL) {
            do {
                q = rson_[q];
            } while (rson_[q] != NIL);

            rson_[dad_[q]] = lson_[q];
            dad_[lson_[q]] = dad_[q];
            lson_[q] = lson_[p];
            dad_[lson_[p]] = q;
        }
        rson_[q] = rson_[p];
        dad_[rson_[p]] = q;
    }

    dad_[q] = dad_[p];

    if (rson_[dad_[p]] == p) {
        rson_[dad_[p]] = q;
    } else {
        lson_[dad_[p]] = q;
    }

    dad_[p] = NIL;
}

void LZHCodec::initHuffman() {
    int i, j;

    for (i = 0; i < N_CHAR; i++) {
        freq_[i] = 1;
        son_[i] = i + T;
        prnt_[i + T] = i;
    }

    i = 0;
    j = N_CHAR;
    while (j <= R) {
        freq_[j] = freq_[i] + freq_[i + 1];
        son_[j] = i;
        prnt_[i] = prnt_[i + 1] = j;
        i += 2;
        j++;
    }

    freq_[T] = 0xFFFF;
    prnt_[R] = 0;
}

void LZHCodec::reconstruct() {
    int i, j, k;
    uint16_t f, l;

    j = 0;
    for (i = 0; i < T; i++) {
        if (son_[i] >= T) {
            freq_[j] = (freq_[i] + 1) / 2;
            son_[j] = son_[i];
            j++;
        }
    }

    for (i = 0, j = N_CHAR; j < T; i += 2, j++) {
        k = i + 1;
        f = freq_[j] = freq_[i] + freq_[k];
        for (k = j - 1; k >= 0 && f < freq_[k]; k--);
        k++;
        l = (j - k) * sizeof(freq_[0]);
        std::memmove(&freq_[k + 1], &freq_[k], l);
        freq_[k] = f;
        std::memmove(&son_[k + 1], &son_[k], l);
        son_[k] = i;
    }

    for (i = 0; i < T; i++) {
        k = son_[i];
        if (k >= T) {
            prnt_[k] = i;
        } else {
            prnt_[k] = prnt_[k + 1] = i;
        }
    }
}

void LZHCodec::update(int c) {
    if (freq_[R] == MAX_FREQ) {
        reconstruct();
    }

    c = prnt_[c + T];

    do {
        freq_[c]++;
        int k = freq_[c];

        int l = c + 1;
        if (k > freq_[l]) {
            while (k > freq_[++l]);
            l--;
            freq_[c] = freq_[l];
            freq_[l] = k;

            int i = son_[c];
            prnt_[i] = l;
            if (i < T) prnt_[i + 1] = l;

            int j = son_[l];
            son_[l] = i;

            prnt_[j] = c;
            if (j < T) prnt_[j + 1] = c;
            son_[c] = j;

            c = l;
        }
        c = prnt_[c];
    } while (c != 0);
}

void LZHCodec::putCode(int l, uint32_t c) {
    putbuf_ |= static_cast<uint16_t>(c >> putlen_);
    putlen_ += l;
    if (putlen_ >= 8) {
        output_.push_back(static_cast<uint8_t>(putbuf_ >> 8));
        putlen_ -= 8;
        if (putlen_ >= 8) {
            output_.push_back(static_cast<uint8_t>(putbuf_));
            putlen_ -= 8;
            putbuf_ = static_cast<uint16_t>(c << (l - putlen_));
        } else {
            putbuf_ <<= 8;
        }
    }
}

void LZHCodec::encodeChar(int c) {
    uint32_t code = 0;
    int len = 0;
    int k = prnt_[c + T];

    do {
        code >>= 1;
        if (k & 1) code |= 0x8000;
        len++;
        k = prnt_[k];
    } while (k != R);

    putCode(len, code);
    update(c);
}

void LZHCodec::encodePosition(int c) {
    int i = c >> 6;
    putCode(p_len_[i], static_cast<uint32_t>(p_code_[i]) << 8);
    putCode(6, static_cast<uint32_t>(c & 0x3F) << 10);
}

void LZHCodec::encodeEnd() {
    if (putlen_ > 0) {
        output_.push_back(static_cast<uint8_t>(putbuf_ >> 8));
    }
}

int LZHCodec::getBit() {
    while (getlen_ <= 8) {
        int c = (input_pos_ < input_->size()) ? (*input_)[input_pos_++] : 0;
        getbuf_ |= static_cast<uint16_t>(c << (8 - getlen_));
        getlen_ += 8;
    }
    int x = (getbuf_ & 0x8000) ? 1 : 0;
    getbuf_ <<= 1;
    getlen_--;
    return x;
}

int LZHCodec::getByte() {
    while (getlen_ <= 8) {
        int c = (input_pos_ < input_->size()) ? (*input_)[input_pos_++] : 0;
        getbuf_ |= static_cast<uint16_t>(c << (8 - getlen_));
        getlen_ += 8;
    }
    int x = getbuf_ >> 8;
    getbuf_ <<= 8;
    getlen_ -= 8;
    return x;
}

int LZHCodec::decodeChar() {
    int c = son_[R];

    // Limit iterations to prevent infinite loops from malformed data
    int iterations = 0;
    while (c < T) {
        if (++iterations > 32) return -1;  // Huffman tree depth cannot exceed ~17
        int bit = getBit();
        if (c + bit < 0 || c + bit >= T) return -1;
        c = son_[c + bit];
    }
    c -= T;
    if (c < 0 || c >= N_CHAR) return -1;
    update(c);
    return c;
}

int LZHCodec::decodePosition() {
    int i = getByte();
    int c = static_cast<int>(d_code_[i]) << 6;
    int j = d_len_[i];

    j -= 2;
    while (j-- > 0) {
        i = (i << 1) | getBit();
    }
    return c | (i & 0x3F);
}

std::vector<uint8_t> LZHCodec::compress(const std::vector<uint8_t>& input) {
    if (input.empty()) {
        return {};
    }

    reset();
    initTree();

    output_.clear();
    output_.reserve(input.size());

    uint32_t textsize = static_cast<uint32_t>(input.size());
    output_.push_back(textsize & 0xFF);
    output_.push_back((textsize >> 8) & 0xFF);
    output_.push_back((textsize >> 16) & 0xFF);
    output_.push_back((textsize >> 24) & 0xFF);

    initHuffman();

    if (textsize == 0) {
        return output_;
    }

    std::memset(text_buf_, ' ', N - F);

    size_t input_pos = 0;
    int len = 0;

    for (len = 0; len < F && input_pos < input.size(); len++) {
        text_buf_[N - F + len] = input[input_pos++];
    }

    for (int i = 1; i <= F; i++) {
        insertNode(N - F - i);
    }
    insertNode(N - F);

    int r = N - F;
    int s = 0;

    do {
        if (match_length_ > len) {
            match_length_ = len;
        }

        if (match_length_ <= THRESHOLD) {
            match_length_ = 1;
            encodeChar(text_buf_[r]);
        } else {
            encodeChar(255 - THRESHOLD + match_length_);
            encodePosition((r - 1 - match_position_) & (N - 1));
        }

        int last_match_length = match_length_;

        int i;
        for (i = 0; i < last_match_length && input_pos < input.size(); i++) {
            deleteNode(s);
            uint8_t c = input[input_pos++];
            text_buf_[s] = c;

            if (s < F - 1) {
                text_buf_[s + N] = c;
            }

            s = (s + 1) & (N - 1);
            r = (r + 1) & (N - 1);
            insertNode(r);
        }

        while (i++ < last_match_length) {
            deleteNode(s);
            s = (s + 1) & (N - 1);
            r = (r + 1) & (N - 1);
            if (--len) {
                insertNode(r);
            }
        }

    } while (len > 0);

    encodeEnd();

    return output_;
}

std::vector<uint8_t> LZHCodec::decompress(const std::vector<uint8_t>& input) {
    if (input.size() < 4) {
        return {};
    }

    reset();

    uint32_t textsize = static_cast<uint32_t>(input[0])
                      | (static_cast<uint32_t>(input[1]) << 8)
                      | (static_cast<uint32_t>(input[2]) << 16)
                      | (static_cast<uint32_t>(input[3]) << 24);

    if (textsize == 0) {
        return {};
    }

    // Guard against decompression bombs: reject claims larger than 1 MB
    // (MAX_FILE_SIZE is 50 KB; even with headers this is very generous)
    static constexpr uint32_t MAX_DECOMPRESS_SIZE = 1024 * 1024;
    if (textsize > MAX_DECOMPRESS_SIZE) {
        return {};
    }

    input_ = &input;
    input_pos_ = 4;

    initHuffman();

    std::vector<uint8_t> output;
    output.reserve(textsize);

    std::memset(text_buf_, ' ', N - F);

    int r = N - F;
    uint32_t count = 0;

    while (count < textsize) {
        int c = decodeChar();
        if (c < 0) break;

        if (c < 256) {
            output.push_back(static_cast<uint8_t>(c));
            text_buf_[r++] = static_cast<uint8_t>(c);
            r &= (N - 1);
            count++;
        } else {
            int i = (r - decodePosition() - 1) & (N - 1);
            int j = c - 255 + THRESHOLD;

            for (int k = 0; k < j && count < textsize; k++) {
                uint8_t ch = text_buf_[(i + k) & (N - 1)];
                output.push_back(ch);
                text_buf_[r++] = ch;
                r &= (N - 1);
                count++;
            }
        }
    }

    return output;
}

// Static member definitions
constexpr uint8_t LZHCodec::p_len_[64];
constexpr uint8_t LZHCodec::p_code_[64];
constexpr uint8_t LZHCodec::d_code_[256];
constexpr uint8_t LZHCodec::d_len_[256];

} // namespace skylink
