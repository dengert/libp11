/* libp11, a simple layer on to of PKCS#11 API
 * Copyright (C) 2005 Olaf Kirch <okir@lst.de>
 * Copyright (C) 2016 Michał Trojnara <Michal.Trojnara@stunnel.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

/*
 * This file implements the handling of RSA keys stored on a
 * PKCS11 token
 */

#include "libp11-int.h"
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

static int rsa_ex_index = 0;

#if OPENSSL_VERSION_NUMBER < 0x10100003L || defined(LIBRESSL_VERSION_NUMBER)
#define EVP_PKEY_get0_RSA(key) ((key)->pkey.rsa)
#endif

static RSA *pkcs11_rsa(PKCS11_KEY *key)
{
	EVP_PKEY *evp_key = pkcs11_get_key(key, key->isPrivate);
	RSA *rsa;
	if (evp_key == NULL)
		return NULL;
	rsa = EVP_PKEY_get0_RSA(evp_key);
	EVP_PKEY_free(evp_key);
	return rsa;
}

/* PKCS#1 v1.5 RSA signature */
/* TODO: remove this function in libp11 0.5.0 */
int pkcs11_sign(int type, const unsigned char *m, unsigned int m_len,
		unsigned char *sigret, unsigned int *siglen, PKCS11_KEY *key)
{
	RSA *rsa = pkcs11_rsa(key);
	if (rsa == NULL)
		return -1;
	return RSA_sign(type, m, m_len, sigret, siglen, rsa);
}

/* Setup PKCS#11 mechanisms for encryption/decryption */
static int pkcs11_mechanism(CK_MECHANISM *mechanism, const int padding)
{
	memset(mechanism, 0, sizeof(CK_MECHANISM));
	switch (padding) {
	case RSA_PKCS1_PADDING:
		mechanism->mechanism = CKM_RSA_PKCS;
		break;
	case RSA_NO_PADDING:
		mechanism->mechanism = CKM_RSA_X_509;
		break;
	case RSA_X931_PADDING:
		mechanism->mechanism = CKM_RSA_X9_31;
		break;
	default:
		fprintf(stderr, "PKCS#11: Unsupported padding type\n");
		return -1;
	}
	return 0;
}

/* RSA private key encryption (also invoked by OpenSSL for signing) */
/* OpenSSL assumes that the output buffer is always big enough */
int pkcs11_private_encrypt(int flen,
		const unsigned char *from, unsigned char *to,
		PKCS11_KEY *key, int padding)
{
	PKCS11_SLOT *slot = KEY2SLOT(key);
	PKCS11_CTX *ctx = KEY2CTX(key);
	PKCS11_KEY_private *kpriv = PRIVKEY(key);
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);
	CK_MECHANISM mechanism;
	CK_ULONG size;
	int rv;

	size = pkcs11_get_key_size(key);

	if (pkcs11_mechanism(&mechanism, padding) < 0)
		return -1;

	CRYPTO_THREAD_write_lock(PRIVCTX(ctx)->rwlock);
	/* Try signing first, as applications are more likely to use it */
	rv = CRYPTOKI_call(ctx,
		C_SignInit(spriv->session, &mechanism, kpriv->object));
	if (kpriv->always_authenticate == CK_TRUE)
		rv = pkcs11_authenticate(key);
	if (!rv)
		rv = CRYPTOKI_call(ctx,
			C_Sign(spriv->session, (CK_BYTE *)from, flen, to, &size));
	if (rv == CKR_KEY_FUNCTION_NOT_PERMITTED) {
		/* OpenSSL may use it for encryption rather than signing */
		rv = CRYPTOKI_call(ctx,
			C_EncryptInit(spriv->session, &mechanism, kpriv->object));
		if (kpriv->always_authenticate == CK_TRUE)
			rv = pkcs11_authenticate(key);
		if (!rv)
			rv = CRYPTOKI_call(ctx,
				C_Encrypt(spriv->session, (CK_BYTE *)from, flen, to, &size));
	}
	CRYPTO_THREAD_unlock(PRIVCTX(ctx)->rwlock);

	if (rv) {
		CKRerr(CKR_F_PKCS11_PRIVATE_ENCRYPT, rv);
		return -1;
	}

	return size;
}

/* RSA private key decryption */
int pkcs11_private_decrypt(int flen, const unsigned char *from, unsigned char *to,
		PKCS11_KEY *key, int padding)
{
	PKCS11_SLOT *slot = KEY2SLOT(key);
	PKCS11_CTX *ctx = KEY2CTX(key);
	PKCS11_KEY_private *kpriv = PRIVKEY(key);
	PKCS11_SLOT_private *spriv = PRIVSLOT(slot);
	CK_MECHANISM mechanism;
	CK_ULONG size = flen;
	CK_RV rv;

	if (pkcs11_mechanism(&mechanism, padding) < 0)
		return -1;

	CRYPTO_THREAD_write_lock(PRIVCTX(ctx)->rwlock);
	rv = CRYPTOKI_call(ctx,
		C_DecryptInit(spriv->session, &mechanism, kpriv->object));
	if (kpriv->always_authenticate == CK_TRUE)
		rv = pkcs11_authenticate(key);
	if (!rv)
		rv = CRYPTOKI_call(ctx,
			C_Decrypt(spriv->session, (CK_BYTE *)from, size,
				(CK_BYTE_PTR)to, &size));
	CRYPTO_THREAD_unlock(PRIVCTX(ctx)->rwlock);

	if (rv) {
		CKRerr(CKR_F_PKCS11_PRIVATE_DECRYPT, rv);
		return -1;
	}

	return size;
}

/* TODO: remove this function in libp11 0.5.0 */
int pkcs11_verify(int type, const unsigned char *m, unsigned int m_len,
		unsigned char *signature, unsigned int siglen, PKCS11_KEY *key)
{
	(void)type;
	(void)m;
	(void)m_len;
	(void)signature;
	(void)siglen;
	(void)key;

	/* PKCS11 calls go here */
	P11err(P11_F_PKCS11_VERIFY, P11_R_NOT_SUPPORTED);
	return -1;
}

/*
 * Get RSA key material
 */
static RSA *pkcs11_get_rsa(PKCS11_KEY *key)
{
	RSA *rsa;
	PKCS11_KEY *keys = NULL;
	unsigned int i, count = 0;
	BIGNUM *rsa_n=NULL, *rsa_e=NULL;

	rsa = RSA_new();
	if (rsa == NULL)
		return NULL;

	/* Retrieve the modulus and the public exponent */
	if (key_getattr_bn(key, CKA_MODULUS, &rsa_n) ||
			key_getattr_bn(key, CKA_PUBLIC_EXPONENT, &rsa_e))
		goto failure;
	if (!BN_is_zero(rsa_e)) /* The public exponent was retrieved */
		goto success;
	BN_clear_free(rsa_e);
	rsa_e = NULL;

	/* The public exponent was not found in the private key:
	 * retrieve it from the corresponding public key */
	if (!PKCS11_enumerate_public_keys(KEY2TOKEN(key), &keys, &count)) {
		for(i = 0; i < count; i++) {
			BIGNUM *pubmod;

			if (key_getattr_bn(&keys[i], CKA_MODULUS, &pubmod))
				continue; /* Failed to retrieve the modulus */
			if (BN_cmp(rsa_n, pubmod) == 0) { /* The key was found */
				BN_clear_free(pubmod);
				if (key_getattr_bn(&keys[i], CKA_PUBLIC_EXPONENT, &rsa_e))
					continue; /* Failed to retrieve the public exponent */
				goto success;
			} else {
				BN_clear_free(pubmod);
			}
		}
	}

	/* Last resort: use the most common default */
	rsa_e = BN_new();
	if (rsa_e && BN_set_word(rsa_e, RSA_F4))
		goto success;

failure:
	RSA_free(rsa);
	return NULL;

success:
#if OPENSSL_VERSION_NUMBER >= 0x10100005L && !defined(LIBRESSL_VERSION_NUMBER)
		RSA_set0_key(rsa, rsa_n, rsa_e, NULL);
#else
		rsa->n=rsa_n;
		rsa->e=rsa_e;
#endif
	return rsa;
}

static void pkcs11_set_ex_data_rsa(RSA* rsa, PKCS11_KEY* key)
{
	RSA_set_ex_data(rsa, rsa_ex_index, key);
}

static void pkcs11_update_ex_data_rsa(PKCS11_KEY* key)
{
	EVP_PKEY* evp = key->evp_key;
	RSA* rsa;
	if (evp == NULL)
		return;
	if (EVP_PKEY_base_id(evp) != EVP_PKEY_RSA)
		return;

	rsa = EVP_PKEY_get1_RSA(evp);
	pkcs11_set_ex_data_rsa(rsa, key);
	RSA_free(rsa);
}
/*
 * Build an EVP_PKEY object
 */
static EVP_PKEY *pkcs11_get_evp_key_rsa(PKCS11_KEY *key)
{
	EVP_PKEY *pk;
	RSA *rsa;

	rsa = pkcs11_get_rsa(key);
	if (rsa == NULL)
		return NULL;
	pk = EVP_PKEY_new();
	if (pk == NULL) {
		RSA_free(rsa);
		return NULL;
	}
	EVP_PKEY_set1_RSA(pk, rsa); /* Also increments the rsa ref count */

	if (key->isPrivate)
		RSA_set_method(rsa, PKCS11_get_rsa_method());
	/* TODO: Retrieve the RSA private key object attributes instead,
	 * unless the key has the "sensitive" attribute set */

#if OPENSSL_VERSION_NUMBER < 0x01010000L
	/* RSA_FLAG_SIGN_VER is no longer needed since OpenSSL 1.1 */
	rsa->flags |= RSA_FLAG_SIGN_VER;
#endif
	pkcs11_set_ex_data_rsa(rsa, key);
	RSA_free(rsa); /* Drops our reference to it */
	return pk;
}

/* TODO: remove this function in libp11 0.5.0 */
int pkcs11_get_key_modulus(PKCS11_KEY *key, BIGNUM **bn)
{
	RSA *rsa = pkcs11_rsa(key);
	const BIGNUM *rsa_n;

	if (rsa == NULL)
		return 0;
#if OPENSSL_VERSION_NUMBER >= 0x10100005L && !defined(LIBRESSL_VERSION_NUMBER)
	RSA_get0_key(rsa, &rsa_n, NULL, NULL);
#else
	rsa_n=rsa->n;
#endif
	*bn = BN_dup(rsa_n);
	return *bn == NULL ? 0 : 1;
}

/* TODO: remove this function in libp11 0.5.0 */
int pkcs11_get_key_exponent(PKCS11_KEY *key, BIGNUM **bn)
{
	RSA *rsa = pkcs11_rsa(key);
	const BIGNUM *rsa_e;

	if (rsa == NULL)
		return 0;
#if OPENSSL_VERSION_NUMBER >= 0x10100005L && !defined(LIBRESSL_VERSION_NUMBER)
	RSA_get0_key(rsa, NULL, &rsa_e, NULL);
#else
	rsa_e=rsa->e;
#endif
	*bn = BN_dup(rsa_e);
	return *bn == NULL ? 0 : 1;
}

/* TODO: make this function static in libp11 0.5.0 */
int pkcs11_get_key_size(PKCS11_KEY *key)
{
	RSA *rsa = pkcs11_rsa(key);
	if (rsa == NULL)
		return 0;
	return RSA_size(rsa);
}

#if OPENSSL_VERSION_NUMBER < 0x10100005L || defined(LIBRESSL_VERSION_NUMBER)

int (*RSA_meth_get_priv_enc(const RSA_METHOD *meth))
		(int flen, const unsigned char *from,
			unsigned char *to, RSA *rsa, int padding)
{
    return meth->rsa_priv_enc;
}

int (*RSA_meth_get_priv_dec(const RSA_METHOD *meth))
		(int flen, const unsigned char *from,
			unsigned char *to, RSA *rsa, int padding)
{
    return meth->rsa_priv_dec;
}

#endif

/*
 * if we can not handle the this, call the original pkey_rsa_sign
 */

int pkcs11_pkey_rsa_sign(EVP_PKEY_CTX *ctx, unsigned char *sig,
                         size_t *siglen, const unsigned char *tbs,
                         size_t tbslen)
{
	int ret;
	EVP_PKEY *pkey = NULL;
	RSA *rsa = NULL;
	const EVP_MD *sigmd = NULL, *mgf1md = NULL;
	int pad = -1;
	RSA_PSS_PARAMS *pss = NULL;
	ASN1_STRING *os = NULL;
	int saltlen, rv = 0;


	fprintf(stderr, " pkcs11_pkey_rsa_sign called\n");

	pkey = EVP_PKEY_CTX_get0_pkey(ctx);
	if (pkey)
		rsa = EVP_PKEY_get1_RSA(pkey);

	EVP_PKEY_CTX_get_signature_md(ctx, &sigmd);

	if (!rsa || !pkey)
		goto do_original;

	if (sigmd) {
		if (tbslen != (size_t)EVP_MD_size(sigmd)) {
			goto do_original;
		}
	}

	EVP_PKEY_CTX_get_rsa_padding(ctx, &pad);

	switch (pad) {
		case RSA_PKCS1_PSS_PADDING:
			fprintf(stderr, "RSA_PSS\n");
			if (EVP_PKEY_CTX_get_signature_md(ctx, &sigmd) <= 0)
				goto do_original;
			if (EVP_PKEY_CTX_get_rsa_mgf1_md(ctx, &mgf1md) <= 0)
				goto do_original;
			if (!EVP_PKEY_CTX_get_rsa_pss_saltlen(ctx, &saltlen))
				goto do_original;
			if (saltlen == -1)
				saltlen = EVP_MD_size(sigmd);
			else if (saltlen == -2) {
				saltlen = EVP_PKEY_size(pkey) - EVP_MD_size(sigmd) - 2;
				if (((EVP_PKEY_bits(pkey) - 1) & 0x7) == 0)
					saltlen--;
			}

/*
 * Add code here to make CKM_* and see if card supports it. If it does, call PKCS#11
 * functions to do the signature.  Then fill in sig snd siglen
 * and return 1; 
 * if card can not do it or fails  goto do_original. or return  error or -1.
 * 
 * Look at openssl/crypto/rsa/rsa_pmeth.c pkey_rsa_sign. 
 */

			fprintf(stderr,"saltlen=%d sigmd=%d mdf1=%d \n",
				saltlen, EVP_MD_type(sigmd),EVP_MD_type(mgf1md));
			break;
		default:
			fprintf(stderr, "not RSA PSS pad= %d\n", pad);
		goto do_original;
	}


do_original:
	if (rsa)
	    RSA_free(rsa);
	return (*original_rsa_pkey_sign)(ctx, sig, siglen, tbs, tbslen);
}

static int pkcs11_rsa_priv_dec_method(int flen, const unsigned char *from,
		unsigned char *to, RSA *rsa, int padding)
{
	PKCS11_KEY *key = RSA_get_ex_data(rsa, rsa_ex_index);
	int (*priv_dec) (int flen, const unsigned char *from,
		unsigned char *to, RSA *rsa, int padding);
	if (key == NULL) {
		priv_dec = RSA_meth_get_priv_dec(RSA_get_default_method());
		return priv_dec(flen, from, to, rsa, padding);
	}
	return PKCS11_private_decrypt(flen, from, to, key, padding);
}

static int pkcs11_rsa_priv_enc_method(int flen, const unsigned char *from,
		unsigned char *to, RSA *rsa, int padding)
{
	PKCS11_KEY *key = RSA_get_ex_data(rsa, rsa_ex_index);
	int (*priv_enc) (int flen, const unsigned char *from,
		unsigned char *to, RSA *rsa, int padding);
	if (key == NULL) {
		priv_enc = RSA_meth_get_priv_enc(RSA_get_default_method());
		return priv_enc(flen, from, to, rsa, padding);
	}
	return PKCS11_private_encrypt(flen, from, to, key, padding);
}

static int pkcs11_rsa_free_method(RSA *rsa)
{
	RSA_set_ex_data(rsa, rsa_ex_index, NULL);
	return 1;
}

static void alloc_rsa_ex_index()
{
	if (rsa_ex_index == 0) {
		while (rsa_ex_index == 0) /* Workaround for OpenSSL RT3710 */
			rsa_ex_index = RSA_get_ex_new_index(0, "libp11 rsa",
				NULL, NULL, NULL);
		if (rsa_ex_index < 0)
			rsa_ex_index = 0; /* Fallback to app_data */
	}
}

static void free_rsa_ex_index()
{
	/* CRYPTO_free_ex_index requires OpenSSL version >= 1.1.0-pre1 */
#if OPENSSL_VERSION_NUMBER >= 0x10100001L && !defined(LIBRESSL_VERSION_NUMBER)
	if (rsa_ex_index > 0) {
		CRYPTO_free_ex_index(CRYPTO_EX_INDEX_RSA, rsa_ex_index);
		rsa_ex_index = 0;
	}
#endif
}

#if OPENSSL_VERSION_NUMBER < 0x10100005L || defined(LIBRESSL_VERSION_NUMBER)

static RSA_METHOD *RSA_meth_dup(const RSA_METHOD *meth)
{
	RSA_METHOD *ret = OPENSSL_malloc(sizeof(RSA_METHOD));
	if (ret == NULL)
		return NULL;
	memcpy(ret, meth, sizeof(RSA_METHOD));
	ret->name = OPENSSL_strdup(meth->name);
	if (ret->name == NULL) {
		OPENSSL_free(ret);
		return NULL;
	}
	return ret;
}

static int RSA_meth_set1_name(RSA_METHOD *meth, const char *name)
{
	char *tmp = OPENSSL_strdup(name);
	if (tmp == NULL)
		return 0;
	OPENSSL_free((char *)meth->name);
	meth->name = tmp;
	return 1;
}

static int RSA_meth_set_flags(RSA_METHOD *meth, int flags)
{
	meth->flags = flags;
	return 1;
}

static int RSA_meth_set_priv_enc(RSA_METHOD *meth,
		int (*priv_enc) (int flen, const unsigned char *from,
		unsigned char *to, RSA *rsa, int padding))
{
	meth->rsa_priv_enc = priv_enc;
	return 1;
}

static int RSA_meth_set_priv_dec(RSA_METHOD *meth,
		int (*priv_dec) (int flen, const unsigned char *from,
		unsigned char *to, RSA *rsa, int padding))
{
	meth->rsa_priv_dec = priv_dec;
	return 1;
}

static int RSA_meth_set_finish(RSA_METHOD *meth, int (*finish)(RSA *rsa))
{
	meth->finish = finish;
	return 1;
}

#endif

/*
 * Overload the default OpenSSL methods for RSA
 */
RSA_METHOD *PKCS11_get_rsa_method(void)
{
	static RSA_METHOD *ops = NULL;

	if (ops == NULL) {
		alloc_rsa_ex_index();
		ops = RSA_meth_dup(RSA_get_default_method());
		if (ops == NULL)
			return NULL;
		RSA_meth_set1_name(ops, "libp11 RSA method");
		RSA_meth_set_flags(ops, 0);
		RSA_meth_set_priv_enc(ops, pkcs11_rsa_priv_enc_method);
		RSA_meth_set_priv_dec(ops, pkcs11_rsa_priv_dec_method);
		RSA_meth_set_finish(ops, pkcs11_rsa_free_method);
	}
	return ops;
}

/* This function is *not* currently exported */
void PKCS11_rsa_method_free(void)
{
	free_rsa_ex_index();
}

PKCS11_KEY_ops pkcs11_rsa_ops = {
	EVP_PKEY_RSA,
	pkcs11_get_evp_key_rsa,
	pkcs11_update_ex_data_rsa
};

/* vim: set noexpandtab: */
