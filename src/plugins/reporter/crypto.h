#pragma once

#include <string>

namespace reporter {

// 与 go-daemon（CryptoJS/OpenSSL）兼容的 AES-256-CBC 加解密。
// 密文格式：base64("Salted__" + 8字节salt + AES-CBC密文)，PKCS7 填充，
// 密钥/IV 由 EVP_BytesToKey(MD5, 单轮) 从 password+salt 派生。

// 加密明文，返回 base64 密文。失败时返回空串。
std::string encryptAES(const std::string& secret, const std::string& plain);

// 解密 base64 密文到 out。成功返回 true。
bool decryptAES(const std::string& secret, const std::string& b64, std::string& out);

}  // namespace reporter
