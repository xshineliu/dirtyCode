/*
 * toeplitzHash.c
 *
 * REF1: https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/82599-10-gbe-controller-datasheet.pdf
 * Page 309
 *
 *  Created on: Apr 9, 2018
 *      Author: shine
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Mellanox Linux's driver key
/*
 uint8_t default_rsskey_40bytes[40] = { 0xd1, 0x81, 0xc6, 0x2c, 0xf7, 0xf4, 0xdb,
 0x5b, 0x19, 0x83, 0xa2, 0xfc, 0x94, 0x3e, 0x1a, 0xdb, 0xd9, 0x38, 0x9e,
 0x6b, 0xd1, 0x03, 0x9c, 0x2c, 0xa7, 0x44, 0x99, 0xad, 0x59, 0x3d, 0x56,
 0xd9, 0xf3, 0x25, 0x3c, 0x06, 0x2a, 0xdc, 0x1f, 0xfc };
 */

uint8_t default_rsskey_40bytes[40] = { 0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e,
		0xc2, 0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0, 0xd0, 0xca, 0x2b,
		0xcb, 0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2,
		0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa };

uint8_t rss_sym_key[40] = {
/* L1 */
0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
/* L2 */
0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
/* L3 */
0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
/* L4 */
0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
/* L5 */
0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, };


uint8_t rss_hack_dport[40] = {
/* L1 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* L2 : bit 112 to bit 127 marked with FF */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
/* L3 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* L4 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* L5 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, };


uint8_t rss_hack_sip_256[40] = {
/* L1 */
0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
/* L2 : bit 122 to bit 127 marked with FF */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* L3 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* L4 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* L5 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, };

uint8_t rss_hack_sip_16[40] = {
/* L1 */
0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
/* L2 : bit 122 to bit 127 marked with FF */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* L3 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* L4 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/* L5 */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, };


uint8_t rss_sym_key_00[40] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, };

uint8_t rss_sym_key_ff[40] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, };

uint8_t rss_sym_key_small[8] =
		{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, };

uint8_t rss_sym_key_smallx[8] =
		{ 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, };

uint8_t rss_sym_key_small1[8] =
		{ 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, };

uint8_t rss_sym_key_smallX[8] =
		{ 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, };

uint32_t toeplitz_hash_debug(unsigned keylen, const uint8_t *key,
		unsigned datalen, const uint8_t *data) {
	uint32_t hash = 0, v;
	uint32_t i, b;

	/* XXXRW: Perhaps an assertion about key length vs. data length? */

	v = (key[0] << 24) + (key[1] << 16) + (key[2] << 8) + key[3];
	printf("toeplitz_hash_debug - %08x input\n", *(uint32_t *)data);
	printf("toeplitz_hash_debug - byte_idx bit_idx bit HASH KEY_32B\n");
	for (i = 0; i < datalen; i++) {
		printf("toeplitz_hash_debug - byte %d: %02X\n", i, data[i]);
		for (b = 0; b < 8; b++) {
			printf("toeplitz_hash_debug - %02d %02d %02X %08X %08X %02X ||\t", i,
					b, data[i] & (1 << (7 - b)), hash, v,
					(key[i + 4] & (1 << (7 - b))));
			if (data[i] & (1 << (7 - b))) {
				hash ^= v;
			}
			v <<= 1;
			printf("%02d %02d %02X %08X %08X %02X\n", i,
					b, data[i] & (1 << (7 - b)), hash, v,
					(key[i + 4] & (1 << (7 - b))));
			if ((i + 4) < keylen && (key[i + 4] & (1 << (7 - b)))) {
				v |= 1;
			}
		}
	}
	printf("toeplitz_hash_debug - %08x ret\n", hash);
	return (hash);
}

uint32_t toeplitz_hash(unsigned keylen, const uint8_t *key, unsigned datalen,
		const uint8_t *data) {
	uint32_t hash = 0, v;
	uint32_t i, b;

	/* XXXRW: Perhaps an assertion about key length vs. data length? */

	v = (key[0] << 24) + (key[1] << 16) + (key[2] << 8) + key[3];
	for (i = 0; i < datalen; i++) {
		for (b = 0; b < 8; b++) {
			if (data[i] & (1 << (7 - b)))
				hash ^= v;
			v <<= 1;
			if ((i + 4) < keylen && (key[i + 4] & (1 << (7 - b))))
				v |= 1;
		}
	}
	return (hash);
}

int test1(unsigned char p, int a, int b, short aa, short bb) {

	char data[13];
	int j;
	*(int *) (data) = p;
	*(int *) (data + 1) = a;
	*(short *) (data + 5) = b;
	*(short *) (data + 9) = aa;
	*(short *) (data + 11) = bb;

	j = toeplitz_hash(sizeof(rss_sym_key), rss_sym_key, 13,
			(const uint8_t *) data);
	printf("A - 0x%08X\n", j);

	*(int *) (data) = p;
	*(int *) (data + 1) = a;
	*(short *) (data + 5) = b;
	*(short *) (data + 9) = aa;
	*(short *) (data + 11) = bb;

	j = toeplitz_hash(sizeof(rss_sym_key), rss_sym_key, 13,
			(const uint8_t *) data);
	printf("B - 0x%08X\n", j);

	return 0;
}

int test21(int a, int b, short aa, short bb) {

	char data[12];
	int j;
	*(int *) (data) = a;
	*(int *) (data + 4) = b;
	*(short *) (data + 8) = aa;
	*(short *) (data + 10) = bb;
	j = toeplitz_hash(sizeof(default_rsskey_40bytes), default_rsskey_40bytes,
			12, (const uint8_t *) data);
	printf("default_rsskey_40bytes A - 0x%08X\n", j);

	*(int *) (data) = b;
	*(int *) (data + 4) = a;
	*(short *) (data + 8) = bb;
	*(short *) (data + 10) = aa;
	j = toeplitz_hash(sizeof(default_rsskey_40bytes), default_rsskey_40bytes,
			12, (const uint8_t *) data);
	printf("default_rsskey_40bytes B - 0x%08X\n", j);

	return 0;
}

int test22(int a, int b, short aa, short bb) {

	char data[12];
	int j;
	*(int *) (data) = a;
	*(int *) (data + 4) = b;
	*(short *) (data + 8) = aa;
	*(short *) (data + 10) = bb;
	j = toeplitz_hash(sizeof(rss_sym_key), rss_sym_key, 12,
			(const uint8_t *) data);
	printf("rss_sym_key A - 0x%08X\n", j);

	*(int *) (data) = b;
	*(int *) (data + 4) = a;
	*(short *) (data + 8) = bb;
	*(short *) (data + 10) = aa;
	j = toeplitz_hash(sizeof(rss_sym_key), rss_sym_key, 12,
			(const uint8_t *) data);
	printf("rss_sym_key B - 0x%08X\n", j);

	return 0;
}

int test3(short aa, short bb) {

	char data[4];
	int j;
	*(int *) (data) = aa;
	*(int *) (data + 2) = bb;
	j = toeplitz_hash(sizeof(rss_sym_key_small), rss_sym_key_small, 4,
			(const uint8_t *) data);
	printf("T3A - 0x%08X\n", j);

	*(int *) (data) = bb;
	*(int *) (data + 2) = aa;
	j = toeplitz_hash(sizeof(rss_sym_key_small), rss_sym_key_small, 4,
			(const uint8_t *) data);
	printf("T3B - 0x%08X\n", j);

	return 0;
}

static uint32_t softrss(uint16_t sp, uint16_t dp, uint32_t *rss_key) {
	uint32_t ret = 0;
	rss_key = (uint32_t *) rss_sym_key_small;
	int i;
	printf("softrss raw - 0x%08X\n", (sp << 16) | dp);
	for (i = 0; i < 32; i++) {
		printf("softrss %02d - 0x%08X - 0x%08X - 0x%08X - 0x%08X - 0x%08X\n", i,
				ret, ((sp << 16) | dp) & (1 << (31 - i)), ((*rss_key) << i),
				((*(rss_key + 1)) >> (i + 1)),
				((*rss_key) << i) | ((*(rss_key + 1)) >> (i + 1)));
		if (((sp << 16) | dp) & (1 << (31 - i))) {
			ret ^= ((*rss_key) << i) | ((*(rss_key + 1)) >> (i + 1));

		}
	}
	printf("softrss - 0x%08X\n", ret);
	return ret;
}


int testF1() {
	uint32_t hash, hash1, hash2;
	uint16_t i;
	char data[12];

	uint32_t *sip = (uint32_t *) (data);
	uint32_t *dip = (uint32_t *) (data + 4);
	uint16_t *sport = (uint16_t *) (data + 8);
	uint16_t *dport = (uint16_t *) (data + 10);


	*sip = 0xc0a80103;
	*dip = 0x72f5269e;
	*sport = 0x1F23;
	*dport = 0x1a90;

	toeplitz_hash_debug(sizeof(rss_hack_dport), rss_hack_dport,
			sizeof(data), (uint8_t*)data);

	/* only for 1 to 65534, 0 and 65535 is not used */
	for (i = 1; ;i++) {
		/*
		 * need convert to network order to the hash function
		 */
		*dport = ((i & 0xFF) << 8) + ((i & 0xFF00) >> 8 );
		hash = toeplitz_hash(sizeof(rss_hack_dport), rss_hack_dport,
					sizeof(data), (uint8_t*)data);
		printf("Map - %06d %04x %06d %08x %04x %04x\n", i, *dport,
				hash, hash, (hash >> 4) % 256, hash % 256);
		if(i == 0xFFFF) {
			break;
		}
	}


	/* dest port is 6800 */
	*dport = 0x1a90;
 	hash = toeplitz_hash(sizeof(rss_hack_dport), rss_hack_dport,
				sizeof(data), (uint8_t*)data);
	/* only for 1 to 65534, 0 and 65535 is not used */
	for (i = 1; i < 0xFFFF; i++) {
		/*
		 * need convert to network order to the hash function
		 */
		*sport = ((i & 0xFF) << 8) + ((i & 0xFF00) >> 8 );
		hash1 = toeplitz_hash(sizeof(rss_hack_dport), rss_hack_dport,
					sizeof(data), (uint8_t*)data);
		if(hash1 != hash) {
			printf("Miss_Match_Map - %06d %04x %08x %08x.\n", i, *sport, hash1, hash);
		}
	}

	return 0;
}


int testF2() {
	uint32_t hash, hash1, hash2;
	uint16_t i;
	char data[12];

	uint32_t *sip = (uint32_t *) (data);
	uint32_t *dip = (uint32_t *) (data + 4);
	uint16_t *sport = (uint16_t *) (data + 8);
	uint16_t *dport = (uint16_t *) (data + 10);


	*sip = 0xc0a80100;
	*dip = 0x72f5269e;
	*sport = 0x1F23;
	*dport = 0x1a90;

	toeplitz_hash_debug(sizeof(rss_hack_sip_16), rss_hack_sip_16,
			sizeof(data), (uint8_t*)data);

	/* only for 1 to 65534, 0 and 65535 is not used */
	for (i = 0; i < 16; i++) {
		/*
		 * need convert to network order to the hash function
		 */
		hash = toeplitz_hash(sizeof(rss_hack_sip_16), rss_hack_sip_16,
					sizeof(data), (uint8_t*)data);
		printf("Map - %06d %04x %06d %08x %d\n", i, *sip, hash, hash, hash%16);
		(*sip)++;

	}

	return 0;
}

int main(int argc, char** argv) {
	int i, j = 0;

	uint8_t d1[4] = { 0xde, 0xbe, 0x1a, 0x90, };
	uint8_t d2[4] = { 0x1a, 0x90, 0xde, 0xbe, };

	toeplitz_hash_debug(sizeof(rss_sym_key_small), rss_sym_key_small,
			sizeof(d1), d1);
	toeplitz_hash_debug(sizeof(rss_sym_key_small), rss_sym_key_small,
			sizeof(d2), d2);

	softrss(0xbede, 0x1a90, NULL);
	softrss(0x1a90, 0xbede, NULL);
	test3(0xbede, 0x1a90);

	//test1(0x06, 0x11223344, 0x55667788, 0xAA, 0xBB);
	//test1(0x06, 0xc0a80103, 0x72f5269e, 0xbede, 0x1a90);
	//test21(0x11223344, 0x55667788, 0xAA, 0xBB);
	test21(0xc0a80103, 0x72f5269e, 0xbede, 0x1a90);
	test22(0xc0a80103, 0x72f5269e, 0xbede, 0x1a90);

	testF1();

	/*
	 j = toeplitz_hash(sizeof(default_rsskey_40bytes), default_rsskey_40bytes,
	 12, default_rsskey_40bytes);
	 printf("0x%08X\n", j);

	 j = 0;
	 for (i = 0; i < 65535; i++) {
	 j += toeplitz_hash(sizeof(default_rsskey_40bytes),
	 default_rsskey_40bytes, 12, default_rsskey_40bytes);
	 }
	 printf("0x%08X\n", j);
	 */
	return j;
}
