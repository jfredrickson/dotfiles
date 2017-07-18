/* Copyright (C) 2000-2005 MySQL AB & Innobase Oy

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA */

/*
  InnoDB offline file checksum utility.  85% of the code in this file
  was taken wholesale fron the InnoDB codebase.

  The final 15% was originally written by Mark Smith of Danga
  Interactive, Inc. <junior@danga.com>

  Published with a permission.
*/

/* needed to have access to 64 bit file functions */
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE

#define _XOPEN_SOURCE 600 /* needed to include getopt.h on some platforms and get posix_fadvise */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

/* all of these ripped from InnoDB code from MySQL 4.0.22 */
#define UT_HASH_RANDOM_MASK     1463735687
#define UT_HASH_RANDOM_MASK2    1653893711
#define FIL_PAGE_LSN          16 
#define FIL_PAGE_FILE_FLUSH_LSN 26
#define FIL_PAGE_OFFSET     4
#define FIL_PAGE_TYPE           24
#define FIL_PAGE_DATA       38
#define FIL_PAGE_END_LSN_OLD_CHKSUM 8
#define FIL_PAGE_SPACE_OR_CHKSUM 0
#define UNIV_PAGE_SIZE          (2 * 8192)

#define FIL_PAGE_INDEX		17855	/*!< B-tree node */
#define FIL_PAGE_UNDO_LOG	2	/*!< Undo log page */
#define FIL_PAGE_INODE		3	/*!< Index node */
#define FIL_PAGE_IBUF_FREE_LIST	4	/*!< Insert buffer free list */
#define FIL_PAGE_TYPE_ALLOCATED	0	/*!< Freshly allocated page */
#define FIL_PAGE_IBUF_BITMAP	5	/*!< Insert buffer bitmap */
#define FIL_PAGE_TYPE_SYS	6	/*!< System page */
#define FIL_PAGE_TYPE_TRX_SYS	7	/*!< Transaction system data */
#define FIL_PAGE_TYPE_FSP_HDR	8	/*!< File space header */
#define FIL_PAGE_TYPE_XDES	9	/*!< Extent descriptor page */
#define FIL_PAGE_TYPE_BLOB	10	/*!< Uncompressed BLOB page */
#define FIL_PAGE_TYPE_ZBLOB	11	/*!< First compressed BLOB page */
#define FIL_PAGE_TYPE_ZBLOB2	12	/*!< Subsequent compressed BLOB page */

#define PAGE_INDEX_ID    28   

#define FSEG_PAGE_DATA          FIL_PAGE_DATA
#define PAGE_HEADER     	FSEG_PAGE_DATA
#define TRX_UNDO_PAGE_HDR       FSEG_PAGE_DATA

#define FIL_ADDR_SIZE 6
#define FLST_NODE_SIZE          (2 * FIL_ADDR_SIZE)

#define TRX_UNDO_PAGE_HDR_SIZE  (6 + FLST_NODE_SIZE)
#define TRX_UNDO_SEG_HDR        (TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE)
#define TRX_UNDO_STATE          0       /*!< TRX_UNDO_ACTIVE, ... */

#define TRX_UNDO_PAGE_TYPE      0       /*!< TRX_UNDO_INSERT or TRX_UNDO_UPDATE */

/* Types of an undo log segment */
#define TRX_UNDO_INSERT         1       /* contains undo entries for inserts */
#define TRX_UNDO_UPDATE         2       /* contains undo entries for updates
                                        and delete markings: in short,
                                        modifys (the name 'UPDATE' is a
                                        historical relic) */
/* States of an undo log segment */
#define TRX_UNDO_ACTIVE         1       /* contains an undo log of an active
                                        transaction */
#define TRX_UNDO_CACHED         2       /* cached for quick reuse */
#define TRX_UNDO_TO_FREE        3       /* insert undo segment can be freed */
#define TRX_UNDO_TO_PURGE       4       /* update undo segment will not be
                                        reused: it can be freed in purge when
                                        all undo data in it is removed */
#define TRX_UNDO_PREPARED       5       /* contains an undo log of an
                                        prepared transaction */

/* command line argument to do page checks (that's it) */
/* another argument to specify page ranges... seek to right spot and go from there */

typedef unsigned long int ulint;
typedef unsigned char uchar;
typedef unsigned int uint32;
typedef unsigned long ulong;

/** Type definition for a 64-bit unsigned integer, which works also
in 32-bit machines. NOTE! Access the fields only with the accessor
functions. This definition appears here only for the compiler to
know the size of a dulint. */
struct dulint_struct{
	ulint	high;	/*!< most significant 32 bits */
	ulint	low;	/*!< least significant 32 bits */
};
typedef	struct dulint_struct	dulint;

int n_undo_state_active;
int n_undo_state_cached;
int n_undo_state_to_free;
int n_undo_state_to_purge;
int n_undo_state_prepared;
int n_undo_state_other;
int n_undo_insert, n_undo_update, n_undo_other;
int n_bad_checksum;
int n_fil_page_index;
int n_fil_page_undo_log;
int n_fil_page_inode;
int n_fil_page_ibuf_free_list;
int n_fil_page_allocated;
int n_fil_page_ibuf_bitmap;
int n_fil_page_type_sys;
int n_fil_page_type_trx_sys;
int n_fil_page_type_fsp_hdr;
int n_fil_page_type_allocated;
int n_fil_page_type_xdes;
int n_fil_page_type_blob;
int n_fil_page_type_zblob;
int n_fil_page_type_other;

int n_fil_page_max_index_id;

#define MAX_INDEX_ID 10000000
unsigned long long index_ids[MAX_INDEX_ID];

/*******************************************************//**
Gets the high-order 32 bits of a dulint.
@return	32 bits in ulint */
ulint
ut_dulint_get_high(
/*===============*/
	dulint	d)	/*!< in: dulint */
{
	return(d.high);
}

/*******************************************************//**
Gets the low-order 32 bits of a dulint.
@return	32 bits in ulint */
ulint
ut_dulint_get_low(
/*==============*/
	dulint	d)	/*!< in: dulint */
{
	return(d.low);
}

/*******************************************************//**
Gets the low-order 32 bits of a dulint.
@return	32 bits in ulint */
unsigned long long
ut_dulint_to_ull(
/*==============*/
	dulint	d)	/*!< in: dulint */
{
	return(((unsigned long long) d.high << 32) |
		(unsigned long long) d.low);
}

/*******************************************************//**
Creates a 64-bit dulint out of two ulints.
@return	created dulint */
dulint
ut_dulint_create(
/*=============*/
	ulint	high,	/*!< in: high-order 32 bits */
	ulint	low)	/*!< in: low-order 32 bits */
{
	dulint	res;

	res.high = high;
	res.low	 = low;

	return(res);
}


/* innodb function in name; modified slightly to not have the ASM version (lots of #ifs that didn't apply) */
ulint mach_read_from_4(uchar *b)
{
  return( ((ulint)(b[0]) << 24)
          + ((ulint)(b[1]) << 16)
          + ((ulint)(b[2]) << 8)
          + (ulint)(b[3])
          );
}

/********************************************************//**
The following function is used to fetch data from 2 consecutive
bytes. The most significant byte is at the lowest address.
@return	ulint integer */
ulint
mach_read_from_2(
/*=============*/
	uchar*	b)	/*!< in: pointer to 2 bytes */
{
	return( ((ulint)(b[0]) << 8)
		+ (ulint)(b[1])
		);
}

/********************************************************//**
The following function is used to fetch data from 8 consecutive
bytes. The most significant byte is at the lowest address.
@return	dulint integer */
dulint
mach_read_from_8(
/*=============*/
	uchar*	b)	/*!< in: pointer to 8 bytes */
{
	ulint	high;
	ulint	low;

	high = mach_read_from_4(b);
	low = mach_read_from_4(b + 4);

	return(ut_dulint_create(high, low));
}

/*********************************************************************//**
Gets the file page type.
@return type; NOTE that if the type has not been written to page, the
return value not defined */
ulint
fil_page_get_type(
/*==============*/
	uchar*	page)	/*!< in: file page */
{
	return(mach_read_from_2(page + FIL_PAGE_TYPE));
}

/**************************************************************//**
Gets the index id field of a page.
@return	index id */
dulint
btr_page_get_index_id(
/*==================*/
	uchar*	page)	/*!< in: index page */
{
	return(mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID));
}

ulint
ut_fold_ulint_pair(
/*===============*/
            /* out: folded value */
    ulint   n1, /* in: ulint */
    ulint   n2) /* in: ulint */
{
    return(((((n1 ^ n2 ^ UT_HASH_RANDOM_MASK2) << 8) + n1)
                        ^ UT_HASH_RANDOM_MASK) + n2);
}

ulint
ut_fold_binary(
/*===========*/
            /* out: folded value */
    uchar*   str,    /* in: string of bytes */
    ulint   len)    /* in: length */
{
    ulint   i;
    ulint   fold= 0;

    for (i= 0; i < len; i++)
    {
      fold= ut_fold_ulint_pair(fold, (ulint)(*str));

      str++;
    }

    return(fold);
}

ulint
buf_calc_page_new_checksum(
/*=======================*/
               /* out: checksum */
    uchar*    page) /* in: buffer page */
{
    ulint checksum;

    /* Since the fields FIL_PAGE_FILE_FLUSH_LSN and ..._ARCH_LOG_NO
    are written outside the buffer pool to the first pages of data
    files, we have to skip them in the page checksum calculation.
    We must also skip the field FIL_PAGE_SPACE_OR_CHKSUM where the
    checksum is stored, and also the last 8 bytes of page because
    there we store the old formula checksum. */

    checksum= ut_fold_binary(page + FIL_PAGE_OFFSET,
                             FIL_PAGE_FILE_FLUSH_LSN - FIL_PAGE_OFFSET)
            + ut_fold_binary(page + FIL_PAGE_DATA,
                             UNIV_PAGE_SIZE - FIL_PAGE_DATA
                             - FIL_PAGE_END_LSN_OLD_CHKSUM);
    checksum= checksum & 0xFFFFFFFF;

    return(checksum);
}

ulint
buf_calc_page_old_checksum(
/*=======================*/
               /* out: checksum */
    uchar*    page) /* in: buffer page */
{
    ulint checksum;

    checksum= ut_fold_binary(page, FIL_PAGE_FILE_FLUSH_LSN);

    checksum= checksum & 0xFFFFFFFF;

    return(checksum);
}

void
parse_page(
/*=======*/
	uchar* page) /* in: buffer page */
{
	unsigned long long id;
	ulint x;

	switch (fil_page_get_type(page)) {
	case FIL_PAGE_INDEX:
		n_fil_page_index++;
		id = ut_dulint_to_ull(btr_page_get_index_id(page));
		if (id < MAX_INDEX_ID)
			index_ids[id]++;
		else
			n_fil_page_max_index_id++;
		break;
	case FIL_PAGE_UNDO_LOG:
		n_fil_page_undo_log++;
		x = mach_read_from_2(page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE);
		if (x == TRX_UNDO_INSERT)
			n_undo_insert++;
		else if (x == TRX_UNDO_UPDATE)
			n_undo_update++;
		else
			n_undo_other++;

		x = mach_read_from_2(page + TRX_UNDO_SEG_HDR + TRX_UNDO_STATE);
		switch (x) {
			case TRX_UNDO_ACTIVE: n_undo_state_active++; break;
			case TRX_UNDO_CACHED: n_undo_state_cached++; break;
			case TRX_UNDO_TO_FREE: n_undo_state_to_free++; break;
			case TRX_UNDO_TO_PURGE: n_undo_state_to_purge++; break;
			case TRX_UNDO_PREPARED: n_undo_state_prepared++; break;
			default: n_undo_state_other++; break;
		}
		break;
	case FIL_PAGE_INODE:
		n_fil_page_inode++;
		break;
	case FIL_PAGE_IBUF_FREE_LIST:
		n_fil_page_ibuf_free_list++;
		break;
	case FIL_PAGE_TYPE_ALLOCATED:
		n_fil_page_type_allocated++;
		break;
	case FIL_PAGE_IBUF_BITMAP:
		n_fil_page_ibuf_bitmap++;
		break;
	case FIL_PAGE_TYPE_SYS:
		n_fil_page_type_sys++;
		break;
	case FIL_PAGE_TYPE_TRX_SYS:
		n_fil_page_type_trx_sys++;
		break;
	case FIL_PAGE_TYPE_FSP_HDR:
		n_fil_page_type_fsp_hdr++;
		break;
	case FIL_PAGE_TYPE_XDES:
		n_fil_page_type_xdes++;
		break;
	case FIL_PAGE_TYPE_BLOB:
		n_fil_page_type_blob++;
		break;
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:
		n_fil_page_type_zblob++;
		break;
	default:
		n_fil_page_type_other++;
	}
}

void
print_stats()
/*========*/
{
	unsigned long long i;

	printf("%d\tbad checksum\n", n_bad_checksum);
	printf("%d\tFIL_PAGE_INDEX\n", n_fil_page_index);
	printf("%d\tFIL_PAGE_UNDO_LOG\n", n_fil_page_undo_log);
	printf("%d\tFIL_PAGE_INODE\n", n_fil_page_inode);
	printf("%d\tFIL_PAGE_IBUF_FREE_LIST\n", n_fil_page_ibuf_free_list);
	printf("%d\tFIL_PAGE_TYPE_ALLOCATED\n", n_fil_page_type_allocated);
	printf("%d\tFIL_PAGE_IBUF_BITMAP\n", n_fil_page_ibuf_bitmap);
	printf("%d\tFIL_PAGE_TYPE_SYS\n", n_fil_page_type_sys);
	printf("%d\tFIL_PAGE_TYPE_TRX_SYS\n", n_fil_page_type_trx_sys);
	printf("%d\tFIL_PAGE_TYPE_FSP_HDR\n", n_fil_page_type_fsp_hdr);
	printf("%d\tFIL_PAGE_TYPE_XDES\n", n_fil_page_type_xdes);
	printf("%d\tFIL_PAGE_TYPE_BLOB\n", n_fil_page_type_blob);
	printf("%d\tFIL_PAGE_TYPE_ZBLOB\n", n_fil_page_type_zblob);
	printf("%d\tother\n", n_fil_page_type_other);
	printf("%d\tmax index_id\n", n_fil_page_max_index_id);
	printf("undo type: %d insert, %d update, %d other\n",
		n_undo_insert, n_undo_update, n_undo_other);
	printf("undo state: %d active, %d cached, %d to_free, %d to_purge,"
		" %d prepared, %d other\n", n_undo_state_active,
		n_undo_state_cached, n_undo_state_to_free,
		n_undo_state_to_purge, n_undo_state_prepared, n_undo_state_other);

	printf("#pages\tindex_id\n");
	for (i=0; i < MAX_INDEX_ID; i++) {
		if (index_ids[i])
			printf("%lld\t%lld\n", index_ids[i], i);
	}
}

int main(int argc, char **argv)
{
  FILE *f;                     /* our input file */
  uchar *p;                     /* storage of pages read */
  int bytes;                   /* bytes read count */
  ulint ct;                    /* current page number (0 based) */
  int now;                     /* current time */
  int lastt;                   /* last time */
  ulint oldcsum, oldcsumfield, csum, csumfield, logseq, logseqfield; /* ulints for checksum storage */
  struct stat st;              /* for stat, if you couldn't guess */
  unsigned long long int size; /* size of file (has to be 64 bits) */
  ulint pages;                 /* number of pages in file */
  ulint start_page= 0, end_page= 0, use_end_page= 0; /* for starting and ending at certain pages */
  off_t offset= 0;
  int just_count= 0;          /* if true, just print page count */
  int verbose= 0;
  int debug= 0;
  int c;
  int fd;
  int skip_corrupt= 0;

  /* remove arguments */
  while ((c= getopt(argc, argv, "cvds:e:p:u")) != -1)
  {
    switch (c)
    {
    case 'v':
      verbose= 1;
      break;
    case 'c':
      just_count= 1;
      break;
    case 's':
      start_page= atoi(optarg);
      break;
    case 'e':
      end_page= atoi(optarg);
      use_end_page= 1;
      break;
    case 'p':
      start_page= atoi(optarg);
      end_page= atoi(optarg);
      use_end_page= 1;
      break;
    case 'd':
      debug= 1;
      break;
    case 'u':
      skip_corrupt= 1;
      break;
    case ':':
      fprintf(stderr, "option -%c requires an argument\n", optopt);
      return 1;
      break;
    case '?':
      fprintf(stderr, "unrecognized option: -%c\n", optopt);
      return 1;
      break;
    }
  }

  /* debug implies verbose... */
  if (debug) verbose= 1;

  /* make sure we have the right arguments */
  if (optind >= argc)
  {
    printf("InnoDB offline file checksum utility.\n");
    printf("usage: %s [-c] [-s <start page>] [-e <end page>] [-p <page>] [-v] [-d] <filename>\n", argv[0]);
    printf("\t-c\tprint the count of pages in the file\n");
    printf("\t-s n\tstart on this page number (0 based)\n");
    printf("\t-e n\tend at this page number (0 based)\n");
    printf("\t-p n\tcheck only this page (0 based)\n");
    printf("\t-v\tverbose (prints progress every 5 seconds)\n");
    printf("\t-d\tdebug mode (prints checksums for each page)\n");
    return 1;
  }

  /* stat the file to get size and page count */
  if (stat(argv[optind], &st))
  {
    perror("error statting file");
    return 1;
  }
  size= st.st_size;
  pages= size / UNIV_PAGE_SIZE;
  if (just_count)
  {
    printf("%lu\n", pages);
    return 0;
  }
  else if (verbose)
  {
    printf("file %s = %llu bytes (%lu pages)...\n", argv[optind], size, pages);
    printf("checking pages in range %lu to %lu\n", start_page, use_end_page ? end_page : (pages - 1));
  }

  /* open the file for reading */
  f= fopen(argv[optind], "r");
  if (!f)
  {
    perror("error opening file");
    return 1;
  }

  if (posix_fadvise(fileno(f), 0, 0, POSIX_FADV_SEQUENTIAL) ||
      posix_fadvise(fileno(f), 0, 0, POSIX_FADV_NOREUSE))
  {
    perror("posix_fadvise failed");
  }

  /* seek to the necessary position */
  if (start_page)
  {
    fd= fileno(f);
    if (!fd)
    {
      perror("unable to obtain file descriptor number");
      return 1;
    }

    offset= (off_t)start_page * (off_t)UNIV_PAGE_SIZE;

    if (lseek(fd, offset, SEEK_SET) != offset)
    {
      perror("unable to seek to necessary offset");
      return 1;
    }
  }

  /* allocate buffer for reading (so we don't realloc every time) */
  p= (uchar *)malloc(UNIV_PAGE_SIZE);

  /* main checksumming loop */
  ct= start_page;
  lastt= 0;
  while (!feof(f))
  {
    int page_ok = 1;

    bytes= fread(p, 1, UNIV_PAGE_SIZE, f);
    if (!bytes && feof(f))
    {
      print_stats();
      return 0;
    }

    if (bytes != UNIV_PAGE_SIZE)
    {
      fprintf(stderr, "bytes read (%d) doesn't match universal page size (%d)\n", bytes, UNIV_PAGE_SIZE);
      print_stats();
      return 1;
    }

    /* check the "stored log sequence numbers" */
    logseq= mach_read_from_4(p + FIL_PAGE_LSN + 4);
    logseqfield= mach_read_from_4(p + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM + 4);
    if (debug)
      printf("page %lu: log sequence number: first = %lu; second = %lu\n", ct, logseq, logseqfield);
    if (logseq != logseqfield)
    {
      fprintf(stderr, "page %lu invalid (fails log sequence number check)\n", ct);
      if (!skip_corrupt) return 1;
      page_ok = 0;
    }

    /* check old method of checksumming */
    oldcsum= buf_calc_page_old_checksum(p);
    oldcsumfield= mach_read_from_4(p + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM);
    if (debug)
      printf("page %lu: old style: calculated = %lu; recorded = %lu\n", ct, oldcsum, oldcsumfield);
    if (oldcsumfield != mach_read_from_4(p + FIL_PAGE_LSN) && oldcsumfield != oldcsum)
    {
      fprintf(stderr, "page %lu invalid (fails old style checksum)\n", ct);
      if (!skip_corrupt) return 1;
      page_ok = 0;
    }

    /* now check the new method */
    csum= buf_calc_page_new_checksum(p);
    csumfield= mach_read_from_4(p + FIL_PAGE_SPACE_OR_CHKSUM);
    if (debug)
      printf("page %lu: new style: calculated = %lu; recorded = %lu\n",
          ct, csum, csumfield);
    if (csumfield != 0 && csum != csumfield)
    {
      fprintf(stderr, "page %lu invalid (fails new style checksum)\n", ct);
      if (!skip_corrupt) return 1;
      page_ok = 0;
    }

    /* end if this was the last page we were supposed to check */
    if (use_end_page && (ct >= end_page))
    {
      print_stats();
      return 0;
    }

    ct++;

    if (!page_ok)
    {
      n_bad_checksum++;
      continue;
    }

    parse_page(p);

    /* progress printing */
    if (verbose)
    {
      if (ct % 10000 == 0)
      {
        now= time(0);
        if (!lastt) lastt= now;
        if (now - lastt >= 1)
        {
          fprintf(stderr, "page %lu okay: %.3f%% done\n", (ct - 1), (float) ct / pages * 100);
          lastt= now;
        }
      }
    }
  }
  print_stats();

  return 0;
}

