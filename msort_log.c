/*
 * msort_log.c
 *
 * Pratap Pereira, March 2016
 *
 * Copyright (c) 2016 by cisco Systems, Inc.
 * All rights reserved
 */

#define BTRACE_APP_MACROS
#define _GNU_SOURCE 1
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <inttypes.h>
#include <time.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#ifdef ZLIB_SUPPORTED
#include <zlib.h>
#endif
#include <bsdlib/queue_macros.h>

#include "priv_hash.h"

#ifndef STANDALONE
#include <binos/berror.h>
#include <binos/common.h>
#include <btrace/btrace.h>
#else 
#define __UNUSED __attribute__((unused))
#define TRUE 1
#define FALSE 0
#define EOK 0
typedef int boolean;
typedef int bstatus_t;
#endif

/* START_EXPANSION */ 

#define MAX_FILES 8192
#define MAX_FILENAME_SIZE 256
#define BASE_SIZE (8L * 1024 * 1024)
#define GROWTH_SIZE (4L * 1024 * 1024)
#define MAX_COMPRESSION_RATIO 16L
#define TS_LEN 18 /* MM/DD HH:MM:SS.XXX */
#define UUID_STR_LEN 64

/*
 *
 * Incremental hash function modified from 
 * 
 * http://www.burtleburtle.net/bob/hash/evahash.html
 *
 */
static u_int32_t 
msort_jenkins_hash (void *ptr, size_t key_len, u_int32_t curr) 
{
    u_int32_t hash = curr;
    size_t i;
    caddr_t key = ptr;
    
    for (i = 0; i < key_len; i++) {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }   
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

#define HASH_FUNC msort_jenkins_hash

/* 
 * return a value to a power 2 larger than specified
 *
 * public domain code from
 * http://graphics.stanford.edu/~seander/bithacks.html
 */
static u_int32_t 
hash_pow2_larger (u_int32_t x) 
{

        x |= (x >> 1);
        x |= (x >> 2);
        x |= (x >> 4);
        x |= (x >> 8);
        x |= (x >> 16);

        return (x+1);
}

static u_int32_t 
hash_log2 (u_int32_t v)
{
    u_int32_t r;

    static const int MultiplyDeBruijnBitPosition[32] = 
        {
            0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8, 
            31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
        };

    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v = (v >> 1) + 1;

    r = MultiplyDeBruijnBitPosition[((v * 0x077CB531UL) & 
        (0x00000000FFFFFFFFUL)) >> 27];
    
    return (r);
}




/*
 * A hash compare function pointer definition
 */
typedef int (*binheap_compare_t)(void *node_one, void *node_two);

/*
 * The base type for the binary heap
 *
 * Used to reference any binary heap generically.
 * specific heaps will have correctly sized arrays
 *
 * We are allocating bin heaps separate from the 
 * hash table deliberately, since the hash table is
 * used in places where the ordered traversal is not
 * needed.
 */

#define VS_HASH_BINHEAP_BASE            \
    binheap_compare_t compare_fn;       \
    u_int32_t capacity;                 \
    u_int32_t size;                     \
    
typedef struct binheap_s {
    VS_HASH_BINHEAP_BASE
    void *array[MAX_FILES];
} binheap_t;

binheap_t filenode_binheap;

struct uuid_entry_s {
    HASH_ENTRY(uuid_entry_s) uuid_hash_entry;
                 char uuid[UUID_STR_LEN];
                 size_t uuid_len;
};

#define uuid_NODE_OFFSET offsetof(struct uuid_entry_s, uuid_hash_entry)

typedef struct uuid_entry_s uuid_entry_t;

HASHTABLE_STRUCT(uuid_hash , uuid_entry_s)

HASH_HEAD(uuid_hash , uuid_entry_s) uuid_hash_head;

HASH_PROTOTYPE(uuid_hash, uuid_entry_s, uuid_hash_entry, uuid_hash_compare, 
    uuid_hash_internal)

static u_int32_t 
uuid_hash_internal (void *node) {
    u_int32_t hash = 13;
    if (node == NULL) return 0;

    uuid_entry_t *a = (uuid_entry_t *)((caddr_t *) node - uuid_NODE_OFFSET);
    hash = HASH_FUNC(a->uuid, a->uuid_len, hash);
    return hash;
}

static int
uuid_hash_compare (uuid_entry_t *a, uuid_entry_t *b) {
    if (a && b) {
        size_t s = (b->uuid_len < a->uuid_len) ? b->uuid_len : a->uuid_len;
        return strncmp(a->uuid, b->uuid, s);
    }
    /* one of a/b or both are NULL */
    if (a) {
        return 1;
    }
    if (b) {
        return -1;
    }
    /* both NULL */
    return 0;
}

HASH_GENERATE(uuid_hash, uuid_entry_s, uuid_hash_entry, 
          uuid_hash_compare, uuid_hash_internal)


struct output_file_state_s {
    char *filename;
    int data_fd;
    char *base_ptr;
    size_t write_offset;
    size_t size;
};

typedef struct output_file_state_s output_file_state_t;

struct file_state_s {
    char *basename;
    size_t basename_len;
    char *filename;
    int data_fd;
    char *base_ptr;
    char *start_ts_ptr;
    char *end_ts_ptr;
    char *start_payload;
    char *end_payload;
    size_t written_size;
    size_t size;
#ifdef ZLIB_SUPPORTED
    char *zlib_ptr;
    size_t zlib_size;
#endif
    int used;
    int eof;
};

typedef struct file_state_s file_state_t;

static int filenode_binheap_compare (void *node_one, void *node_two) {
    file_state_t *f1 = node_one;
    file_state_t *f2 = node_two;
    int ret;
    ret = strncmp(f1->start_payload, f2->start_payload, TS_LEN);
    return ret;
}


static int next_file_state = 0;
static file_state_t file_states[MAX_FILES];

/* command line option flags */
static int addfilename = 0;
static int emit_line_always = 1;
static char *mac_address_filter = NULL;

#define BINHEAP_LT(a, b)                        \
    ((binheap->compare_fn((a), (b)) <= -1 ? 1 : 0))


static void binheap_init(binheap_t *binheap, binheap_compare_t comp_func) {
    memset(binheap, 0, sizeof(binheap_t));
    binheap->compare_fn = comp_func;
    binheap->capacity = MAX_FILES;
}

static void 
binheap_sift_down (binheap_t *binheap, 
           u_int32_t ndx) 
{
    u_int32_t child;
    void *tmp = binheap->array[ndx];

    for( ; ndx * 2 <= binheap->size; ndx = child) {
    child = ndx * 2;
        if( child != binheap->size 
        && BINHEAP_LT(binheap->array[child + 1], 
              binheap->array[child])) {
        child++;
    }
    if( BINHEAP_LT(binheap->array[child],  tmp)) {
        binheap->array[ndx] = binheap->array[child];
    } else {
        break;
    }
    }

    binheap->array[ndx] = tmp;
}

static bstatus_t 
insert_binheap (binheap_t *binheap, void *item)
{
    if ( binheap->size >= binheap->capacity) {
    return ENOSPC;
    }

    /* sift up */
    int ndx = ++binheap->size;
    for( ; ndx > 1 
         && BINHEAP_LT(item , binheap->array[ndx/2]); 
     ndx /= 2) {
    binheap->array[ndx] = binheap->array[ndx/2];
    }
    binheap->array[ndx] = item;
    return (EOK);
}

static bstatus_t 
pop_binheap (binheap_t *binheap, void **item) 
{
    if( binheap->size == 0) {
    return ENOENT;
    }
    *item = binheap->array[1];
    binheap->array[1] = binheap->array[binheap->size--];
    binheap_sift_down( binheap, 1);
    return (EOK);
}

static bstatus_t 
add_array_binheap (binheap_t *binheap, void *item) 
{
    if ( binheap->size >= binheap->capacity) {
    return ENOSPC;
    }
    
    int ndx = ++binheap->size;
    binheap->array[ndx] = item;
    return (EOK);
}

static void 
heapify_binheap (binheap_t *binheap) 
{
    int i;
    for( i = binheap->size/2; i > 0; i--) {
    binheap_sift_down(binheap,  i);
    }
}

static output_file_state_t * open_output_file (char *name) {
    output_file_state_t *fs = malloc(sizeof(output_file_state_t));
    memset(fs, 0, sizeof(output_file_state_t));
    fs->filename = strdup(name);
    fs->data_fd = open(fs->filename, O_RDWR|O_CREAT|O_TRUNC, 0777);
    if (fs->data_fd < 0) {
        printf("could not open file %s\n", fs->filename);
        perror("file open error");
        exit(1);
    }
    if (ftruncate(fs->data_fd, BASE_SIZE) == 0) {
        fs->base_ptr = mmap(0, BASE_SIZE,
                PROT_WRITE, 
                MAP_SHARED,
                fs->data_fd, 0);
        if (fs->base_ptr ==  MAP_FAILED) {
            printf("mmap of file %s of size %ld failed", fs->filename, 
                BASE_SIZE);
            perror("mmap failed");
            exit(1);
        }
    } else {
        printf("ftruncate failed to initial size\n");
        perror("ftruncate failed");
        exit(1);
    }
    fs->size = BASE_SIZE;
    fs->write_offset = 0;
    return fs;
}

static void write_output_file (output_file_state_t *fs, char *data, 
    size_t size) {
    if ((fs->base_ptr + fs->write_offset + size) > (fs->base_ptr + fs->size)) {
    /* grow_output_file */
        if (ftruncate(fs->data_fd, fs->size + GROWTH_SIZE) == 0) {
            munmap(fs->base_ptr, fs->size);
            fs->base_ptr = mmap(0, fs->size + GROWTH_SIZE, PROT_WRITE, 
                MAP_SHARED, fs->data_fd, 0);
            if (fs->base_ptr ==  MAP_FAILED) {
                printf("mmap grow operation failed! - %s", strerror(errno));
                exit(4);
            } else {
                fs->size = fs->size + GROWTH_SIZE;
            }   
        } else {
            printf("ftruncate failed!");
        }
    }
    memcpy(fs->base_ptr + fs->write_offset, data, size);
    fs->write_offset = fs->write_offset + size;
}

static void close_output_file (output_file_state_t *fs) {
    if ((fs->base_ptr + fs->write_offset) < (fs->base_ptr + fs->size)) {
        /* our file is longer than needed, unmap and ftruncate to correct size */
        if (ftruncate(fs->data_fd, fs->write_offset) != 0) {
            printf("ftruncate on close failed:");
            exit(5);
        }
    }
    munmap(fs->base_ptr, fs->size);
    free(fs);
}   

/* Decompress from file source to file dest until stream ends or EOF.
   inf() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_DATA_ERROR if the deflate data is
   invalid or incomplete, Z_VERSION_ERROR if the version of zlib.h and
   the version of the library linked do not match, or Z_ERRNO if there
   is an error reading or writing the files. */

#ifdef ZLIB_SUPPORTED
static int zlib_inflate(file_state_t *fs)
{
    int ret;
    z_stream strm;

    /* allocate inflate state */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, 16+15);
    if (ret != Z_OK)
        return ret;

    /* decompress until deflate stream ends or end of file */
    do {
        strm.avail_in = fs->zlib_size;
        if (strm.avail_in == 0)
            break;
        strm.next_in = fs->zlib_ptr;

        /* run inflate() on input until output buffer not full */
        do {
            strm.avail_out = fs->size;
            strm.next_out = fs->base_ptr;
            ret = inflate(&strm, Z_NO_FLUSH);
            assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
            switch (ret) {
            case Z_NEED_DICT:
                ret = Z_DATA_ERROR;     /* and fall through */
            case Z_DATA_ERROR:
            case Z_MEM_ERROR:
                (void)inflateEnd(&strm);
                return ret;
        default:
        return ret;
            }
            fs->written_size = fs->size - strm.avail_out;       
        } while (strm.avail_out == 0);

        /* done when inflate() says it's done */
    } while (ret != Z_STREAM_END);

    /* clean up and return */
    (void)inflateEnd(&strm);
    return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

/* report a zlib or i/o error */
static void zerr(int ret)
{
    fputs("zpipe: ", stderr);
    switch (ret) {
    case Z_ERRNO:
        if (ferror(stdin))
            fputs("error reading stdin\n", stderr);
        if (ferror(stdout))
            fputs("error writing stdout\n", stderr);
        break;
    case Z_STREAM_ERROR:
        fputs("invalid compression level\n", stderr);
        break;
    case Z_DATA_ERROR:
        fputs("invalid or incomplete deflate data\n", stderr);
        break;
    case Z_MEM_ERROR:
        fputs("out of memory\n", stderr);
        break;
    case Z_VERSION_ERROR:
        fputs("zlib version mismatch!\n", stderr);
    break;
    default:
    fputs("unknown error!\n", stderr);
    break;
    }
}
#endif

static void open_file (file_state_t *fs) {
    struct stat fbuf;
    fs->data_fd = open(fs->filename, O_RDONLY, 0777);
    if (fs->data_fd < 0) {
        printf("could not open file %s\n", fs->filename);
        perror("file open error");
        exit(1);
    }
    if (fstat(fs->data_fd, &fbuf)) {
        printf("could not stat file %s\n", fs->filename);
        perror("file stat error");
        exit(1);
    }
    fs->base_ptr = mmap(0, fbuf.st_size,
             PROT_READ, 
             MAP_SHARED,
             fs->data_fd, 0);
    if (fs->base_ptr ==  MAP_FAILED) {
        printf("mmap of file %s of size %ld failed", fs->filename, 
            fbuf.st_size);
        perror("mmap failed");
        //exit(1);
    }
    fs->size = fbuf.st_size;
    fs->written_size = fs->size;
    /* check if it is a zlib compressed file  */
#ifdef ZLIB_SUPPORTED
    if ((*fs->base_ptr == 0x1f) && (*((unsigned char *)fs->base_ptr + 1) 
        == 0x8b)) {
    fs->zlib_ptr = fs->base_ptr;
    fs->zlib_size = fs->size;
    fs->base_ptr = mmap(0, fs->size * MAX_COMPRESSION_RATIO, PROT_WRITE, 
                MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
    if (fs->base_ptr == MAP_FAILED) {
        printf("mmap of uncompressed buffer %s of size %ld failed", 
           fs->filename, fs->size * MAX_COMPRESSION_RATIO);
        perror("mmap failed");
        exit(1);
    }
    fs->size = fs->size * MAX_COMPRESSION_RATIO;
    int ze = zlib_inflate(fs);
    if (ze != 0) {
        zerr(ze);
        exit(1);
    }
    }
#endif
}

static void close_file (file_state_t *fs) {
    munmap(fs->base_ptr, fs->size);
    fs->base_ptr = NULL;
    fs->size = 0;
    close(fs->data_fd);
    fs->data_fd = 0;
    fs->used = 0;
    free(fs->basename);
}

/*
 * Find the needle in the haystack routine, implemented from CLRS - KMP
 */

static char * KMP (char* needle, size_t nsize, char* haystack, size_t hsize, 
    int* matched) {
    *matched = 0;
    char *cp = haystack;
    if (!nsize) {
        *matched = 1;
        return cp;
    } else if (nsize > hsize) {
        return cp;
    }
    size_t i = 0;
    int pi[nsize];
    pi[0] = 0;
    size_t q = 0;
    int k = 0;
    for (i = 1; i < nsize; i++) {
        while (k > 0 && needle[k] != needle[i]) {
            k = pi[k-1];
        }
        if (needle[k] == needle[i]) {
            k = k + 1;
        }
        pi[i] = k;
    }
    for (i = 0; i < hsize; i++) {
        while (q > 0 && needle[q] != haystack[i]) {
            q = pi[q-1];
        }
        if (needle[q] == haystack[i]) {
            q = q + 1;
        }
        if (q == nsize) {
            *matched = 1;
            return (cp + i - nsize + 1);
        }
    }
    return cp;
}

static char * uuid_val_matcher (char *start, size_t size, size_t *offset) {
    int matched = 0;
    char uuid[] = "UUID: ";
    int uuid_len = 6;
    char *cp = KMP(uuid, uuid_len, start, size, &matched);
    if (matched) {
        cp = cp + uuid_len;
        *offset = 0;
        char* ptr = cp;
        while (*ptr != ',') {
            ptr++;
            (*offset)++;
        }
        return cp;
    }
    return NULL;
}

static void appctx_matcher (char *start, size_t size, int *matched) {
    *matched = 0;
    char appctx[] = "(appctx):";
    int appctx_len = 9;
    KMP(appctx, appctx_len, start, size, matched);
}

/*
 * Mac address matcher
 */

#define IS_VALID_CHAR(s) (( (*s) >= '0' && (*s) <= '9') \
    || (( (*s) >= 'a' && (*s) <= 'f')) || (( (*s) >= 'A' && (*s) <= 'F')))

/* Match one character portion of a valid sextet */
static int match_one (const char *s, int last) {
    if (IS_VALID_CHAR(s)) {
    s++;
    if (!last && *s == ':') {
        return 2;
    }
    if (last && *s == ' ') {
        return 1;
    }
    }
    return 0;
}

/* Match two characters one portion of a valid sextet */
static int match_two (const char *s, int last) {
    if (IS_VALID_CHAR(s)) {
    s++;
    if (IS_VALID_CHAR(s)) {
        s++;
        if (!last && *s == ':') {
        return 3;
        }
        if (last && *s == ' ') {
        return 2;
        }
    }
    }
    return 0;
}

static int 
merge_strncmp (const char *needle, const char *candidate) {
    //Attempt complete match on well formed mac
    int i;
        int a;
    if (candidate && needle) {
        a  = strncmp(candidate, needle, strlen(needle));    
    } else {
      return 1;
    }
    if (!a) {
    return 0;
    }
    //Probably not well formed candidate, make it well formed
    char norm[18];
    for (i =0; i < 6; i++) {
    //candidate mac special casing when last token delimiting character is ' '
    if (i == 5) {
        if (*(candidate+1) == ' ') {
        norm[(3*i)]='0';
        norm[(3*i)+1]=*candidate;
        } else {
        norm[(3*i)] = *candidate;
        norm[(3*i)+1] = *(candidate+1);
        norm[(3*i)+2]='\0';
        }
        break;
    }
    //candidate mac special casing when token delimiting character is ':'
    if (*(candidate+1) == ':') {
        norm[(3*i)]='0';
        norm[(3*i)+1]=*candidate;
        candidate += 2;
    } else {
        norm[(3*i)] = *candidate;
        norm[(3*i)+1] = *(candidate+1);
        candidate += 3;
    }
    if (i != 5) {
        norm[(3*i)+2]=':';
    }
    }
    // Attempt full match now
    a = strncmp(norm, needle, 16);
    if (!a) {
    return 0;
    } else {
    // no match, keep strcmp semantics
    return 1;
    }
}

static int find_one_or_two_byte_mac (const char *s, int toks_matched, 
    int *skip, const char *needle) {
    int last = 0;
    if (toks_matched == 6) {
    return 1;
    }
    if (toks_matched == 5) {
    last = 1;
    }
    if (match_two(s, last)) {
    *skip += 3;
    return find_one_or_two_byte_mac(s+3, ++toks_matched, skip, needle);
    }
    if (match_one(s, last)) {
    *skip += 2;
    return find_one_or_two_byte_mac(s+2, ++toks_matched, skip, needle);
    }
    return 0;
}

#define MAX_WORD_LENGTH 256

// If mac, return mac size, else 0
static int match_mac (const char *start, const char *end, const char *needle) 
{ 
    int num = 0;
    char word[MAX_WORD_LENGTH] = {'\0'};
    int  i =0;
    
    while (start < end-1) {
        i = 0;
        while (*start != ' ') {
            word[i] = *start;
            i++;start++;
        }
        word[i] = ' ';
        start++;
        if (find_one_or_two_byte_mac(word, 0, &num, needle)) {
            if (merge_strncmp(needle, word) != 0) {
                return 0;
            } else {
                return 1;
            }
        }
    }
    return 0;
}

#if MACMATCH

#define START_SEEK 0
#define DIGIT_SEEK 1
#define DIGIT_SEEK2 2
#define DELIMITER_SEEK 3
#define NEXT_OCTET 4
#define NO_MAC 5

static char * find_mac (char *start, size_t size, int *matched) {
    write(1, start, size);
    char *end_ptr = start + size;
    *matched = 0;
    static char outmac[32];
    memset(outmac, 0, 32);
    char *om = outmac;
    char *oe = outmac + 31;
    char *cp = start;
    int octet_count = 0;
    int cst = START_SEEK;
    int ncst = START_SEEK;
    while (cp < end_ptr) {
    printf("current state %d, %p, %s\n", cst, cp, outmac);
    switch (cst) {
    case START_SEEK:
        om = outmac;
        octet_count = 0;
        memset(outmac, 0, 32);
        if (isxdigit(*cp)) {
        ncst = DIGIT_SEEK;
        } else {
        ncst = NO_MAC;
        }
        break;
    case DIGIT_SEEK:
        if (isxdigit(*cp)) {
        ncst = DIGIT_SEEK2;
        if (om < oe) {
            *om = *cp;
            om++;
        }
        cp++;
        } else {
        ncst = NO_MAC;
        }
        break;
    case DIGIT_SEEK2:
        if (isxdigit(*cp)) {
        ncst = DELIMITER_SEEK;
        if (om < oe) {
            *om = *cp;
            om++;
        }
        cp++;
        } else if (*cp == ':') {
        /* found a non-normalized digit make prefix space */
        ncst = NEXT_OCTET;
        if (om < oe) {
            *om = *(om - 1);
            *(om - 1) = '0';
            om++;
            *om = ':';
            om++;
        }
        cp++;
        } else if (octet_count == 5) {
        if (om < oe) {
            *om = *(om - 1);
            *(om - 1) = '0';
            om++;
        }
        ncst = NEXT_OCTET;
        } else {
        ncst = NO_MAC;
        }
        break;
    case DELIMITER_SEEK:
        if (*cp == ':') {
        ncst = NEXT_OCTET;
        if (om < oe) {
            *om = *cp;
            om++;
        }
        cp++;
        } else {
        ncst = NO_MAC;
        }
        break;
    case NEXT_OCTET:
        octet_count++;
        if (octet_count > 5) {
        if (om < oe) {
            *om = '\0';
        }
        printf("norm mac = %s\n", om);
        *matched = 1;
        return outmac;
        }
        ncst = DIGIT_SEEK;
        break;
    case NO_MAC:
        ncst = START_SEEK;
        printf("partial match %s\n", outmac);
        cp++;
        break;
    default:
        printf("unknown state %d\n", cst);
        break;
    }
    cst = ncst;
    }
    return NULL;
}    

static char * match_mac (char *start, char *end, char *needle) {    
    int match = 0;
    char *mymac = find_mac(start, end - start, &match);
    printf("out match In match mac: mymac %s, needle %s\n", mymac, needle);
    int scmp = 0;
    if (match) {
    printf("in match In match mac: mymac %s, needle %s\n", mymac, needle);
    scmp = (strcmp(mymac, needle) == 0)? 1 : 0;
    }
    if (scmp) {
    return mymac;
    }
    return NULL;
}

#endif //end alternate MACMATCH

static char * date_time_matcher (char *start, size_t size, int *matched) {
    size_t total_len = size - TS_LEN;
    *matched = 0;
    char *cp = start;
    if (size <= 2*TS_LEN) {
    return cp;
    }
    while (total_len > 0) {
    if (isdigit(*cp)) { /* Month */
        if (isdigit(*(cp + 1))) {
        if (*(cp + 2) == '/') {
            if (isdigit(*(cp + 3))) { /* Day */
            if (isdigit(*(cp + 4))) {
                if (*(cp + 5) == ' ') {
                if (isdigit(*(cp + 6))) { /* Hour */
                    if (isdigit(*(cp + 7))) {
                    if (*(cp + 8) == ':') {
                        if (isdigit(*(cp + 9))) { /*Minute */
                        if (isdigit(*(cp + 10))) {
                            if (*(cp + 11) == ':') {
                            if (isdigit(*(cp + 12))) { /*Second */
                                if (isdigit(*(cp + 13))) {
                                if (*(cp + 14) == '.') {
                                    if (isdigit(*(cp + 15))) { /*microsecond */
                                    if (isdigit(*(cp + 16))) {
                                        if (isdigit(*(cp + 17))) {
                                        if (*(cp + 18) == ' ') { /*space */
                                            *matched = 1;
                                            break;
                                        }
                                        }
                                    }
                                    }
                                }
                                }
                            }
                            }
                        }
                        }
                    }
                    }
                }
                }
            }
            }
        }
        }
    }
    cp++;
    total_len--;
    }
    return cp;
}

/*
 * The initial file offset setting is to skip over parts of the file until the
  first timestamp located 
 */
static void initial_file_offset (file_state_t *fs) {
    int match = 0;
    char *start_ts = date_time_matcher(fs->base_ptr, fs->written_size, &match);
    if (match == 0) {
        fs->eof = 1;
    } else {
        fs->start_ts_ptr = start_ts;
        fs->end_ts_ptr = start_ts + TS_LEN;
    }
    fs->start_payload = fs->start_ts_ptr;
    fs->end_payload = fs->start_ts_ptr;
}

static int next_file_offset (file_state_t *fs) {
    int match = 0;
    char *start_ts = date_time_matcher(fs->end_ts_ptr, fs->written_size 
                       - (fs->end_ts_ptr - fs->base_ptr), &match);
    if (match == 0) {
        fs->eof = 1;
        fs->start_payload = fs->start_ts_ptr;
        fs->end_payload = start_ts;
    } else {
        fs->start_payload = fs->start_ts_ptr;
        fs->end_payload = start_ts;
        fs->start_ts_ptr = start_ts;
        fs->end_ts_ptr = start_ts + TS_LEN;
    }
    return match;
}

static void sort_files (file_state_t *states, int num_files, 
    output_file_state_t *ofs) 
{
    int findex = 0;
    for (findex = 0; findex < num_files; findex++) {
    open_file(&states[findex]);
    initial_file_offset(&states[findex]);
    /* insert the first nodes into the binary heap */
    if (!states[findex].eof) {
        if (add_array_binheap (&filenode_binheap, &states[findex]) != EOK) {
        printf("add to binheap failed\n");
        }
    }
    heapify_binheap (&filenode_binheap);
    }
    int done = 0;
    file_state_t *next_file = NULL;
    while (!done) {
    if (next_file != NULL) {
        if (next_file->eof != 1) {
        next_file_offset(next_file);
        insert_binheap(&filenode_binheap, next_file);
        } else {
        /* close the file and cleanup */
        close_file(next_file);
        }
    }
    file_state_t *popped_state;
    if(pop_binheap(&filenode_binheap, (void **) &popped_state) == EOK) {
        int emit_line = 0;
        int appctx_match = 0;
        if (popped_state->start_payload != popped_state->end_payload) {
        appctx_matcher(popped_state->start_payload + TS_LEN,
             popped_state->end_payload - popped_state->start_payload - TS_LEN,
             &appctx_match);
        if (appctx_match) {
            int mac = 
                match_mac(popped_state->start_payload + TS_LEN, 
                    popped_state->end_payload, mac_address_filter);
            if (mac) {
                size_t uuid_offset = 0;
                /*printf("ps %p %lx\n", 
                    popped_state->start_payload + TS_LEN,
                     popped_state->end_payload
                   - popped_state->start_payload - TS_LEN); */
                char *uuid_read = 
                    uuid_val_matcher(popped_state->start_payload + TS_LEN, 
                    popped_state->end_payload
                               - popped_state->start_payload - TS_LEN, 
                               &uuid_offset);
                if (uuid_read != NULL) {
                    uuid_entry_t *uuid = 
                        (uuid_entry_t*)malloc(sizeof(uuid_entry_t));
                    memset((void*)uuid, 0, sizeof(uuid_entry_t));
                    memcpy(uuid->uuid, uuid_read, uuid_offset);
                    uuid->uuid[uuid_offset] = '\0';
                    uuid->uuid_len = uuid_offset + 1;
                   uuid_hash_HASH_INSERT(&uuid_hash_head, uuid);
                }
            }
        }
        if (HASH_COUNT(&uuid_hash_head) > 0) {
            size_t uuid_offset = 0;
            char *uuid_read = 
                uuid_val_matcher(popped_state->start_payload + TS_LEN, 
                    popped_state->end_payload
                               - popped_state->start_payload - TS_LEN, 
                    &uuid_offset);
            if (uuid_read != NULL) {
                char tmp_str[uuid_offset+1];
                memcpy(tmp_str, uuid_read, uuid_offset);
                tmp_str[uuid_offset] = '\0';
                uuid_entry_t uuid;
                memcpy(uuid.uuid, uuid_read, uuid_offset);
                uuid.uuid[uuid_offset + 1] = '\0';
                uuid.uuid_len = uuid_offset + 1;
                uuid_entry_t *found = 
                    uuid_hash_HASH_FIND(&uuid_hash_head, &uuid);
                if (found) {
                    emit_line = 1;
                }
            }
        }

        if (emit_line || emit_line_always) {
            if (ofs) {
            write_output_file(ofs, popped_state->start_payload, TS_LEN);
            if (addfilename) {
                write_output_file(ofs, popped_state->basename, 
                          popped_state->basename_len);
            }
            write_output_file(ofs, popped_state->start_payload + TS_LEN, 
                      popped_state->end_payload 
                      - popped_state->start_payload - TS_LEN);
            } else {
            write(1, popped_state->start_payload, TS_LEN);
            if (addfilename) {
                write(1, popped_state->basename, 
                  popped_state->basename_len);
            }
            write(1, popped_state->start_payload + TS_LEN,
                  popped_state->end_payload 
                  - popped_state->start_payload - TS_LEN);
            }
        }
        }
        next_file = popped_state;
    } else {
        done = 1;
        break;
    }
    }
}

#if TEST_HASH

static boolean uuid_walk_fn(uuid_entry_t *uuid, void *ctx __UNUSED) {
    printf("walking uuid %s\n", uuid->uuid);
    return TRUE;
}

#endif

int
main (int argc, char **argv) {
    int c;
    output_file_state_t *ofs = NULL;
    uuid_hash_head.htable = 
        calloc(1, sizeof(struct uuid_hashht) * HASH_SIZE(256));
    HASH_INIT(&uuid_hash_head, 256);


#if TEST_HASH
    int i;


    static char val[200];

    for (i = 0; i < 100; i++) {
    sprintf(val, "UUID: %x", i);
    uuid_entry_t *uuid = calloc(1, sizeof(uuid));
    memcpy(uuid->uuid, val, strlen(val));
    uuid->uuid_len = strlen(val);
    //printf("uuid insert %p %s %ld\n", uuid, uuid->uuid, uuid->uuid_len);
    uuid_hash_HASH_INSERT(&uuid_hash_head, uuid);
    //printf("uuid after insert %p %s %ld\n", uuid, uuid->uuid, uuid->uuid_len);
    }

    HASH_FOREACH(uuid_entry_s, &uuid_hash_head, 
         uuid_hash_entry, uuid_walk_fn, NULL);


    uuid_entry_t lkup_uuid;
    strcpy(lkup_uuid.uuid, "UUID: e");
    lkup_uuid.uuid_len = strlen(lkup_uuid.uuid);

    uuid_entry_t *found = uuid_hash_HASH_FIND(&uuid_hash_head, &lkup_uuid);
    if (found) {
    printf("found uuid %s\n", found->uuid);
    } else {
        printf("not found\n");
    }

    uuid_entry_t lkup_uuid2;
    strcpy(lkup_uuid2.uuid, "UUID: yy");
    lkup_uuid2.uuid_len = strlen(lkup_uuid2.uuid);

    uuid_entry_t *found2 = uuid_hash_HASH_FIND(&uuid_hash_head, &lkup_uuid2);
    if (found2) {
    printf("found uuid %s\n", found2->uuid);
    } else {
    printf("not found\n");
    }

    HASH_FOREACH(uuid_entry_s, &uuid_hash_head, 
         uuid_hash_entry, uuid_walk_fn, NULL);

#endif

    while (1) {
    int option_index = 0;
    static struct option long_options[] = {
        {"output", 1, 0, 'o'},
        {"addfilename", 0, 0, 'a'},
        {"filter", 1, 0, 'f'},
        {0, 0, 0, 0}
    };

    c = getopt_long (argc, argv, "ao:f:",
             long_options, &option_index);
    if (c == -1)
        break;

    switch (c) {
    case 'a':
        addfilename = 1;
        break;
    case 'f':
        emit_line_always = 0;
        /* macaddr filter */
        mac_address_filter = strdup(optarg);
        printf("Matching mac_address %s\n", mac_address_filter);
        break;
    case 'o':
        printf("option o with value '%s'\n", optarg);
        ofs = open_output_file(optarg);
        break;


    case '?':
        break;

    default:
        printf("?? getopt returned character code 0%o ??\n", c);
    }
    }


    binheap_init(&filenode_binheap, filenode_binheap_compare);

    int inp_file_count = 0, tmp_opt_ind = 0;
    if (optind < argc) {
        inp_file_count = argc;
        tmp_opt_ind = optind;
        while (optind < argc) {
            memset(&file_states[next_file_state], 0, sizeof(file_state_t));
            file_states[next_file_state].filename = strdup(argv[optind]);
            file_states[next_file_state].basename = malloc(MAX_FILENAME_SIZE);
            strcat(file_states[next_file_state].basename, " ");
            strcat(file_states[next_file_state].basename, 
            basename(file_states[next_file_state].filename));
            file_states[next_file_state].basename_len = 
                strlen(file_states[next_file_state].basename);
            file_states[next_file_state].used = 1;
            next_file_state++;
            optind++;
        }
    }

    sort_files(file_states, next_file_state, ofs);
   
    if (ofs) {
        close_output_file(ofs);
    }

    exit (0);
}

