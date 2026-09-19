#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/md5.h>

#include <cstring>
#include <string>

namespace reporter {

namespace {

// 复刻 OpenSSL EVP_BytesToKey（MD5，单轮迭代），派生 key+iv。
void evpBytesToKey(const std::string& password, const unsigned char* salt,
                   int saltLen, int keyLen, int ivLen,
                   std::string& key, std::string& iv) {
  int need = keyLen + ivLen;
  std::string derived;
  std::string prev;
  while ((int)derived.size() < need) {
    MD5_CTX ctx;
    MD5_Init(&ctx);
    if (!prev.empty()) MD5_Update(&ctx, prev.data(), prev.size());
    MD5_Update(&ctx, password.data(), password.size());
    MD5_Update(&ctx, salt, saltLen);
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &ctx);
    prev.assign((char*)digest, MD5_DIGEST_LENGTH);
    derived.append(prev);
  }
  key = derived.substr(0, keyLen);
  iv = derived.substr(keyLen, ivLen);
}

std::string pkcs7Pad(const std::string& data, int block) {
  int pad = block - (int)(data.size() % block);
  if (pad == 0) pad = block;
  std::string out = data;
  out.append(pad, (char)pad);
  return out;
}

// 简单 base64（EVP 也可用，这里手写避免额外依赖面）
static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64Encode(const unsigned char* in, size_t len) {
  std::string out;
  size_t i = 0;
  while (i + 3 <= len) {
    uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out.push_back(kB64[(n >> 18) & 0x3f]);
    out.push_back(kB64[(n >> 12) & 0x3f]);
    out.push_back(kB64[(n >> 6) & 0x3f]);
    out.push_back(kB64[n & 0x3f]);
    i += 3;
  }
  size_t rem = len - i;
  if (rem == 1) {
    uint32_t n = in[i] << 16;
    out.push_back(kB64[(n >> 18) & 0x3f]);
    out.push_back(kB64[(n >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    uint32_t n = (in[i] << 16) | (in[i + 1] << 8);
    out.push_back(kB64[(n >> 18) & 0x3f]);
    out.push_back(kB64[(n >> 12) & 0x3f]);
    out.push_back(kB64[(n >> 6) & 0x3f]);
    out.push_back('=');
  }
  return out;
}

bool b64Decode(const std::string& in, std::string& out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  out.clear();
  int buf = 0, bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    int v = val(c);
    if (v < 0) return false;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((char)((buf >> bits) & 0xff));
    }
  }
  return true;
}

}  // namespace

std::string encryptAES(const std::string& secret, const std::string& plain) {
  unsigned char salt[8];
  if (RAND_bytes(salt, sizeof(salt)) != 1) return "";
  std::string key, iv;
  evpBytesToKey(secret, salt, 8, 32, 16, key, iv);

  std::string padded = pkcs7Pad(plain, 16);
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return "";
  std::string ct(padded.size(), '\0');
  int len1 = 0, len2 = 0;
  EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
                     (const unsigned char*)key.data(), (const unsigned char*)iv.data());
  EVP_CIPHER_CTX_set_padding(ctx, 0);  // 已手动 PKCS7
  EVP_EncryptUpdate(ctx, (unsigned char*)&ct[0], &len1,
                    (const unsigned char*)padded.data(), (int)padded.size());
  EVP_EncryptFinal_ex(ctx, (unsigned char*)&ct[len1], &len2);
  EVP_CIPHER_CTX_free(ctx);
  ct.resize(len1 + len2);

  std::string out;
  out.append("Salted__");
  out.append((char*)salt, 8);
  out.append(ct);
  return b64Encode((const unsigned char*)out.data(), out.size());
}

bool decryptAES(const std::string& secret, const std::string& b64, std::string& out) {
  std::string raw;
  if (!b64Decode(b64, raw)) return false;
  const char magic[] = "Salted__";
  if (raw.size() < 16 || raw.compare(0, 8, magic) != 0) return false;
  std::string salt = raw.substr(8, 8);
  std::string ct = raw.substr(16);
  if (ct.empty() || ct.size() % 16 != 0) return false;

  std::string key, iv;
  evpBytesToKey(secret, (const unsigned char*)salt.data(), 8, 32, 16, key, iv);

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  std::string pt(ct.size(), '\0');
  int len1 = 0, len2 = 0;
  EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
                     (const unsigned char*)key.data(), (const unsigned char*)iv.data());
  EVP_CIPHER_CTX_set_padding(ctx, 0);
  EVP_DecryptUpdate(ctx, (unsigned char*)&pt[0], &len1,
                    (const unsigned char*)ct.data(), (int)ct.size());
  EVP_DecryptFinal_ex(ctx, (unsigned char*)&pt[len1], &len2);
  EVP_CIPHER_CTX_free(ctx);
  pt.resize(len1 + len2);

  // 去 PKCS7
  if (pt.empty()) return false;
  unsigned char pad = (unsigned char)pt.back();
  if (pad < 1 || pad > 16 || pad > pt.size()) return false;
  for (size_t i = pt.size() - pad; i < pt.size(); ++i)
    if ((unsigned char)pt[i] != pad) return false;
  out = pt.substr(0, pt.size() - pad);
  return true;
}

}  // namespace reporter
