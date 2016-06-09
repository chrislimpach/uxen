/*
 * ntreg.c - Windows (NT and up) Registry Hive access library
 * 
 * 2011-may: Seems like large values >16k or something like that is split
 *           into several blocks (db), have tried to implement that.
 *           Vista seems to accept it. Not tested on others yet.
 * 2011-may: Expansion now seems to be working, have lot of test accepted by
 *           vista and win7. But no warranties..
 * 2011-may: Found a couple of nasty bugs inn add_key(), making Vista and newer
 *           reject (remove) keys on hive load.
 *           May have been there for a long time according to reports.
 * 2011-apr: Fixed some problems with the allocator when at the end of a hbin.
 * 2011-apr: .reg file import. Ugly one, but it seems to work. Found
 *           quite a lot of bugs in other places while testing it.
 *           String handling when international characters or wide (UTF-16)
 *           is a pain, and very ugly. May not work in some cases.
 *           Will keep wide (16 bit) characters in strings when importing from
 *           .reg file that has it, like what regedit.exe generates for example.
 * 2011-apr: Added routines for hive expansion. Will add new hbin at end of file
 *           when needed. If using library, please read ugly warnings in "alloc_block()".
 * 2010-jun: Patches from Frediano Ziglio adding support for wide characters
 *           and some bugfixes. Thank you!
 * 2008-mar: Type QWORD (XP/Vista and newer) now recognized
 * 2008-mar: Most functions accepting a path now also have a parameter specifying if
 *           the search should be exact or on first match basis
 * 2008-mar: Fixed bug which skipped first indirect index table when deleting keys,
 *           usually leading to endless loop when recursive deleting.
 * 2008-mar: Export to .reg file by Leo von Klenze, expanded a bit by me.
 * 2008-mar: 64 bit compatible patch by Mike Doty, via Alon Bar-Lev
 *           http://bugs.gentoo.org/show_bug.cgi?id=185411
 * 2007-sep: Verbosity/debug messages minor changes
 * 2007-apr: LGPL license.
 * 2004-aug: Deep indirect index support. NT351 support. Recursive delete.
 *           Debugged a lot in allocation routines. Still no expansion.
 * 2004-jan: Verbosity updates
 * 2003-jan: Allocation of new data, supports adding/deleting keys & stuff.
 *           Missing is expanding the file.
 * 2003-jan: Seems there may be garbage pages at end of file, not zero pages
 *           now stops enumerating at first non 'hbin' page.
 * 
 * NOTE: The API is not frozen. It can and will change every release.
 *
 *****
 *
 * NTREG - Window registry file reader / writer library
 * Copyright (c) 1997-2010 Petter Nordahl-Hagen.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * See file LGPL.txt for the full license.
 * 
 */ 

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disklib.h"
#include "reghive.h"
#include "ntreg.h"

// #define WRITE_HIVE 1

/* Hive open modes */
#define HMODE_RW        0
#define HMODE_RO        0x1
#define HMODE_DIRTY     0x4
#define HMODE_NOALLOC   0x8        /* Don't allocate new blocks */
#define HMODE_NOEXPAND  0x10       /* Don't expand file with new hbin */
#define HMODE_DIDEXPAND 0x20       /* File has been expanded */
#define HMODE_VERBOSE 0x1000
#define HMODE_TRACE   0x2000

/* Hive definition, allocated by openHive(), dealloc by closeHive()
 * contains state data, must be passed in all functions
 */
struct hive {
  unsigned int refcnt;
  const struct hive_iops *iops;
  void *filedesc;        /* File descriptor (only valid if state == OPEN) */
  int  state;            /* Current state of hive */
  int  pages;            /* Number of pages, total */
  int  useblk;           /* Total # of used blocks */
  int  unuseblk;         /* Total # of unused blocks */
  int  usetot;           /* total # of bytes in useblk */
  int  unusetot;         /* total # of bytes in unuseblk */
  int  size;             /* Hives size (filesize) in bytes, incl regf header */
  int  rootofs;          /* Offset of root-node */
  int  lastbin;          /* Offset to last HBIN */
  int  endofs;           /* Offset of first non HBIN page, we can expand from here */
  short nkindextype;     /* Subkey-indextype the root key uses */
  char *buffer;          /* Files raw contents */
};

/******* Function prototypes **********/
/***************************************************/

/* Various nice macros */

#define CREATE(result, type, number)\
    { \
        if (!((result) = (type *) calloc ((number), sizeof(type)))) { \
            perror("malloc failure"); \
            abort() ; \
       } \
    }
#define ALLOC(result, size, number)\
    { \
        if (!((result) = (void *) calloc ((number), (size)))) { \
            perror("malloc failure"); \
            abort() ; \
       } \
    }
#define FREE(p) { if (p) { free(p); (p) = 0; } }

/* Debug / verbosity message macro */

#define VERB(h, string) \
     { \
       if ((h)->state & HMODE_VERBOSE) RTPrintf((string)); \
     }

#define VERBF(h, ...) \
     { \
       if ((h)->state & HMODE_VERBOSE) RTPrintf(__VA_ARGS__); \
     }

#define debugit(x...) do{}while(0)

/* Set to abort() and debug on more critical errors */
#define DOCORE 1

#define ZEROFILL      1  /* Fill blocks with zeroes when allocating and deallocating */
#define ZEROFILLONLOAD  0  /* Fill blocks marked as unused/deallocated with zeroes on load. FOR DEBUG */

const char ntreg_version[] = "ntreg lib routines, v0.95 110511, (c) Petter N Hagen";

const char *val_types[REG_MAX] = {
  "REG_NONE", "REG_SZ", "REG_EXPAND_SZ", "REG_BINARY", "REG_DWORD",       /* 0 - 4 */
  "REG_DWORD_BIG_ENDIAN", "REG_LINK",                                     /* 5 - 6 */
  "REG_MULTI_SZ", "REG_RESOUCE_LIST", "REG_FULL_RES_DESC", "REG_RES_REQ", /* 7 - 10 */
  "REG_QWORD",                                                            /* 11     */
};

static char * string_prog2regw(void *string, int len, int* out_len);

/* Utility routines */

/* toupper() table for registry hashing functions, so we don't have to
 * dependent upon external locale lib files
 */

static const unsigned char reg_touppertable[] = {

  /* ISO 8859-1 is probably not the one.. */

        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, /* 0x00-0x07 */
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, /* 0x08-0x0f */
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, /* 0x10-0x17 */
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, /* 0x18-0x1f */
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, /* 0x20-0x27 */
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, /* 0x28-0x2f */
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, /* 0x30-0x37 */
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, /* 0x38-0x3f */
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, /* 0x40-0x47 */
        0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, /* 0x48-0x4f */
        0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, /* 0x50-0x57 */
        0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, /* 0x58-0x5f */
        0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, /* 0x60-0x67 */
        0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, /* 0x68-0x6f */
        0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, /* 0x70-0x77 */
        0x58, 0x59, 0x5a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, /* 0x78-0x7f */

        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, /* 0x80-0x87 */
        0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, /* 0x88-0x8f */
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, /* 0x90-0x97 */
        0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, /* 0x98-0x9f */
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, /* 0xa0-0xa7 */
        0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, /* 0xa8-0xaf */
        0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0x00, 0xb6, 0xb7, /* 0xb0-0xb7 */
        0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, /* 0xb8-0xbf */
        0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, /* 0xc0-0xc7 */
        0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, /* 0xc8-0xcf */
        0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, /* 0xd0-0xd7 */
        0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, /* 0xd8-0xdf */
        0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, /* 0xe0-0xe7 */
        0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, /* 0xe8-0xef */
        0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xf7, /* 0xf0-0xf7 */
        0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0x00, /* 0xf8-0xff */

};

static int hdesc_filesize(struct hive *hdesc, size_t *sz)
{
    return (*hdesc->iops->filesize)(hdesc->filedesc, sz);
}

static int hdesc_read(struct hive *hdesc, uint8_t *ptr, size_t len)
{
    return (*hdesc->iops->read)(hdesc->filedesc, ptr, len);
}

#ifdef WRITE_HIVE
static int hdesc_write(struct hive *hdesc, uint8_t *ptr, size_t len)
{
    return (*hdesc->iops->write)(hdesc->filedesc, ptr, len);
}
#endif  /* WRITE_HIVE */

static void hdesc_close(struct hive *hdesc)
{
    return (*hdesc->iops->close)(hdesc->filedesc);
}

static const char *hdesc_filename(struct hive *hdesc)
{
    if ( hdesc->iops->filename ) {
        return (*hdesc->iops->filename)(hdesc->filedesc);
    }
    return "Unnamed hive file";
}

#ifdef WRITE_HIVE
/* Use table above in strcasecmp else add_key may put names in wrong order
   and windows actually verifies that on hive load!!
   or at least it finds out in some cases..
*/


static int strn_casecmp(const char *s1, const char *s2, size_t n)
{
  char r;

  while ( *s1 && *s2 && n ) {
    r = (unsigned char)reg_touppertable[(unsigned char)*s1] - (unsigned char)reg_touppertable[(unsigned char)*s2];
    if (r) return(r);
    n--;
    s1++;
    s2++;
  }
  if ( (!*s1 && !*s2) || !n) return(0);
  if ( !*s1 ) return(-1);
  return(1);
}
#endif  /* WRITE_HIVE */


/* Copy non-terminated string to buffer we allocate and null terminate it
 * Uses length only, does not check for nulls
 */

static char *mem_str( const char *str, int len )
{
    char *str_new;

    if (!str)
        return 0 ;

    CREATE( str_new, char, len + 1 );    
    memcpy( str_new, str, len);
    *(str_new+len) = 0;
    return str_new;
}


#ifdef WRITE_HIVE
/* Print len number of hexbytes */

static void hexprnt(char *s, unsigned char *bytes, int len)
{
   int i;

   RTPrintf("%s",s);
   for (i = 0; i < len; i++) {
      RTPrintf("%02x ",bytes[i]);
   }
   RTPrintf("\n");
}

/* HexDump all or a part of some buffer */

static void hexdump(char *hbuf, int start, int stop, int ascii)
{
   char c;
   int diff,i;
   
   while (start < stop ) {
      
      diff = stop - start;
      if (diff > 16) diff = 16;
      
      RTPrintf(":%05X  ",start);

      for (i = 0; i < diff; i++) {
	 RTPrintf("%02X ",(unsigned char)*(hbuf+start+i));
      }
      if (ascii) {
	for (i = diff; i < 16; i++) RTPrintf("   ");
	for (i = 0; i < diff; i++) {
	  c = *(hbuf+start+i);
	  RTPrintf("%c", isprint(c) ? c : '.');
	}
      }
      RTPrintf("\n");
      start += 16;
   }
}

/* General search routine, find something in something else */
static int find_in_buf(char *buf, char *what, int sz, int len, int start)
{
   int i;
   
   for (; start < sz; start++) {
      for (i = 0; i < len; i++) {
	if (*(buf+start+i) != *(what+i)) break;
      }
      if (i == len) return(start);
   }
   return(0);
}
#endif  /* WRITE_HIVE */

/* Get INTEGER from memory. This is probably low-endian specific? */
static int get_int( char *array )
{
	return ((array[0]&0xff) + ((array[1]<<8)&0xff00) +
		   ((array[2]<<16)&0xff0000) +
		   ((array[3]<<24)&0xff000000));
}


/* Quick and dirty UNICODE to std. ascii */

static void cheap_uni2ascii(char *src, char *dest, int l)
{
   
   for (; l > 0; l -=2) {
      *dest = *src;
      dest++; src +=2;
   }
   *dest = 0;
}


#ifdef WRITE_HIVE
/* Quick and dirty ascii to unicode */

static void cheap_ascii2uni(char *src, char *dest, int l)
{
   for (; l > 0; l--) {
      *dest++ = *src++;
      *dest++ = 0;

   }
}

static void skipspace(char **c)
{
   while( **c == ' ' ) (*c)++;
}
#endif  /* WRITE_HIVE */


/* ========================================================================= */

/* The following routines are mostly for debugging, I used it
 * much during discovery. the -t command line option uses it,
 * also the 'st' and 's' from the editor & hexdebugger.
 * All offsets shown in these are unadjusted (ie you must add
 * headerpage (most often 0x1000) to get file offset)
 */

/* Parse the nk datablock
 * vofs = offset into struct (after size linkage)
 */
static void parse_nk(struct hive *hdesc, int vofs, int blen)
{

  struct nk_key *key;
  int i;

  RTPrintf("== nk at offset %0x\n",vofs);

  /* #define D_OFFS2(o) ( (void *)&(key->o)-(void *)hdesc->buffer-vofs ) */
#define D_OFFS(o) ( (void *)&(key->o)-(void *)hdesc->buffer-vofs )

  key = (struct nk_key *)(hdesc->buffer + vofs);
  RTPrintf("%04"PRIxS"   type              = 0x%02x %s\n", D_OFFS(type)  ,key->type,
	                           (key->type == KEY_ROOT ? "ROOT_KEY" : "") );
  RTPrintf("%04"PRIxS"   timestamp skipped\n", D_OFFS(timestamp) );
  RTPrintf("%04"PRIxS"   parent key offset = 0x%0x\n", D_OFFS(ofs_parent) ,key->ofs_parent + 0x1000);
  RTPrintf("%04"PRIxS"   number of subkeys = %d\n", D_OFFS(no_subkeys),key->no_subkeys);
  RTPrintf("%04"PRIxS"   lf-record offset  = 0x%0x\n",D_OFFS(ofs_lf),key->ofs_lf + 0x1000);
  RTPrintf("%04"PRIxS"   number of values  = %d\n", D_OFFS(no_values),key->no_values);
  RTPrintf("%04"PRIxS"   val-list offset   = 0x%0x\n",D_OFFS(ofs_vallist),key->ofs_vallist + 0x1000);
  RTPrintf("%04"PRIxS"   sk-record offset  = 0x%0x\n",D_OFFS(ofs_sk),key->ofs_sk + 0x1000);
  RTPrintf("%04"PRIxS"   classname offset  = 0x%0x\n",D_OFFS(ofs_classnam),key->ofs_classnam + 0x1000);

  RTPrintf("%04"PRIxS"   dummy3            = 0x%0x (%d)\n",D_OFFS(dummy3),key->dummy3,key->dummy3);
  RTPrintf("%04"PRIxS"   dummy4            = 0x%0x (%d)\n",D_OFFS(dummy4),key->dummy4,key->dummy4);
  RTPrintf("%04"PRIxS"   dummy5            = 0x%0x (%d)\n",D_OFFS(dummy5),key->dummy5,key->dummy5);
  RTPrintf("%04"PRIxS"   dummy6            = 0x%0x (%d)\n",D_OFFS(dummy6),key->dummy6,key->dummy6);
  RTPrintf("%04"PRIxS"   dummy7            = 0x%0x (%d)\n",D_OFFS(dummy7),key->dummy7,key->dummy7);

  RTPrintf("%04"PRIxS"   name length       = %d\n", D_OFFS(len_name),key->len_name);
  RTPrintf("%04"PRIxS"   classname length  = %d\n", D_OFFS(len_classnam),key->len_classnam);

  RTPrintf("%04"PRIxS"   Key name: <",D_OFFS(keyname) );
  for(i = 0; i < key->len_name; i++) putchar(key->keyname[i]);
  RTPrintf(">\n== End of key info.\n");

}

/* Parse the vk datablock
 * vofs = offset into struct (after size linkage)
 */
static void parse_vk(struct hive *hdesc, int vofs, int blen)
{
  struct vk_key *key;
  int i;

  RTPrintf("== vk at offset %0x\n",vofs);


  key = (struct vk_key *)(hdesc->buffer + vofs);
  RTPrintf("%04"PRIxS"   name length       = %d (0x%0x)\n", D_OFFS(len_name),
	                             key->len_name, key->len_name  );
  RTPrintf("%04"PRIxS"   length of data    = %d (0x%0x)\n", D_OFFS(len_data),
	                             key->len_data, key->len_data  );
  RTPrintf("%04"PRIxS"   data offset       = 0x%0x\n",D_OFFS(ofs_data),key->ofs_data + 0x1000);
  RTPrintf("%04"PRIxS"   value type        = 0x%0x  %s\n", D_OFFS(val_type), key->val_type,
                 (key->val_type <= REG_MAX ? val_types[key->val_type] : "(unknown)") ) ;

  RTPrintf("%04"PRIxS"   flag              = 0x%0x\n",D_OFFS(flag),key->flag);
  RTPrintf("%04"PRIxS"   *unused?*         = 0x%0x\n",D_OFFS(dummy1),key->dummy1);

  RTPrintf("%04"PRIxS"   Key name: <",D_OFFS(keyname) );
  for(i = 0; i < key->len_name; i++) putchar(key->keyname[i]);
  RTPrintf(">\n== End of key info.\n");

}

/* Parse the sk datablock
 * Gee, this is the security info. Who cares? *evil grin*
 * vofs = offset into struct (after size linkage)
 */
void parse_sk(struct hive *hdesc, int vofs, int blen)
{
  struct sk_key *key;
  /* int i; */

  RTPrintf("== sk at offset %0x\n",vofs);

  key = (struct sk_key *)(hdesc->buffer + vofs);
  RTPrintf("%04"PRIxS"   *unused?*         = %d\n"   , D_OFFS(dummy1),     key->dummy1    );
  RTPrintf("%04"PRIxS"   Offset to prev sk = 0x%0x\n", D_OFFS(ofs_prevsk), key->ofs_prevsk + 0x1000);
  RTPrintf("%04"PRIxS"   Offset to next sk = 0x%0x\n", D_OFFS(ofs_nextsk), key->ofs_nextsk + 0x1000);
  RTPrintf("%04"PRIxS"   Usage counter     = %d (0x%0x)\n", D_OFFS(no_usage),
	                                            key->no_usage,key->no_usage);
  RTPrintf("%04"PRIxS"   Security data len = %d (0x%0x)\n", D_OFFS(len_sk),
	                                            key->len_sk,key->len_sk);

  RTPrintf("== End of key info.\n");

}


/* Parse the lf datablock (>4.0 'nk' offsets lookuptable)
 * vofs = offset into struct (after size linkage)
 */
static void parse_lf(struct hive *hdesc, int vofs, int blen)
{
  struct lf_key *key;
  int i;

  RTPrintf("== lf at offset %0x\n",vofs);

  key = (struct lf_key *)(hdesc->buffer + vofs);
  RTPrintf("%04"PRIxS"   number of keys    = %d\n", D_OFFS(no_keys), key->no_keys  );

  for(i = 0; i < key->no_keys; i++) {
    RTPrintf("%04"PRIxS"      %3d   Offset: 0x%0x  - <%c%c%c%c>\n", 
	   D_OFFS(hash[i].ofs_nk), i,
	   key->hash[i].ofs_nk + 0x1000,
           key->hash[i].name[0],
           key->hash[i].name[1],
           key->hash[i].name[2],
           key->hash[i].name[3] );
  }

  RTPrintf("== End of key info.\n");

}

/* Parse the lh datablock (WinXP offsets lookuptable)
 * vofs = offset into struct (after size linkage)
 * The hash is most likely a base 37 conversion of the name string
 */
void parse_lh(struct hive *hdesc, int vofs, int blen)
{
  struct lf_key *key;
  int i;

  RTPrintf("== lh at offset %0x\n",vofs);

  key = (struct lf_key *)(hdesc->buffer + vofs);
  RTPrintf("%04"PRIxS"   number of keys    = %d\n", D_OFFS(no_keys), key->no_keys  );

  for(i = 0; i < key->no_keys; i++) {
    RTPrintf("%04"PRIxS"      %3d   Offset: 0x%0x  - <hash: %08x>\n", 
	   D_OFFS(lh_hash[i].ofs_nk), i,
	   key->lh_hash[i].ofs_nk + 0x1000,
           key->lh_hash[i].hash );
  }

  RTPrintf("== End of key info.\n");

}


/* Parse the li datablock (3.x 'nk' offsets list)
 * vofs = offset into struct (after size linkage)
 */
static void parse_li(struct hive *hdesc, int vofs, int blen)
{
  struct li_key *key;
  int i;

  RTPrintf("== li at offset %0x\n",vofs);

  /* #define D_OFFS(o) ( (void *)&(key->o)-(void *)hdesc->buffer-vofs ) */

  key = (struct li_key *)(hdesc->buffer + vofs);
  RTPrintf("%04"PRIxS"   number of keys    = %d\n", D_OFFS(no_keys), key->no_keys  );

  for(i = 0; i < key->no_keys; i++) {
    RTPrintf("%04"PRIxS"      %3d   Offset: 0x%0x\n", 
	   D_OFFS(hash[i].ofs_nk), i,
	   key->hash[i].ofs_nk + 0x1000);
  }
  RTPrintf("== End of key info.\n");

}

/* Parse the ri subindex-datablock
 * (Used to list li/lf/lh's when ~>500keys)
 * vofs = offset into struct (after size linkage)
 */
static void parse_ri(struct hive *hdesc, int vofs, int blen)
{
  struct ri_key *key;
  int i;

  RTPrintf("== ri at offset %0x\n",vofs);

  /* #define D_OFFS(o) ( (void *)&(key->o)-(void *)hdesc->buffer-vofs ) */

  key = (struct ri_key *)(hdesc->buffer + vofs);
  RTPrintf("%04"PRIxS"   number of subindices = %d\n", D_OFFS(no_lis), key->no_lis  );

  for(i = 0; i < key->no_lis; i++) {
    RTPrintf("%04"PRIxS"      %3d   Offset: 0x%0x\n", 
	   D_OFFS(hash[i].ofs_li), i,
	   key->hash[i].ofs_li + 0x1000);
  }
  RTPrintf("== End of key info.\n");

}


/* Parse the db block (used when value data >4k or something)
 * vofs = offset into struct (after size linkage)
 */
static void parse_db(struct hive *hdesc, int vofs, int blen)
{
  struct db_key *key;

  RTPrintf("== db at offset %0x\n",vofs);

  key = (struct db_key *)(hdesc->buffer + vofs);
  RTPrintf("%04"PRIxS"   number of parts    = %d\n", D_OFFS(no_part), key->no_part  );

  RTPrintf("%04"PRIxS"   Data list at offset: 0x%0x\n", 
	   D_OFFS(ofs_data),
	   key->ofs_data + 0x1000);
  
  RTPrintf("== End of key info.\n");

}



/* Parse the datablock
 * vofs = offset into struct (after size linkage)
 */

static int parse_block(struct hive *hdesc, int vofs,int verbose)
{
  unsigned short id;
  int seglen;

  seglen = get_int(hdesc->buffer+vofs);

  //  if (vofs > 0xaef000) verbose = 1;

#if 0
  if (verbose || seglen == 0) {
    RTPrintf("** Block at offset %0x\n",vofs);
    RTPrintf("seglen: %d, %u, 0x%0x\n",seglen,seglen,seglen);
  }
#endif
  if (seglen == 0) {
    RTPrintf("parse_block: FATAL! Zero data block size! (not registry or corrupt file?)\n");
    debugit(hdesc->buffer,hdesc->size);
    return(0);
  }
  
  if (seglen < 0) {
    seglen = -seglen;
    hdesc->usetot += seglen;
    hdesc->useblk++;
    if (verbose) {
      RTPrintf("USED BLOCK @ %06x to %06x : %d, 0x%0x\n",vofs,vofs+seglen,seglen,seglen);
      /*      hexdump(hdesc->buffer,vofs,vofs+seglen+4,1); */
    }
  } else {
    hdesc->unusetot += seglen;
    hdesc->unuseblk++;
    /* Useful to zero blocks we think are empty when debugging.. */
#if ZEROFILLONLOAD
    memset(hdesc->buffer+vofs+4, 0, seglen-4);
#endif

    if (verbose) {
      RTPrintf("FREE BLOCK @ %06x to %06x : %d, 0x%0x\n",vofs,vofs+seglen,seglen,seglen); 
      /*      hexdump(hdesc->buffer,vofs,vofs+seglen+4,1); */
    }
  }


  vofs += 4;
  id = (*(hdesc->buffer + vofs)<<8) + *(hdesc->buffer+vofs+1);

  if (verbose > 1) {
    switch (id) {
    case 0x6e6b: /* nk */
      parse_nk(hdesc, vofs, seglen);
      break;
    case 0x766b: /* vk */
      parse_vk(hdesc, vofs, seglen);
      break;
    case 0x6c66: /* lf */
      parse_lf(hdesc, vofs, seglen);
      break;
    case 0x6c68: /* lh */
      parse_lh(hdesc, vofs, seglen);
      break;
    case 0x6c69: /* li */
      parse_li(hdesc, vofs, seglen);
      break;
    case 0x736b: /* sk */
      parse_sk(hdesc, vofs, seglen);
      break;
    case 0x7269: /* ri */
      parse_ri(hdesc, vofs, seglen);
      break;
    case 0x6462: /* db */
      parse_db(hdesc, vofs, seglen);
      break;
    default:
      RTPrintf("value data, or not handeled yet!\n");
      break;
    }
  }
  return(seglen);
}

/* ================================================================ */
/* Scan and allocation routines */

/* Find start of page given a current pointer into the buffer
 * hdesc = hive
 * vofs = offset pointer into buffer
 * returns: offset to start of page (and page header)
 */

static int find_page_start(struct hive *hdesc, int vofs)
{
  int r,prev;
  struct hbin_page *h;

  /* Again, assume start at 0x1000 */

  r = 0x1000;
  while (r < hdesc->size) {
    prev = r;
    h = (struct hbin_page *)(hdesc->buffer + r);
    if (h->id != 0x6E696268) return(0);
    if (h->ofs_next == 0) {
      RTPrintf("find_page_start: zero len or ofs_next found in page at 0x%x\n",r);
      return(0);
    }
    r += h->ofs_next;
    if (r > vofs) return (prev);
  }
  return(0);
}

/* Find free space in page
 * size = requested size in bytes
 * pofs = offset to start of actual page header
 * returns: offset to free block, or 0 for error
 */

#define FB_DEBUG 0

static int find_free_blk(struct hive *hdesc, int pofs, int size)
{
  int vofs = pofs + 0x20;
  int seglen;
  struct hbin_page *p;
  
  p = (struct hbin_page *)(hdesc->buffer + pofs);

  while (vofs-pofs < (p->ofs_next - HBIN_ENDFILL)) {

    seglen = get_int(hdesc->buffer+vofs);  

#if FB_DEBUG
    if (vofs > 0x400000) {
    RTPrintf("** Block at offset %0x\n",vofs);
    RTPrintf("seglen: %d, %u, 0x%0x\n",seglen,seglen,seglen);
    }
#endif

    if (seglen == 0) {
      RTPrintf("find_free_blk: FATAL! Zero data block size! (not registry or corrupt file?)\n");
      RTPrintf("             : Block at offset %0x\n",vofs);
      if ( (vofs - pofs) == (p->ofs_next - 4) ) {
	RTPrintf("find_free_blk: at exact end of hbin, do not care..\n");
	return(0);
      }
      abort();
      debugit(hdesc->buffer,hdesc->size);
      return(0);
    }
    
    if (seglen < 0) {
      seglen = -seglen;
#if FB_DEBUG
      if (vofs >0x400000) RTPrintf("USED BLOCK: %d, 0x%0x\n",seglen,seglen);
#endif
	/*      hexdump(hdesc->buffer,vofs,vofs+seglen+4,1); */
    } else {
#if FB_DEBUG
	if (vofs >0x400000) RTPrintf("FREE BLOCK!\n"); 
#endif
	/*      hexdump(hdesc->buffer,vofs,vofs+seglen+4,1); */
	if (seglen >= size) {
#if FB_DEBUG
	  if (vofs >0x400000) RTPrintf("find_free_blk: found size %d block at 0x%x\n",seglen,vofs);
#endif
#if 0
	  if (vofs == 0x19fb8) {
	    RTPrintf("find_free_blk: vofs = %x, seglen = %x\n",vofs,seglen);
	    debugit(hdesc->buffer,hdesc->size);
	    abort();
	  }
#endif
	  return(vofs);
	}
    }
    vofs += seglen;
  }
  return(0);
  
}

#undef FB_DEBUG

/* Search pages from start to find free block
 * hdesc - hive
 * size - space requested, in bytes
 * returns: offset to free block, 0 if not found or error
 */

static int find_free(struct hive *hdesc, int size)
{
  int r,blk;
  struct hbin_page *h;

  /* Align to 8 byte boundary */
  if (size & 7) size += (8 - (size & 7));

  /* Again, assume start at 0x1000 */

  r = 0x1000;
  while (r < hdesc->endofs) {
    h = (struct hbin_page *)(hdesc->buffer + r);
    if (h->id != 0x6E696268) return(0);
    if (h->ofs_next == 0) {
      RTPrintf("find_free: zero len or ofs_next found in page at 0x%x\n",r);
      return(0);
    }
    blk = find_free_blk(hdesc,r,size);
    if (blk) return (blk);
    r += h->ofs_next;
  }
  return(0);
}

/* Add new hbin to end of file. If file contains data at end
 * that is not in a hbin, include that too
 * hdesc - hive as usual
 * size - minimum size (will be rounded up to next 0x1000 alignment)
 * returns offset to first block in new hbin
  */

#define ADDBIN_DEBUG
static int add_bin(struct hive *hdesc, int size)
{
  int r,newsize,newbinofs;
  struct hbin_page *newbin;
  struct regf_header *hdr;

  if (hdesc->state & HMODE_NOEXPAND) {
   RTPrintf("ERROR: ERROR: Registry hive <%s> need to be expanded,\n"
	   "but that is not allowed according to selected options. Operations will fail.\n", hdesc_filename(hdesc));
    return(0);
  }

  r = ((size + 0x20 + 4) & ~0xfff) + HBIN_PAGESIZE;  /* Add header and link, round up to page boundary, usually 0x1000 */

  newbinofs = hdesc->endofs;


#ifdef ADDBIN_DEBUG
  RTPrintf("add_bin: request size = %d [%x], rounded to %d [%x]\n",size,size,r,r);
  RTPrintf("add_bin: old buffer size = %d [%x]\n",hdesc->size,hdesc->size);
  RTPrintf("add_bin: firs nonbin off = %d [%x]\n",newbinofs,newbinofs);
  RTPrintf("add_bin: free at end     = %d [%x]\n",hdesc->size-newbinofs,hdesc->size-newbinofs);
#endif

  if ( (newbinofs + r) >= hdesc->size) { /* We must allocate more buffer */
    newsize = ( (newbinofs + r) & ~(REGF_FILEDIVISOR-1) ) + REGF_FILEDIVISOR; /* File normally multiple of 0x40000 bytes */

#ifdef ADDBIN_DEBUG
    RTPrintf("add_bin: new buffer size = %d [%x]\n",newsize,newsize);
#endif

    hdesc->buffer = realloc(hdesc->buffer, newsize);
    if (!hdesc->buffer) {
      perror("add_bin : realloc() ");
      abort();
    }
    hdesc->size = newsize;

  }

  /* At this point, we have large enough space at end of file */

  newbin = (struct hbin_page *)(hdesc->buffer + newbinofs);

  memset((void *)newbin, 0, r); /* zero out new hbin, easier to debug too */

  newbin->id = 0x6E696268; /* 'hbin' */
  newbin->ofs_self = newbinofs - 0x1000;     /* Point to ourselves minus regf. Seem to be that.. */
  newbin->ofs_next = r;                  /* size of this new bin */

  /* Wonder if anything else in the hbin header matters? */

  /* Set whole hbin to be one contious unused block */
  newbin->firstlink = (r - 0x20 - 0);  /* Positive linkage = unused */

  /* Update REGF header */
  hdr = (struct regf_header *) hdesc->buffer;
  hdr->filesize = newbinofs + r - 0x1000;               /* Point header to new end of data */

#ifdef ADDBIN_DEBUG
  RTPrintf("add_bin: adjusting size field in REGF: %d [%x]\n",hdr->filesize,hdr->filesize);
#endif

  /* Update state */

  hdesc->state |= HMODE_DIDEXPAND | HMODE_DIRTY;
  hdesc->lastbin = newbinofs;  /* Last bin */
  hdesc->endofs = newbinofs + r;   /* New data end */

 return(newbinofs + 0x20);

}



/* Allocate a block of requested size if possible
 * hdesc - hive
 * pofs - If >0 will try this page first (ptr may be inside page)
 * size - number of bytes to allocate
 * returns: 0 - failed, else pointer to allocated block.
 * WARNING: Will realloc() buffer if it has to be expanded!
 * ALL POINTERS TO BUFFER IS INVALID AFTER THAT. (offsets are still correct)
 * Guess I'd better switch to mmap() one day..
 * This function WILL CHANGE THE HIVE (change block linkage) if it
 * succeeds.
 */

static int alloc_block(struct hive *hdesc, int ofs, int size)
{
  int pofs = 0;
  int blk = 0;
  int newbin;
  int trail, trailsize, oldsz;

  if (hdesc->state & HMODE_NOALLOC) {
    RTPrintf("\nERROR: alloc_block: Hive <%s> is in no allocation safe mode,"
	   "new space not allocated. Operation will fail!\n", hdesc_filename(hdesc));
    return(0);
  }

  size += 4;  /* Add linkage */
  if (size & 7) size += (8 - (size & 7));

  /* Check current page first */
  if (ofs) {
    pofs = find_page_start(hdesc,ofs);
    blk = find_free_blk(hdesc,pofs,size);
  }

  /* Then check whole hive */
  if (!blk) {
    blk = find_free(hdesc,size);
  }

  if (blk) {  /* Got the space */
    oldsz = get_int(hdesc->buffer+blk);
#if 0
    RTPrintf("Block at         : %x\n",blk);
    RTPrintf("Old block size is: %x\n",oldsz);
    RTPrintf("New block size is: %x\n",size);
#endif
    trailsize = oldsz - size;

    if (trailsize == 4) {
      trailsize = 0;
      size += 4;
    }

 #if 1
    if (trailsize & 7) { /* Trail must be 8 aligned */
      trailsize -= (8 - (trailsize & 7));
      size += (8 - (trailsize & 7));
    }
    if (trailsize == 4) {
      trailsize = 0;
      size += 4;
    }
#endif

#if 0
    RTPrintf("trail after comp: %x\n",trailsize);
    RTPrintf("size  after comp: %x\n",size);
#endif

    /* Now change pointers on this to reflect new size */
    *(int *)((hdesc->buffer)+blk) = -(size);
    /* If the fit was exact (unused block was same size as wee need)
     * there is no need for more, else make free block after end
     * of newly allocated one */

    hdesc->useblk++;
    hdesc->unuseblk--;
    hdesc->usetot += size;
    hdesc->unusetot -= size;

    if (trailsize) {
      trail = blk + size;

      *(int *)((hdesc->buffer)+trail) = (int)trailsize;

      hdesc->useblk++;    /* This will keep blockcount */
      hdesc->unuseblk--;
      hdesc->usetot += 4; /* But account for more linkage bytes */
      hdesc->unusetot -= 4;

    }  
    /* Clear the block data, makes it easier to debug */
#if ZEROFILL
    memset((void *)(hdesc->buffer+blk+4), 0, size-4);
#endif

    hdesc->state |= HMODE_DIRTY;

#if 0
    RTPrintf("alloc_block: returning %x\n",blk);
#endif
    return(blk);
  } else {
    RTPrintf("alloc_block: failed to alloc %d bytes, trying to expand hive..\n",size);

    newbin = add_bin(hdesc,size);
    if (newbin) return(alloc_block(hdesc,newbin,size)); /* Nasty... recall ourselves. */
    /* Fallthrough to fail if add_bin fails */
  }
  return(0);
}

/* Free a block in registry
 * hdesc - hive
 * blk   - offset of block, MUST POINT TO THE LINKAGE!
 * returns bytes freed (incl linkage bytes) or 0 if fail
 * Will CHANGE HIVE IF SUCCESSFUL (changes linkage)
 */

#define FB_DEBUG 0

static int free_block(struct hive *hdesc, int blk)
{
  int pofs,vofs,seglen,prev=0,next,nextsz,prevsz,size;
  struct hbin_page *p;

  if (hdesc->state & HMODE_NOALLOC) {
    RTPrintf("free_block: ERROR: Hive %s is in no allocation safe mode,"
	   "space not freed. Operation will fail!\n", hdesc_filename(hdesc));
    return(0);
  }

  size = get_int(hdesc->buffer+blk);
  if (size >= 0) {
    RTPrintf("free_block: trying to free already free block!\n");
#ifdef DOCORE
      RTPrintf("blk = %x\n",blk);
      debugit(hdesc->buffer,hdesc->size);
      abort();
#endif
    return(0);
  }
  size = -size;

  /* So, we must find start of the block BEFORE us */
  pofs = find_page_start(hdesc,blk);
  if (!pofs) return(0);

  p = (struct hbin_page *)(hdesc->buffer + pofs);
  vofs = pofs + 0x20;

  prevsz = -32;

  if (vofs != blk) {  /* Block is not at start of page? */
    while (vofs-pofs < (p->ofs_next - HBIN_ENDFILL) ) {

      seglen = get_int(hdesc->buffer+vofs);  
      
      if (seglen == 0) {
	RTPrintf("free_block: EEEK! Zero data block size! (not registry or corrupt file?)\n");
	debugit(hdesc->buffer,hdesc->size);
	return(0);
      }
      
      if (seglen < 0) {
	seglen = -seglen;
	/*      hexdump(hdesc->buffer,vofs,vofs+seglen+4,1); */
      } 
      prev = vofs;
      vofs += seglen;
      if (vofs == blk) break;
    }
    
    if (vofs != blk) {
      RTPrintf("free_block: ran off end of page!?!? Error in chains?\n");
#ifdef DOCORE
      RTPrintf("vofs = %x, pofs = %x, blk = %x\n",vofs,pofs,blk);
      debugit(hdesc->buffer,hdesc->size);
      abort();
#endif
      return(0);
    }
    
    prevsz = get_int(hdesc->buffer+prev);
    
  }

  /* We also need details on next block (unless at end of page) */
  next = blk + size;

  nextsz = 0;
  if (next-pofs < (p->ofs_next - HBIN_ENDFILL) ) nextsz = get_int(hdesc->buffer+next);

#if 0
  RTPrintf("offset prev : %x , blk: %x , next: %x\n",prev,blk,next);
  RTPrintf("size   prev : %x , blk: %x , next: %x\n",prevsz,size,nextsz);
#endif

  /* Now check if next block is free, if so merge it with the one to be freed */
  if ( nextsz > 0) {
#if 0
    RTPrintf("Swallow next\n");
#endif
    size += nextsz;   /* Swallow it in current block */
    hdesc->useblk--;
    hdesc->usetot -= 4;
    hdesc->unusetot -= 4;   /* FIXME !??!?? */
  }

  /* Now free the block (possibly with ajusted size as above) */
#if ZEROFILL
   memset((void *)(hdesc->buffer+blk), 0, size);
#endif

  *(int *)((hdesc->buffer)+blk) = (int)size;
  hdesc->usetot -= size;
  hdesc->unusetot -= size;  /* FIXME !?!? */
  hdesc->unuseblk--;

  hdesc->state |= HMODE_DIRTY;
 
  /* Check if previous block is also free, if so, merge.. */
  if (prevsz > 0) {
#if 0
    RTPrintf("Swallow prev\n");
#endif
    hdesc->usetot -= prevsz;
    hdesc->unusetot += prevsz;
    prevsz += size;
    /* And swallow current.. */
#if ZEROFILL
      memset((void *)(hdesc->buffer+prev), 0, prevsz);
#endif
    *(int *)((hdesc->buffer)+prev) = (int)prevsz;
    hdesc->useblk--;
    return(prevsz);
  }
  return(size);
}


/*
 * converts a value string from an registry entry into a c string. It does not
 * use any encoding functions.
 * It works very primitive by just taking every second char.
 * The caller must free the resulting string, that was allocated with malloc.
 *
 * string:  string where every second char is \0
 * len:     length of the string
 * return:  the converted string as char*
 */
static char *
string_regw2prog(void *string, int len)
{
    int i, k;
    char *cstring;

    int out_len = 0;
    for(i = 0; i < len; i += 2)
    {
        unsigned v = ((unsigned char *)string)[i] + ((unsigned char *)string)[i+1] * 256u;
        if (v < 128)
            out_len += 1;
        else if(v < 0x800)
            out_len += 2;
        else
            out_len += 3;
    }
    CREATE(cstring,char,out_len+1);

    for(i = 0, k = 0; i < len; i += 2)
    {
        unsigned v = ((unsigned char *)string)[i] + ((unsigned char *)string)[i+1] * 256u;
        if (v < 128)
            cstring[k++] = v;
        else if(v < 0x800) {
            cstring[k++] = 0xc0 | (v >> 6);
            cstring[k++] = 0x80 | (v & 0x3f);
        } else {
            cstring[k++] = 0xe0 | (v >> 12);
            cstring[k++] = 0x80 | ((v >> 6) & 0x3f);
            cstring[k++] = 0x80 | (v & 0x3f);
        }
    }
    cstring[out_len] = '\0';

    return cstring;
}




/* ================================================================ */

/* ** Registry manipulation routines ** */



/* "directory scan", return next name/pointer of a subkey on each call
 * nkofs = offset to directory to scan
 * lfofs = pointer to int to hold the current scan position,
 *         set position to 0 to start.
 * sptr  = pointer to struct to hold a single result
 * returns: -1 = error. 0 = end of key. 1 = more subkeys to scan
 * NOTE: caller must free the name-buffer (struct ex_data *name)
 */
static int ex_next_n(struct hive *hdesc, int nkofs, int *count, int *countri, struct ex_data *sptr)
{
  struct nk_key *key, *newnkkey;
  int newnkofs;
  struct lf_key *lfkey;
  struct li_key *likey;
  struct ri_key *rikey;


  if (!nkofs) return(-1);
  key = (struct nk_key *)(hdesc->buffer + nkofs);
  if (key->id != NTREG_ID_NK_KEY) {
    RTPrintf("ex_next error: Not a 'nk' node at 0x%0x\n",nkofs);
    return(-1);
  }

#define EXNDEBUG 0

  lfkey = (struct lf_key *)(hdesc->buffer + key->ofs_lf + 0x1004);
  rikey = (struct ri_key *)(hdesc->buffer + key->ofs_lf + 0x1004);

  if (rikey->id == 0x6972) {   /* Is it extended 'ri'-block? */
#if EXNDEBUG
    RTPrintf("%d , %d\n",*countri,*count);
#endif
    if (*countri < 0 || *countri >= rikey->no_lis) { /* End of ri's? */
      return(0);
    }
    /* Get the li of lf-struct that's current based on countri */
    likey = (struct li_key *)( hdesc->buffer + rikey->hash[*countri].ofs_li + 0x1004 ) ;
    if (likey->id == 0x696c) {
      newnkofs = likey->hash[*count].ofs_nk + 0x1000;
    } else {
      lfkey = (struct lf_key *)( hdesc->buffer + rikey->hash[*countri].ofs_li + 0x1004 ) ;
      newnkofs = lfkey->hash[*count].ofs_nk + 0x1000;
    }

    /* Check if current li/lf is exhausted */
#if EXNDEBUG
    RTPrintf("likey->no_keys = %d\n",likey->no_keys);
#endif
    if (*count >= likey->no_keys-1) { /* Last legal entry in li list? */
      (*countri)++;  /* Bump up ri count so we take next ri entry next time */
      (*count) = -1;  /* Reset li traverse counter for next round, not used later here */
    }
  } else { /* Plain handler */
    if (key->no_subkeys <= 0 || *count >= key->no_subkeys) {
      return(0);
    }
    if (lfkey->id == 0x696c) {   /* Is it 3.x 'li' instead? */
      likey = (struct li_key *)(hdesc->buffer + key->ofs_lf + 0x1004);
      newnkofs = likey->hash[*count].ofs_nk + 0x1000;
    } else {
      newnkofs = lfkey->hash[*count].ofs_nk + 0x1000;
    }
  }

  sptr->nkoffs = newnkofs;
  newnkkey = (struct nk_key *)(hdesc->buffer + newnkofs + 4);
  sptr->nk = newnkkey;

  if (newnkkey->id != NTREG_ID_NK_KEY) {
    RTPrintf("ex_next: ERROR: not 'nk' node at 0x%0x\n",newnkofs);

    return(-1);
  } else {
    if (newnkkey->len_name <= 0) {
      RTPrintf("ex_next: nk at 0x%0x has no name!\n",newnkofs);
    } else if (newnkkey->type & 0x20) {
#if 0
      RTPrintf("dummy1 %x\n", *((int*)newnkkey->dummy1));
      RTPrintf("dummy2 %x\n", *((int*)newnkkey->dummy2));
      RTPrintf("type %x\n", newnkkey->type);
      RTPrintf("timestamp+8 %x\n", *((int*)(newnkkey->timestamp+8)));
      RTPrintf("dummy3+0 %x\n", *((int*)(newnkkey->dummy3+0)));
      RTPrintf("dummy3+4 %x\n", *((int*)(newnkkey->dummy3+4)));
      RTPrintf("dummy3+8 %x\n", *((int*)(newnkkey->dummy3+8)));
      RTPrintf("dummy3+12 %x\n", *((int*)(newnkkey->dummy3+12)));
      RTPrintf("dummy4 %x\n", *((int*)&newnkkey->dummy4));
      RTPrintf("len %d\n", newnkkey->len_name);
      RTPrintf("len class %d\n", newnkkey->len_classnam);
      fflush(stdout);
#endif

      sptr->name =  mem_str(newnkkey->keyname,newnkkey->len_name);
      //      sptr->name = string_rega2prog(newnkkey->keyname, newnkkey->len_name);
    } else {
      sptr->name = string_regw2prog(newnkkey->keyname, newnkkey->len_name);
    }
  } /* if */
  (*count)++;
  return(1);
  /*  return( *count <= key->no_subkeys); */
}

static int interpret_vk(struct hive *hdesc, struct vex_data *sptr)
{
  struct vk_key *vkkey;

  vkkey = (struct vk_key *)(hdesc->buffer + sptr->vkoffs);
  if (vkkey->id != NTREG_ID_VK_KEY) {
    RTPrintf("interpret_vk: not a valuekey (vk) node at offs 0x%0x\n",sptr->vkoffs);
    return(-1);
  }

  /*  parse_vk(hdesc, sptr->vkoffs, 4); */

  sptr->vk = vkkey;
  sptr->name = 0;

  if (vkkey->len_name >0) {
    if (vkkey->flag & 1) {

      sptr->name = mem_str(vkkey->keyname, vkkey->len_name);
      //      sptr->name = string_rega2prog(vkkey->keyname, vkkey->len_name);
    } else {
      sptr->name = string_regw2prog(vkkey->keyname, vkkey->len_name);
    }
  } else {
    sptr->name = strdup("");
  }

  sptr->type = vkkey->val_type;
  sptr->size = (vkkey->len_data & 0x7fffffff);
  sptr->val = (uint8_t *)(hdesc->buffer + vkkey->ofs_data + 0x1004);

  if (sptr->size) {
    if (vkkey->val_type == REG_DWORD) {
      if (vkkey->len_data & 0x80000000) {
        sptr->val = (uint8_t *)&vkkey->ofs_data;
        sptr->size = sizeof(vkkey->ofs_data);
      }
    }
  }
 else if (vkkey->len_data == 0x80000000) { 
    /* Data SIZE is 0, high bit set: special inline case, data is DWORD and in TYPE field!! */
    /* Used a lot in SAM, and maybe in SECURITY I think */
    sptr->val = (uint8_t *)&vkkey->val_type;
    sptr->size = sizeof(vkkey->val_type);
    sptr->type = REG_DWORD;
  }

  return 0;
}

/* "directory scan" for VALUES, return next name/pointer of a value on each call
 * nkofs = offset to directory to scan
 * lfofs = pointer to int to hold the current scan position,
 *         set position to 0 to start.
 * sptr  = pointer to struct to hold a single result
 * returns: -1 = error. 0 = end of key. 1 = more values to scan
 * NOTE: caller must free the name-buffer (struct vex_data *name)
 */
static int ex_next_v(struct hive *hdesc, int nkofs, int *count, struct vex_data *sptr)
{
  struct nk_key *key /* , *newnkkey */ ;
  int vkofs,vlistofs;
  int *vlistkey;


  if (!nkofs) return(-1);
  key = (struct nk_key *)(hdesc->buffer + nkofs);
  if (key->id != NTREG_ID_NK_KEY) {
    RTPrintf("ex_next_v error: Not a 'nk' node at 0x%0x\n",nkofs);
    return(-1);
  }

  if (key->no_values <= 0 || *count >= key->no_values) {
    return(0);
  }

  vlistofs = key->ofs_vallist + 0x1004;
  vlistkey = (int *)(hdesc->buffer + vlistofs);

  vkofs = vlistkey[*count] + 0x1004;

  sptr->vkoffs = vkofs;
  if ( interpret_vk(hdesc, sptr) )
    return -1;

  (*count)++;
  return( *count <= key->no_values );
}

#ifdef WRITE_HIVE
/* traceback - trace nk's back to root,
 * building path string as we go.
 * nkofs  = offset to nk-node
 * path   = pointer to pathstring-buffer
 * maxlen = max length of path-buffer
 * return: length of path string
 */
static int get_abs_path(struct hive *hdesc, int nkofs, char *path, int maxlen)
{
  /* int newnkofs; */
  struct nk_key *key;
  char tmp[ABSPATHLEN+1];
  char *keyname;
  int len_name;

  maxlen = (maxlen < ABSPATHLEN ? maxlen : ABSPATHLEN);

  key = (struct nk_key *)(hdesc->buffer + nkofs);
  
  if (key->id != NTREG_ID_NK_KEY) {
    RTPrintf("get_abs_path: Not a 'nk' node!\n");
    return(0);
  }

  if (key->type == KEY_ROOT) {   /* We're at the root */
    return(strlen(path));
  }

  strncpy(tmp,path,ABSPATHLEN-1);

  if (key->type & 0x20)
    keyname = mem_str(key->keyname, key->len_name);
  //    keyname = string_rega2prog(key->keyname, key->len_name);
  else
    keyname = string_regw2prog(key->keyname, key->len_name);
  len_name = strlen(keyname);
  if ( (strlen(path) + len_name) >= maxlen-6) {
    free(keyname);
    RTStrPrintf(path,maxlen,"(...)%s",tmp);
    return(strlen(path));   /* Stop trace when string exhausted */
  }
  *path = '\\';
  memcpy(path+1,keyname,len_name);
  free(keyname);
  strncpy(path+len_name+1,tmp,maxlen-6-len_name);
  return(get_abs_path(hdesc, key->ofs_parent+0x1004, path, maxlen)); /* go back one more */
}
#endif  /* WRITE_HIVE */


/* Value index table lookup
 * hdesc - hive as usual
 * vlistofs - offset of table
 * name - value name to look for
 * returns index into table or -1 if err
 */

static int vlist_find(struct hive *hdesc, int vlistofs, int numval, char *name, int type)
{
  struct vk_key *vkkey;
  int i,vkofs,len;
  int32_t *vlistkey;
  int approx = -1;

  len = strlen(name);
  vlistkey = (int32_t *)(hdesc->buffer + vlistofs);

  //  RTPrintf("vlist_find: <%s> len = %d\n",name,len);

  for (i = 0; i < numval; i++) {
    vkofs = vlistkey[i] + 0x1004;
    vkkey = (struct vk_key *)(hdesc->buffer + vkofs);
    if (vkkey->len_name == 0 && *name == '@' && len == 1) { /* @ is alias for nameless value */
      return(i);
    }

    // RTPrintf("vlist_find: matching against: <%s> len = %d\n",vkkey->keyname,vkkey->len_name);

    if ( (type & TPF_EXACT) && vkkey->len_name != len ) continue;  /* Skip if exact match and not exact size */

    if ( vkkey->len_name >= len ) {                  /* Only check for names that are longer or equal than we seek */
      if ( !strncmp(name, vkkey->keyname, len) ) {    /* Name match */
	if (vkkey->len_name == len) return(i);        /* Exact match always best, returns */
	if (approx == -1) approx = i;                 /* Else remember first partial match */
      }
    }

  }
  return(approx);

}

/* de-escape a string, handling \ backslash
 * s = string buffer, WILL BE CHANGED
 * wide = true to make it handle wide characters
 * returns new lenght
 */

static int de_escape(char *s, int wide)
{
  int src = 0;
  int dst = 0;

  if (wide) {
    while ( *(s + src) || *(s + src +1)) {
      if ( *(s + src) == '\\' && *(s + src + 1) == 0) src += 2; /* Skip over backslash */
      *(s + dst) = *(s + src);
      *(s + dst + 1) = *(s + src + 1);
      dst += 2;
      src += 2;
    }
    *(s + dst) = 0;
    *(s + dst + 1) = 0;
    dst += 2;
  } else {
    while ( *(s + src) ) {
      if ( *(s + src) == '\\' ) src++;
      *(s + dst) = *(s + src);
      dst++;
      src++;
    }
    *(s + dst) = 0;
    dst++;
  }

  return(dst);
}

/* Recursevely follow 'nk'-nodes based on a path-string,
 * returning offset of last 'nk' or 'vk'
 * vofs - offset to start node
 * path - null-terminated pathname (relative to vofs, \ is separator)
 * type - type to return TPF_??, see ntreg.h
 * return: offset to nk or vk (or NULL if not found)
 */

static int trav_path(struct hive *hdesc, int vofs, const char *path, int type)
{
  struct nk_key *key, *newnkkey;
  struct lf_key *lfkey;
  struct li_key *likey;
  struct ri_key *rikey;

  int32_t *vlistkey;
  int newnkofs, plen=0, i, lfofs, vlistofs, adjust=0, r, ricnt, subs;
  char *buf;
  char part[ABSPATHLEN+1];
  char *partptr = NULL;

  if (!hdesc) return(0);
  buf = hdesc->buffer;

  //  RTPrintf("trav_path: called with vofs = %x, path = <%s>, type = %x\n",vofs, path, type);


  if (!vofs) vofs = hdesc->rootofs+4;     /* No current key given , so start at root */

  if ( !(type & TPF_ABS) && *path == '\\' && *(path+1) != '\\') {      /* Start from root if path starts with \ */
    path++;
    vofs = hdesc->rootofs+4;
  }

  key = (struct nk_key *)(buf + vofs);
  //  RTPrintf("check of nk at offset: 0x%0x\n",vofs); 

  if (key->id != NTREG_ID_NK_KEY) {
    RTPrintf("trav_path: Error: Not a 'nk' node!\n");
    return(0);
  }

  if ( !(type & TPF_ABS)) {  /* Only traverse path if not absolute literal value name passed */

    /* TODO: Need to rethink this.. */

    /* Find \ delimiter or end of string, copying to name part buffer as we go,
       rewriting double \\s */
    partptr = part;
    for(plen = 0; path[plen] && (path[plen] != '\\' || path[plen+1] == '\\'); plen++) {
      if (path[plen] == '\\' && path[plen+1] == '\\') plen++; /* Skip one if double */
      *partptr++ = path[plen];
    }
    *partptr = '\0';

#if 0    
      RTPrintf("Name part: <%s>\n",part); 
      RTPrintf("Name path: <%s>\n",path); 
#endif

    adjust = (path[plen] == '\\' ) ? 1 : 0;
    // RTPrintf("Checking for <%s> with len %d\n",path,plen); 

    if (!plen) return(vofs-4);     /* Path has no lenght - we're there! */

    if ( (plen == 1) && (*(path+1) && *path == '.') && !(type & TPF_EXACT)) {     /* Handle '.' current dir */
      //  RTPrintf("** handle current\n");
      return(trav_path(hdesc,vofs,path+plen+adjust,type));
    }
    if ( !(type & TPF_EXACT) && (plen == 2) && !strncmp("..",path,2) ) { /* Get parent key */
      newnkofs = key->ofs_parent + 0x1004;
      /* Return parent (or only root if at the root) */
      return(trav_path(hdesc, (key->type == KEY_ROOT ? vofs : newnkofs), path+plen+adjust, type));
    }
    
  }

  /* at last name of path, and we want vk, and the nk has values */
  if ((type & TPF_VK_ABS) || (!path[plen] && (type & TPF_VK) && key->no_values) ) {   
    //  if ( (!path[plen] && (type & TPF_VK) && key->no_values) ) {   
    if (type & TPF_ABS) {
      strcpy(part, path);
      plen = de_escape(part,0);
      partptr = part + plen;
    }

    //RTPrintf("VK namematch for <%s>, type = %d\n",part,type);
    vlistofs = key->ofs_vallist + 0x1004;
    vlistkey = (int32_t *)(buf + vlistofs);
    i = vlist_find(hdesc, vlistofs, key->no_values, part, type);
    if (i != -1) {
      return(vlistkey[i] + 0x1000);
    }
  }

  if (key->no_subkeys > 0) {    /* If it has subkeys, loop through the hash */
    char *partw = NULL;
    int partw_len, part_len;

    // RTPrintf("trav_path: subkey loop: path = %s, part = %s\n",path,part);

    lfofs = key->ofs_lf + 0x1004;    /* lf (hash) record */
    lfkey = (struct lf_key *)(buf + lfofs);

    if (lfkey->id == 0x6972) { /* ri struct need special parsing */
      /* Prime loop state */

      rikey = (struct ri_key *)lfkey;
      ricnt = rikey->no_lis;
      r = 0;
      likey = (struct li_key *)( hdesc->buffer + rikey->hash[r].ofs_li + 0x1004 ) ;
      subs = likey->no_keys;
      if (likey->id != 0x696c) {  /* Bwah, not li anyway, XP uses lh usually which is actually smarter */
	lfkey = (struct lf_key *)( hdesc->buffer + rikey->hash[r].ofs_li + 0x1004 ) ;
	likey = NULL;
      }
    } else {
      if (lfkey->id == 0x696c) { /* li? */
	likey = (struct li_key *)(buf + lfofs);
      } else {
	likey = NULL;
      }
      rikey = NULL;
      ricnt = 0; r = 0; subs = key->no_subkeys;
    }

    partw = string_prog2regw(part, partptr-part, &partw_len);
    //    string_prog2rega(part, partptr-part);
    part_len = strlen(part);
    do {
      for(i = 0; i < subs; i++) {
	if (likey) newnkofs = likey->hash[i].ofs_nk + 0x1004;
	else newnkofs = lfkey->hash[i].ofs_nk + 0x1004;
	newnkkey = (struct nk_key *)(buf + newnkofs);
	if (newnkkey->id != NTREG_ID_NK_KEY) {
	  RTPrintf("ERROR: not 'nk' node! (strange?)\n");
	} else {
	  if (newnkkey->len_name <= 0) {
	    RTPrintf("[No name]\n");
	  } else if ( 
		     ( ( part_len <= newnkkey->len_name ) && !(type & TPF_EXACT) ) ||
		     ( ( part_len == newnkkey->len_name ) && (type & TPF_EXACT)  )
		      ) {
	    /* Can't match if name is shorter than we look for */
            int cmp;
	    //	    RTPrintf("trav_path: part = <%s>, part_len = %d\n",part,part_len);
	    if (newnkkey->type & 0x20) 
              cmp = strncmp(part,newnkkey->keyname,part_len);
            else
              cmp = memcmp(partw, newnkkey->keyname, partw_len);
	    if (!cmp) {
	      //  RTPrintf("Key at 0x%0x matches! recursing! new path = %s\n",newnkofs,path+plen+adjust); 
	      free(partw);
	      return(trav_path(hdesc, newnkofs, path+plen+adjust, type));
	    }
	  }
	} /* if id OK */
      } /* hash loop */
      r++;
      if (ricnt && r < ricnt) {
	newnkofs = rikey->hash[r].ofs_li;
	likey = (struct li_key *)( hdesc->buffer + newnkofs + 0x1004 ) ;
	subs = likey->no_keys;
	if (likey->id != 0x696c) {  /* Bwah, not li anyway, XP uses lh usually which is actually smarter */
	  lfkey = (struct lf_key *)( hdesc->buffer + rikey->hash[r].ofs_li + 0x1004 ) ;
	  likey = NULL;
	}
      }
    } while (r < ricnt && ricnt);
    free(partw);

  } /* if subkeys */

  /* Not found */
  return(0);
}


#ifdef WRITE_HIVE
/* ls - list a 'nk' nodes subkeys and values
 * vofs - offset to start of data (skipping block linkage)
 * type - 0 = full, 1 = keys only. 2 = values only
 */
static void nk_ls(struct hive *hdesc, const char *path, int vofs, int type)
{
  struct nk_key *key;
  int nkofs;
  struct ex_data ex = {};
  struct vex_data vex = {};
  int count = 0, countri = 0;
  

  nkofs = trav_path(hdesc, vofs, path, 0);

  if(!nkofs) {
    RTPrintf("nk_ls: Key <%s> not found\n",path);
    return;
  }
  nkofs += 4;

  key = (struct nk_key *)(hdesc->buffer + nkofs);
  VERBF(hdesc,"ls of node at offset 0x%0x\n",nkofs);

  if (key->id != NTREG_ID_NK_KEY) {
    RTPrintf("Error: Not a 'nk' node!\n");

    debugit(hdesc->buffer,hdesc->size);
    
  }
  
  RTPrintf("Node has %d subkeys and %d values",key->no_subkeys,key->no_values);
  if (key->len_classnam) RTPrintf(", and class-data of %d bytes",key->len_classnam);
  RTPrintf("\n");

  if (key->no_subkeys) {
    RTPrintf("  key name\n");
    while ((ex_next_n(hdesc, nkofs, &count, &countri, &ex) > 0)) {
      if (!(hdesc->state & HMODE_VERBOSE)) RTPrintf("%c <%s>\n", (ex.nk->len_classnam)?'*':' ',ex.name);
      else RTPrintf("[%6x] %c <%s>\n", ex.nkoffs, (ex.nk->len_classnam)?'*':' ',ex.name);
      FREE(ex.name);
    }
  }
  count = 0;
  if (key->no_values) {
    RTPrintf("  size     type            value name             [value if type DWORD]\n");
    while ((ex_next_v(hdesc, nkofs, &count, &vex) > 0)) {
      if (hdesc->state & HMODE_VERBOSE) RTPrintf("[%6x] %6d  %-16s  <%s>", vex.vkoffs - 4, vex.size,
					       (vex.type < REG_MAX ? val_types[vex.type] : "(unknown)"), vex.name);
      else
      RTPrintf("%6d  %-16s  <%s>", vex.size,
	     (vex.type < REG_MAX ? val_types[vex.type] : "(unknown)"), vex.name);

      if (vex.type == REG_DWORD) RTPrintf(" %*s [%p]",25-(int)strlen(vex.name), vex.val , vex.val);
      RTPrintf("\n");
      FREE(vex.name);
    }
  }
}

/* Get the type of a value */
static int get_val_type(struct hive *hdesc, int vofs, char *path, int exact)
{
  struct vk_key *vkkey;
  int vkofs;

  vkofs = trav_path(hdesc, vofs,path,exact | TPF_VK);
  if (!vkofs) {
    return -1;
  }
  vkofs +=4;
  vkkey = (struct vk_key *)(hdesc->buffer + vkofs);

  return(vkkey->val_type);
}
#endif  /* WRITE_HIVE */


/* Get len of a value, given current key + path */
static int get_val_len(struct hive *hdesc, int vofs, char *path, int exact)
{
  struct vk_key *vkkey;
  int vkofs;
  int len;

  vkofs = trav_path(hdesc, vofs,path,exact | TPF_VK);
  if (!vkofs) {
    return -1;
  }
  vkofs +=4;
  vkkey = (struct vk_key *)(hdesc->buffer + vkofs);

  len = vkkey->len_data & 0x7fffffff;

  if ( vkkey->len_data == 0x80000000 && (exact & TPF_VK_SHORT)) {  /* Special inline case, return size of 4 (dword) */
    len = 4;
  }

  return len;
}

/* Get void-pointer to value-data, also if inline.
 * If val_type != 0 a check for correct value type is done
 * Caller must keep track of value's length (call function above to get it)
 */
static void *get_val_data(struct hive *hdesc, int vofs, char *path, int val_type, int exact)
{
  struct vk_key *vkkey;
  int vkofs;

  //  RTPrintf("get_val_data: path = %s\n",path);

  vkofs = trav_path(hdesc,vofs,path,exact | TPF_VK);
  if (!vkofs) {
    RTPrintf("get_val_data: %s not found\n",path);
    abort();
    return NULL;
  }
  vkofs +=4;
  vkkey = (struct vk_key *)(hdesc->buffer + vkofs);

  if (vkkey->len_data == 0) {
    return NULL;
  }

  if (vkkey->len_data == 0x80000000 /*&& (exact & TPF_VK_SHORT)*/) {  /* Special inline case (len = 0x80000000) */
    return(&vkkey->val_type); /* Data (4 bytes?) in type field */
  }    

  if (val_type && vkkey->val_type && (vkkey->val_type) != val_type) {
#if DOCORE
    abort();
#endif
    return NULL;
  }

  /* Negative len is inline, return ptr to offset-field which in
   * this case contains the data itself
   */
  if (vkkey->len_data & 0x80000000) return(&vkkey->ofs_data);
  /* Normal return, return data pointer */
  return(hdesc->buffer + vkkey->ofs_data + 0x1004);
}


#ifdef WRITE_HIVE
/* Get and copy key data (if any) to buffer
 * if kv==NULL will allocate needed return struct & buffer
 * else will use buffer allocated for it (if it fits)
 * return len+data or NULL if not found (or other error)
 * NOTE: caller must deallocate buffer! a simple free(keyval) will suffice.
 */
static struct keyval *get_val2buf(struct hive *hdesc, struct keyval *kv,
			   int vofs, char *path, int type, int exact )
{
  int l,i,parts,list,blockofs,blocksize,point,copylen,restlen;
  struct keyval *kr;
  void *keydataptr;
  struct db_key *db;
  void *addr;

  l = get_val_len(hdesc, vofs, path, exact);
  if (l == -1) return(NULL);  /* error */
  if (kv && (kv->len < l)) return(NULL); /* Check for overflow of supplied buffer */

  keydataptr = get_val_data(hdesc, vofs, path, type, exact);
  //  if (!keydataptr) return(NULL);

  /* Allocate space for data + header, or use supplied buffer */
  if (kv) {
    kr = kv;
  } else {
    ALLOC(kr,1,l*sizeof(int)+4);
  }

  kr->len = l;

  // RTPrintf("get_val2buf: keydataprtr = %x, l = %x\n",keydataptr,l);


  if (l > VAL_DIRECT_LIMIT) {       /* Where do the db indirects start? seems to be around 16k */
    db = (struct db_key *)keydataptr;
    if (db->id != 0x6264) abort();
    parts = db->no_part;
    list = db->ofs_data + 0x1004;
    RTPrintf("get_val2buf: Long value: parts = %d, list = %x\n",parts,list);

    point = 0;
    restlen = l;
    for (i = 0; i < parts; i++) {
      blockofs = get_int(hdesc->buffer + list + (i << 2)) + 0x1000;
      blocksize = -get_int(hdesc->buffer + blockofs) - 8;

      /* Copy this part, up to size of block or rest lenght in last block */
      copylen = (blocksize > restlen) ? restlen : blocksize;

      RTPrintf("get_val2buf: Datablock %d offset %x, size %x (%d)\n",i,blockofs,blocksize,blocksize);
      RTPrintf("             : Point = %x, restlen = %x, copylen = %x\n",point,restlen,copylen);

      addr = (void *)&(kr->data) + point;
      memcpy( addr, hdesc->buffer + blockofs + 4, copylen);

      //      debugit((char *)&(kr->data), l);
      
      point += copylen;
      restlen -= copylen;

    }


  } else {    
    if (l && kr && keydataptr) memcpy(&(kr->data), keydataptr, l);
  }

  return(kr);
}

/* DWORDs are so common that I make a small function to get it easily */

static int get_dword(struct hive *hdesc, int vofs, char *path, int exact)
{
  struct keyval *v;
  int dword;

  v = get_val2buf(hdesc, NULL, vofs, path, REG_DWORD, exact | TPF_VK);
  if (!v) return(-1); /* well... -1 COULD BE THE STORED VALUE TOO */

  dword = (int)v->data;

  FREE(v);

  return(dword);
  
}
#endif  /* WRITE_HIVE */

/* Sanity checker when transferring data into a block
 * ofs = offset to data block, point to start of actual datablock linkage
 * data = data to copy
 * size = size of data to copy
 */

static int fill_block(struct hive *hdesc, int ofs, void *data, int size)
{
  int blksize;

  blksize = get_int(hdesc->buffer + ofs);
  blksize = -blksize;

#if 0
  RTPrintf("fill_block: ofs = %x - %x, size = %x, blksize = %x\n",ofs,ofs+size,size,blksize);
#endif
  /*  if (blksize < size || ( (ofs & 0xfffff000) != ((ofs+size) & 0xfffff000) )) { */
  if (blksize < size) {
    RTPrintf("fill_block: ERROR: block to small for data: ofs = %x, size = %x, blksize = %x\n",ofs,size,blksize);
    debugit(hdesc->buffer,hdesc->size);
    abort();
  }

  memcpy(hdesc->buffer + ofs + 4, data, size);
  return(0);
}


/* Free actual data of a value, and update value descriptor
 * hdesc - hive
 * vofs  - current value offset
 */

static int free_val_data(struct hive *hdesc, int vkofs)
{
  struct vk_key *vkkey;
  struct db_key *db;
  int len,i,blockofs,blocksize,parts,list;


  vkkey = (struct vk_key *)(hdesc->buffer + vkofs);

  len = vkkey->len_data;

  if (!(len & 0x80000000)) {  /* Check for inline, if so, skip it, nothing to do */ 

    if (len > VAL_DIRECT_LIMIT) {       /* Where do the db indirects start? seems to be around 16k */

      db = (struct db_key *)(hdesc->buffer + vkkey->ofs_data + 0x1004);
      
      if (db->id != 0x6264) abort();
      
      parts = db->no_part;
      list = db->ofs_data + 0x1004;
      
      RTPrintf("free_val_data: Long value: parts = %d, list = %x\n",parts,list);
      
      for (i = 0; i < parts; i++) {
	blockofs = get_int(hdesc->buffer + list + (i << 2)) + 0x1000;
	blocksize = -get_int(hdesc->buffer + blockofs);
	RTPrintf("free_val_data: Freeing long datablock %d offset %x, size %x (%d)\n",i,blockofs,blocksize,blocksize);
	free_block(hdesc, blockofs);		
      }
      
      RTPrintf("free_val_data: Freeing indirect list at %x\n", list-4);
      free_block(hdesc, list - 4);
      RTPrintf("free_val_data: Freeing db structure at %x\n", vkkey->ofs_data + 0x1000);
    } /* Fall through to regular which deallocs data or db block ofs_data point to */
      
    if (len) free_block(hdesc, vkkey->ofs_data + 0x1000);  
      
  } /* inline check */


  vkkey->len_data = 0;
  vkkey->ofs_data = 0;

  return(vkofs);

}


/* Allocate data for value. Frees old data (if any) which will be destroyed
 * hdesc - hive
 * vofs  - current key
 * path  - path to value
 * size  - size of data
 * Returns: 0 - error, >0 pointer to actual dataspace
 */

static int alloc_val_data(struct hive *hdesc, int vofs, char *path, int size,int exact)
{
  struct vk_key *vkkey;
  struct db_key *db;
  int vkofs,dbofs,listofs,blockofs,blocksize,parts;
  int datablk,i;
  int *ptr;

  vkofs = trav_path(hdesc,vofs,path,exact);
  if (!vkofs) {
    return (0);
  }

  vkofs +=4;
  vkkey = (struct vk_key *)(hdesc->buffer + vkofs);


  free_val_data(hdesc, vkofs);   /* Get rid of old data if any */

  /* Allocate space for new data */
  if (size > 4) {
    if (size > VAL_DIRECT_LIMIT) {  /* We must allocate indirect stuff *sigh* */
      parts = size / VAL_DIRECT_LIMIT + 1;
      RTPrintf("alloc_val_data: doing large key: size = %x (%d), parts = %d\n",size,size,parts);

      dbofs = alloc_block(hdesc, vkofs, sizeof(struct db_key));    /* Alloc db structure */
      db = (struct db_key *)(hdesc->buffer + dbofs + 4);
      db->id = 0x6264;
      db->no_part = parts;
      listofs = alloc_block(hdesc, vkofs, 4 * parts);  /* block offset list */
      db = (struct db_key *)(hdesc->buffer + dbofs + 4);
      db->ofs_data = listofs - 0x1000;
      RTPrintf("alloc_val_data: dbofs = %x, listofs = %x\n",dbofs,listofs);

      for (i = 0; i < parts; i++) {
	blocksize = VAL_DIRECT_LIMIT;      /* Windows seem to alway allocate the whole block */
	blockofs = alloc_block(hdesc, vkofs, blocksize);
	RTPrintf("alloc_val_data: block # %d, blockofs = %x\n",i,blockofs);	
	ptr = (int *)(hdesc->buffer + listofs + 4 + (i << 2));
	*ptr = blockofs - 0x1000;
      }
      datablk = dbofs;

    } else { /* Regular size < 16 k direct alloc */
      datablk = alloc_block(hdesc, vkofs, size);
    }



  } else { /* 4 bytes or less are inlined */
    datablk = vkofs + (ptrdiff_t)&(vkkey->ofs_data) - (ptrdiff_t)vkkey;
    size |= 0x80000000;
  }

  if (!datablk) return(0);

  vkkey = (struct vk_key *)(hdesc->buffer + vkofs); /* alloc_block may move pointer, realloc() buf */

  // RTPrintf("alloc_val_data: datablk = %x, size = %x, vkkey->len_data = %x\n",datablk, size, vkkey->len_data);



  /* Link in new datablock */
  if ( !(size & 0x80000000)) vkkey->ofs_data = datablk - 0x1000;
  vkkey->len_data = size;
 
  return(datablk + 4);
}


#ifdef WRITE_HIVE
/* Add a value to a key.
 * Just add the metadata (empty value), to put data into it, use
 * put_buf2val afterwards
 * hdesc - hive
 * nkofs - current key
 * name  - name of value
 * type  - type of value
 * returns: 0 err, >0 offset to value metadata
 */

static struct vk_key *add_value(struct hive *hdesc, int nkofs, char *name, int type)
{
  struct nk_key *nk;
  int oldvlist = 0, newvlist, newvkofs;
  struct vk_key *newvkkey;
  char *blank="";

  if (!name || !*name) return(NULL);


  nk = (struct nk_key *)(hdesc->buffer + nkofs);
  if (nk->id != NTREG_ID_NK_KEY) {
    RTPrintf("add_value: Key pointer not to 'nk' node!\n");
    return(NULL);
  }

  if (vlist_find(hdesc, nk->ofs_vallist + 0x1004, nk->no_values, name, TPF_EXACT) != -1) {
    RTPrintf("add_value: value %s already exists\n",name);
    return(NULL);
  }

  if (!strcmp(name,"@")) name = blank;
 
  if (nk->no_values) oldvlist = nk->ofs_vallist;

  newvlist = alloc_block(hdesc, nkofs, nk->no_values * 4 + 4);
  if (!newvlist) {
    RTPrintf("add_value: failed to allocate new value list!\n");
    return(NULL);
  }

  nk = (struct nk_key *)(hdesc->buffer + nkofs); /* In case buffer was moved.. */

  if (oldvlist) {   /* Copy old data if any */
    memcpy(hdesc->buffer + newvlist + 4, hdesc->buffer + oldvlist + 0x1004, nk->no_values * 4 + 4);
  }

  /* Allocate value descriptor including its name */
  newvkofs = alloc_block(hdesc, newvlist, sizeof(struct vk_key) + strlen(name));
  if (!newvkofs) {
    RTPrintf("add_value: failed to allocate value descriptor\n");
    free_block(hdesc, newvlist);
    return(NULL);
  }

  nk = (struct nk_key *)(hdesc->buffer + nkofs); /* In case buffer was moved.. */


  /* Success, now fill in the metadata */

  newvkkey = (struct vk_key *)(hdesc->buffer + newvkofs + 4);

  /* Add pointer in value list */
  *(int *)(hdesc->buffer + newvlist + 4 + (nk->no_values * 4)) = newvkofs - 0x1000;

  /* Fill in vk struct */
  newvkkey->id = NTREG_ID_VK_KEY;
  newvkkey->len_name = strlen(name);
  if (type == REG_DWORD || type == REG_DWORD_BIG_ENDIAN) {
    newvkkey->len_data = 0x80000004;  /* Prime the DWORD inline stuff */
  } else {
    newvkkey->len_data = 0x80000000;  /* Default inline zero size */
  }
  newvkkey->ofs_data = 0;
  newvkkey->val_type = type;
  newvkkey->flag     = newvkkey->len_name ? 1 : 0;  /* Seems to be 1, but 0 for no name default value */
  newvkkey->dummy1   = 0;
  memcpy((char *)&newvkkey->keyname, name, newvkkey->len_name);  /* And copy name */

  /* Finally update the key and free the old valuelist */
  nk->no_values++;
  nk->ofs_vallist = newvlist - 0x1000;
  if (oldvlist) free_block(hdesc,oldvlist + 0x1000);

  return(newvkkey);

}

/* Remove a vk-struct incl dataspace if any
 * Mostly for use by higher level stuff
 * hdesc - hive
 * vkofs - offset to vk
 */

static void del_vk(struct hive *hdesc, int vkofs)
{
  struct vk_key *vk;
;
  vk = (struct vk_key *)(hdesc->buffer + vkofs);
  if (vk->id != NTREG_ID_VK_KEY) {
    RTPrintf("del_vk: Key pointer not to 'vk' node!\n");
    return;
  }
  
  if ( !(vk->len_data & 0x80000000) && vk->ofs_data) {
    free_val_data(hdesc, vkofs);
  }

  free_block(hdesc, vkofs - 4);
}

/* Delete all values from key (used in recursive delete)
 * hdesc - yer usual hive
 * nkofs - current keyoffset
 */

static void del_allvalues(struct hive *hdesc, int nkofs)
{
  int vlistofs, o, vkofs;
  int32_t *vlistkey;
  struct nk_key *nk;

  nk = (struct nk_key *)(hdesc->buffer + nkofs);
  if (nk->id != NTREG_ID_NK_KEY) {
    RTPrintf("del_allvalues: Key pointer not to 'nk' node!\n");
    return;
  }

  if (!nk->no_values) {
    /*    RTPrintf("del_avalues: Key has no values!\n"); */
    return;
  }

  vlistofs = nk->ofs_vallist + 0x1004;
  vlistkey = (int32_t *)(hdesc->buffer + vlistofs);

  /* Loop through index and delete all vk's */
  for (o = 0; o < nk->no_values; o++) {
    vkofs = vlistkey[o] + 0x1004;
    del_vk(hdesc, vkofs);
  }

  /* Then zap the index, and update nk */
  free_block(hdesc, vlistofs-4);
  nk->ofs_vallist = -1;
  nk->no_values = 0;
}


/* Delete single value from key
 * hdesc - yer usual hive
 * nkofs - current keyoffset
 * name  - name of value to delete
 * exact - NKF_EXACT to do exact match, else first match
 * returns: 0 - ok, 1 - failed
 */

static int del_value(struct hive *hdesc, int nkofs, char *name, int exact)
{
  int vlistofs, slot, o, n, vkofs, newlistofs;
  int32_t *vlistkey, *tmplist, *newlistkey;
  struct nk_key *nk;
  char *blank="";

  if (!name || !*name) return(1);

  if (!strcmp(name,"@")) name = blank;

  nk = (struct nk_key *)(hdesc->buffer + nkofs);
  if (nk->id != NTREG_ID_NK_KEY) {
    RTPrintf("del_value: Key pointer not to 'nk' node!\n");
    return(1);
  }

  if (!nk->no_values) {
    RTPrintf("del_value: Key has no values!\n");
    return(1);
  }

  vlistofs = nk->ofs_vallist + 0x1004;
  vlistkey = (int32_t *)(hdesc->buffer + vlistofs);

  slot = vlist_find(hdesc, vlistofs, nk->no_values, name, TPF_VK);

  if (slot == -1) {
    RTPrintf("del_value: value %s not found!\n",name);
    return(1);
  }

  /* Delete vk and data */
  vkofs = vlistkey[slot] + 0x1004;
  del_vk(hdesc, vkofs);

  /* Copy out old index list */
  CREATE(tmplist,int32_t,nk->no_values);
  memcpy(tmplist, vlistkey, nk->no_values * sizeof(int32_t));

  free_block(hdesc,vlistofs-4);  /* Get rid of old list */

  nk->no_values--;

  if (nk->no_values) {
    newlistofs = alloc_block(hdesc, vlistofs, nk->no_values * sizeof(int32_t));
    if (!newlistofs) {
      RTPrintf("del_value: FATAL: Was not able to alloc new index list\n");
      abort();
    }
    nk = (struct nk_key *)(hdesc->buffer + nkofs); /* In case buffer was moved */

    /* Now copy over, omitting deleted entry */
    newlistkey = (int32_t *)(hdesc->buffer + newlistofs + 4);
    for (n = 0, o = 0; o < nk->no_values+1; o++, n++) {
      if (o == slot) o++;
      newlistkey[n] = tmplist[o];
    }
    nk->ofs_vallist = newlistofs - 0x1000;
  } else {
    nk->ofs_vallist = -1;
  }
  return(0);
}




/* Add a subkey to a key
 * hdesc - usual..
 * nkofs - offset of current nk
 * name  - name of key to add
 * return: ptr to new keystruct, or NULL
 */

#undef AKDEBUG

static struct nk_key *add_key(struct hive *hdesc, int nkofs, char *name)
{

  int slot, newlfofs = 0, oldlfofs = 0, newliofs = 0;
  int oldliofs = 0;
  int o, n, i, onkofs, newnkofs, cmp;
  int rimax, rislot, riofs, namlen;
  struct ri_key *ri = NULL;
  struct lf_key *newlf = NULL, *oldlf;
  struct li_key *newli = NULL, *oldli;
  struct nk_key *key, *newnk, *onk;
  int32_t hash;

  key = (struct nk_key *)(hdesc->buffer + nkofs);

  if (key->id != NTREG_ID_NK_KEY) {
    RTPrintf("add_key: current ptr not 'nk'\n");
    return(NULL);
  }

  namlen = strlen(name);

  slot = -1;
  if (key->no_subkeys) {   /* It already has subkeys */
    
    oldlfofs = key->ofs_lf;
    oldliofs = key->ofs_lf;
   
    oldlf = (struct lf_key *)(hdesc->buffer + oldlfofs + 0x1004);
    if (oldlf->id != 0x666c && oldlf->id != 0x686c && oldlf->id != 0x696c && oldlf->id != 0x6972)  {
      RTPrintf("add_key: index type not supported: 0x%04x\n",oldlf->id);
      return(NULL);
    }

    rimax = 0; ri = NULL; riofs = 0; rislot = -1;
    if (oldlf->id == 0x6972) {  /* Indirect index 'ri', init loop */
      riofs = key->ofs_lf;
      ri = (struct ri_key *)(hdesc->buffer + riofs + 0x1004);
      rimax = ri->no_lis-1;

#ifdef AKDEBUG
      RTPrintf("add_key: entering 'ri' traverse, rimax = %d\n",rimax);
#endif

      oldliofs = ri->hash[rislot+1].ofs_li;
      oldlfofs = ri->hash[rislot+1].ofs_li;

    }

    do {   /* 'ri' loop, at least run once if no 'ri' deep index */

      if (ri) { /* Do next 'ri' slot */
	rislot++;
	oldliofs = ri->hash[rislot].ofs_li;
	oldlfofs = ri->hash[rislot].ofs_li;
	oldli = (struct li_key *)(hdesc->buffer + oldliofs + 0x1004);
	oldlf = (struct lf_key *)(hdesc->buffer + oldlfofs + 0x1004);
      }

      oldli = (struct li_key *)(hdesc->buffer + oldliofs + 0x1004);
      oldlf = (struct lf_key *)(hdesc->buffer + oldlfofs + 0x1004);

#ifdef AKDEBUG
      RTPrintf("add_key: top of ri-loop: rislot = %d, rimax = %d\n",rislot,rimax);
#endif
      slot = -1;

      if (oldli->id == 0x696c) {  /* li */
	
#ifdef AKDEBUG
	RTPrintf("add_key: li slot allocate\n");
#endif	

	FREE(newli);
	ALLOC(newli, 8 + 4*oldli->no_keys + 4, 1);
	newli->no_keys = oldli->no_keys;
	newli->id = oldli->id;
	
	/* Now copy old, checking where to insert (alphabetically) */
	for (o = 0, n = 0; o < oldli->no_keys; o++,n++) {
	  onkofs = oldli->hash[o].ofs_nk;
	  onk = (struct nk_key *)(onkofs + hdesc->buffer + 0x1004);
	  if (slot == -1) {
#if 1
	    RTPrintf("add_key: cmp <%s> with <%s>\n",name,onk->keyname);
#endif

	    cmp = strn_casecmp(name, onk->keyname, (namlen > onk->len_name) ? namlen : onk->len_name);
	    if (!cmp) {
	      RTPrintf("add_key: key %s already exists!\n",name);
	      FREE(newli);
	      return(NULL);
	    }
	    if ( cmp < 0) {
	      slot = o;
	      rimax = rislot; /* Cause end of 'ri' search, too */
	      n++;
#ifdef AKDEBUG
	      RTPrintf("add_key: li-match: slot = %d\n",o);
#endif
	    }
	  }
	  newli->hash[n].ofs_nk = oldli->hash[o].ofs_nk;
	}
	if (slot == -1) slot = oldli->no_keys;
	
      } else { /* lf or lh */

	oldlf = (struct lf_key *)(hdesc->buffer + oldlfofs + 0x1004);
	
	FREE(newlf);
	ALLOC(newlf, 8 + 8*oldlf->no_keys + 8, 1);
	newlf->no_keys = oldlf->no_keys;
	newlf->id = oldlf->id;
#ifdef AKDEBUG	
	RTPrintf("add_key: new lf/lh no_keys: %d\n",newlf->no_keys);
#endif
	
	/* Now copy old, checking where to insert (alphabetically) */
	for (o = 0, n = 0; o < oldlf->no_keys; o++,n++) {
	  onkofs = oldlf->hash[o].ofs_nk;
	  onk = (struct nk_key *)(onkofs + hdesc->buffer + 0x1004);
	  if (slot == -1) {

#if 0
	    RTPrintf("add_key: cmp <%s> with <%s>\n",name,onk->keyname);
#endif
	    cmp = strn_casecmp(name, onk->keyname, (namlen > onk->len_name) ? namlen : onk->len_name);
	    if (!cmp) {
	      RTPrintf("add_key: key %s already exists!\n",name);
	      FREE(newlf);
	      return(NULL);
	    }
	    if ( cmp < 0 ) {
	      slot = o;
	      rimax = rislot;  /* Cause end of 'ri' search, too */
	      n++;
#ifdef AKDEBUG
	      RTPrintf("add_key: lf-match: slot = %d\n",o);
#endif
	    }
	  }
	  newlf->hash[n].ofs_nk = oldlf->hash[o].ofs_nk;
	  newlf->hash[n].name[0] = oldlf->hash[o].name[0];
	  newlf->hash[n].name[1] = oldlf->hash[o].name[1];
	  newlf->hash[n].name[2] = oldlf->hash[o].name[2];
	  newlf->hash[n].name[3] = oldlf->hash[o].name[3];
	}
	if (slot == -1) slot = oldlf->no_keys;
      } /* li else check */


    } while ( (rislot < rimax) && (rimax > 0));  /* 'ri' wrapper loop */

  } else { /* Parent was empty, make new index block */
#ifdef AKDEBUG
    RTPrintf("add_key: new index!\n");
#endif
    ALLOC(newlf, 8 + 8, 1);
    newlf->no_keys = 0 ;    /* Will increment to 1 when filling in the offset later */
    /* Use ID (lf, lh or li) we fetched from root node, so we use same as rest of hive */
    newlf->id = hdesc->nkindextype;
    slot = 0;
  } /* if has keys before */


  /* Make and fill in new nk */
  newnkofs = alloc_block(hdesc, nkofs, sizeof(struct nk_key) + strlen(name));
  if (!newnkofs) {
    RTPrintf("add_key: unable to allocate space for new key descriptor for %s!\n",name);
    FREE(newlf);
    FREE(newli);
    return(NULL);
  }
  key = (struct nk_key *)(hdesc->buffer + nkofs);  /* In case buffer moved */
  newnk = (struct nk_key *)(hdesc->buffer + newnkofs + 4);
  
  newnk->id            = NTREG_ID_NK_KEY;
  newnk->type          = KEY_NORMAL;    /* Some versions use 0x1020 a lot.. */
  newnk->ofs_parent    = nkofs - 0x1004;
  newnk->no_subkeys    = 0;
  newnk->ofs_lf        = -1;
  newnk->no_values     = 0;
  newnk->ofs_vallist   = -1;
  newnk->ofs_sk        = key->ofs_sk; /* Get parents for now. 0 or -1 here crashes XP */
  newnk->ofs_classnam  = -1;
  newnk->len_name      = strlen(name);
  newnk->len_classnam  = 0;
  memcpy(newnk->keyname, name, newnk->len_name);
  
  if (newli) {  /* Handle li */

#if AKDEBUG
    RTPrintf("add_key: li fill at slot: %d\n",slot);
#endif

    /* And put its offset into parents index list */
    newli->hash[slot].ofs_nk = newnkofs - 0x1000;
    newli->no_keys++;
    
    /* Allocate space for our new li list and copy it into reg */
    newliofs = alloc_block(hdesc, nkofs, 8 + 4*newli->no_keys);
    if (!newliofs) {
      RTPrintf("add_key: unable to allocate space for new index table for %s!\n",name);
      FREE(newli);
      free_block(hdesc,newnkofs);
      return(NULL);
    }
    key = (struct nk_key *)(hdesc->buffer + nkofs);  /* In case buffer moved */
    newnk = (struct nk_key *)(hdesc->buffer + newnkofs + 4);

    /*    memcpy(hdesc->buffer + newliofs + 4, newli, 8 + 4*newli->no_keys); */
    fill_block(hdesc, newliofs, newli, 8 + 4*newli->no_keys);


  } else {  /* lh or lf */

#ifdef AKDEBUG
    RTPrintf("add_key: lf/lh fill at slot: %d, rislot: %d\n",slot,rislot);
#endif
    /* And put its offset into parents index list */
    newlf->hash[slot].ofs_nk = newnkofs - 0x1000;
    newlf->no_keys++;
    if (newlf->id == 0x666c) {        /* lf hash */
      newlf->hash[slot].name[0] = 0;
      newlf->hash[slot].name[1] = 0;
      newlf->hash[slot].name[2] = 0;
      newlf->hash[slot].name[3] = 0;
      strncpy(newlf->hash[slot].name, name, 4);
    } else if (newlf->id == 0x686c) {  /* lh. XP uses this. hashes whole name */
      for (i = 0,hash = 0; i < strlen(name); i++) {
	hash *= 37;
	hash += reg_touppertable[(unsigned char)name[i]];
      }
      newlf->lh_hash[slot].hash = hash;
    }
    
    /* Allocate space for our new lf list and copy it into reg */
    newlfofs = alloc_block(hdesc, nkofs, 8 + 8*newlf->no_keys);
    if (!newlfofs) {
      RTPrintf("add_key: unable to allocate space for new index table for %s!\n",name);
      FREE(newlf);
      free_block(hdesc,newnkofs);
      return(NULL);
    }

    key = (struct nk_key *)(hdesc->buffer + nkofs);  /* In case buffer moved */
    newnk = (struct nk_key *)(hdesc->buffer + newnkofs + 4);

    /*    memcpy(hdesc->buffer + newlfofs + 4, newlf, 8 + 8*newlf->no_keys); */
    fill_block(hdesc, newlfofs, newlf, 8 + 8*newlf->no_keys);
    
  } /* li else */


  /* Update parent, and free old lf list */
  key->no_subkeys++;
  if (ri) {  /* ri index */
    ri->hash[rislot].ofs_li = (newlf ? newlfofs : newliofs) - 0x1000;
  } else { /* Parent key */
    key->ofs_lf = (newlf ? newlfofs : newliofs) - 0x1000;
  }

  if (newlf && oldlfofs) free_block(hdesc,oldlfofs + 0x1000);
  if (newli && oldliofs) free_block(hdesc,oldliofs + 0x1000);

  FREE(newlf);
  FREE(newli);
  return(newnk);

}

/* Delete a subkey from a key
 * hdesc - usual..
 * nkofs - offset of current nk
 * name  - name of key to delete (must match exactly, also case)
 * return: 1 - err, 0 - ok
 */

#undef DKDEBUG

static int del_key(struct hive *hdesc, int nkofs, char *name)
{

  int slot = 0, newlfofs = 0, oldlfofs = 0, o, n, onkofs,  delnkofs;
  int oldliofs = 0, no_keys = 0, newriofs = 0;
  int namlen;
  int rimax, riofs, rislot;
  struct ri_key *ri, *newri = NULL;
  struct lf_key *newlf = NULL, *oldlf = NULL;
  struct li_key *newli = NULL, *oldli = NULL;
  struct nk_key *key, *onk, *delnk;
  char fullpath[501];

  key = (struct nk_key *)(hdesc->buffer + nkofs);

  namlen = strlen(name);

#ifdef DKDEBUG
  RTPrintf("del_key: deleting: <%s>\n",name);
#endif


  if (key->id != NTREG_ID_NK_KEY) {
    RTPrintf("add_key: current ptr not nk\n");
    return(1);
  }

  slot = -1;
  if (!key->no_subkeys) {
    RTPrintf("del_key: key has no subkeys!\n");
    return(1);
  }

  oldlfofs = key->ofs_lf;
  oldliofs = key->ofs_lf;
  
  oldlf = (struct lf_key *)(hdesc->buffer + oldlfofs + 0x1004);
  if (oldlf->id != 0x666c && oldlf->id != 0x686c && oldlf->id != 0x696c && oldlf->id != 0x6972)  {
    RTPrintf("del_key: index other than 'lf', 'li' or 'lh' not supported yet. 0x%04x\n",oldlf->id);
    return(1);
  }

  rimax = 0; ri = NULL; riofs = 0;
  rislot = 0;

  if (oldlf->id == 0x6972) {  /* Indirect index 'ri', init loop */
    riofs = key->ofs_lf;
    ri = (struct ri_key *)(hdesc->buffer + riofs + 0x1004);
    rimax = ri->no_lis-1;
    
#ifdef DKDEBUG
    RTPrintf("del_key: entering 'ri' traverse, rimax = %d\n",rimax);
#endif
    
    rislot = -1; /* Starts at slot 0 below */
    
  }
  
  do {   /* 'ri' loop, at least run once if no 'ri' deep index */
    
    if (ri) { /* Do next 'ri' slot */
      rislot++;
      oldliofs = ri->hash[rislot].ofs_li;
      oldlfofs = ri->hash[rislot].ofs_li;
    }
    
    oldli = (struct li_key *)(hdesc->buffer + oldliofs + 0x1004);
    oldlf = (struct lf_key *)(hdesc->buffer + oldlfofs + 0x1004);
    
#ifdef DKDEBUG
    RTPrintf("del_key: top of ri-loop: rislot = %d\n",rislot);
#endif
    slot = -1;
    
    if (oldlf->id == 0x696c) {   /* 'li' handler */
#ifdef DKDEBUG      
      RTPrintf("del_key: li handler\n");
#endif
      
      FREE(newli);
      ALLOC(newli, 8 + 4*oldli->no_keys - 4, 1);
      newli->no_keys = oldli->no_keys - 1; no_keys = newli->no_keys;
      newli->id = oldli->id;
      
      /* Now copy old, checking where to delete */
      for (o = 0, n = 0; o < oldli->no_keys; o++,n++) {
	onkofs = oldli->hash[o].ofs_nk;
	onk = (struct nk_key *)(onkofs + hdesc->buffer + 0x1004);
	if (slot == -1 && onk->len_name == namlen && !strncmp(name, onk->keyname, (onk->len_name > namlen) ? onk->len_name : namlen)) {
	  slot = o;
	  delnkofs = onkofs; delnk = onk;
	  rimax = rislot;
	  o++;
	}
	newli->hash[n].ofs_nk = oldli->hash[o].ofs_nk;
      }
      
      
    } else { /* 'lf' or 'lh' are similar */
      
#ifdef DKDEBUG
      RTPrintf("del_key: lf or lh handler\n");
#endif
      FREE(newlf);
      ALLOC(newlf, 8 + 8*oldlf->no_keys - 8, 1);
#ifdef DKDEBUG
      RTPrintf("alloc newlf: %x\n",newlf);
#endif
      newlf->no_keys = oldlf->no_keys - 1; no_keys = newlf->no_keys;
      newlf->id = oldlf->id;
      
      /* Now copy old, checking where to delete */
      for (o = 0, n = 0; o < oldlf->no_keys; o++,n++) {

	onkofs = oldlf->hash[o].ofs_nk;
	onk = (struct nk_key *)(onkofs + hdesc->buffer + 0x1004);

	if (slot == -1 && (onk->len_name == namlen) && !strncmp(name, onk->keyname, onk->len_name)) {
	  slot = o;
	  delnkofs = onkofs; delnk = onk;
	  rimax = rislot;
	  o++;
	}

	if (n < newlf->no_keys) { /* Only store if not last index in old */
#ifdef DKDEBUG
	  RTPrintf("del_key: n = %d, o = %d\n",n,o);
#endif
	  newlf->hash[n].ofs_nk = oldlf->hash[o].ofs_nk;
	  newlf->hash[n].name[0] = oldlf->hash[o].name[0];
	  newlf->hash[n].name[1] = oldlf->hash[o].name[1];
	  newlf->hash[n].name[2] = oldlf->hash[o].name[2];
	  newlf->hash[n].name[3] = oldlf->hash[o].name[3];
	}

      }
    } /* else lh or lf */

  } while (rislot < rimax);  /* ri traverse loop */

  if (slot == -1) {
    RTPrintf("del_key: subkey %s not found!\n",name);
    FREE(newlf);
    FREE(newli);
    return(1);
  }

#ifdef DKDEBUG
  RTPrintf("del_key: key found at slot %d\n",slot);
#endif

  if (delnk->no_values || delnk->no_subkeys) {
    RTPrintf("del_key: subkey %s has subkeys or values. Not deleted.\n",name);
    FREE(newlf);
    FREE(newli);
    return(1);
  }

  /* Allocate space for our new lf list and copy it into reg */
  if ( no_keys && (newlf || newli) ) {
    newlfofs = alloc_block(hdesc, nkofs, 8 + (newlf ? 8 : 4) * no_keys);
#ifdef DKDEBUG
    RTPrintf("del_key: alloc_block for index returns: %x\n",newlfofs);
#endif
    if (!newlfofs) {
      RTPrintf("del_key: WARNING: unable to allocate space for new key descriptor for %s! Not deleted\n",name);
      FREE(newlf);
      return(1);
    }
    key = (struct nk_key *)(hdesc->buffer + nkofs);
    oldli = (struct li_key *)(hdesc->buffer + oldliofs + 0x1004);
    oldlf = (struct lf_key *)(hdesc->buffer + oldlfofs + 0x1004);



    /*    memcpy(hdesc->buffer + newlfofs + 4,
	   ((void *)newlf ? (void *)newlf : (void *)newli), 8 + (newlf ? 8 : 4) * no_keys);
    */
    fill_block(hdesc, newlfofs,
	   ((void *)newlf ? (void *)newlf : (void *)newli), 8 + (newlf ? 8 : 4) * no_keys);


  } else {  /* Last deleted, will throw away index */
    newlfofs = 0xfff;  /* We subtract 0x1000 later */
  }

  if (newlfofs < 0xfff) {
    RTPrintf("del_key: ERROR: newlfofs = %x\n",newlfofs);
#if DOCORE
    debugit(hdesc->buffer,hdesc->size);
    abort();
#endif
  }

  /* Check for CLASS data, if so, deallocate it too */
  if (delnk->len_classnam) {
    free_block(hdesc, delnk->ofs_classnam + 0x1000);
  }
  /* Now it's safe to zap the nk */
  free_block(hdesc, delnkofs + 0x1000);
  /* And the old index list */
  free_block(hdesc, (oldlfofs ? oldlfofs : oldliofs) + 0x1000);

  /* Update parent */
  key->no_subkeys--;

  key = (struct nk_key *)(hdesc->buffer + nkofs);
  oldli = (struct li_key *)(hdesc->buffer + oldliofs + 0x1004);
  oldlf = (struct lf_key *)(hdesc->buffer + oldlfofs + 0x1004);

  if (ri) {
    ri = (struct ri_key *)(hdesc->buffer + riofs + 0x1004); /* In case realloc */

    if (newlfofs == 0xfff) {

      *fullpath = 0;
      get_abs_path(hdesc, nkofs, fullpath, 480);

      VERBF(hdesc,"del_key: need to delete ri-slot %d for %x - %s\n", rislot,nkofs,fullpath );

      if (ri->no_lis > 1) {  /* We have subindiceblocks left? */
	/* Delete from array */
	ALLOC(newri, 8 + 4*ri->no_lis - 4, 1);
	newri->no_lis = ri->no_lis - 1;
	newri->id = ri->id;
	for (o = 0, n = 0; o < ri->no_lis; o++,n++) {
	  if (n == rislot) o++;
	  newri->hash[n].ofs_li = ri->hash[o].ofs_li;
	}
	newriofs = alloc_block(hdesc, nkofs, 8 + newri->no_lis*4 );
	if (!newriofs) {
	  RTPrintf("del_key: WARNING: unable to allocate space for ri-index for %s! Not deleted\n",name);
	  FREE(newlf);
	  FREE(newri);
	  return(1);
	}
	key = (struct nk_key *)(hdesc->buffer + nkofs);
	oldli = (struct li_key *)(hdesc->buffer + oldliofs + 0x1004);
	oldlf = (struct lf_key *)(hdesc->buffer + oldlfofs + 0x1004);

	fill_block(hdesc, newriofs, newri, 8 + newri->no_lis * 4);
	free_block(hdesc, riofs + 0x1000);
	key->ofs_lf = newriofs - 0x1000;
	FREE(newri);
      } else { /* Last entry in ri was deleted, get rid of it, key is empty */
	VERB(hdesc,"del_key: .. and that was the last one. key now empty!\n");
	free_block(hdesc, riofs + 0x1000);
	key->ofs_lf = -1;
      }
    } else {
      ri->hash[rislot].ofs_li = newlfofs - 0x1000; 
    }
  } else {
    key->ofs_lf = newlfofs - 0x1000;
  }

  FREE(newlf);
  return(0);

}

/* Recursive delete keys
 * hdesc - usual..
 * nkofs - offset of current nk
 * name  - name of key to delete
 * return: 0 - ok, 1 fail
 */
static void rdel_keys(struct hive *hdesc, char *path, int vofs)
{
  struct nk_key *key;
  int nkofs;
  struct ex_data ex = {};
  int count = 0, countri = 0;
  

  if (!path || !*path) return;

  nkofs = trav_path(hdesc, vofs, path, TPF_NK_EXACT);

  if(!nkofs) {
    RTPrintf("rdel_keys: Key <%s> not found\n",path);
    return;
  }
  nkofs += 4;

  key = (struct nk_key *)(hdesc->buffer + nkofs);

  /*
  VERBF(hdesc,"rdel of node at offset 0x%0x\n",nkofs);
  */

  if (key->id != NTREG_ID_NK_KEY) {
    RTPrintf("Error: Not a 'nk' node!\n");

    debugit(hdesc->buffer,hdesc->size);
    
  }
  
#if 0
  RTPrintf("Node has %d subkeys and %d values\n",key->no_subkeys,key->no_values);
#endif
  if (key->no_subkeys) {
    while ((ex_next_n(hdesc, nkofs, &count, &countri, &ex) > 0)) {
#if 0
      RTPrintf("%s\n",ex.name);
#endif
      rdel_keys(hdesc, ex.name, nkofs);
      count = 0;
      countri = 0;
      FREE(ex.name);
    }
  }

  del_allvalues(hdesc, nkofs);
  key = (struct nk_key *)(hdesc->buffer + nkofs);
  del_key(hdesc, key->ofs_parent+0x1004, path);

}
  

/* Get and copy keys CLASS-data (if any) to buffer
 * Returns a buffer with the data (first int32_t is size). see ntreg.h
 * NOTE: caller must deallocate buffer! a simple free(keyval) will suffice.
 */
static struct keyval *get_class(struct hive *hdesc,
			    int curnk, char *path)
{
  int clen = 0, dofs = 0, nkofs;
  struct nk_key *key;
  struct keyval *data;
  void *classdata;

  if (!path && !curnk) return(NULL);

  nkofs = trav_path(hdesc, curnk, path, 0);

  if(!nkofs) {
    RTPrintf("get_class: Key <%s> not found\n",path);
    return(NULL);
  }
  nkofs += 4;
  key = (struct nk_key *)(hdesc->buffer + nkofs);

  clen = key->len_classnam;
  if (!clen) {
    RTPrintf("get_class: Key has no class data.\n");
    return(NULL);
  }

  dofs = key->ofs_classnam;
  classdata = (void *)(hdesc->buffer + dofs + 0x1004);
  
#if 0
  RTPrintf("get_class: len_classnam = %d\n",clen);
  RTPrintf("get_class: ofs_classnam = 0x%x\n",dofs);
#endif

  ALLOC(data, sizeof(struct keyval) + clen,1);
  data->len = clen;
  memcpy(&data->data, classdata, clen);
  return(data);
}
#endif  /* WRITE_HIVE */


/* Write to registry value.
 * If same size as existing, copy back in place to avoid changing too much
 * otherwise allocate new dataspace, then free the old
 * Thus enough space to hold both new and old data is needed
 * Pass inn buffer with data len as first DWORD (as routines above)
 * returns: 0 - error, len - OK (len of data)
 */

static int put_buf2val(struct hive *hdesc, struct keyval *kv,
		int vofs, char *path, int type, int exact )
{
  int l;
  void *keydataptr, *addr;
  struct db_key *db;
  int copylen, blockofs, blocksize, restlen, point, i, list, parts;

  if (!kv) return(0);


  l = get_val_len(hdesc, vofs, path, exact);
  if (l == -1) return(0);  /* error */
  //  RTPrintf("put_buf2val: l = %d\n",l);

  //  RTPrintf("put_buf2val: %s, kv len = %d, l = %d\n",path,kv->len,l);


  if (kv->len != l) {  /* Realloc data block if not same size as existing */
    if (!alloc_val_data(hdesc, vofs, path, kv->len, exact)) {
      RTPrintf("put_buf2val: %s : alloc_val_data failed!\n",path);
      return(0);
    }
  }

  keydataptr = get_val_data(hdesc, vofs, path, type, exact);
  if (!keydataptr) {
      RTPrintf("put_buf2val: %s : get_val_data failed!\n",path);
      return(0); /* error */
  }



  if (kv->len > VAL_DIRECT_LIMIT) {       /* Where do the db indirects start? seems to be around 16k */
    db = (struct db_key *)keydataptr;
    if (db->id != 0x6264) abort();
    parts = db->no_part;
    list = db->ofs_data + 0x1004;
    RTPrintf("put_buf2val: Long value: parts = %d, list = %x\n",parts,list);

    point = 0;
    restlen = kv->len;
    for (i = 0; i < parts; i++) {
      blockofs = get_int(hdesc->buffer + list + (i << 2)) + 0x1000;
      blocksize = -get_int(hdesc->buffer + blockofs) - 8;

      /* Copy this part, up to size of block or rest lenght in last block */
      copylen = (blocksize > restlen) ? restlen : blocksize;

      RTPrintf("put_buf2val: Datablock %d offset %x, size %x (%d)\n",i,blockofs,blocksize,blocksize);
      RTPrintf("             : Point = %x, restlen = %x, copylen = %x\n",point,restlen,copylen);

      addr = (void *)&(kv->data) + point;
      fill_block( hdesc, blockofs, addr, copylen);

      //      debugit((char *)&(kr->data), l);
      
      point += copylen;
      restlen -= copylen;
    }
  } else {
    memcpy(keydataptr, &kv->data, kv->len);
  }
  
  hdesc->state |= HMODE_DIRTY;

  return(kv->len);
}

/* And, yer basic DWORD write */

int put_dword(struct hive *hdesc, int vofs, char *path, int exact, int dword)
{
  struct keyval *kr;
  int r;

  ALLOC(kr,1,sizeof(int)+sizeof(int));
  
  kr->len = sizeof(int);
  kr->data = dword;

  r = put_buf2val(hdesc, kr, vofs, path, REG_DWORD, exact);

  FREE(kr);

  return(r);
}

/* ================================================================ */

/* Code to export registry entries to .reg file initiated by
 * Leo von Klenze
 * Then expanded a bit to handle more types etc.
 */

#if 0        // Not used at the moment
static char *
string_rega2prog(void *string, int len)
{
    int i, k;
    char *cstring;

    int out_len = 0;
    for(i = 0; i < len; ++i)
    {
        unsigned v = ((unsigned char *)string)[i];
        if (v < 128)
            out_len += 1;
        else
            out_len += 2;
    }
    CREATE(cstring,char,out_len+1);

    for(i = 0, k = 0; i < len; ++i)
    {
        unsigned v = ((unsigned char *)string)[i];
        if (v < 128)
            cstring[k++] = v;
        else {
            cstring[k++] = 0xc0 | (v >> 6);
            cstring[k++] = 0x80 | (v & 0x3f);
        }
    }
    cstring[out_len] = '\0';

    return cstring;
}

static void
string_prog2rega(char *string, int len)
{
    char *out = string;
    unsigned char *in = (unsigned char*) string;

    for (;*in; ++in) {
        if (!(*in & 0x80)) {
            *out++ = *in;
        } else if ((in[0] & 0xe0) == 0xc0 && in[1]) {
            *out++ = (in[0] & 0x1f) << 6 | (in[1] & 0x3f);
            ++in;
        } else if (in[1] && in[2]) {
            /* assume 3 byte*/
            *out++ = (in[1] & 0xf) << 6 | (in[2] & 0x3f);
            in += 2;
        }
    }
    *out = 0;
}
#endif  // Not used


static char *
string_prog2regw(void *string, int len, int *out_len)
{
    unsigned char *regw = (unsigned char*) malloc(len*2+2);
    unsigned char *out = regw;
    unsigned char *in = (unsigned char*) string;

    for (;len>0; ++in, --len) {
        if (!(in[0] & 0x80)) {
            *out++ = *in;
            *out++ = 0;
        } else if ((in[0] & 0xe0) == 0xc0 && len >= 2) {
            *out++ = (in[0] & 0x1f) << 6 | (in[1] & 0x3f);
            *out++ = (in[0] & 0x1f) >> 2;
            ++in, --len;
        } else if (len >= 3) {
            /* assume 3 byte*/
            *out++ = (in[1] & 0xf) << 6 | (in[2] & 0x3f);
            *out++ = (in[0] & 0xf) << 4 | ((in[1] & 0x3f) >> 2);
            in += 2;
            len -= 2;
        }
    }
    *out_len = out - regw;
    out[0] = out[1] = 0;
    return (char *) regw;
}

#ifdef WRITE_HIVE
static char *
quote_string(const char *s)
{
	int len = strlen(s);
	const char *p;
	char *dst, *out;

	for (p = s; *p; ++p)
		if (*p == '\\' || *p == '\"') 
			++len;

	dst = out = (char*) malloc(len + 1);
	for (p = s; *p; ++p) {
		if (*p == '\\' || *p == '\"')
			*dst++ = '\\';
		*dst++ = *p;
	}
	*dst = 0;
	return out;
}
#endif  /* WRITE_HIVE */



/* ================================================================ */

/* Hive control (load/save/close) etc */

static void closeHive(struct hive *hdesc)
{
  //  RTPrintf("closing hive %s\n",hdesc_filename(hdesc));
  hdesc_close(hdesc);
  FREE(hdesc->buffer);
  FREE(hdesc);
}


/* Compute checksum of REGF header page
 * hdesc = hive
 * returns checksum value, 32 bit int
 */

static int32_t calc_regfsum(struct hive *hdesc)
{
  int32_t checksum = 0;
  struct regf_header *hdr;
  int i;

  hdr = (struct regf_header *) hdesc->buffer;

  for (i = 0; i < 0x1fc/4; ++i)
    checksum ^= ((int32_t *) hdr)[i];

  return(checksum);

}

#ifdef WRITE_HIVE
/* Write the hive back to disk (only if dirty & not readonly */
static int writeHive(struct hive *hdesc)
{
  int len;
  struct regf_header *hdr;

  if (hdesc->state & HMODE_RO) return(0);
  if ( !(hdesc->state & HMODE_DIRTY)) return(0);

  /* compute new checksum */

  hdr = (struct regf_header *) hdesc->buffer;

  hdr->checksum = calc_regfsum(hdesc);


  len = hdesc_write(hdesc, (uint8_t *)hdesc->buffer, hdesc->size);
  if (len != hdesc->size) {
    RTPrintf("ERROR: writeHive: write of %s failed.\n",hdesc_filename(hdesc));
    return(1);
  }

  hdesc->state &= (~HMODE_DIRTY);
  return(0);
}
#endif  /* WRITE_HIVE */

#undef LOAD_DEBUG

static struct hive *hive_ref(struct hive *hive)
{
    hive->refcnt++;
    return hive;
}

static void hive_unref(struct hive *hive)
{
    --hive->refcnt;
    if ( !hive->refcnt ) {
        closeHive(hive);
    }
}

static struct hive *openHive(const struct hive_iops *iops, void *user)
{

  struct hive *hdesc;
  int vofs;
  uint32_t pofs;
  int32_t checksum;
  char *c;
  struct hbin_page *p = NULL;
  struct regf_header *hdr;
  struct nk_key *nk;
  struct ri_key *rikey;
  size_t sz;
  int verbose = 0;
  int trace   = 0;

  CREATE(hdesc,struct hive,1);

  hdesc->iops = iops;
  hdesc->filedesc = user;
  hdesc->state = 0;
  hdesc->size = 0;
  hdesc->buffer = NULL;

  if ( !hdesc_filesize(hdesc, &sz) ) {
    FREE(hdesc);
    return NULL;
  }

  hdesc->size = sz;
  /*  RTPrintf("ERROR: hiveOpen(%s) successful\n",hdesc_filename(hdesc)); */
  
  /* Read the whole file */

  ALLOC(hdesc->buffer,1,hdesc->size);

  if ( !hdesc_read(hdesc, (uint8_t *)hdesc->buffer, hdesc->size) ) {
    RTPrintf("ERROR: Could not read file\n");
    closeHive(hdesc);
    return(NULL);
  }

  /* Now run through file, tallying all pages */
  /* NOTE/KLUDGE: Assume first page starts at offset 0x1000 */

   pofs = 0x1000;

   hdr = (struct regf_header *)hdesc->buffer;
   if (hdr->id != 0x66676572) {
     RTPrintf("ERROR: openHive(%s): File does not seem to be a registry hive!\n",hdesc_filename(hdesc));
     return(hdesc);
   }

   checksum = calc_regfsum(hdesc);

#ifdef LOAD_DEBUG
   RTPrintf("openhive: calculated checksum: %08x\n",checksum);
   RTPrintf("openhive: file REGF  checksum: %08x\n",hdr->checksum);
#endif
   if (checksum != hdr->checksum) {
     RTPrintf("ERROR: openHive(%s): WARNING: REGF header checksum mismatch! calc: 0x%08x != file: 0x%08x\n",hdesc_filename(hdesc),checksum,hdr->checksum);
   }



   RTPrintf("Hive <%s> name (from header): <",hdesc_filename(hdesc));
   for (c = hdr->name; *c && (c < hdr->name + 64); c += 2) putchar(*c);

   hdesc->rootofs = hdr->ofs_rootkey + 0x1000;
   RTPrintf(">\nROOT KEY at offset: 0x%06x * ",hdesc->rootofs);

   /* Cache the roots subkey index type (li,lf,lh) so we can use the correct
    * one when creating the first subkey in a key */
   
   nk = (struct nk_key *)(hdesc->buffer + hdesc->rootofs + 4);
   if (nk->id == NTREG_ID_NK_KEY) {
     rikey = (struct ri_key *)(hdesc->buffer + nk->ofs_lf + 0x1004);
     hdesc->nkindextype = rikey->id;
     if (hdesc->nkindextype == 0x6972) {  /* Gee, big root, must check indirectly */
       RTPrintf("openHive: DEBUG: BIG ROOT!\n");
       rikey = (struct ri_key *)(hdesc->buffer + rikey->hash[0].ofs_li + 0x1004);
       hdesc->nkindextype = rikey->id;
     }
     if (hdesc->nkindextype != 0x666c &&
	 hdesc->nkindextype != 0x686c &&
	 hdesc->nkindextype != 0x696c) {
       hdesc->nkindextype = 0x666c;
     }

     RTPrintf("Subkey indexing type is: %04x <%c%c>\n",
	    hdesc->nkindextype,
	    hdesc->nkindextype & 0xff,
	    hdesc->nkindextype >> 8);
   } else {
     RTPrintf("ERROR: openHive: WARNING: ROOT key does not seem to be a key! (not type == nk)\n");
   }


   while (pofs < hdr->filesize + 0x1000) {   /* Loop through hbins until end according to regf header */
#ifdef LOAD_DEBUG
     int htrace = 1;
     //     if (htrace) hexdump(hdesc->buffer,pofs,pofs+0x20,1);
#endif
     p = (struct hbin_page *)(hdesc->buffer + pofs);
     if (p->id != 0x6E696268) {
       RTPrintf("Page at 0x%x is not 'hbin', assuming file contains garbage at end\n",pofs);
       break;
     }
     hdesc->pages++;

     if (verbose) RTPrintf("###### Page at 0x%0x ofs_self 0x%0x, size (delta ofs_next) 0x%0x ######\n",
			pofs,p->ofs_self,p->ofs_next);

     if (p->ofs_next == 0) {
       RTPrintf("ERROR: openHive: ERROR: Page at 0x%x has size zero! File may be corrupt, or program has a bug\n",pofs);
       return(hdesc);
     }


     vofs = pofs + 0x20; /* Skip page header, and run through blocks in hbin */

     while (vofs-pofs < p->ofs_next && vofs < hdesc->size) {
       vofs += parse_block(hdesc,vofs,trace);

     }

     pofs += p->ofs_next;

   } /* hbin loop */


   hdesc->endofs  = hdr->filesize + 0x1000;
   hdesc->lastbin = pofs - p->ofs_next;  /* Compensate for loop that added at end above */

   if (verbose) {
     RTPrintf("Last HBIN at offset       : 0x%x\n",hdesc->lastbin);
     RTPrintf("First non-HBIN page offset: 0x%x\n",hdesc->endofs);
     RTPrintf("hdr->unknown4 (version?)  : 0x%x\n",hdr->unknown4);
   }

   RTPrintf("File size %d [%x] bytes, containing %d pages (+ 1 headerpage)\n",hdesc->size,hdesc->size, hdesc->pages);
   RTPrintf("Used for data: %d/%d blocks/bytes, unused: %d/%d blocks/bytes.\n",
	  hdesc->useblk,hdesc->usetot,hdesc->unuseblk,hdesc->unusetot);
  
   return hive_ref(hdesc);
}

struct _rhkey {
    struct hive *k_hive;
    struct nk_key *k_nk;
    int k_ofs;
};

static int do_open_key(struct hive *hive, int nkofs, rhkey_t *res)
{
    struct _rhkey *key;
    struct nk_key *nk;

    if (hive->buffer + nkofs + sizeof(*nk) > (hive->buffer + hive->size)) {
        RTPrintf("ntreg: bad nk offset\n");
        return 0;
    }

    nk = (struct nk_key *)(hive->buffer + nkofs);
    if (nk->id != NTREG_ID_NK_KEY) {
        RTPrintf("ntreg: bad nk key id\n");
        return 0;
    }

    key = RTMemAllocZ(sizeof(*key));
    if (NULL == key) {
        return 0;
    }

    key->k_hive = hive_ref(hive);
    key->k_nk = nk;
    key->k_ofs = nkofs;
    *res = key;

    return 1;
}

int reghive_open_hive(const struct hive_iops *iops, void *user, rhkey_t *res)
{
    struct hive *hive;

    hive = openHive(iops, user);
    if ( NULL == hive )
        return 0;

    return do_open_key(hive, hive->rootofs + 4, res);
}

int reghive_open_key(rhkey_t key, const char *path, rhkey_t *res)
{
    int nkofs;

    nkofs = trav_path(key->k_hive, key->k_ofs, path, TPF_EXACT);
    if (!nkofs) {
        return 0;
    }

    return do_open_key(key->k_hive, nkofs + 4, res);
}

void reghive_close_key(rhkey_t key)
{
    if ( key ) {
        hive_unref(key->k_hive);
        RTMemFree(key);
    }
}

int reghive_query_info_key(rhkey_t key, struct reg_key_info *info)
{
    struct vex_data vex = {};
    struct ex_data ex = {};
    int c, ri, i;

    memset(info, 0, sizeof(*info));
    info->ki_subkeys = key->k_nk->no_subkeys;
    info->ki_values = key->k_nk->no_values;

    for(i = ri = c = 0;
            ex_next_n(key->k_hive, key->k_ofs, &c, &ri, &ex) > 0; i++) {
        size_t nlen;
        nlen = strlen(ex.name);
        FREE(ex.name);
        if ( nlen > info->ki_max_subkey_len )
            info->ki_max_subkey_len = nlen;
    }

    for(i = c = 0;
            ex_next_v(key->k_hive, key->k_ofs, &c, &vex) > 0; i++) {
        size_t nlen;
        nlen = strlen(vex.name);
        FREE(vex.name);
        if ( nlen > info->ki_max_value_name_len )
            info->ki_max_subkey_len = nlen;
        if ( vex.size > info->ki_max_value_len )
            info->ki_max_value_len = vex.size;
    }

    return 1;
}

int reghive_enum_key(rhkey_t key, unsigned int index, char *name, size_t nlen)
{
    struct ex_data ex = {};
    int c, ri, i;

    for(i = ri = c = 0;
            ex_next_n(key->k_hive, key->k_ofs, &c, &ri, &ex) > 0; i++) {
        size_t len;

        if ( i != index )
            continue;

        len = strlen(ex.name);
        if ( len + 1 > nlen ) {
            FREE(ex.name);
            return 0;
        }

        strcpy(name, ex.name);
        FREE(ex.name);
        return 1;
    }

    return 0;
}

int reghive_enum_value(rhkey_t key, unsigned int index,
                        char *name, size_t nlen,
                        uint8_t *data, size_t *dlen,
                        unsigned int *type)
{
    struct vex_data vex = {};
    int c, i;

    for(i = c = 0;
            ex_next_v(key->k_hive, key->k_ofs, &c, &vex) > 0; i++) {
        size_t len;

        if ( i != index )
            continue;

        len = strlen(vex.name);

        if ( type )
            *type = vex.type;
        if ( name ) {
            if ( len + 1 > nlen ) {
                FREE(vex.name);
                return 0;
            }
            strcpy(name, vex.name);
        }
        if ( data || dlen )
            *dlen = vex.size;
        if ( data ) {
            if ( vex.size > *dlen) {
                FREE(vex.name);
                return 0;
            }
            switch(vex.type) {
            case REG_SZ:
            case REG_MULTI_SZ:
                cheap_uni2ascii((char *)vex.val, (char *)data, vex.size);
                break;
            default:
                memcpy(data, vex.val, vex.size);
                break;
            }
        }
        FREE(vex.name);
        return 1;
    }

    return 0;
}

int reghive_get_value(rhkey_t key, const char *subkey, const char *val,
                        uint8_t *data, size_t *dlen,
                        unsigned int *type)
{
    int nkofs = key->k_ofs;
    struct vex_data vex = {};
    int vkofs;

    if ( subkey ) {
        nkofs = trav_path(key->k_hive, key->k_ofs, subkey, TPF_EXACT);
        if (!nkofs) {
            return 0;
        }
        nkofs += 4;
    }

    vkofs = trav_path(key->k_hive, nkofs, val, TPF_VK_EXACT);
    if (!vkofs) {
        return 0;
    }

    memset(&vex, 0, sizeof(vex));
    vex.vkoffs = vkofs + 4;
    if ( interpret_vk(key->k_hive, &vex) ) {
        return 0;
    }

    if ( type )
        *type = vex.type;
    if ( data || dlen )
        *dlen = vex.size;
    if ( data ) {
        if ( vex.size > *dlen) {
            FREE(vex.name);
            return 0;
        }
        switch(vex.type) {
        case REG_SZ:
        case REG_MULTI_SZ:
            cheap_uni2ascii((char *)vex.val, (char *)data, vex.size);
            break;
        default:
            memcpy(data, vex.val, vex.size);
            break;
        }
    }

    FREE(vex.name);
    return 1;
}

const char *reghive_type_name(unsigned int type)
{
    if ( type >= REG_MAX )
        return "UNKNOWN";
    return val_types[type];
}
