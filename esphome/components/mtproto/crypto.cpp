#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "crypto.h"

#include "esphome/components/sha256/sha256.h"
#include "esphome/core/helpers.h"

#include <esp_idf_version.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define MTPROTO_PSA 1
#include <psa/crypto.h>
#else
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha1.h>
#endif
#include <mbedtls/bignum.h>

namespace esphome::mtproto {

namespace {

const ServerKey SERVER_KEYS[] = {
    {0xc3b42b026ce86b21ULL,
     "c150023e2f70db7985ded064759cfecf0af328e69a41daf4d6f01b538135a6f91f8f8b2a0ec9ba9720ce352efcf6c5680ffc424bd634864902de0b4bd6d49f4e580230e3ae97d95c8b19442b3c0a10d8f5633fecedd6926a7f6dab0ddb7d457f9ea81b8465fcd6fffeed114011df91c059caedaf97625f6c96ecc74725556934ef781d866b34f011fce4d835a090196e9a5f0e4449af7eb697ddb9076494ca5f81104a305b6dd27665722c46b60e5df680fb16b210607ef217652e60236c255f6a28315f4083a96791d7214bf64c1df4fd0db1944fb26a2a57031b32eee64ad15a8ba68885cde74a5bfc920f6abf59ba5c75506373e7130f9042da922179251f"},
    {0x0bc35f3509f7b7a5ULL,
     "aeec36c8ffc109cb099624685b97815415657bd76d8c9c3e398103d7ad16c9bba6f525ed0412d7ae2c2de2b44e77d72cbf4b7438709a4e646a05c43427c7f184debf72947519680e651500890c6832796dd11f772c25ff8f576755afe055b0a3752c696eb7d8da0d8be1faf38c9bdd97ce0a77d3916230c4032167100edd0f9e7a3a9b602d04367b689536af0d64b613ccba7962939d3b57682beb6dae5b608130b2e52aca78ba023cf6ce806b1dc49c72cf928a7199d22e3d7ac84e47bc9427d0236945d10dbd15177bab413fbf0edfda09f014c7a7da088dde9759702ca760af2b8e4e97cc055c617bd74c3d97008635b98dc4d621b4891da9fb0473047927"},
    {0x15ae5fa8b5529542ULL,
     "bdf2c77d81f6afd47bd30f29ac76e55adfe70e487e5e48297e5a9055c9c07d2b93b4ed3994d3eca5098bf18d978d54f8b7c713eb10247607e69af9ef44f38e28f8b439f257a11572945cc0406fe3f37bb92b79112db69eedf2dc71584a661638ea5becb9e23585074b80d57d9f5710dd30d2da940e0ada2f1b878397dc1a72b5ce2531b6f7dd158e09c828d03450ca0ff8a174deacebcaa22dde84ef66ad370f259d18af806638012da0ca4a70baa83d9c158f3552bc9158e69bf332a45809e1c36905a5caa12348dd57941a482131be7b2355a5f4635374f3bd3ddf5ff925bf4809ee27c1e67d9120c5fe08a9de458b1b4a3c5d0a428437f2beca81f4e2d5ff"},
    {0xaeae98e13cd7f94fULL,
     "b3f762b739be98f343eb1921cf0148cfa27ff7af02b6471213fed9daa0098976e667750324f1abcea4c31e43b7d11f1579133f2b3d9fe27474e462058884e5e1b123be9cbbc6a443b2925c08520e7325e6f1a6d50e117eb61ea49d2534c8bb4d2ae4153fabe832b9edf4c5755fdd8b19940b81d1d96cf433d19e6a22968a85dc80f0312f596bd2530c1cfb28b5fe019ac9bc25cd9c2a5d8a0f3a1c0c79bcca524d315b5e21b5c26b46babe3d75d06d1cd33329ec782a0f22891ed1db42a1d6c0dea431428bc4d7aabdcf3e0eb6fda4e23eb7733e7727e9a1915580796c55188d2596d2665ad1182ba7abf15aaa5a8b779ea996317a20ae044b820bff35b6e8a1"},
    {0x5a181b2235057d98ULL,
     "be6a71558ee577ff03023cfa17aab4e6c86383cff8a7ad38edb9fafe6f323f2d5106cbc8cafb83b869cffd1ccf121cd743d509e589e68765c96601e813dc5b9dfc4be415c7a6526132d0035ca33d6d6075d4f535122a1cdfe017041f1088d1419f65c8e5490ee613e16dbf662698c0f54870f0475fa893fc41eb55b08ff1ac211bc045ded31be27d12c96d8d3cfc6a7ae8aa50bf2ee0f30ed507cc2581e3dec56de94f5dc0a7abee0be990b893f2887bd2c6310a1e0a9e3e38bd34fded2541508dc102a9c9b4c95effd9dd2dfe96c29be647d6c69d66ca500843cfaed6e440196f1dbe0e2e22163c61ca48c79116fa77216726749a976a1c4b0944b5121e8c01"},
};

const char DH_PRIME_HEX[] = "c71caeb9c6b1c9048e6c522f70f13f73980d40238e3e21c14934d037563d930f"
                            "48198a0aa7c14058229493d22530f4dbfa336f6e0ac925139543aed44cce7c37"
                            "20fd51f69458705ac68cd4fe6b6b13abdc9746512969328454f18faf8c595f64"
                            "2477fe96bb2a941d5bcd1d4ac8cc49880708fa9b378e3c4f3a9060bee67cf9a4"
                            "a4a695811051907e162753b56b0f6b410dba74d8a84b2a14b3144e0ef1284754"
                            "fd17ed950d5965b4b9dd46582db1178d169c6bc465b0d6ff9ca3928fef5b9ae4"
                            "e418fc15e83ebea0f87fa9ff5eed70050ded2849f47bf959d956850ce929851f"
                            "0d8115f635b105ee2e4e15d04b2454bf6f4fadf034b10403119cd8e3b92fcc5b";

class Mpi {
 public:
  Mpi() { mbedtls_mpi_init(&this->value); }
  ~Mpi() { mbedtls_mpi_free(&this->value); }
  Mpi(const Mpi &) = delete;
  Mpi &operator=(const Mpi &) = delete;
  bool read(const uint8_t *data, size_t length) { return mbedtls_mpi_read_binary(&this->value, data, length) == 0; }
  bool write(uint8_t *out, size_t length) const { return mbedtls_mpi_write_binary(&this->value, out, length) == 0; }
  mbedtls_mpi value;
};

bool known_prime(const uint8_t *prime, size_t prime_len) {
  if (prime_len != DH_BYTES)
    return false;
  uint8_t known[DH_BYTES];
  if (!parse_hex(DH_PRIME_HEX, sizeof(DH_PRIME_HEX) - 1, known, sizeof(known)))
    return false;
  return memcmp(known, prime, DH_BYTES) == 0;
}

uint32_t mod_small(const mbedtls_mpi &value, uint32_t divisor) {
  mbedtls_mpi_uint remainder = 0;
  mbedtls_mpi_mod_int(&remainder, &value, static_cast<mbedtls_mpi_sint>(divisor));
  return static_cast<uint32_t>(remainder);
}

uint64_t add_mod(uint64_t a, uint64_t b, uint64_t m) { return a >= m - b ? a - (m - b) : a + b; }

uint64_t mul_mod(uint64_t a, uint64_t b, uint64_t m) {
  uint64_t result = 0;
  a %= m;
  while (b != 0) {
    if (b & 1)
      result = add_mod(result, a, m);
    a = add_mod(a, a, m);
    b >>= 1;
  }
  return result;
}

uint64_t gcd(uint64_t a, uint64_t b) {
  while (b != 0) {
    const uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

int random_mpi(void *, unsigned char *out, size_t length) {
  esp_fill_random(out, length);
  return 0;
}

}

void random_bytes(uint8_t *out, size_t length) { esp_fill_random(out, length); }

void sha1(const uint8_t *data, size_t length, uint8_t *out20) { sha1_parts(data, length, nullptr, 0, out20); }

void sha1_parts(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len, uint8_t *out20) {
#ifdef MTPROTO_PSA
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  size_t length = 0;
  alignas(32) uint8_t digest[32];
  psa_hash_setup(&op, PSA_ALG_SHA_1);
  psa_hash_update(&op, a, a_len);
  if (b_len != 0)
    psa_hash_update(&op, b, b_len);
  psa_hash_finish(&op, digest, sizeof(digest), &length);
  memcpy(out20, digest, 20);
#else
  mbedtls_sha1_context ctx;
  alignas(32) uint8_t digest[32];
  mbedtls_sha1_init(&ctx);
  mbedtls_sha1_starts(&ctx);
  mbedtls_sha1_update(&ctx, a, a_len);
  if (b_len != 0)
    mbedtls_sha1_update(&ctx, b, b_len);
  mbedtls_sha1_finish(&ctx, digest);
  mbedtls_sha1_free(&ctx);
  memcpy(out20, digest, 20);
#endif
}

void sha256_parts(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len, uint8_t *out32) {
  sha256::SHA256 hash;
  hash.init();
  hash.add(a, a_len);
  if (b_len != 0)
    hash.add(b, b_len);
  hash.calculate();
  hash.get_bytes(out32);
}

bool aes_ige(const uint8_t *key, const uint8_t *iv, uint8_t *data, size_t length, bool encrypt) {
  if (length % 16 != 0)
    return false;
  uint8_t prev_in[16];
  uint8_t prev_out[16];
  if (encrypt) {
    memcpy(prev_out, iv, 16);
    memcpy(prev_in, iv + 16, 16);
  } else {
    memcpy(prev_in, iv, 16);
    memcpy(prev_out, iv + 16, 16);
  }
#ifdef MTPROTO_PSA
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attributes, 256);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);
  psa_key_id_t key_id = 0;
  if (psa_import_key(&attributes, key, 32, &key_id) != PSA_SUCCESS)
    return false;
#else
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if ((encrypt ? mbedtls_aes_setkey_enc(&ctx, key, 256) : mbedtls_aes_setkey_dec(&ctx, key, 256)) != 0) {
    mbedtls_aes_free(&ctx);
    return false;
  }
#endif
  bool ok = true;
  uint8_t block[16];
  uint8_t result[16];
  for (size_t offset = 0; offset < length && ok; offset += 16) {
    uint8_t *chunk = data + offset;
    for (int i = 0; i < 16; i++)
      block[i] = chunk[i] ^ prev_out[i];
#ifdef MTPROTO_PSA
    size_t produced = 0;
    ok = (encrypt ? psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING, block, 16, result, 16, &produced)
                  : psa_cipher_decrypt(key_id, PSA_ALG_ECB_NO_PADDING, block, 16, result, 16, &produced)) ==
             PSA_SUCCESS &&
         produced == 16;
#else
    ok = mbedtls_aes_crypt_ecb(&ctx, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, block, result) == 0;
#endif
    for (int i = 0; i < 16; i++)
      result[i] ^= prev_in[i];
    memcpy(prev_in, chunk, 16);
    memcpy(prev_out, result, 16);
    memcpy(chunk, result, 16);
  }
#ifdef MTPROTO_PSA
  psa_destroy_key(key_id);
#else
  mbedtls_aes_free(&ctx);
#endif
  return ok;
}

bool pbkdf2_sha512(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t *out64) {
#ifdef MTPROTO_PSA
  psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
  bool ok = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_512)) == PSA_SUCCESS &&
            psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations) == PSA_SUCCESS &&
            psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len) == PSA_SUCCESS &&
            psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, password, password_len) ==
                PSA_SUCCESS &&
            psa_key_derivation_output_bytes(&op, out64, 64) == PSA_SUCCESS;
  psa_key_derivation_abort(&op);
  return ok;
#else
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA512, password, password_len, salt, salt_len, iterations, 64,
                                       out64) == 0;
#endif
}

const ServerKey *find_server_key(uint64_t fingerprint) {
  for (const ServerKey &key : SERVER_KEYS) {
    if (key.fingerprint == fingerprint)
      return &key;
  }
  return nullptr;
}

bool rsa_encrypt(const ServerKey &key, const uint8_t *in256, uint8_t *out256, bool &below_modulus) {
  uint8_t modulus_bytes[RSA_BYTES];
  if (!parse_hex(key.modulus_hex, RSA_BYTES * 2, modulus_bytes, sizeof(modulus_bytes)))
    return false;
  Mpi modulus;
  Mpi exponent;
  Mpi value;
  Mpi result;
  if (!modulus.read(modulus_bytes, RSA_BYTES) || !value.read(in256, RSA_BYTES) ||
      mbedtls_mpi_lset(&exponent.value, 65537) != 0)
    return false;
  below_modulus = mbedtls_mpi_cmp_mpi(&value.value, &modulus.value) < 0;
  if (!below_modulus)
    return true;
  return mbedtls_mpi_exp_mod(&result.value, &value.value, &exponent.value, &modulus.value, nullptr) == 0 &&
         result.write(out256, RSA_BYTES);
}

uint64_t factor_pq(uint64_t pq) {
  if (pq % 2 == 0)
    return 2;
  uint8_t seed[8];
  for (int attempt = 0; attempt < 8; attempt++) {
    random_bytes(seed, sizeof(seed));
    uint64_t y = 0;
    memcpy(&y, seed, sizeof(y));
    y %= pq;
    const uint64_t c = (y % (pq - 1)) + 1;
    const uint64_t m = 128;
    uint64_t g = 1;
    uint64_t r = 1;
    uint64_t q = 1;
    uint64_t x = 0;
    uint64_t ys = 0;
    while (g == 1) {
      x = y;
      for (uint64_t i = 0; i < r; i++)
        y = add_mod(mul_mod(y, y, pq), c, pq);
      uint64_t k = 0;
      while (k < r && g == 1) {
        ys = y;
        const uint64_t steps = std::min(m, r - k);
        for (uint64_t i = 0; i < steps; i++) {
          y = add_mod(mul_mod(y, y, pq), c, pq);
          q = mul_mod(q, x > y ? x - y : y - x, pq);
        }
        g = gcd(q, pq);
        k += m;
      }
      r *= 2;
      if (r > (1ULL << 24))
        break;
    }
    if (g == pq) {
      do {
        ys = add_mod(mul_mod(ys, ys, pq), c, pq);
        g = gcd(x > ys ? x - ys : ys - x, pq);
      } while (g == 1);
    }
    if (g != 1 && g != pq)
      return std::min(g, pq / g);
  }
  return 0;
}

bool dh_check(uint32_t g, const uint8_t *prime, size_t prime_len) {
  if (prime_len != DH_BYTES || (prime[0] & 0x80) == 0 || g < 2 || g > 7)
    return false;
  Mpi p;
  if (!p.read(prime, prime_len))
    return false;
  if (!known_prime(prime, prime_len)) {
    Mpi half;
    if (mbedtls_mpi_sub_int(&half.value, &p.value, 1) != 0 || mbedtls_mpi_shift_r(&half.value, 1) != 0)
      return false;
    if (mbedtls_mpi_is_prime_ext(&p.value, 30, random_mpi, nullptr) != 0 ||
        mbedtls_mpi_is_prime_ext(&half.value, 30, random_mpi, nullptr) != 0)
      return false;
  }
  switch (g) {
    case 2:
      return mod_small(p.value, 8) == 7;
    case 3:
      return mod_small(p.value, 3) == 2;
    case 4:
      return true;
    case 5: {
      const uint32_t r = mod_small(p.value, 5);
      return r == 1 || r == 4;
    }
    case 6: {
      const uint32_t r = mod_small(p.value, 24);
      return r == 19 || r == 23;
    }
    case 7: {
      const uint32_t r = mod_small(p.value, 7);
      return r == 3 || r == 5 || r == 6;
    }
    default:
      return false;
  }
}

bool dh_check_value(const uint8_t *value, size_t value_len, const uint8_t *prime, size_t prime_len) {
  Mpi v;
  Mpi p;
  Mpi low;
  Mpi high;
  if (!v.read(value, value_len) || !p.read(prime, prime_len) || mbedtls_mpi_lset(&low.value, 1) != 0 ||
      mbedtls_mpi_shift_l(&low.value, 2048 - 64) != 0 || mbedtls_mpi_sub_mpi(&high.value, &p.value, &low.value) != 0)
    return false;
  return mbedtls_mpi_cmp_mpi(&v.value, &low.value) > 0 && mbedtls_mpi_cmp_mpi(&v.value, &high.value) < 0;
}

bool mod_exp(const uint8_t *base, size_t base_len, const uint8_t *exponent, size_t exponent_len,
             const uint8_t *modulus, size_t modulus_len, uint8_t *out256) {
  Mpi b;
  Mpi e;
  Mpi m;
  Mpi r;
  return b.read(base, base_len) && e.read(exponent, exponent_len) && m.read(modulus, modulus_len) &&
         mbedtls_mpi_exp_mod(&r.value, &b.value, &e.value, &m.value, nullptr) == 0 && r.write(out256, DH_BYTES);
}

bool srp_check(const SrpInput &in, uint8_t *a_out256, uint8_t *m1_out32) {
  if (!dh_check(in.g, in.p, in.p_len) || in.srp_b_len == 0 || in.srp_b_len > DH_BYTES)
    return false;
  uint8_t g_padded[DH_BYTES] = {};
  g_padded[DH_BYTES - 1] = static_cast<uint8_t>(in.g);
  uint8_t b_padded[DH_BYTES] = {};
  memcpy(b_padded + DH_BYTES - in.srp_b_len, in.srp_b, in.srp_b_len);
  if (!dh_check_value(b_padded, DH_BYTES, in.p, in.p_len))
    return false;

  uint8_t hash[32];
  {
    sha256::SHA256 h;
    h.init();
    h.add(in.salt1, in.salt1_len);
    h.add(reinterpret_cast<const uint8_t *>(in.password), strlen(in.password));
    h.add(in.salt1, in.salt1_len);
    h.calculate();
    h.get_bytes(hash);
  }
  {
    sha256::SHA256 h;
    h.init();
    h.add(in.salt2, in.salt2_len);
    h.add(hash, sizeof(hash));
    h.add(in.salt2, in.salt2_len);
    h.calculate();
    h.get_bytes(hash);
  }
  uint8_t stretched[64];
  if (!pbkdf2_sha512(hash, sizeof(hash), in.salt1, in.salt1_len, 100000, stretched))
    return false;
  uint8_t x_bytes[32];
  {
    sha256::SHA256 h;
    h.init();
    h.add(in.salt2, in.salt2_len);
    h.add(stretched, sizeof(stretched));
    h.add(in.salt2, in.salt2_len);
    h.calculate();
    h.get_bytes(x_bytes);
  }
  uint8_t k_bytes[32];
  {
    sha256::SHA256 h;
    h.init();
    h.add(in.p, in.p_len);
    h.add(g_padded, sizeof(g_padded));
    h.calculate();
    h.get_bytes(k_bytes);
  }

  Mpi p;
  Mpi g;
  Mpi x;
  Mpi k;
  Mpi v;
  Mpi kv;
  Mpi a;
  Mpi ga;
  Mpi gb;
  Mpi u;
  Mpi t;
  Mpi exponent;
  Mpi sa;
  if (!p.read(in.p, in.p_len) || mbedtls_mpi_lset(&g.value, in.g) != 0 || !x.read(x_bytes, sizeof(x_bytes)) ||
      !k.read(k_bytes, sizeof(k_bytes)) || !gb.read(b_padded, DH_BYTES))
    return false;
  if (mbedtls_mpi_exp_mod(&v.value, &g.value, &x.value, &p.value, nullptr) != 0 ||
      mbedtls_mpi_mul_mpi(&kv.value, &k.value, &v.value) != 0 || mbedtls_mpi_mod_mpi(&kv.value, &kv.value, &p.value) != 0)
    return false;

  uint8_t a_bytes[DH_BYTES];
  uint8_t ga_bytes[DH_BYTES];
  uint8_t u_bytes[32];
  for (int attempt = 0;; attempt++) {
    if (attempt > 4)
      return false;
    random_bytes(a_bytes, sizeof(a_bytes));
    if (!a.read(a_bytes, sizeof(a_bytes)) ||
        mbedtls_mpi_exp_mod(&ga.value, &g.value, &a.value, &p.value, nullptr) != 0 || !ga.write(ga_bytes, DH_BYTES))
      return false;
    if (!dh_check_value(ga_bytes, DH_BYTES, in.p, in.p_len))
      continue;
    sha256_parts(ga_bytes, DH_BYTES, b_padded, DH_BYTES, u_bytes);
    if (!u.read(u_bytes, sizeof(u_bytes)))
      return false;
    if (mbedtls_mpi_cmp_int(&u.value, 0) > 0)
      break;
  }
  if (mbedtls_mpi_sub_mpi(&t.value, &gb.value, &kv.value) != 0 || mbedtls_mpi_mod_mpi(&t.value, &t.value, &p.value) != 0)
    return false;
  if (mbedtls_mpi_mul_mpi(&exponent.value, &u.value, &x.value) != 0 ||
      mbedtls_mpi_add_mpi(&exponent.value, &exponent.value, &a.value) != 0 ||
      mbedtls_mpi_exp_mod(&sa.value, &t.value, &exponent.value, &p.value, nullptr) != 0)
    return false;
  uint8_t sa_bytes[DH_BYTES];
  if (!sa.write(sa_bytes, DH_BYTES))
    return false;
  uint8_t ka[32];
  sha256_parts(sa_bytes, DH_BYTES, nullptr, 0, ka);

  uint8_t hp[32];
  uint8_t hg[32];
  uint8_t hs1[32];
  uint8_t hs2[32];
  sha256_parts(in.p, in.p_len, nullptr, 0, hp);
  sha256_parts(g_padded, DH_BYTES, nullptr, 0, hg);
  sha256_parts(in.salt1, in.salt1_len, nullptr, 0, hs1);
  sha256_parts(in.salt2, in.salt2_len, nullptr, 0, hs2);
  for (int i = 0; i < 32; i++)
    hp[i] ^= hg[i];
  sha256::SHA256 h;
  h.init();
  h.add(hp, sizeof(hp));
  h.add(hs1, sizeof(hs1));
  h.add(hs2, sizeof(hs2));
  h.add(ga_bytes, DH_BYTES);
  h.add(b_padded, DH_BYTES);
  h.add(ka, sizeof(ka));
  h.calculate();
  h.get_bytes(m1_out32);
  memcpy(a_out256, ga_bytes, DH_BYTES);
  return true;
}

}
