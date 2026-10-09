/* ftest_mega.c -- the MEGA adapter (area "mega").
**
** Offline:
**   - SHA-512 (FIPS 180-4 examples: "", "abc", the 896-bit message, a
**     million 'a'), HMAC-SHA512 (RFC 4231 cases 1, 2, 6), PBKDF2-HMAC-SHA512
**     (RFC 6070's inputs, expected values from Python's hashlib), MEGA's v2
**     key derivation (100 000 rounds), CTR-AES128 with a big-endian counter
**     (SP 800-38A F.5.1, the counter wraps through 0xFF..).
**   - Sign-in reply of a fabricated account: an RSA-2048 key made with
**     Python (p, q, d, u and csid = m^e mod n), master key and private key
**     encrypted here, so mega_login_reply must find the session id. Also a
**     "tsid" session and a wrong password.
**   - Attributes round trip; a canned "f" reply (own folders and files,
**     an outgoing share with "sk", an "ok" share key, an RSA-keyed node and
**     a node with a wrong key, both of which must be left out).
**   - Upload-style CTR + chunk MAC over a generated 5 MB file, then the
**     download-style decryption in odd-sized pieces; the MAC is compared with
**     a naive per-chunk CBC-MAC and must fail after a one-byte change.
**   - Link forms, the fingerprint, base64url.
**
** Live, opt-in:
**   MMCFM_MEGA_LINK="link[|link...]"   opens each public link, lists it (a
**       folder: recursively, first 200 entries) and downloads the first file
**       under 20 MB, which must pass the MAC check.
**   MMCFM_MEGA_LOGIN="email|password"  signs in, lists the drive, the quota;
**       with MMCFM_MEGA_WRITE=1 it also makes "mmcfm-test-<ms>", uploads a
**       300 KB file, downloads and compares it, renames, moves and removes.
*/
#include "ftest.h"
#include "fcloud_mega_int.h"
#include "fbignum.h"
#include "fplat.h"

extern const FmCloud g_cloud_mega;

/* vectors (Python 3.9 hashlib / pow, see the header) */
static const char *kRsaP =
  "f98b7dcd75307d09174df559829c619dff26ada9b5cd08edf69cace362e04da07ade3d1fe2272e9825b5307abcf67625"
  "ad9f05b2270ab7e4019ca27e0498fb04d1db94553d9f469a71861dd0427189055d911244c1c728525f13e03de2d45e9e"
  "09c4af145ccbf0793f13ed53c5dd7708671090d3bff4821fea8de2992170a203";
static const char *kRsaQ =
  "d8ffd50450dfc9208fa384351d1ab090b6c520e6aef80db93c6b65f4bca8ff4c0fdeecbad186464f6e35622d4bb3ce39"
  "2db4e1ffb805171af2f286b995482861c42dd06ee5bb76ab8c95869103ae15458fbb6dfbd3874635e19492869a3ea676"
  "f4c9f2384ee2f670c3c24d082dd0fe0277ca64f71add16b7fb4e680296c07aaf";
static const char *kRsaD =
  "88deeea76d9ce4d795461f582e798a83ba56fb430e38797ae2a607fb93d17eaee6fe742adb230f2a1d6e561ed5d9a208"
  "67a434dddcce691e100a36fc270c286a67eb6343aae76c612803100d7a19fdd84839be29bc6d906d56b95bfc8d561c59"
  "097c6d650589289741872c80ba1c4d8c7570f93691f1bb8fbc1aa7ce8a52957ac616dccaa607dc8bd293ac16cc147486"
  "9494a1c84b86c58a8244c17e98e4e4d5ecf700f34ade2fccbd36fd913bcae81d17e97da3038a0d54beed24b516519730"
  "8fdd2ada55c99e877e2baeb71e43ef99935917077bf986e186c8b69fd829f4fe7c25407fbf579ab86ac5ca77215be5e8"
  "20829f027f664e053658cced863a74a5";
static const char *kRsaU =
  "c22d96ad395a1b5d88f73442986c461a75e8884f9228a1d57dc8a1345d0481174ad51a7f365c31db217dda9907a39e32"
  "092b14039640463b62b04f51255e03742f62cb49912916ba16800d0e3d40c0d623ef2512f5416e0dc37626e70440ea5c"
  "145073040dad578a9c5c67a72bd07f4e029a621f40a7019361b9291bd5202743";
static const char *kRsaC =
  "709145ccfbc3d723bfef37002298757ad72ec5084e6bb22d4f2d69bb25fe20915f4200e63186ba212cce1b338660f494"
  "dc18283fef7adb408fcd5df9c5f0fed9662a02b60e96b505584d9cbff74fca72fee257146e3bd198e0a62cd22b4c943d"
  "dd734789f104d6dd51487a2c4364c3b4f4bd430412dbc39a2a6b819fcfbab97514a93b323af82896d4a15f7c9ac2f82c"
  "8ab0e7127dbaa14faae8db01b47739b7251ce393a93c7e1d84f20d862af7e88b6ac900336f2f33cdf5b804c434bf4fa2"
  "72a721aad69c02a121f8a27d2a6f8285bfd801895cac638bcd2340ffec1b160d7003c5baa113ee9884ce557dbfd8d957"
  "a2c4a2b66a9477ca8f604905cf4e8322";
static const char *kRsaSid = "WlhfJpPjtCYbZKhDbvlS6Yr62vBGefXPovqnazOLMxLzaFoPEexuepsmJA";
static const char *kSha512Empty =
  "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f"
  "63b931bd47417a81a538327af927da3e";
static const char *kSha512Abc =
  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
  "454d4423643ce80e2a9ac94fa54ca49f";
static const char *kSha512_896 =
  "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433a"
  "c7d329eeb6dd26545e96e55b874be909";
static const char *kSha512Million =
  "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31b"
  "eb009c5c2c49aa2e4eadb217ad8cc09b";
static const char *kHmac1 =
  "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4"
  "be9d914eeb61f1702e696c203a126854";
static const char *kHmac2 =
  "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fd"
  "caeab1a34d4a6b4b636e070a38bce737";
static const char *kHmac6 =
  "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215d6a1e52"
  "95e64f73f63f0aec8b915a985d786598";
static const char *kPbkdf1 =
  "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252c02d470a285a0501bad999bfe943c08f"
  "050235d7d68b1da55e63f73b60a57fce";
static const char *kPbkdf2 =
  "e1d9c16aa681708a45f5c7c4e215ceb66e011a2e9f0040713f18aefdb866d53cf76cab2868a39b9f7840edce4fef5a82"
  "be67335c77a6068e04112754f27ccf4e";
static const char *kPbkdf4096 =
  "d197b1b33db0143e018b12f3d1d1479e6cdebdcc97c5c0f87f6902e072f457b5143f30602641b3d55cd335988cb36b84"
  "376060ecd532e039b742a239434af2d5";
static const char *kPbkdfLong =
  "8c0511f4c6e597c6ac6315d8f0362e225f3c501495ba23b868c005174dc4ee71115b59f9e60cd9532fa33e0f75aefe30"
  "225c583a186cd82bd4daea9724a3d3b804f75bdd41494fa324cab24bcc680fb3";
static const char *kV2Pk = "b109761b8a61d2a5d73ca33d3ed99e9f";
static const char *kV2Uh = "pRcOIPsNi7OuRaJnuBneJA";

static int unhex(const char *h, u8 *out) {
  int n = 0;
  for (; h[0] && h[1]; h += 2) {
    unsigned v;
    sscanf(h, "%2x", &v);
    out[n++] = (u8)v;
  }
  return n;
}

static bool eq_hex(const u8 *got, const char *hex) {
  u8 want[128];
  int n = unhex(hex, want);
  return memcmp(got, want, (size_t)n) == 0;
}

static u64 g_seed = 0x9E3779B97F4A7C15ull;
static u8 rnd8(void) {
  g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
  return (u8)(g_seed >> 24);
}

/* ---- primitives ------------------------------------------------------------------------------ */

static void test_hashes(void) {
  u8 out[80];
  FmSha512 s;
  sha512_init(&s); sha512_final(&s, out);
  TEST_CHECK(eq_hex(out, kSha512Empty));
  sha512_init(&s); sha512_update(&s, "abc", 3); sha512_final(&s, out);
  TEST_CHECK(eq_hex(out, kSha512Abc));
  const char *m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs"
                     "mnopqrstnopqrstu";
  sha512_init(&s); sha512_update(&s, m896, strlen(m896)); sha512_final(&s, out);
  TEST_CHECK(eq_hex(out, kSha512_896));
  /* a million 'a' in uneven pieces (crosses the buffer at every offset) */
  {
    char a[1000];
    memset(a, 'a', sizeof a);
    size_t left = 1000000, step = 1;
    sha512_init(&s);
    while (left) {
      size_t n = FM_MIN(left, step);
      sha512_update(&s, a, n);
      left -= n;
      step = step % 997 + 1;
    }
    sha512_final(&s, out);
    TEST_CHECK(eq_hex(out, kSha512Million));
  }
  u8 k[131];
  memset(k, 0x0b, 20);
  hmac_sha512(k, 20, "Hi There", 8, out);
  TEST_CHECK(eq_hex(out, kHmac1));
  hmac_sha512((const u8 *)"Jefe", 4, "what do ya want for nothing?", 28, out);
  TEST_CHECK(eq_hex(out, kHmac2));
  memset(k, 0xaa, 131);
  const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  hmac_sha512(k, 131, m6, strlen(m6), out);
  TEST_CHECK(eq_hex(out, kHmac6));
  pbkdf2_sha512((const u8 *)"password", 8, (const u8 *)"salt", 4, 1, out, 64);
  TEST_CHECK(eq_hex(out, kPbkdf1));
  pbkdf2_sha512((const u8 *)"password", 8, (const u8 *)"salt", 4, 2, out, 64);
  TEST_CHECK(eq_hex(out, kPbkdf2));
  pbkdf2_sha512((const u8 *)"password", 8, (const u8 *)"salt", 4, 4096, out, 64);
  TEST_CHECK(eq_hex(out, kPbkdf4096));
  pbkdf2_sha512((const u8 *)"passwordPASSWORDpassword", 24, (const u8 *)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096,
                out, 80);
  TEST_CHECK(eq_hex(out, kPbkdfLong));
  /* MEGA v2: 100 000 rounds */
  u8 salt[32], pk[16];
  char uh[32];
  for (int i = 0; i < 32; i++) salt[i] = (u8)i;
  u64 t0 = plat_now_ms();
  mega_v2_derive("correct horse", salt, 32, pk, uh);
  printf("  v2 key derivation: %llu ms\n", (unsigned long long)(plat_now_ms() - t0));
  TEST_CHECK(eq_hex(pk, kV2Pk));
  TEST_CHECK(!strcmp(uh, kV2Uh));
}

static void test_ctr(void) {
  /* SP 800-38A F.5.1 CTR-AES128.Encrypt */
  u8 key[16], iv[16], buf[64];
  unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
  unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv);
  const char *pt = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                   "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
  const char *ct = "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
                   "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee";
  FmAes a;
  aes_init(&a, key, 16);
  unhex(pt, buf);
  aes_ctr_be(&a, iv, 0, buf, 64);
  TEST_CHECK(eq_hex(buf, ct));
  /* the same in odd slices at their positions */
  unhex(pt, buf);
  size_t cuts[] = { 0, 5, 16, 17, 40, 64 };
  for (int i = 0; i + 1 < FM_COUNT(cuts); i++) aes_ctr_be(&a, iv, cuts[i], buf + cuts[i], cuts[i + 1] - cuts[i]);
  TEST_CHECK(eq_hex(buf, ct));
  /* a counter carry across all 64 low bits: iv ..ff ff ff ff ff ff ff ff, block 1 -> next byte up */
  u8 iv2[16], blk[16], want[16];
  memset(iv2, 0xff, 16);
  iv2[0] = 0x01;
  memset(blk, 0, 16);
  aes_ctr_be(&a, iv2, 16, blk, 16);
  u8 c2[16];
  memset(c2, 0, 16);
  c2[0] = 0x02;
  aes_encrypt_block(&a, c2, want);
  TEST_CHECK(!memcmp(blk, want, 16));
}

static void test_bignum(void) {
  /* small: 4^13 mod 497 = 445 */
  FmBn b, e, m, r;
  u8 x[2];
  x[0] = 4; bn_from_bytes(&b, x, 1);
  x[0] = 13; bn_from_bytes(&e, x, 1);
  x[0] = 0x01; x[1] = 0xF1; bn_from_bytes(&m, x, 2);
  TEST_CHECK(bn_modexp(&r, &b, &e, &m) && r.n == 1 && r.d[0] == 445);
  u8 out[8];
  TEST_CHECK(bn_to_bytes(&r, out, sizeof out) == 2 && out[0] == 0x01 && out[1] == 0xBD);
}

/* ---- sign-in ----------------------------------------------------------------------------------- */

static size_t put_mpi(u8 *p, const char *hex) {
  u8 b[600];
  int n = unhex(hex, b);
  int bits = n * 8;
  for (u8 t = b[0]; !(t & 0x80); t <<= 1) bits--;
  p[0] = (u8)(bits >> 8);
  p[1] = (u8)bits;
  memcpy(p + 2, b, (size_t)n);
  return (size_t)n + 2;
}

static void test_login(void) {
  u8 pk[16], mk[16], kenc[16];
  for (int i = 0; i < 16; i++) { pk[i] = (u8)(0x10 + i); mk[i] = (u8)(0xA0 ^ (i * 7)); }
  FmAes ap, am;
  aes_init(&ap, pk, 16);
  aes_init(&am, mk, 16);
  aes_encrypt_block(&ap, mk, kenc);
  /* private key blob: p q d u, padded to 16, under the master key */
  u8 priv[1200];
  size_t n = 0;
  n += put_mpi(priv + n, kRsaP);
  n += put_mpi(priv + n, kRsaQ);
  n += put_mpi(priv + n, kRsaD);
  n += put_mpi(priv + n, kRsaU);
  while (n % 16) priv[n++] = 0;
  for (size_t i = 0; i < n; i += 16) aes_encrypt_block(&am, priv + i, priv + i);
  u8 csid[600];
  size_t cn = put_mpi(csid, kRsaC);
  char k64[64], p64[1700], c64[900], js[3000];
  mega_b64_enc(kenc, 16, k64, sizeof k64);
  mega_b64_enc(priv, n, p64, sizeof p64);
  mega_b64_enc(csid, cn, c64, sizeof c64);
  fm_snprintf(js, sizeof js, "{\"k\":\"%s\",\"privk\":\"%s\",\"csid\":\"%s\",\"u\":\"AbCdEfGhIjK\"}", k64, p64, c64);
  FmJson j;
  TEST_CHECK(json_parse(&j, js, strlen(js)) == FM_OK);
  char sid[160], err[200];
  u8 got[16];
  u64 t0 = plat_now_ms();
  TEST_CHECK(mega_login_reply(json_root(&j), pk, sid, sizeof sid, got, err, sizeof err) == FM_OK);
  printf("  RSA-2048 session decrypt: %llu ms\n", (unsigned long long)(plat_now_ms() - t0));
  TEST_CHECK(!strcmp(sid, kRsaSid));
  TEST_CHECK(!memcmp(got, mk, 16));
  /* wrong password key: no session */
  u8 bad[16];
  memcpy(bad, pk, 16);
  bad[3] ^= 1;
  sid[0] = 0;
  FmErr be = mega_login_reply(json_root(&j), bad, sid, sizeof sid, got, err, sizeof err);
  TEST_CHECK(be != FM_OK || strcmp(sid, kRsaSid) != 0);
  json_free(&j);
  /* tsid: 16 random bytes + their encryption under the master key */
  u8 ts[32];
  for (int i = 0; i < 16; i++) ts[i] = (u8)(i * 3 + 1);
  aes_encrypt_block(&am, ts, ts + 16);
  char t64[64];
  mega_b64_enc(ts, 32, t64, sizeof t64);
  fm_snprintf(js, sizeof js, "{\"k\":\"%s\",\"tsid\":\"%s\"}", k64, t64);
  TEST_CHECK(json_parse(&j, js, strlen(js)) == FM_OK);
  TEST_CHECK(mega_login_reply(json_root(&j), pk, sid, sizeof sid, got, err, sizeof err) == FM_OK && !strcmp(sid, t64));
  json_free(&j);
  /* v1 derivation: deterministic, password-sensitive, and a 16-byte key per 16 password bytes */
  u8 k1[16], k2[16], k3[16];
  mega_prepare_key("password", k1);
  mega_prepare_key("password", k2);
  mega_prepare_key("passwore", k3);
  TEST_CHECK(!memcmp(k1, k2, 16) && memcmp(k1, k3, 16) != 0);
  char uh1[16], uh2[16];
  mega_stringhash("user@example.com", k1, uh1);
  mega_stringhash("user@example.com", k3, uh2);
  TEST_CHECK(strlen(uh1) == 11 && strcmp(uh1, uh2) != 0);
}

/* ---- attributes and the tree ----------------------------------------------------------------- */

static void test_attr(void) {
  u8 k[16];
  for (int i = 0; i < 16; i++) k[i] = (u8)(i * 11);
  char enc[512], name[256], fp[64];
  const char *nm = "Caf\xC3\xA9 \"quoted\" \\ back/slash \xF0\x9F\x98\x80.txt";
  TEST_CHECK(mega_attr_encrypt(k, nm, "AbCd", enc, sizeof enc));
  TEST_CHECK(mega_attr_decrypt(k, enc, name, sizeof name, fp, sizeof fp));
  TEST_CHECK(!strcmp(name, nm) && !strcmp(fp, "AbCd"));
  k[0] ^= 1;
  TEST_CHECK(!mega_attr_decrypt(k, enc, name, sizeof name, NULL, 0));
}

/* one node object for the canned "f" reply */
static void node_json(char *out, size_t cap, const char *h, const char *p, int t, const u8 *key, int klen,
                      const char *kh, const u8 *wrap, const char *name, const char *fp, u64 size, const char *extra) {
  char attr[512] = "", k64[80] = "";
  if (klen) {
    u8 ak[16], ek[32];
    for (int i = 0; i < 16; i++) ak[i] = klen == 32 ? (u8)(key[i] ^ key[16 + i]) : key[i];
    mega_attr_encrypt(ak, name, fp, attr, sizeof attr);
    memcpy(ek, key, (size_t)klen);
    FmAes w;
    aes_init(&w, wrap, 16);
    for (int i = 0; i < klen; i += 16) aes_encrypt_block(&w, ek + i, ek + i);
    mega_b64_enc(ek, (size_t)klen, k64, sizeof k64);
  }
  fm_snprintf(out, cap, "{\"h\":\"%s\",\"p\":\"%s\",\"u\":\"OwnerUser01\",\"t\":%d,\"a\":\"%s\",\"k\":\"%s:%s\",\"s\":%llu,"
              "\"ts\":1700000000%s}", h, p, t, attr, kh, k64, (unsigned long long)size, extra ? extra : "");
}

static void test_tree(void) {
  u8 mk[16], fk[16], filek[32], sk[16], sfk[32], other[16];
  for (int i = 0; i < 16; i++) { mk[i] = (u8)(i + 1); fk[i] = (u8)(0x40 + i); sk[i] = (u8)(0x70 ^ i); other[i] = (u8)(0x99 + i); }
  for (int i = 0; i < 32; i++) { filek[i] = (u8)(0x20 + i * 3); sfk[i] = (u8)(0xC0 ^ (i * 5)); }
  /* fingerprint with mtime 1600000000 (0x5F5E1000): count 4, little-endian */
  u8 fpb[21];
  memset(fpb, 0x11, 16);
  fpb[16] = 4; fpb[17] = 0x00; fpb[18] = 0x10; fpb[19] = 0x5E; fpb[20] = 0x5F;
  char fp[40];
  mega_b64_enc(fpb, 21, fp, sizeof fp);
  TEST_CHECK(mega_fp_mtime(fp) == 1600000000);
  u8 skenc[16];
  FmAes am;
  aes_init(&am, mk, 16);
  aes_encrypt_block(&am, sk, skenc);
  char sk64[32], extra[80];
  mega_b64_enc(skenc, 16, sk64, sizeof sk64);
  fm_snprintf(extra, sizeof extra, ",\"su\":\"OwnerUser01\",\"sk\":\"%s\"", sk64);
  char js[8192], nd[1200];
  size_t o = (size_t)fm_snprintf(js, sizeof js, "{\"f\":[{\"h\":\"ROOTHAND\",\"p\":\"\",\"u\":\"OwnerUser01\",\"t\":2,\"a\":\"\","
                                 "\"k\":\"\",\"ts\":1},{\"h\":\"RUBBISH1\",\"p\":\"\",\"u\":\"OwnerUser01\",\"t\":4,\"ts\":1}");
  node_json(nd, sizeof nd, "FOLDER01", "ROOTHAND", 1, fk, 16, "OwnerUser01", mk, "Photos", NULL, 0, NULL);
  o += (size_t)fm_snprintf(js + o, sizeof js - o, ",%s", nd);
  node_json(nd, sizeof nd, "FILE0001", "FOLDER01", 0, filek, 32, "OwnerUser01", mk, "a.jpg", fp, 12345, NULL);
  o += (size_t)fm_snprintf(js + o, sizeof js - o, ",%s", nd);
  /* an outgoing share: its own key under "sk"; a file in it keyed by the share */
  node_json(nd, sizeof nd, "SHARED01", "ROOTHAND", 1, fk, 16, "OwnerUser01", mk, "Shared", NULL, 0, extra);
  o += (size_t)fm_snprintf(js + o, sizeof js - o, ",%s", nd);
  node_json(nd, sizeof nd, "SFILE001", "SHARED01", 0, sfk, 32, "SHARED01", sk, "b.txt", NULL, 77, NULL);
  o += (size_t)fm_snprintf(js + o, sizeof js - o, ",%s", nd);
  /* a node keyed by an "ok" share key */
  node_json(nd, sizeof nd, "OKFILE01", "ROOTHAND", 0, sfk, 32, "OKSHARE1", other, "ok.bin", NULL, 5, NULL);
  o += (size_t)fm_snprintf(js + o, sizeof js - o, ",%s", nd);
  /* wrong key: encrypted with another key under our user handle -> left out */
  node_json(nd, sizeof nd, "BADKEY01", "ROOTHAND", 0, filek, 32, "OwnerUser01", other, "bad", NULL, 1, NULL);
  o += (size_t)fm_snprintf(js + o, sizeof js - o, ",%s", nd);
  /* an RSA-sized key (incoming share) -> left out */
  o += (size_t)fm_snprintf(js + o, sizeof js - o, ",{\"h\":\"RSAKEY01\",\"p\":\"ROOTHAND\",\"u\":\"x\",\"t\":1,\"a\":\"AAAA\","
                           "\"k\":\"OwnerUser01:%0256d\"}", 0);
  u8 okenc[16];
  aes_encrypt_block(&am, other, okenc);
  char ok64[32];
  mega_b64_enc(okenc, 16, ok64, sizeof ok64);
  fm_snprintf(js + o, sizeof js - o, "],\"ok\":[{\"h\":\"OKSHARE1\",\"ha\":\"x\",\"k\":\"%s\"}]}", ok64);
  FmJson j;
  TEST_CHECK(json_parse(&j, js, strlen(js)) == FM_OK);
  MgTree t;
  TEST_CHECK(mega_tree_build(&t, json_root(&j), mk, false) == FM_OK);
  json_free(&j);
  TEST_CHECK(!strcmp(t.root, "ROOTHAND") && !strcmp(t.rubbish, "RUBBISH1"));
  TEST_CHECK(t.count == 7);
  MgNode *f = mega_tree_find(&t, "FOLDER01");
  TEST_CHECK(f && f->type == MG_FOLDER && !strcmp(f->name, "Photos") && f->klen == 16 && !memcmp(f->key, fk, 16));
  MgNode *a = mega_tree_find(&t, "FILE0001");
  TEST_CHECK(a && !strcmp(a->name, "a.jpg") && a->size == 12345 && !strcmp(a->p, "FOLDER01") && a->mtime == 1600000000 &&
             !memcmp(a->key, filek, 32));
  MgNode *s = mega_tree_find(&t, "SFILE001");
  TEST_CHECK(s && !strcmp(s->name, "b.txt") && s->size == 77 && s->ts == 1700000000);
  MgNode *k = mega_tree_find(&t, "OKFILE01");
  TEST_CHECK(k && !strcmp(k->name, "ok.bin"));
  TEST_CHECK(!mega_tree_find(&t, "BADKEY01") && !mega_tree_find(&t, "RSAKEY01"));
  /* grow past the index size: every handle still found */
  for (int i = 0; i < 3000; i++) {
    char h[12];
    fm_snprintf(h, sizeof h, "N%07d", i);
    mega_tree_add(&t, h)->type = MG_FILE;
  }
  int found = 0;
  for (int i = 0; i < 3000; i++) {
    char h[12];
    fm_snprintf(h, sizeof h, "N%07d", i);
    found += mega_tree_find(&t, h) != NULL;
  }
  TEST_CHECK(found == 3000 && mega_tree_find(&t, "FILE0001"));
  mega_tree_free(&t);
}

/* ---- transfers ----------------------------------------------------------------------------------- */

/* the MAC the simple way: each chunk from scratch */
static void naive_mac(const FmAes *a, const u8 nonce[8], const u8 *p, u64 size, u8 meta[8]) {
  u8 file[16];
  memset(file, 0, 16);
  for (u64 pos = 0; pos < size;) {
    u64 end = mega_chunk_end(pos);
    if (end > size) end = size;
    u8 mac[16];
    memcpy(mac, nonce, 8);
    memcpy(mac + 8, nonce, 8);
    for (u64 q = pos; q < end; q += 16) {
      u8 b[16];
      memset(b, 0, 16);
      memcpy(b, p + q, (size_t)FM_MIN((u64)16, end - q));
      for (int i = 0; i < 16; i++) mac[i] ^= b[i];
      aes_encrypt_block(a, mac, mac);
    }
    for (int i = 0; i < 16; i++) file[i] ^= mac[i];
    aes_encrypt_block(a, file, file);
    pos = end;
  }
  for (int i = 0; i < 4; i++) {
    meta[i] = (u8)(file[i] ^ file[4 + i]);
    meta[4 + i] = (u8)(file[8 + i] ^ file[12 + i]);
  }
}

static void test_transfer(const char *tmp) {
  /* the chunk schedule */
  TEST_CHECK(mega_chunk_end(0) == 131072);
  TEST_CHECK(mega_chunk_end(131072) == 393216);
  TEST_CHECK(mega_chunk_end(393216) == 786432);
  TEST_CHECK(mega_chunk_end(3670016) == 4718592);
  TEST_CHECK(mega_chunk_end(4718592) == 5767168);
  TEST_CHECK(mega_chunk_end(5767168) == 6815744);
  int chunks = 0;
  for (u64 p = 0; p < 5000000; p = mega_chunk_end(p)) chunks++;
  TEST_CHECK(chunks == 9);                 /* 8 growing chunks = 4.5 MB, then one */

  const u64 size = 5 * 1000 * 1000 + 7;    /* 5 MB and a partial last block */
  u8 *plain = (u8 *)fm_alloc((size_t)size), *enc = (u8 *)fm_alloc((size_t)size), *dec = (u8 *)fm_alloc((size_t)size);
  for (u64 i = 0; i < size; i++) plain[i] = rnd8();
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "five.bin");
  FILE *fw = fm_fopen(path, "wb");
  TEST_CHECK(fw && fwrite(plain, 1, (size_t)size, fw) == size);
  if (fw) fclose(fw);

  u8 key[24];
  for (int i = 0; i < 24; i++) key[i] = rnd8();
  FmAes aes;
  aes_init(&aes, key, 16);
  u8 iv[16];
  memset(iv, 0, 16);
  memcpy(iv, key + 16, 8);
  /* "upload": read the file chunk by chunk, MAC the plaintext, encrypt */
  MgMac mac;
  mega_mac_init(&mac, &aes, key + 16);
  FILE *fr = fm_fopen(path, "rb");
  u64 pos = 0;
  while (fr && pos < size) {
    u64 end = mega_chunk_end(pos);
    if (end > size) end = size;
    size_t n = (size_t)(end - pos);
    TEST_CHECK(fread(enc + pos, 1, n, fr) == n);
    mega_mac_feed(&mac, enc + pos, n);
    aes_ctr_be(&aes, iv, pos, enc + pos, n);
    pos = end;
  }
  if (fr) fclose(fr);
  u8 up_meta[8], naive[8];
  mega_mac_final(&mac, up_meta);
  naive_mac(&aes, key + 16, plain, size, naive);
  TEST_CHECK(!memcmp(up_meta, naive, 8));
  TEST_CHECK(memcmp(enc, plain, 4096) != 0);
  /* "download": odd pieces that straddle chunk boundaries */
  MgMac dm;
  mega_mac_init(&dm, &aes, key + 16);
  size_t piece = 7777;
  for (u64 p = 0; p < size;) {
    size_t n = (size_t)FM_MIN((u64)piece, size - p);
    memcpy(dec + p, enc + p, n);
    aes_ctr_be(&aes, iv, p, dec + p, n);
    mega_mac_feed(&dm, dec + p, n);
    p += n;
    piece = piece * 3 % 65521 + 1;
  }
  u8 dl_meta[8];
  mega_mac_final(&dm, dl_meta);
  TEST_CHECK(!memcmp(dec, plain, (size_t)size));
  TEST_CHECK(!memcmp(dl_meta, up_meta, 8));
  /* one flipped byte in the cipher text: the MAC must change */
  enc[2500000] ^= 0x40;
  mega_mac_init(&dm, &aes, key + 16);
  memcpy(dec, enc, (size_t)size);
  aes_ctr_be(&aes, iv, 0, dec, (size_t)size);
  mega_mac_feed(&dm, dec, (size_t)size);
  mega_mac_final(&dm, dl_meta);
  TEST_CHECK(memcmp(dl_meta, up_meta, 8) != 0);
  fm_free(plain);
  fm_free(enc);
  fm_free(dec);

  /* fingerprints: verbatim for tiny files, CRC quarters up to 8 KB, mtime at the end */
  char fp[64];
  fm_path_join(path, sizeof path, tmp, "tiny.txt");
  fw = fm_fopen(path, "wb");
  if (fw) { fwrite("hello", 1, 5, fw); fclose(fw); }
  TEST_CHECK(mega_fingerprint(path, 5, 1700000000, fp, sizeof fp));
  u8 fb[32];
  TEST_CHECK(mega_b64_dec(fp, fb, sizeof fb) == 21 && !memcmp(fb, "hello\0\0\0", 8) && fb[16] == 4);
  TEST_CHECK(mega_fp_mtime(fp) == 1700000000);
  u8 small[1000];
  for (int i = 0; i < 1000; i++) small[i] = (u8)(i * 7);
  fm_path_join(path, sizeof path, tmp, "small.bin");
  fw = fm_fopen(path, "wb");
  if (fw) { fwrite(small, 1, 1000, fw); fclose(fw); }
  TEST_CHECK(mega_fingerprint(path, 1000, 1, fp, sizeof fp));
  TEST_CHECK(mega_b64_dec(fp, fb, sizeof fb) == 18);
  u32 c1 = crc32_update(0, small + 250, 250);
  TEST_CHECK(fb[4] == (u8)(c1 >> 24) && fb[7] == (u8)c1);
}

/* the proof of work, checked against the plain 12 MB buffer go-mega hashes */
static void test_hashcash(void) {
  u8 tok[48];
  for (int i = 0; i < 48; i++) tok[i] = (u8)(i * 13 + 5);
  char t64[80], hdr[160], out[200];
  mega_b64_enc(tok, 48, t64, sizeof t64);
  fm_snprintf(hdr, sizeof hdr, "1:255:1760000000:%s", t64);     /* threshold 127 << 24: ~1 in 2 tries */
  volatile int cancel = 0;
  TEST_CHECK(mega_hashcash(hdr, out, sizeof out, &cancel));
  char want[100];
  fm_snprintf(want, sizeof want, "X-Hashcash: 1:%s:", t64);
  TEST_CHECK(!strncmp(out, want, strlen(want)));
  u8 pre[8];
  char cash[16];
  fm_strlcpy(cash, out + strlen(want), sizeof cash);
  cash[strcspn(cash, "\r\n")] = 0;
  TEST_CHECK(mega_b64_dec(cash, pre, sizeof pre) == 4);
  size_t n = 4 + (size_t)262144 * 48;
  u8 *buf = (u8 *)fm_alloc(n);
  memcpy(buf, pre, 4);
  for (size_t i = 0; i < 262144; i++) memcpy(buf + 4 + i * 48, tok, 48);
  FmSha256 s;
  u8 d[32];
  sha256_init(&s);
  sha256_update(&s, buf, n);
  sha256_final(&s, d);
  u32 v = ((u32)d[0] << 24) | ((u32)d[1] << 16) | ((u32)d[2] << 8) | d[3];
  TEST_CHECK(v <= (u32)127 << 24);
  fm_free(buf);
  TEST_CHECK(!mega_hashcash("2:10:0:abc", out, sizeof out, &cancel));
}

static void test_links(void) {
  MgLink l;
  TEST_CHECK(mega_parse_link("https://mega.nz/file/muAVRRbb#zp9dvPvoVck8-4IwTazqsUqol6yiUK7kwLWOwrD8Jqo", &l));
  TEST_CHECK(!l.folder && !strcmp(l.id, "muAVRRbb") && l.klen == 32);
  TEST_CHECK(mega_parse_link("https://mega.nz/folder/GvgkUIIK#v2hd_5GSvciGKazNeWSa6A", &l));
  TEST_CHECK(l.folder && !strcmp(l.id, "GvgkUIIK") && l.klen == 16 && !l.sub[0]);
  TEST_CHECK(mega_parse_link("https://mega.nz/folder/GvgkUIIK#v2hd_5GSvciGKazNeWSa6A/folder/T2wilTgb", &l));
  TEST_CHECK(l.folder && !strcmp(l.sub, "T2wilTgb"));
  TEST_CHECK(mega_parse_link("https://mega.co.nz/#!muAVRRbb!zp9dvPvoVck8-4IwTazqsUqol6yiUK7kwLWOwrD8Jqo", &l));
  TEST_CHECK(!l.folder && !strcmp(l.id, "muAVRRbb"));
  TEST_CHECK(mega_parse_link("mega.nz/#F!GvgkUIIK!v2hd_5GSvciGKazNeWSa6A", &l) && l.folder);
  TEST_CHECK(!mega_parse_link("https://mega.nz/file/muAVRRbb", &l));          /* no key */
  TEST_CHECK(!mega_parse_link("https://mega.nz/file/muAVRRbb#short", &l));
  TEST_CHECK(!mega_parse_link("https://example.com/file/x#y", &l));
  /* base64url */
  u8 b[8];
  char s[16];
  TEST_CHECK(mega_b64_enc((const u8 *)"\xfb\xff\xbf", 3, s, sizeof s) == 4 && !strcmp(s, "-_-_"));
  TEST_CHECK(mega_b64_dec("-_-_", b, sizeof b) == 3 && b[0] == 0xfb && b[2] == 0xbf);
  TEST_CHECK(mega_b64_dec("+/+/", b, sizeof b) == 3 && b[1] == 0xff);
  TEST_CHECK(mega_b64_dec("QQ==", b, sizeof b) == 1 && b[0] == 'A');
  TEST_CHECK(mega_b64_dec("Q*", b, sizeof b) < 0);
}

/* ---- live ----------------------------------------------------------------------------------------- */

typedef struct Found { FmCloudEntry e; bool have; int listed; } Found;

static void walk(FmCloudAcct *a, const char *dir, int depth, Found *f) {
  volatile int cancel = 0;
  FmCloudList l;
  memset(&l, 0, sizeof l);
  FmErr e = g_cloud_mega.list(a, dir, &l, &cancel);
  TEST_CHECK(e == FM_OK);
  if (e != FM_OK) printf("  list failed: %s\n", l.error);
  for (int i = 0; i < l.count && f->listed < 200; i++, f->listed++) {
    const FmCloudEntry *x = &l.items[i];
    printf("  %*s%s %s (%llu bytes)\n", depth * 2, "", x->dir ? "[dir]" : "     ", x->name, (unsigned long long)x->size);
    if (!x->dir && !f->have && x->size < (20u << 20)) { f->e = *x; f->have = true; }
    if (x->dir && depth < 4) walk(a, x->id, depth + 1, f);
  }
  cloud_list_free(&l);
}

static bool live_progress(void *u, u64 done, u64 total) {
  FM_UNUSED(u); FM_UNUSED(done); FM_UNUSED(total);
  return true;
}

static void live_link(const char *link, const char *tmp) {
  FmCloudAcct a;
  FmCloudEntry root;
  char err[256] = "";
  volatile int cancel = 0;
  printf("  link %s\n", link);
  FmErr e = g_cloud_mega.open_link(link, &a, &root, err, sizeof err, &cancel);
  TEST_CHECK(e == FM_OK);
  if (e != FM_OK) { printf("  open_link: %s\n", err); return; }
  printf("  root \"%s\" (label \"%s\")\n", root.name, a.label);
  Found f;
  memset(&f, 0, sizeof f);
  walk(&a, root.id, 1, &f);
  TEST_CHECK(f.have);
  if (!f.have) return;
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "live.bin");
  u64 t0 = plat_now_ms();
  e = g_cloud_mega.download(&a, &f.e, path, live_progress, NULL, err, sizeof err, &cancel);
  printf("  download %s: %s (%llu ms) %s\n", f.e.name, e == FM_OK ? "MAC ok" : "FAILED",
         (unsigned long long)(plat_now_ms() - t0), e == FM_OK ? "" : err);
  TEST_CHECK(e == FM_OK);
  FmStat st;
  TEST_CHECK(plat_stat(path, &st) && st.size == f.e.size);
  if (f.e.size && f.e.size < 200) {
    FILE *fr = fm_fopen(path, "rb");
    char txt[256] = "";
    if (fr) { size_t n = fread(txt, 1, sizeof txt - 1, fr); txt[n] = 0; fclose(fr); }
    printf("  content: %s\n", txt);
  }
  plat_remove_file(path);
}

static void live_login(const char *spec, const char *tmp) {
  char user[256], *bar;
  fm_strlcpy(user, spec, sizeof user);
  if (!(bar = strchr(user, '|'))) return;
  *bar = 0;
  FmCloudAcct a;
  memset(&a, 0, sizeof a);
  fm_strlcpy(a.provider, "mega", sizeof a.provider);
  fm_strlcpy(a.user, user, sizeof a.user);
  fm_strlcpy(a.secret, bar + 1, sizeof a.secret);
  char err[256] = "";
  volatile int cancel = 0;
  u64 t0 = plat_now_ms();
  FmErr e = g_cloud_mega.login(&a, err, sizeof err, &cancel);
  printf("  login: %s (%llu ms) %s\n", e == FM_OK ? "ok" : "FAILED", (unsigned long long)(plat_now_ms() - t0), err);
  TEST_CHECK(e == FM_OK && a.session_changed && !a.secret[0]);
  if (e != FM_OK) return;
  u64 used = 0, total = 0;
  TEST_CHECK(g_cloud_mega.quota(&a, &used, &total, err, sizeof err, &cancel) == FM_OK);
  printf("  quota: %llu / %llu bytes\n", (unsigned long long)used, (unsigned long long)total);
  FmCloudList l;
  memset(&l, 0, sizeof l);
  TEST_CHECK(g_cloud_mega.list(&a, "", &l, &cancel) == FM_OK);
  printf("  drive: %d entries\n", l.count);
  cloud_list_free(&l);
  const char *w = getenv("MMCFM_MEGA_WRITE");
  if (!w || strcmp(w, "1") != 0) return;
  char dname[64];
  fm_snprintf(dname, sizeof dname, "mmcfm-test-%llu", (unsigned long long)plat_now_ms());
  FmCloudEntry dir, sub, up;
  TEST_CHECK(g_cloud_mega.mkdir(&a, "", dname, &dir, err, sizeof err, &cancel) == FM_OK);
  TEST_CHECK(g_cloud_mega.mkdir(&a, dir.id, "sub", &sub, err, sizeof err, &cancel) == FM_OK);
  char src[FM_PATH_MAX], back[FM_PATH_MAX];
  fm_path_join(src, sizeof src, tmp, "up.bin");
  fm_path_join(back, sizeof back, tmp, "back.bin");
  u8 *data = (u8 *)fm_alloc(300000);
  for (int i = 0; i < 300000; i++) data[i] = rnd8();
  FILE *fw = fm_fopen(src, "wb");
  if (fw) { fwrite(data, 1, 300000, fw); fclose(fw); }
  e = g_cloud_mega.upload(&a, dir.id, src, "up.bin", &up, live_progress, NULL, err, sizeof err, &cancel);
  printf("  upload: %s %s\n", e == FM_OK ? "ok" : "FAILED", e == FM_OK ? "" : err);
  TEST_CHECK(e == FM_OK && up.size == 300000);
  e = g_cloud_mega.download(&a, &up, back, live_progress, NULL, err, sizeof err, &cancel);
  TEST_CHECK(e == FM_OK);
  FILE *fr = fm_fopen(back, "rb");
  u8 *got = (u8 *)fm_alloc(300000);
  TEST_CHECK(fr && fread(got, 1, 300000, fr) == 300000 && !memcmp(got, data, 300000));
  if (fr) fclose(fr);
  /* replace: the same name again becomes a new version */
  TEST_CHECK(g_cloud_mega.upload(&a, dir.id, src, "up.bin", &up, NULL, NULL, err, sizeof err, &cancel) == FM_OK);
  TEST_CHECK(g_cloud_mega.rename(&a, &up, "renamed.bin", err, sizeof err, &cancel) == FM_OK);
  TEST_CHECK(g_cloud_mega.move(&a, &up, sub.id, err, sizeof err, &cancel) == FM_OK);
  memset(&l, 0, sizeof l);
  TEST_CHECK(g_cloud_mega.list(&a, sub.id, &l, &cancel) == FM_OK && l.count == 1 && !strcmp(l.items[0].name, "renamed.bin"));
  cloud_list_free(&l);
  TEST_CHECK(g_cloud_mega.remove(&a, &dir, err, sizeof err, &cancel) == FM_OK);
  fm_free(data);
  fm_free(got);
}

int test_mega(const char *tmp) {
  int before = g_test_fail;
  test_hashes();
  test_ctr();
  test_bignum();
  test_login();
  test_attr();
  test_tree();
  test_transfer(tmp);
  test_links();
  test_hashcash();
  const char *links = getenv("MMCFM_MEGA_LINK");
  if (links && *links) {
    char buf[2048];
    fm_strlcpy(buf, links, sizeof buf);
    for (char *p = buf, *e; p && *p; p = e) {
      e = strchr(p, '|');
      if (e) *e++ = 0;
      live_link(p, tmp);
    }
  }
  const char *login = getenv("MMCFM_MEGA_LOGIN");
  if (login && *login) live_login(login, tmp);
  return g_test_fail - before;
}
