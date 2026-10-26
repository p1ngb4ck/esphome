#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::mtproto {

static constexpr size_t RSA_BYTES = 256;
static constexpr size_t DH_BYTES = 256;

struct ServerKey {
  uint64_t fingerprint;
  const char *modulus_hex;
};

void random_bytes(uint8_t *out, size_t length);

void sha1(const uint8_t *data, size_t length, uint8_t *out20);
void sha1_parts(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len, uint8_t *out20);
void sha256_parts(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len, uint8_t *out32);

bool aes_ige(const uint8_t *key, const uint8_t *iv, uint8_t *data, size_t length, bool encrypt);
bool aes_ctr(const uint8_t *key, const uint8_t *iv, uint8_t *data, size_t length);
bool pbkdf2_sha512(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t *out64);

const ServerKey *find_server_key(uint64_t fingerprint);
bool rsa_encrypt(const ServerKey &key, const uint8_t *in256, uint8_t *out256, bool &below_modulus);

uint64_t factor_pq(uint64_t pq);

bool dh_check(uint32_t g, const uint8_t *prime, size_t prime_len);
bool dh_check_value(const uint8_t *value, size_t value_len, const uint8_t *prime, size_t prime_len);
bool mod_exp(const uint8_t *base, size_t base_len, const uint8_t *exponent, size_t exponent_len,
             const uint8_t *modulus, size_t modulus_len, uint8_t *out256);

struct SrpInput {
  uint32_t g;
  const uint8_t *p;
  size_t p_len;
  const uint8_t *salt1;
  size_t salt1_len;
  const uint8_t *salt2;
  size_t salt2_len;
  const uint8_t *srp_b;
  size_t srp_b_len;
  const char *password;
};

bool srp_check(const SrpInput &input, uint8_t *a_out256, uint8_t *m1_out32);

}
