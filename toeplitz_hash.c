/*
 * toeplitzHash.c
 *
 *  Created on: Apr 9, 2018
 *      Author: shine
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Mellanox Linux's driver key
uint8_t default_rsskey_40bytes[40] = {
    0xd1, 0x81, 0xc6, 0x2c, 0xf7, 0xf4, 0xdb, 0x5b,
    0x19, 0x83, 0xa2, 0xfc, 0x94, 0x3e, 0x1a, 0xdb,
    0xd9, 0x38, 0x9e, 0x6b, 0xd1, 0x03, 0x9c, 0x2c,
    0xa7, 0x44, 0x99, 0xad, 0x59, 0x3d, 0x56, 0xd9,
    0xf3, 0x25, 0x3c, 0x06, 0x2a, 0xdc, 0x1f, 0xfc
};

uint32_t toeplitz_hash(unsigned keylen, const uint8_t *key,
    unsigned datalen, const uint8_t *data)
{
    uint32_t hash = 0, v;
    uint32_t i, b;

    /* XXXRW: Perhaps an assertion about key length vs. data length? */

    v = (key[0]<<24) + (key[1]<<16) + (key[2] <<8) + key[3];
    for (i = 0; i < datalen; i++) {
        for (b = 0; b < 8; b++) {
            if (data[i] & (1<<(7-b)))
                hash ^= v;
            v <<= 1;
            if ((i + 4) < keylen &&
                (key[i+4] & (1<<(7-b))))
                v |= 1;
        }
    }
    return (hash);
}


int test(int a, int b, short aa, short bb)
{

	char data[12];
	int j;
	*(int *)(data) = a;
	*(int *)(data + 4) = b;
	*(short *)(data + 8) = aa;
	*(short *)(data + 10) = bb;
	j = toeplitz_hash(sizeof(default_rsskey_40bytes), default_rsskey_40bytes, 12, (const uint8_t *) data);
	printf("0x%08X\n", j);

	*(int *)(data) = b;
	*(int *)(data + 4) = a;
	*(short *)(data + 8) = bb;
	*(short *)(data + 10) = aa;
	j = toeplitz_hash(sizeof(default_rsskey_40bytes), default_rsskey_40bytes, 12, (const uint8_t *) data);
	printf("0x%08X\n", j);
}

int main(int argc, char** argv)
{
	int i, j = 0;
	test(0x11223344, 0x55667788, 0xAA, 0xBB);


	j = toeplitz_hash(sizeof(default_rsskey_40bytes), default_rsskey_40bytes,
				12, default_rsskey_40bytes);
	printf("0x%08X\n", j);
	for (i = 0; i < 1000000; i++) {
		 j += toeplitz_hash(sizeof(default_rsskey_40bytes), default_rsskey_40bytes,
				12, default_rsskey_40bytes);
	}
	printf("0x%08X\n", j);
	return j;
}
