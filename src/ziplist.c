/* The ziplist is a specially encoded dually linked list that is designed
 * to be very memory efficient. It stores both strings and integer values,
 * where integers are encoded as actual integers instead of a series of
 * characters. It allows push and pop operations on either side of the list
 * in O(1) time. However, because every operation requires a reallocation of
 * the memory used by the ziplist, the actual complexity is related to the
 * amount of memory used by the ziplist.
 *
 * ----------------------------------------------------------------------------
 *
 * ZIPLIST OVERALL LAYOUT:
 * The general layout of the ziplist is as follows:
 * <zlbytes><zltail><zllen><entry><entry><zlend>
 *
 * <zlbytes> is an unsigned integer to hold the number of bytes that the
 * ziplist occupies. This value needs to be stored to be able to resize the
 * entire structure without the need to traverse it first.
 * <zlbytes>��һ��4�ֽڵ��޷������ͣ������洢����ziplistռ�õ��ֽ���������resizeʱʹ��

 * <zltail> is the offset to the last entry in the list. This allows a pop
 * operation on the far side of the list without the need for full traversal.
   <zltail>һ��4�ֽ��޷����������洢ziplist���һ���ڵ��ƫ������
   ziplist��ͷ��ַ+zltail�������һ���ڵ���׵�ַ
 *
 * <zllen> is the number of entries.When this value is larger than 2**16-2,
 * we need to traverse the entire list to know how many items it holds.
   <zllen>��һ��ռ2���ֽڵ��޷����������洢ziplist�еĽڵ���(���ֵΪ2^16 - 2)
   �����ֵ�洢����2�ֽ��޷������͵����ֵʱ����Ҫ���������ȡ����Ľ����
 *
 * <zlend> is a single byte special value, equal to 255, which indicates the
 * end of the list.
   <zlend>һ��ռ1���ֽڵ�ֵ������255����Ϊziplist�Ľ�β��

   <entry>ziplist�еĽڵ�
 *
 * ZIPLIST ENTRIES:
   <��һ���ڵ�ռ�õĳ���><�����뵱ǰ�ڵ�ռ�õĳ���><��ǰ�ڵ�����>
   ��һ��������ռ�õĳ���ռ�õ��ֽ������ݱ������Ͷ���
   ����������С��254ʹ��һ���ֽڴ洢�����ֽڴ洢����ֵ���Ǹó��ȣ�
   ���������ݴ��ڵ���254ʱ��ʹ��5���ֽڴ洢����һ���ֽڵ���ֵΪ254��
   ��ʾ��������4���ֽڲ�������ʾ����
 * Every entry in the ziplist is prefixed by a header that contains two pieces
 * of information. First, the length of the previous entry is stored to be
 * able to traverse the list from back to front. Second, the encoding with an
 * optional string length of the entry itself is stored.
 *
 * The length of the previous entry is encoded in the following way:
 * If this length is smaller than 254 bytes, it will only consume a single
 * byte that takes the length as value. When the length is greater than or
 * equal to 254, it will consume 5 bytes. The first byte is set to 254 to
 * indicate a larger value is following. The remaining 4 bytes take the
 * length of the previous entry as value.
 *
 * The other header field of the entry itself depends on the contents of the
 * entry. When the entry is a string, the first 2 bits of this header will hold
 * the type of encoding used to store the length of the string, followed by the
 * actual length of the string. When the entry is an integer the first 2 bits
 * are both set to 1. The following 2 bits are used to specify what kind of
 * integer will be stored after this header. An overview of the different
 * types and encodings is as follows:
 * ��ǰ�ڵ�ĳ��Ⱥ����ݴ洢
   ��һ���ֽڵ�ǰ��λ�������ֳ��ȴ洢�������ͺ����ݱ������ͣ��������£�
   �ַ������ͱ��룻
 * |00pppppp| - 1 byte
    ����С�ڵ���63�ֽڵ��ַ�������6λ���ڴ洢�ַ�������
 *      String value with length less than or equal to 63 bytes (6 bits).
 * |01pppppp|qqqqqqqq| - 2 bytes
    ����С�ڵ���(2^14 - 1)�ַ�������14λ���ڴ洢�ַ�������
 *      String value with length less than or equal to 16383 bytes (14 bits).
 * |10______|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt| - 5 bytes
    ���ȴ���(2^14 - 1)�ַ������ú�4���ֽڴ洢�ַ�������
 *      String value with length greater than or equal to 16384 bytes.
   ���ͱ��룺
 * |11000000| - 1 byte
 *      Integer encoded as int16_t (2 bytes).
        �������ֽڴ洢��ֵ���Ǹ�����
 * |11010000| - 1 byte
 *      Integer encoded as int32_t (4 bytes).
        ��4���ֽڴ洢��ֵ���Ǹ�����
 * |11100000| - 1 byte
 *      Integer encoded as int64_t (8 bytes).
        ��8���ֽ�
 * |11110000| - 1 byte
        ��3���ֽ�
 *      Integer encoded as 24 bit signed (3 bytes).
 * |11111110| - 1 byte
        ��1���ֽ�
 *      Integer encoded as 8 bit signed (1 byte).
 * |1111xxxx| - (with xxxx between 0000 and 1101) immediate 4 bit integer
 *      Unsigned integer from 0 to 12. The encoded value is actually from
 *      1 to 13 because 0000 and 1111 can not be used, so 1 should be
 *      subtracted from the encoded 4 bit value to obtain the right value.
        (���� 0000 �� 1101 ֮��)�� 4 λ�����������ڱ�ʾ�޷������� 0 �� 12 ��
 *      ��Ϊ 0000 �� 1111 ���Ѿ���ռ�ã���ˣ��ɱ������ֵʵ����ֻ���� 1 �� 13 ��
 *      Ҫ�����ֵ��ȥ 1 �����ܵõ���ȷ��ֵ��
 * |11111111| - End of ziplist.β
 *
 * All the integers are represented in little endian byte order.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "zmalloc.h"
#include "util.h"
#include "ziplist.h"
#include "endianconv.h"
#include "redisassert.h"

#define ZIP_END 255
#define ZIP_BIGLEN 254

/* Different encoding/length possibilities */
#define ZIP_STR_MASK 0xc0 //1100 0000
#define ZIP_INT_MASK 0x30 //0011 0000
#define ZIP_STR_06B (0 << 6) //0000 0000
#define ZIP_STR_14B (1 << 6) //0100 0000
#define ZIP_STR_32B (2 << 6) //1000 0000
#define ZIP_INT_16B (0xc0 | 0<<4) //1100 0000
#define ZIP_INT_32B (0xc0 | 1<<4) //1101 0000
#define ZIP_INT_64B (0xc0 | 2<<4) //1110 0000
#define ZIP_INT_24B (0xc0 | 3<<4) //1111 0000
#define ZIP_INT_8B 0xfe //1111 1110
/* 4 bit integer immediate encoding */
//ֱ�Ӻ�4������Ϊ�洢��ֵ
#define ZIP_INT_IMM_MASK 0x0f //0000 1111 ����
#define ZIP_INT_IMM_MIN 0xf1    /* 11110001 *///��Сֵ
#define ZIP_INT_IMM_MAX 0xfd    /* 11111101 *///���ֵ
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK)

#define INT24_MAX 0x7fffff //3�ֽ����������
#define INT24_MIN (-INT24_MAX - 1)

/* Macro to determine type */
#define ZIP_IS_STR(enc) (((enc) & ZIP_STR_MASK) < ZIP_STR_MASK)

/* Utility macros */
#define ZIPLIST_BYTES(zl)       (*((uint32_t*)(zl))) //�õ�zlbytesֵ
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl)+sizeof(uint32_t))))//�õ�zltail
#define ZIPLIST_LENGTH(zl)      (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))//�õ�zllen
#define ZIPLIST_HEADER_SIZE     (sizeof(uint32_t)*2+sizeof(uint16_t))//�õ�ziplist��header����,����10���ֽ�
#define ZIPLIST_ENTRY_HEAD(zl)  ((zl)+ZIPLIST_HEADER_SIZE)//�õ�ziplist��һ��entry�ĵ�ַ
#define ZIPLIST_ENTRY_TAIL(zl)  ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))//�õ�ziplist���һ��entry�ĵ�ַ
#define ZIPLIST_ENTRY_END(zl)   ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)//����ziplist��ĩ�ˣ���zlend֮ǰ�ĵ�ַ

/**


                                   ziplist���ͷֲ��ṹͼ
area        |<------ziplist header------->|<-----------entries--------------->|<--end-->|

size          4 bytes    4 bytes   2 bytes    ?         ?       ?        ?       1 bytes
            +----------+---------+--------+--------+--------+--------+--------+---------+
component   | zlbytes  |  zltail | zllen  | entry1 | entry2 |  ...   | entryn |  zlend  |
            +----------+---------+--------+--------+--------+--------+--------+---------+
                                                                              ^
address     |<---ZIPLIST_HEADER_SIZE----->|                                   |
                                          ^                          ^  ZIPLIST_ENTRY_END
                                          |                          |
                                  ZIPLIST_ENTRY_HEAD          ZIPLIST_ENTRY_TAIL



                            ziplist entries���ͷֲ��ṹͼ

            +--------------------------+----------------------+-----------------+
component   | prev_entry_bytes_length  |  encoding &  length  |     contents    |
            +--------------------------+----------------------+-----------------+

������
    prev_entry_bytes_length��
        ��ʾ�ϸ��ڵ���ռ���ֽ��������ϸ��ڵ�ĳ���
        �����Ҫ�����ϸ��ڵ㣬����֪����ǰ�ڵ���׵�ַp,�ϸ��ڵ���׵�ַprev = p-prev_entry_bytes_length
        ���ݱ��뷽ʽ�Ĳ�ͬ��prev_entry_bytes_length����ռ1 bytes��5 bytes��
            1 bytes������ϸ��ڵ�ĳ���С��254����ô��ֻ��Ҫ1���ֽ�
            5 bytes������ϸ��ڵ�ĳ��ȴ��ڵ���254����ô�ͽ���һ���ֽ���Ϊ254(1111 1110),Ȼ���������4���ֽڱ���ʵ�ʵĳ���ֵ
    encoding��length��
        ziplist�ı������ͷ�Ϊ�ַ���������
        encoding��ǰ��������λ�����жϱ����������ַ�����������
            00, 01, 10��ʾcontents�б������ַ���
            11��ʾcontents�б���������
        �ַ����ľ�����뷽ʽ��(_:Ԥ����a:ʵ�ʵĶ�������)
             ����                     ���볤��             contents�е�ֵ
           00aaaaaa                    1 bytes         ����[0,63]���ֽڵ��ַ���
        01aaaaaa bbbbbbbb              2 bytes         ����[64,16383]���ֽڵ��ַ���
        01______ aaaaaaaa
        bbbbbbbb cccccccc              5 bytes         ����[16384,2^32-1]���ֽڵ��ַ���
        dddddddd

                                contents�б����ַ���entry�ṹ����

                    +--------------------------+--------------------+-----------------+
        component   | prev_entry_bytes_length  |  encoding & length |     contents    |
                    |       1 or 5 bytes       |   00        001011 |   Hello World   |
                    +--------------------------+--------------------+-----------------+

        11��ͷ�����ͱ��뷽ʽ��
                ����          ���볤��            contents�е�ֵ
              1100 0000        1 bytes            int16_t��������
              1101 0000        1 bytes            int32_t��������
              1110 0000        1 bytes            int64_t��������
              1111 0000        1 bytes            24 bit �з�������
              1111 1110        1 bytes            8 bit �з�������
              1111 xxxx        1 bytes            4 bit �޷�������,[0,12]

                                contents�б�������entry�ṹ����

                    +--------------------------+--------------------+-----------------+
        component   | prev_entry_bytes_length  |  encoding & length |     contents    |
                    |       1 or 5 bytes       |   11        000000 | 10000 (2 bytes) |
                    +--------------------------+--------------------+-----------------+

*/

/* We know a positive increment can only be 1 because entries can only be
 * pushed one at a time. */
//ZIPLIST_LENGTH(zl) �����ֵΪ UINT16_MAX
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX) \
        ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl))+incr); \
}

typedef struct zlentry {
    //ǰһ���ڵ㳤�ȵĴ洢��ռ���ֽ������ϸ��ڵ�ռ�õĳ���
    unsigned int prevrawlensize, prevrawlen;
    //��ǰ�ڵ㳤�ȵĴ洢��ռ���ֽ�������ǰ�ڵ�ռ�õĳ���
    unsigned int lensize,len;
    unsigned int headersize;//��ǰ�ڵ��ͷ����С
    unsigned char encoding;//��ǰ�����㳤�ȣ����ֶ�len��ʹ�õı�������
    unsigned char *p; //ָ��ǰ�����ʼλ�õ�ָ��
} zlentry;

/* Extract the encoding from the byte pointed by 'ptr' and set it into
 * 'encoding'. */
//�ж��Ƿ�Ϊ�ַ�������
#define ZIP_ENTRY_ENCODING(ptr, encoding) do {  \
    (encoding) = (ptr[0]); \
    if ((encoding) < ZIP_STR_MASK) (encoding) &= ZIP_STR_MASK; \
} while(0)

/* Return bytes needed to store integer encoded by 'encoding' */
//���� encoding ָ�����������뷽ʽ����ĳ���
static unsigned int zipIntSize(unsigned char encoding) {
    switch(encoding) {
    case ZIP_INT_8B:  return 1;
    case ZIP_INT_16B: return 2;
    case ZIP_INT_24B: return 3;
    case ZIP_INT_32B: return 4;
    case ZIP_INT_64B: return 8;
    default: return 0; /* 4 bit immediate */
    }
    assert(NULL);
    return 0;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
//�����볤��rawlen����Ҫ���ֽ����洢��P��
//���p==NULL����ô�����ر���rawlen��Ҫ���ֽ���
static unsigned int zipEncodeLength(unsigned char *p, unsigned char encoding, unsigned int rawlen) {
    unsigned char len = 1, buf[5];

    if (ZIP_IS_STR(encoding)) {//�ַ���
        /* Although encoding is given it may not be set for strings,
         * so we determine it here using the raw length. */
        if (rawlen <= 0x3f) { //0011 1111
            if (!p) return len;
            buf[0] = ZIP_STR_06B | rawlen;
        } else if (rawlen <= 0x3fff) {
            len += 1;
            if (!p) return len;
            buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);//��λ
            buf[1] = rawlen & 0xff; //��λ
        } else {
            len += 4;
            if (!p) return len;
            buf[0] = ZIP_STR_32B;
            buf[1] = (rawlen >> 24) & 0xff;
            buf[2] = (rawlen >> 16) & 0xff;
            buf[3] = (rawlen >> 8) & 0xff;
            buf[4] = rawlen & 0xff;
        }
    } else {//����
        /* Implies integer encoding, so length is always 1. */
        if (!p) return len;
        buf[0] = encoding;
    }

    /* Store this length at p */
    memcpy(p,buf,len);
    return len;
}

/* Decode the length encoded in 'ptr'. The 'encoding' variable will hold the
 * entries encoding, the 'lensize' variable will hold the number of bytes
 * required to encode the entries length, and the 'len' variable will hold the
 * entries length. */
//�� ptr ָ����ȡ���ڵ�ı��롢����ڵ㳤������ĳ��ȡ��Լ��ڵ�ĳ���
//�ڵ㱣������ͷ��ַ���������
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len) do {                    \
    ZIP_ENTRY_ENCODING((ptr), (encoding));                                     \
    if ((encoding) < ZIP_STR_MASK) {                                           \
        if ((encoding) == ZIP_STR_06B) {                                       \
            (lensize) = 1;                                                     \
            (len) = (ptr)[0] & 0x3f;                                           \
        } else if ((encoding) == ZIP_STR_14B) {                                \
            (lensize) = 2;                                                     \
            (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];                       \
        } else if (encoding == ZIP_STR_32B) {                                  \
            (lensize) = 5;                                                     \
            (len) = ((ptr)[1] << 24) |                                         \
                    ((ptr)[2] << 16) |                                         \
                    ((ptr)[3] <<  8) |                                         \
                    ((ptr)[4]);                                                \
        } else {                                                               \
            assert(NULL);                                                      \
        }                                                                      \
    } else {                                                                   \
        (lensize) = 1;                                                         \
        (len) = zipIntSize(encoding);                                          \
    }                                                                          \
} while(0);

/* Encode the length of the previous entry and write it to "p". Return the
 * number of bytes needed to encode this length if "p" is NULL. */
//p:��ǰ�ڵ��׵�ַ��len:��һ���ڵ�ĳ���ֵ
//���p==NULL���򷵻ر���len��Ҫ���ٸ��ֽڣ����򽫸�len�洢�ڵ�ǰ�ڵ��prev_entry_bytes_length����
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return (len < ZIP_BIGLEN) ? 1 : sizeof(len)+1;
    } else {
        if (len < ZIP_BIGLEN) {
            p[0] = len;
            return 1;
        } else {
            p[0] = ZIP_BIGLEN;
            memcpy(p+1,&len,sizeof(len));
            memrev32ifbe(p+1);
            return 1+sizeof(len);
        }
    }
}

/* Encode the length of the previous entry and write it to "p". This only
 * uses the larger encoding (required in __ziplistCascadeUpdate). */
//Ϊ�ڵ�ĳ��ȱ���ǿ������
static void zipPrevEncodeLengthForceLarge(unsigned char *p, unsigned int len) {
    if (p == NULL) return;
    p[0] = ZIP_BIGLEN;
    memcpy(p+1,&len,sizeof(len));
    memrev32ifbe(p+1);
}

/* Decode the number of bytes required to store the length of the previous
 * element, from the perspective of the entry pointed to by 'ptr'. */
// ��ָ�� ptr ��ȡ������ǰһ���ڵ�ĳ���������ֽ���
//С��254��1���ֽڣ�������5���ֽ�
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) do {                          \
    if ((ptr)[0] < ZIP_BIGLEN) {                                               \
        (prevlensize) = 1;                                                     \
    } else {                                                                   \
        (prevlensize) = 5;                                                     \
    }                                                                          \
} while(0);

/* Decode the length of the previous element, from the perspective of the entry
 * pointed to by 'ptr'. */
//��ָ�� ptr ��ȡ��ǰһ���ڵ�ĳ���
//�������ǰһ���ڵ㳤��ֻ��Ҫ1���ֽڣ�prevlensize = 1,��ôֱ�ӵõ�ǰһ���ڵ�ĳ���ֵprevlen
//����prevlensize���ĸ��ֽڱ�ʾǰһ���ڵ�ĳ���
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) do {                     \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);                                  \
    if ((prevlensize) == 1) {                                                  \
        (prevlen) = (ptr)[0];                                                  \
    } else if ((prevlensize) == 5) {                                           \
        assert(sizeof((prevlensize)) == 4);                                    \
        memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);                             \
        memrev32ifbe(&prevlen);                                                \
    }                                                                          \
} while(0);

/* Return the difference in number of bytes needed to store the length of the
 * previous element 'len', in the entry pointed to by 'p'. */
//������Ҫ�洢len�����ֽ����뵱ǰ�ڵ�p��prev_entry_bytes_length�Ĳ�ֵ
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
    unsigned int prevlensize;
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);
    return zipPrevEncodeLength(NULL, len) - prevlensize;
}

/* Return the total number of bytes used by the entry pointed to by 'p'. */
//������ڵ���ռ�����ֽ���
static unsigned int zipRawEntryLength(unsigned char *p) {
    unsigned int prevlensize, encoding, lensize, len;
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);//�õ�����ǰһ���ڵ㳤�ȵ��ֽ���
    //�õ��洢��ǰ�ڵ�ĳ��ȵ��ֽ�������ǰ�ڵ����ݵĳ���
    //ע�Ᵽ���ַ���������֮��Ĳ��
    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
    return prevlensize + lensize + len;
}

/* Check if string pointed to by 'entry' can be encoded as an integer.
 * Stores the integer value in 'v' and its encoding in 'encoding'. */
//̽���ַ���ָ��entry�ܷ񱻽��������������Է���1�����ܷ���0
static int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding) {
    long long value;

    if (entrylen >= 32 || entrylen == 0) return 0;
    if (string2ll((char*)entry,entrylen,&value)) {
        /* Great, the string can be encoded. Check what's the smallest
         * of our encoding types that can hold this value. */
        if (value >= 0 && value <= 12) {
            *encoding = ZIP_INT_IMM_MIN+value;
        } else if (value >= INT8_MIN && value <= INT8_MAX) {
            *encoding = ZIP_INT_8B;
        } else if (value >= INT16_MIN && value <= INT16_MAX) {
            *encoding = ZIP_INT_16B;
        } else if (value >= INT24_MIN && value <= INT24_MAX) {
            *encoding = ZIP_INT_24B;
        } else if (value >= INT32_MIN && value <= INT32_MAX) {
            *encoding = ZIP_INT_32B;
        } else {
            *encoding = ZIP_INT_64B;
        }
        *v = value;
        return 1;
    }
    return 0;
}

/* Store integer 'value' at 'p', encoded as 'encoding' */
//��encoding�ı�����ʽ����p�д洢����value
static void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64;
    if (encoding == ZIP_INT_8B) {//1111 1110
        ((int8_t*)p)[0] = (int8_t)value;
    } else if (encoding == ZIP_INT_16B) {//1100 0000
        i16 = value;
        memcpy(p,&i16,sizeof(i16));
        memrev16ifbe(p);
    } else if (encoding == ZIP_INT_24B) {//1111 0000
        i32 = value<<8;
        memrev32ifbe(&i32);
        memcpy(p,((uint8_t*)&i32)+1,sizeof(i32)-sizeof(uint8_t));
    } else if (encoding == ZIP_INT_32B) {
        i32 = value;
        memcpy(p,&i32,sizeof(i32));
        memrev32ifbe(p);
    } else if (encoding == ZIP_INT_64B) {
        i64 = value;
        memcpy(p,&i64,sizeof(i64));
        memrev64ifbe(p);
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        /* Nothing to do, the value is stored in the encoding itself. */
    } else {
        assert(NULL);
    }
}

/* Read integer encoded as 'encoding' from 'p' */
//ͨ�������ı�������encoding�õ�����
static int64_t zipLoadInteger(unsigned char *p, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64, ret = 0;
    if (encoding == ZIP_INT_8B) {
        ret = ((int8_t*)p)[0];
    } else if (encoding == ZIP_INT_16B) {
        memcpy(&i16,p,sizeof(i16));
        memrev16ifbe(&i16);
        ret = i16;
    } else if (encoding == ZIP_INT_32B) {
        memcpy(&i32,p,sizeof(i32));
        memrev32ifbe(&i32);
        ret = i32;
    } else if (encoding == ZIP_INT_24B) {
        i32 = 0;
        memcpy(((uint8_t*)&i32)+1,p,sizeof(i32)-sizeof(uint8_t));
        memrev32ifbe(&i32);
        ret = i32>>8;
    } else if (encoding == ZIP_INT_64B) {
        memcpy(&i64,p,sizeof(i64));
        memrev64ifbe(&i64);
        ret = i64;
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        ret = (encoding & ZIP_INT_IMM_MASK)-1;
    } else {
        assert(NULL);
    }
    return ret;
}

/* Return a struct with all information about an entry. */
//��ָ�� p ����ȡ���ڵ�ĸ������ԣ��������Ա��浽 zlentry �ṹ��Ȼ�󷵻�
static zlentry zipEntry(unsigned char *p) {
    zlentry e;

    ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);
    ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);
    e.headersize = e.prevrawlensize + e.lensize;
    e.p = p;
    return e;
}

/* Create a new empty ziplist. */
//�½�һ��ziplist,
//һ���յ�ziplist����<zlbytes><ztail><zllen><zlend>, size = 4 + 4 + 2 + 1
unsigned char *ziplistNew(void) {
    unsigned int bytes = ZIPLIST_HEADER_SIZE+1;
    unsigned char *zl = zmalloc(bytes);//�����ڴ�
    ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
    ZIPLIST_LENGTH(zl) = 0;
    zl[bytes-1] = ZIP_END;
    return zl;
}

/* Resize the ziplist. */
//Ϊziplist���·����ڴ�
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {
    zl = zrealloc(zl,len);
    ZIPLIST_BYTES(zl) = intrev32ifbe(len);
    zl[len-1] = ZIP_END;
    return zl;
}

/* When an entry is inserted, we need to set the prevlen field of the next
 * entry to equal the length of the inserted entry. It can occur that this
 * length cannot be encoded in 1 byte and the next entry needs to be grow
 * a bit larger to hold the 5-byte encoded prevlen. This can be done for free,
 * because this only happens when an entry is already being inserted (which
 * causes a realloc and memmove). However, encoding the prevlen may require
 * that this entry is grown as well. This effect may cascade throughout
 * the ziplist when there are consecutive entries with a size close to
 * ZIP_BIGLEN, so we need to check that the prevlen can be encoded in every
 * consecutive entry.
 *
 * Note that this effect can also happen in reverse, where the bytes required
 * to encode the prevlen field can shrink. This effect is deliberately ignored,
 * because it can cause a "flapping" effect where a chain prevlen fields is
 * first grown and then shrunk again after consecutive inserts. Rather, the
 * field is allowed to stay larger than necessary, because a large prevlen
 * field implies the ziplist is holding large entries anyway.
 *
 * The pointer "p" points to the first entry that does NOT need to be
 * updated, i.e. consecutive fields MAY need an update. */
 /**
 * ����һ���½ڵ���ӵ�ĳ���ڵ�֮ǰ��ʱ�����ԭ�ڵ��prevlen�����Ա����½ڵ�ĳ��ȣ�
 * ��ô����Ҫ��ԭ�ڵ�Ŀռ������չ���� 1 �ֽ���չ�� 5 �ֽڣ���
 *
 * ���ǣ�����ԭ�ڵ������չ֮��ԭ�ڵ����һ���ڵ�� prevlen ���ܳ��ֿռ䲻�㣬
 * ��������ڶ�������ڵ�ĳ��ȶ��ӽ� ZIP_BIGLEN ʱ���ܷ�����
 *
 * ������������ڴ�������������չ������
 *
 * ��Ϊ�ڵ�ĳ��ȱ�С�������������СҲ�ǿ��ܳ��ֵģ�������Ϊ�˱�����չ-��С-��չ-��С����������������֣�flapping����������
 * ���ǲ���������������������� prevlen ������ĳ��ȸ���
 *
 * ���Ӷȣ�O(N^2)
 *
 * ����ֵ�����º�� ziplist
 * zl: ziplist�׵�ַ��p:��Ҫ��չprevlensize�Ľڵ��׵�ַ
 */
static unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen, rawlensize;
    size_t offset, noffset, extra;
    unsigned char *np;
    zlentry cur, next;

    while (p[0] != ZIP_END) {
        cur = zipEntry(p);
        rawlen = cur.headersize + cur.len; //����entry���ֽ���
        rawlensize = zipPrevEncodeLength(NULL,rawlen); //�洢rawlen��Ҫ���ֽ���

        /* Abort if there is no next entry. */
        if (p[rawlen] == ZIP_END) break;// �Ѿ������β���˳�
        next = zipEntry(p+rawlen);//�õ���һ���ڵ��zlentry

        /* Abort when "prevlen" has not changed. */
        // �����һ��prevlen���ڵ�ǰ�ڵ��rawlen����ô˵�������С����ı䣬�˳�
        if (next.prevrawlen == rawlen) break;

        // ��һ�ڵ�ĳ��ȱ���ռ䲻�㣬������չ
        if (next.prevrawlensize < rawlensize) {
            /* The "prevlen" field of "next" needs more bytes to hold
             * the raw length of "cur". */
            offset = p-zl;
            extra = rawlensize-next.prevrawlensize;//��Ҫ��չ���ֽ���
            zl = ziplistResize(zl,curlen+extra);
            p = zl+offset;

            /* Current pointer and offset for next element. */
            np = p+rawlen;  //�µ���һ���ڵ���׵�ַ
            noffset = np-zl;

            /* Update tail offset when next element is not the tail element. */
            if ((zl+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))) != np) {
                ZIPLIST_TAIL_OFFSET(zl) =
                    intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+extra);
            }

            /* Move the tail to the back. �����ע�Ͳ��Ǻ��������Ҫ�Լ�˼��*/
            //np+rawlensize�µ���һ���ڵ�洢�������ݵ��׵�ַ
            //np+next.prevrawlensize�ɵ���һ���ڵ�洢�������ݵ��׵�ַ
            //���ɵ���һ���ڵ�next����������ziplistβ��ȫ�����ƫ�ƣ������rawlensize���ֽ������洢�ϸ��ڵ�ĳ���
            memmove(np+rawlensize,
                np+next.prevrawlensize,
                curlen-noffset-next.prevrawlensize-1);
            zipPrevEncodeLength(np,rawlen);//�������rawlensize���ֽڴ洢�ϸ��ڵ�ĳ���ֵ

            /* Advance the cursor */
            p += rawlen; //��һ���ڵ�
            curlen += extra; //���µ�ǰziplist�ĳ���
        } else {
            // ��һ�ڵ�ĳ��ȱ���ռ��ж��࣬������������ֻ�ǽ�������ĳ���д��ռ�
            if (next.prevrawlensize > rawlensize) {
                /* This would result in shrinking, which we want to avoid.
                 * So, set "rawlen" in the available bytes. */
                zipPrevEncodeLengthForceLarge(p+rawlen,rawlen);
            } else {
                zipPrevEncodeLength(p+rawlen,rawlen);
            }

            /* Stop here, as the raw length of "next" has not changed. */
            break;//����Ľڵ㲻����չ
        }
    }
    return zl;
}

/* Delete "num" entries, starting at "p". Returns pointer to the ziplist. */
//��ָ�� p ��ʼ��ɾ�� num ���ڵ�
static unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num) {
    unsigned int i, totlen, deleted = 0;
    size_t offset;
    int nextdiff = 0;
    zlentry first, tail;

    first = zipEntry(p); //ɾ�����׸��ڵ�
    for (i = 0; p[0] != ZIP_END && i < num; i++) {
        p += zipRawEntryLength(p); //ƫ�Ƶ��¸��ڵ�
        deleted++;
    }

    totlen = p-first.p;// ��ɾ���Ľڵ�����ֽ���
    if (totlen > 0) {
        if (p[0] != ZIP_END) {
            /* Storing `prevrawlen` in this entry may increase or decrease the
             * number of bytes required compare to the current `prevrawlen`.
             * There always is room to store this, because it was previously
             * stored by an entry that is now being deleted. */
            //����ɾ���ĵ�һ���ڵ�first��prevrawlensize��p�ڵ�prevrawlensize�Ĳ�ֵ
            nextdiff = zipPrevLenByteDiff(p,first.prevrawlen);
            p -= nextdiff; //����nextdiffֵ����p������ǰ�����ƫ�ƣ���ȡ���ֽ�������first.prevrawlen
            zipPrevEncodeLength(p,first.prevrawlen);//��first.prevrawlenֵ�洢��p��prevrawlensize��

            /* Update offset for tail */
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))-totlen);

            /* When the tail contains more than one entry, we need to take
             * "nextdiff" in account as well. Otherwise, a change in the
             * size of prevlen doesn't have an effect on the *tail* offset. */
            //���p����β�ڵ㣬��ôβ�ڵ�ָ����׵�ַ����Ҫ����nextdiff
            tail = zipEntry(p);
            if (p[tail.headersize+tail.len] != ZIP_END) {
                ZIPLIST_TAIL_OFFSET(zl) =
                   intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
            }

            /* Move tail to the front of the ziplist */
            //first.p��p֮��Ľڵ㶼����Ҫɾ���ģ������Ҫ��p��ʼ��������ǰƫ�ƣ�zlend����Ҫ���������Ҫ-1
            memmove(first.p,p,intrev32ifbe(ZIPLIST_BYTES(zl))-(p-zl)-1);
        } else {
            /* The entire tail was deleted. No need to move memory. */
            //����Ѿ�ɾ����zlend����ôβ�ڵ�ָ��Ӧ��ָ��ɾ����first֮ǰ�Ľڵ��׵�ַ
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe((first.p-zl)-first.prevrawlen);
        }

        /* Resize and update length */
        offset = first.p-zl;
        zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl))-totlen+nextdiff);
        ZIPLIST_INCR_LENGTH(zl,-deleted);
        p = zl+offset;

        /* When nextdiff != 0, the raw length of the next entry has changed, so
         * we need to cascade the update throughout the ziplist */
        /**
            ���nextdiff������0��˵�����ڵ�p�ڵ�ĳ��ȱ��ˣ���Ҫ���������¸��ڵ��ܷ񱣴�
            p�ڵ�ĳ���ֵ
        */
        if (nextdiff != 0)
            zl = __ziplistCascadeUpdate(zl,p);
    }
    return zl;
}

/* Insert item at "p". */
//��ӱ������Ԫ��s���½ڵ���뵽��ַp֮ǰ��Ȼ��ԭ�е��������ƫ��
//zl: ziplist�׵�ַ��p:����λ��ָ�룬s:��������ַ����׵�ַ��slen:�������ַ�������
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen, prevlen = 0;
    size_t offset;
    int nextdiff = 0;
    unsigned char encoding = 0;
    long long value = 123456789; /* initialized to avoid warning. Using a value
                                    that is easy to see if for some reason
                                    we use it uninitialized. */
    zlentry entry, tail;

    /* Find out prevlen for the entry that is inserted. */
    // ��ôȡ���ڵ�������ϣ��Լ� prevlen
    if (p[0] != ZIP_END) {//p֮����ڽڵ�
        entry = zipEntry(p);//ȡ��p�ڵ���������
        prevlen = entry.prevrawlen;//�õ�p�ڵ�ǰһ���ڵ���ռ�ֽ���
    } else {//p֮��û�нڵ㣬�ﵽzlend,��ô��ȡ��β�ڵ�
        unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl);
        if (ptail[0] != ZIP_END) {//�������β�ڵ�
            prevlen = zipRawEntryLength(ptail);//�õ�β�ڵ�����ֽ�����Ȼ����β�ڵ�֮��insert
        }//�����Ӧ������һ���յ�ziplist��һ��insertһ���ڵ�
    }

    /* See if the entry can be encoded */
    // �鿴�ܷ���ֵ����Ϊ������������ԵĻ����� 1 ��
    // ������ֵ���浽 value ��������ʽ���浽 encoding
    if (zipTryEncoding(s,slen,&value,&encoding)) {
        /* 'encoding' is set to the appropriate integer encoding */
        //s ���Ա���Ϊ��������ô�������㱣��������Ŀռ�
        reqlen = zipIntSize(encoding);
    } else {
        /* 'encoding' is untouched, however zipEncodeLength will use the
         * string length to figure out how to encode it. */
        // ���ܱ���Ϊ������ֱ��ʹ���ַ�������
        reqlen = slen;
    }
    /* We need space for both the length of the previous entry and
     * the length of the payload. */
    // ������� prevlen ����ĳ���
    reqlen += zipPrevEncodeLength(NULL,prevlen);
    //�������slen����ĳ���
    reqlen += zipEncodeLength(NULL,encoding,slen);

    /* When the insert position is not equal to the tail, we need to
     * make sure that the next entry can hold this entry's length in
     * its prevlen field. */
    //�������λ�ò�Ϊβ��ʱ����Ҫȷ����һ���ڵ�Ĵ洢ǰһ��
    //�ڵ���ռ�Լ����Ŀռ��ܹ��洢��������ڵ�ĳ���
    // zipPrevLenByteDiff �ķ���ֵ�����ֿ��ܣ�
    // 1���¾������ڵ�ı��볤����ȣ����� 0
    // 2���½ڵ���볤�� > �ɽڵ���볤�ȣ����� 5 - 1 = 4
    // 3���ɽڵ���볤�� > �±���ڵ㳤�ȣ����� 1 - 5 = -4
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p,reqlen) : 0;

    /* Store offset because a realloc may change the address of zl. */
    offset = p-zl;//���浱ǰ��ƫ����������ƫ����֮ǰ�����ݲ���Ҫ�ı䣬ֻ��Ҫ�ı��ڴ�֮�������
    // �ط���ռ䣬�����³������Ժͱ�β
    // �¿ռ䳤�� = ���г��� + �½ڵ����賤�� + �����½ڵ㳤������ĳ��Ȳ�
    zl = ziplistResize(zl,curlen+reqlen+nextdiff);
    p = zl+offset;

    /* Apply memory move when necessary and update tail offset. */
    if (p[0] != ZIP_END) {
        /* Subtract one because of the ZIP_END bytes */
        //��ԭ�д�p-nextdiff��ʼȫ�����ƫ�ƣ�������reqlen���漴��insert������
        /**
            nextdiff = -4:ԭ��p��5���ֽ����洢�ϸ��ڵ�ĳ��ȣ�������ֻ��Ҫ1����
                          ���ֻ��Ҫ��p+4������ֽ�ƫ�Ƶ�p+reqlen���ɣ�������
                          ֻ����1���ֽڱ���reqlen�ĳ�����
            nextdiff = 4: ԭ��pֻ��1���ֽ����洢�ϸ��ڵ�ĳ��ȣ�������Ҫ5����
                          �Ǿͽ�p-4������ֽ�ƫ�Ƶ�p+reqlen������pԭ����1���ֽ�
                          ���϶�ƫ������4���ֽھͿ��Ա���reqlen�ĳ�����
            nextdiff = 0: ����Ҫ����
        */
        memmove(p+reqlen,p-nextdiff,curlen-offset-1+nextdiff);

        /* Encode this entry's raw length in the next entry. */
        zipPrevEncodeLength(p+reqlen,reqlen);//�¸��ڵ㱣�漴��insert���ݵ���ռ�ֽ���

        /* Update offset for tail */
        ZIPLIST_TAIL_OFFSET(zl) =
            intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+reqlen);

        /* When the tail contains more than one entry, we need to take
         * "nextdiff" in account as well. Otherwise, a change in the
         * size of prevlen doesn't have an effect on the *tail* offset. */
        // ����Ҫ�Ļ����� nextdiff Ҳ���ϵ� zltail ��
        tail = zipEntry(p+reqlen);
        if (p[reqlen+tail.headersize+tail.len] != ZIP_END) {
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
        }
    } else {
        /* This element will be the new tail. */
        // ���� ziplist �� zltail ���ԣ���������ӽڵ�Ϊ��β�ڵ�
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p-zl);
    }

    /* When nextdiff != 0, the raw length of the next entry has changed, so
     * we need to cascade the update throughout the ziplist */
    /**
        ���nextdiff������0��˵�����ڵ�p+reqlen�ڵ�ĳ��ȱ��ˣ���Ҫ���������¸��ڵ��ܷ񱣴�
        p+reqlen�ڵ�ĳ���ֵ
    */
    if (nextdiff != 0) {
        offset = p-zl;
        zl = __ziplistCascadeUpdate(zl,p+reqlen);
        p = zl+offset;
    }

    /* Write the entry */
    p += zipPrevEncodeLength(p,prevlen);//��д������һ���ڵ㳤�ȵ��ֽ���
    p += zipEncodeLength(p,encoding,slen);//��д���浱ǰ�ڵ㳤�ȵ��ֽ���
    if (ZIP_IS_STR(encoding)) {//���浱ǰ�ڵ���ַ���
        memcpy(p,s,slen);
    } else {
        zipSaveInteger(p,value,encoding);//����
    }
    ZIPLIST_INCR_LENGTH(zl,1);//length + 1
    return zl;
}

//��������ֵs���뵽zl��
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
    unsigned char *p;
    //��ͷ�ڵ����/β���
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
    return __ziplistInsert(zl,p,s,slen);
}

/* Returns an offset to use for iterating with ziplistNext. When the given
 * index is negative, the list is traversed back to front. When the list
 * doesn't contain an element at the provided index, NULL is returned. */
/**
 * ����ָ��ǰ�����ڵ��ָ��
 * ��ƫ��ֵ�Ǹ���ʱ����ʾ�����Ǵӱ�β����ͷ���еġ�
 * ��Ԫ�ر�������ʱ������ NULL
 *
 * ���Ӷȣ�O(N)
 *
 * ����ֵ��ָ��ڵ��ָ��
 */
unsigned char *ziplistIndex(unsigned char *zl, int index) {
    unsigned char *p;
    zlentry entry;
    if (index < 0) {
        index = (-index)-1;
        p = ZIPLIST_ENTRY_TAIL(zl);
        if (p[0] != ZIP_END) {
            entry = zipEntry(p);
            while (entry.prevrawlen > 0 && index--) {
                p -= entry.prevrawlen;
                entry = zipEntry(p);
            }
        }
    } else {
        p = ZIPLIST_ENTRY_HEAD(zl);
        while (p[0] != ZIP_END && index--) {
            p += zipRawEntryLength(p);
        }
    }
    return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

/* Return pointer to next entry in ziplist.
 *
 * zl is the pointer to the ziplist
 * p is the pointer to the current element
 *
 * The element after 'p' is returned, otherwise NULL if we are at the end. */
 /**
 * ����ָ�� p ����һ���ڵ��ָ�룬
 * ��� p �Ѿ������β����ô���� NULL ��
 *
 * ���Ӷȣ�O(1)
 */
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {
    ((void) zl);

    /* "p" could be equal to ZIP_END, caused by ziplistDelete,
     * and we should return NULL. Otherwise, we should return NULL
     * when the *next* element is ZIP_END (there is no next entry). */
    if (p[0] == ZIP_END) {
        return NULL;
    }

    p += zipRawEntryLength(p);
    if (p[0] == ZIP_END) {
        return NULL;
    }

    return p;
}

/* Return pointer to previous entry in ziplist. */
//previous entry
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {
    zlentry entry;

    /* Iterating backwards from ZIP_END should return the tail. When "p" is
     * equal to the first element of the list, we're already at the head,
     * and should return NULL. */
    if (p[0] == ZIP_END) {
        p = ZIPLIST_ENTRY_TAIL(zl);
        return (p[0] == ZIP_END) ? NULL : p;
    } else if (p == ZIPLIST_ENTRY_HEAD(zl)) {
        return NULL;
    } else {
        entry = zipEntry(p);
        assert(entry.prevrawlen > 0);
        return p-entry.prevrawlen;
    }
}

/* Get entry pointed to by 'p' and store in either 'e' or 'v' depending
 * on the encoding of the entry. 'e' is always set to NULL to be able
 * to find out whether the string pointer or the integer value was set.
 * Return 0 if 'p' points to the end of the ziplist, 1 otherwise. */
/**
 * ��ȡ p ��ָ��Ľڵ㣬����������Ա�����ָ��
 *
 * ����ڵ㱣������ַ���ֵ����ô�� sstr ָ��ָ������
 * slen ����Ϊ�ַ����ĳ��ȡ�
 *
 * ����ڵ㱣���������ֵ����ô�� sval ��������
 *
 * p Ϊ��βʱ���� 0 �����򷵻� 1 ��
 *
 * ���Ӷȣ�O(1)
 * ��������ʹ��unsigned char **sstr������ָ�����洢�ַ����׵�ַ�о�����C�ķ�񣬵���C++�ķ��
 * ��ȫ����ֻ��unsigned char *sstr,�Ͼ������洢�Ĳ����ַ�������
 */
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {
    zlentry entry;
    if (p == NULL || p[0] == ZIP_END) return 0;
    if (sstr) *sstr = NULL;

    entry = zipEntry(p);
    if (ZIP_IS_STR(entry.encoding)) {//�ַ�������
        if (sstr) {
            *slen = entry.len;
            *sstr = p+entry.headersize;
        }
    } else {//���ͱ���
        if (sval) {
            *sval = zipLoadInteger(p+entry.headersize,entry.encoding);
        }
    }
    return 1;
}

/* Insert an entry at "p". */
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    return __ziplistInsert(zl,p,s,slen);
}

/* Delete a single entry from the ziplist, pointed to by *p.
 * Also update *p in place, to be able to iterate over the
 * ziplist, while deleting entries. */
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
    size_t offset = *p-zl;
    zl = __ziplistDelete(zl,*p,1);

    /* Store pointer to current element in p, because ziplistDelete will
     * do a realloc which might result in a different "zl"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to ZIP_END, so check this. */
    *p = zl+offset;
    return zl;
}

/* Delete a range of entries from the ziplist. */
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num) {
    unsigned char *p = ziplistIndex(zl,index);
    return (p == NULL) ? zl : __ziplistDelete(zl,p,num);
}

/* Compare entry pointer to by 'p' with 'entry'. Return 1 if equal. */
/**
 * �� p ��ָ��Ľڵ�����Ժ� sstr �Լ� slen ���жԱȣ�
 * �������򷵻� 1 ��
 *
 * ���Ӷȣ�O(N)
 */
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
    zlentry entry;
    unsigned char sencoding;
    long long zval, sval;
    if (p[0] == ZIP_END) return 0;

    entry = zipEntry(p);
    if (ZIP_IS_STR(entry.encoding)) {
        /* Raw compare */
        if (entry.len == slen) {
            return memcmp(p+entry.headersize,sstr,slen) == 0;
        } else {
            return 0;
        }
    } else {
        /* Try to compare encoded values. Don't compare encoding because
         * different implementations may encoded integers differently. */
        if (zipTryEncoding(sstr,slen,&sval,&sencoding)) {
          zval = zipLoadInteger(p+entry.headersize,entry.encoding);
          return zval == sval;
        }
    }
    return 0;
}

/* Find pointer to the entry equal to the specified entry. Skip 'skip' entries
 * between every comparison. Returns NULL when the field could not be found. */
/**
 * ���ݸ����� vstr �� vlen �����Һ����Ժ�������ȵĽڵ�
 * ��ÿ�αȶ�֮�䣬���� skip ���ڵ㡣
 *
 * ���Ӷȣ�O(N)
 * ����ֵ��
 *  ����ʧ�ܷ��� NULL ��
 *  ���ҳɹ�����ָ��Ŀ��ڵ��ָ��
 */
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;
    long long vll = 0;

    while (p[0] != ZIP_END) {
        unsigned int prevlensize, encoding, lensize, len;
        unsigned char *q;

        ZIP_DECODE_PREVLENSIZE(p, prevlensize);
        ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
        q = p + prevlensize + lensize;//�������׵�ַ

        if (skipcnt == 0) {
            /* Compare current entry with specified entry */
            if (ZIP_IS_STR(encoding)) {//�ַ���
                if (len == vlen && memcmp(q, vstr, vlen) == 0) {
                    return p;
                }
            } else {//����
                /* Find out if the searched field can be encoded. Note that
                 * we do it only the first time, once done vencoding is set
                 * to non-zero and vll is set to the integer value. */
                if (vencoding == 0) {
                    // �Դ���ֵ���� decode
                    if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
                        /* If the entry can't be encoded we set it to
                         * UCHAR_MAX so that we don't retry again the next
                         * time. */
                        vencoding = UCHAR_MAX;
                    }
                    /* Must be non-zero by now */
                    assert(vencoding);
                }

                /* Compare current entry with specified entry, do it only
                 * if vencoding != UCHAR_MAX because if there is no encoding
                 * possible for the field it can't be a valid integer. */
                if (vencoding != UCHAR_MAX) {
                    long long ll = zipLoadInteger(q, encoding);//�Ƚ�
                    if (ll == vll) {
                        return p;
                    }
                }
            }

            /* Reset skip count */
            skipcnt = skip;
        } else {
            /* Skip entry */
            skipcnt--;
        }

        /* Move to next entry */
        p = q + len;
    }

    return NULL;
}

/* Return length of ziplist. */
unsigned int ziplistLen(unsigned char *zl) {
    unsigned int len = 0;
    if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) {
        len = intrev16ifbe(ZIPLIST_LENGTH(zl));
    } else {//���ȳ���65534����ô�ͱ���
        unsigned char *p = zl+ZIPLIST_HEADER_SIZE;
        while (*p != ZIP_END) {
            p += zipRawEntryLength(p);
            len++;
        }

        /* Re-store length if small enough */
        if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = intrev16ifbe(len);
    }
    return len;
}

/* Return ziplist blob size in bytes. */
size_t ziplistBlobLen(unsigned char *zl) {
    return intrev32ifbe(ZIPLIST_BYTES(zl));
}

void ziplistRepr(unsigned char *zl) {
    unsigned char *p;
    int index = 0;
    zlentry entry;

    printf(
        "{total bytes %d} "
        "{length %u}\n"
        "{tail offset %u}\n",
        intrev32ifbe(ZIPLIST_BYTES(zl)),
        intrev16ifbe(ZIPLIST_LENGTH(zl)),
        intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
    p = ZIPLIST_ENTRY_HEAD(zl);
    while(*p != ZIP_END) {
        entry = zipEntry(p);
        printf(
            "{"
                "addr 0x%08lx, "
                "index %2d, "
                "offset %5ld, "
                "rl: %5u, "
                "hs %2u, "
                "pl: %5u, "
                "pls: %2u, "
                "payload %5u"
            "} ",
            (long unsigned)p,
            index,
            (unsigned long) (p-zl),
            entry.headersize+entry.len,
            entry.headersize,
            entry.prevrawlen,
            entry.prevrawlensize,
            entry.len);
        p += entry.headersize;
        if (ZIP_IS_STR(entry.encoding)) {
            if (entry.len > 40) {
                if (fwrite(p,40,1,stdout) == 0) perror("fwrite");
                printf("...");
            } else {
                if (entry.len &&
                    fwrite(p,entry.len,1,stdout) == 0) perror("fwrite");
            }
        } else {
            printf("%lld", (long long) zipLoadInteger(p,entry.encoding));
        }
        printf("\n");
        p += entry.len;
        index++;
    }
    printf("{end}\n\n");
}

#ifdef ZIPLIST_TEST_MAIN
#include <sys/time.h>
#include "adlist.h"
#include "sds.h"

#define debug(f, ...) { if (DEBUG) printf(f, __VA_ARGS__); }

unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char*)"1024", 4, ZIPLIST_TAIL);
    return zl;
}

unsigned char *createIntList() {
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

void stress(int pos, int num, int maxsize, int dnum) {
    int i,j,k;
    unsigned char *zl;
    char posstr[2][5] = { "HEAD", "TAIL" };
    long long start;
    for (i = 0; i < maxsize; i+=dnum) {
        zl = ziplistNew();
        for (j = 0; j < i; j++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,ZIPLIST_TAIL);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,pos);
            zl = ziplistDeleteRange(zl,0,1);
        }
        printf("List size: %8d, bytes: %8d, %dx push+pop (%s): %6lld usec\n",
            i,intrev32ifbe(ZIPLIST_BYTES(zl)),num,posstr[pos],usec()-start);
        zfree(zl);
    }
}

void pop(unsigned char *zl, int where) {
    unsigned char *p, *vstr;
    unsigned int vlen;
    long long vlong;

    p = ziplistIndex(zl,where == ZIPLIST_HEAD ? 0 : -1);
    if (ziplistGet(p,&vstr,&vlen,&vlong)) {
        if (where == ZIPLIST_HEAD)
            printf("Pop head: ");
        else
            printf("Pop tail: ");

        if (vstr)
            if (vlen && fwrite(vstr,vlen,1,stdout) == 0) perror("fwrite");
        else
            printf("%lld", vlong);

        printf("\n");
        ziplistDeleteRange(zl,-1,1);
    } else {
        printf("ERROR: Could not pop\n");
        exit(1);
    }
}

int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min+rand()%(max-min+1);
    int minval, maxval;
    switch(rand() % 3) {
    case 0:
        minval = 0;
        maxval = 255;
    break;
    case 1:
        minval = 48;
        maxval = 122;
    break;
    case 2:
        minval = 48;
        maxval = 52;
    break;
    default:
        assert(NULL);
    }

    while(p < len)
        target[p++] = minval+rand()%(maxval-minval+1);
    return len;
}

void verify(unsigned char *zl, zlentry *e) {
    int i;
    int len = ziplistLen(zl);
    zlentry _e;

    for (i = 0; i < len; i++) {
        memset(&e[i], 0, sizeof(zlentry));
        e[i] = zipEntry(ziplistIndex(zl, i));

        memset(&_e, 0, sizeof(zlentry));
        _e = zipEntry(ziplistIndex(zl, -len+i));

        assert(memcmp(&e[i], &_e, sizeof(zlentry)) == 0);
    }
}

int main(int argc, char **argv) {
    unsigned char *zl, *p;
    unsigned char *entry;
    unsigned int elen;
    long long value;

    /* If an argument is given, use it as the random seed. */
    if (argc == 2)
        srand(atoi(argv[1]));

    zl = createIntList();
    ziplistRepr(zl);

    zl = createList();
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_HEAD);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    printf("Get element at index 3:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 3);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index 3\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index 4 (out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Get element at index -1 (last element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -1\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -4 (first element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -4\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -5 (reverse out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -5);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("No entry\n");
        } else {
            printf("ERROR\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front, deleting all items:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            zl = ziplistDelete(zl,&p);
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Delete inclusive range 0,0:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 1);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 2);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 2);
        ziplistRepr(zl);
    }

    printf("Delete with start index out of range:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 5, 1);
        ziplistRepr(zl);
    }

    printf("Delete with num overflow:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 5);
        ziplistRepr(zl);
    }

    printf("Delete foo while iterating:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        while (ziplistGet(p,&entry,&elen,&value)) {
            if (entry && strncmp("foo",(char*)entry,elen) == 0) {
                printf("Delete foo\n");
                zl = ziplistDelete(zl,&p);
            } else {
                printf("Entry: ");
                if (entry) {
                    if (elen && fwrite(entry,elen,1,stdout) == 0)
                        perror("fwrite");
                } else {
                    printf("%lld",value);
                }
                p = ziplistNext(zl,p);
                printf("\n");
            }
        }
        printf("\n");
        ziplistRepr(zl);
    }

    printf("Regression test for >255 byte strings:\n");
    {
        char v1[257],v2[257];
        memset(v1,'x',256);
        memset(v2,'y',256);
        zl = ziplistNew();
        zl = ziplistPush(zl,(unsigned char*)v1,strlen(v1),ZIPLIST_TAIL);
        zl = ziplistPush(zl,(unsigned char*)v2,strlen(v2),ZIPLIST_TAIL);

        /* Pop values again and compare their value. */
        p = ziplistIndex(zl,0);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v1,(char*)entry,elen) == 0);
        p = ziplistIndex(zl,1);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v2,(char*)entry,elen) == 0);
        printf("SUCCESS\n\n");
    }

    printf("Regression test deleting next to last entries:\n");
    {
        char v[3][257];
        zlentry e[3];
        int i;

        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            memset(v[i], 'a' + i, sizeof(v[0]));
        }

        v[0][256] = '\0';
        v[1][  1] = '\0';
        v[2][256] = '\0';

        zl = ziplistNew();
        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            zl = ziplistPush(zl, (unsigned char *) v[i], strlen(v[i]), ZIPLIST_TAIL);
        }

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);
        assert(e[2].prevrawlensize == 1);

        /* Deleting entry 1 will increase `prevrawlensize` for entry 2 */
        unsigned char *p = e[1].p;
        zl = ziplistDelete(zl, &p);

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);

        printf("SUCCESS\n\n");
    }

    printf("Create long list and check indices:\n");
    {
        zl = ziplistNew();
        char buf[32];
        int i,len;
        for (i = 0; i < 1000; i++) {
            len = sprintf(buf,"%d",i);
            zl = ziplistPush(zl,(unsigned char*)buf,len,ZIPLIST_TAIL);
        }
        for (i = 0; i < 1000; i++) {
            p = ziplistIndex(zl,i);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(i == value);

            p = ziplistIndex(zl,-i-1);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(999-i == value);
        }
        printf("SUCCESS\n\n");
    }

    printf("Compare strings with ziplist entries:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        if (!ziplistCompare(p,(unsigned char*)"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl,3);
        if (!ziplistCompare(p,(unsigned char*)"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with random payloads of different encoding:\n");
    {
        int i,j,len,where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        /* Hold temp vars from ziplist */
        unsigned char *sstr;
        unsigned int slen;
        long long sval;

        for (i = 0; i < 20000; i++) {
            zl = ziplistNew();
            ref = listCreate();
            listSetFreeMethod(ref,sdsfree);
            len = rand() % 256;

            /* Create lists */
            for (j = 0; j < len; j++) {
                where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
                if (rand() % 2) {
                    buflen = randstring(buf,1,sizeof(buf)-1);
                } else {
                    switch(rand() % 3) {
                    case 0:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) >> 20);
                        break;
                    case 1:
                        buflen = sprintf(buf,"%lld",(0LL + rand()));
                        break;
                    case 2:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) << 20);
                        break;
                    default:
                        assert(NULL);
                    }
                }

                /* Add to ziplist */
                zl = ziplistPush(zl, (unsigned char*)buf, buflen, where);

                /* Add to reference list */
                if (where == ZIPLIST_HEAD) {
                    listAddNodeHead(ref,sdsnewlen(buf, buflen));
                } else if (where == ZIPLIST_TAIL) {
                    listAddNodeTail(ref,sdsnewlen(buf, buflen));
                } else {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == ziplistLen(zl));
            for (j = 0; j < len; j++) {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. */
                p = ziplistIndex(zl,j);
                refnode = listIndex(ref,j);

                assert(ziplistGet(p,&sstr,&slen,&sval));
                if (sstr == NULL) {
                    buflen = sprintf(buf,"%lld",sval);
                } else {
                    buflen = slen;
                    memcpy(buf,sstr,buflen);
                    buf[buflen] = '\0';
                }
                assert(memcmp(buf,listNodeValue(refnode),buflen) == 0);
            }
            zfree(zl);
            listRelease(ref);
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with variable ziplist size:\n");
    {
        stress(ZIPLIST_HEAD,100000,16384,256);
        stress(ZIPLIST_TAIL,100000,16384,256);
    }

    return 0;
}

#endif
