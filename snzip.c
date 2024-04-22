/* -*- indent-tabs-mode: nil -*-
 *
 * Copyright 2011-2016 Kubo Takehiro <kubo@jiubao.org>
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright notice, this list
 *       of conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ''AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those of the
 * authors and should not be interpreted as representing official policies, either expressed
 * or implied, of the authors.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <fcntl.h>
#include <snappy-c.h>
#ifdef WIN32
/* Windows */
#include <windows.h>
#include <io.h>
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#define PATH_DELIMITER '\\'
#define OPTIMIZE_SEQUENTIAL "S" /* flag to optimize sequential access */
#else
/* Unix */
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#define PATH_DELIMITER '/'
#define OPTIMIZE_SEQUENTIAL ""
#endif
#include "snzip.h"
#ifdef WIN32
#define stat _stati64
#define fstat _fstati64
#endif
#ifndef HAVE_GETOPT
#include "win32/ya_getopt.h"
#endif

#if defined HAVE_STRUCT_STAT_ST_MTIMENSEC
#define SNZ_ST_TIME_NSEC(sbuf, type) ((sbuf).st_##type##timensec)
#elif defined HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC
#define SNZ_ST_TIME_NSEC(sbuf, type) ((sbuf).st_##type##tim.tv_nsec)
#elif defined HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC
#define SNZ_ST_TIME_NSEC(sbuf, type) ((sbuf).st_##type##timespec.tv_nsec)
#else
#define SNZ_ST_TIME_NSEC(sbuf, type) (0)
#endif

int64_t uncompressed_source_len = -1;
int32_t snzip_format_block_size;
uint32_t hadoop_snappy_source_length;
uint32_t hadoop_snappy_compressed_length;

static int trace_flag = FALSE;

static void copy_file_attributes(int infd, int outfd, const char *outfile);
static void show_usage(const char *progname, int exit_code);

static stream_format_t *stream_formats[] = {
  &framing2_format,
  &hadoop_snappy_format,
#ifdef SUPPORT_RAW_FORMAT
  &raw_format,
#endif
  &iwa_format,
  &framing_format,
  &snzip_format,
  &snappy_java_format,
  &snappy_in_java_format,
  &comment_43_format,
};
#define NUM_OF_STREAM_FORMATS (sizeof(stream_formats)/sizeof(stream_formats[0]))

static stream_format_t *find_stream_format_by_name(const char *name)
{
  int idx;
  for (idx = 0; idx < NUM_OF_STREAM_FORMATS; idx++) {
    if (strcmp(stream_formats[idx]->name, name) == 0) {
      return stream_formats[idx];
    }
  }
  return NULL;
}

static stream_format_t *find_stream_format_by_suffix(const char *suffix)
{
  int idx;
  for (idx = 0; idx < NUM_OF_STREAM_FORMATS; idx++) {
    if (strcmp(stream_formats[idx]->suffix, suffix) == 0) {
      return stream_formats[idx];
    }
  }
  return NULL;
}

static stream_format_t *find_stream_format_by_file_header(FILE *fp)
{
  /*  framing        {0xff, 0x06, 0x00, 's',  'N',  'a',  'P',  'p',  'Y'}
   *  framing2       {0xff, 0x06, 0x00, 0x00, 's',  'N',  'a',  'P',  'p',  'Y'}
   *  hadoop-snappy  {--uncompressed length--,--compressed length--,
   *  snzip          {'S',  'N',  'Z',  0x01, block_size}
   *  snappy-java    {0x82, 'S',  'N',  'A',  'P',  'P',  'Y',  0x00}
   *  snappy-in-java {'s',  'n',  'a',  'p',  'p',  'y',  0x00}
   */
  union {
    uint8_t buf[10];
    uint32_t len[2];
  } u;
  size_t idx = 0;
  int chr;
  size_t max_compressed_length;

#define GETCHAR()  ((u.buf[idx++] = chr = getc(fp)))
#define CHK(chr) if (GETCHAR() != (chr)) goto error
  switch (GETCHAR()) {
  case 0xff:
    CHK(0x06); CHK(0x00);
    switch (GETCHAR()) {
    case 's':
      switch (GETCHAR()) {
      case 'N':
        CHK('a'); CHK('P'); CHK('p'); CHK('Y');
        return &framing_format;
      case 'n':
        CHK('a'); CHK('p'); CHK('p'); CHK('y');
        return &comment_43_format;
      }
      break;
    case 0x00:
      CHK('s');  CHK('N'); CHK('a'); CHK('P'); CHK('p'); CHK('Y');
      return &framing2_format;
    }
    break;
  case 'S':
    CHK('N'); CHK('Z'); CHK(0x01);
    snzip_format_block_size = GETCHAR();
    if (snzip_format_block_size <= 0) {
      goto error;
    }
    return &snzip_format;
  case 0x82:
    CHK('S'); CHK('N'); CHK('A'); CHK('P'); CHK('P'); CHK('Y'); CHK(0x00);
    return &snappy_java_format;
  case 's':
    CHK('n'); CHK('a'); CHK('p'); CHK('p'); CHK('y'); CHK(0x00);
    return &snappy_in_java_format;
  }
 error:
  while (idx < sizeof(u.len)) {
    GETCHAR();
  }
  hadoop_snappy_source_length = SNZ_FROM_BE32(u.len[0]);
  hadoop_snappy_compressed_length = SNZ_FROM_BE32(u.len[1]);
  max_compressed_length = snappy_max_compressed_length(hadoop_snappy_max_input_size(0));
  if (hadoop_snappy_compressed_length <= max_compressed_length) {
    /* This may be hadoop-snappy format */
    return &hadoop_snappy_format;
  }
  fprintf(stderr, "Unknown file header\n");
  return NULL;
}

int trc_lineno;
const char *trc_filename = __FILE__;

void print_error_(const char *fmt, ...)
{
  va_list ap;

  if (trace_flag) {
    fprintf(stderr, "%s:%3d: ", trc_filename, trc_lineno);
  }
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

void trace_(const char *fmt, ...)
{
  va_list ap;

  if (!trace_flag) {
    return;
  }
  fprintf(stderr, "%s:%3d: ", trc_filename, trc_lineno);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

int main(int argc, char **argv)
{
  int opt;
  int opt_uncompress = FALSE;
  int opt_keep = FALSE;
  int opt_stdout = FALSE;
  int block_size = 0;
  size_t rsize = 0;
  size_t wsize = 0;
  const char *output_path = NULL;
  const char *format_name = NULL;
  stream_format_t *fmt = &DEFAULT_FORMAT;

  char *progname = strrchr(argv[0], PATH_DELIMITER);
  if (progname != NULL) {
    progname++;
  } else {
    progname = argv[0];
  }

  trace("progname = %s\n", progname);
  if (strstr(progname, "un") != NULL) {
    trace("\"un\" is found in %s\n", progname);
    opt_uncompress = TRUE;
  }
  if (strstr(progname, "cat") != NULL) {
    trace("\"cat\" is found in %s\n", progname);
    opt_stdout = TRUE;
    opt_uncompress = TRUE;
    opt_keep = TRUE;
  }

  while ((opt = getopt(argc, argv, "cdko:t:hs:b:B:R:W:T")) != -1) {
    char *endptr;

    switch (opt) {
    case 'c':
      opt_stdout = TRUE;
      opt_keep = TRUE;
      break;
    case 'd':
      opt_uncompress = TRUE;
      break;
    case 'k':
      opt_keep = TRUE;
      break;
    case 'o':
      output_path = optarg;
      opt_keep = TRUE;
      break;
    case 't':
      format_name = optarg;
      break;
    case 'h':
      show_usage(progname, 0);
      break;
    case 's':
      uncompressed_source_len = strtoull(optarg, &endptr, 10);
      if (*endptr != '\0') {
        fprintf(stderr, "Invalid -s format: %s\n", optarg);
        return 1;
      }
      break;
    case 'b':
      block_size = atoi(optarg);
      break;
    case 'B':
      block_size = 1ul << atoi(optarg);
      break;
    case 'R':
      rsize = strtoul(optarg, NULL, 10);
      break;
    case 'W':
      wsize = strtoul(optarg, NULL, 10);
      break;
    case 'T':
      trace_flag = TRUE;
      break;
    case '?':
      show_usage(progname, 1);
      break;
    }
  }

#ifdef WIN32
  _setmode(0, _O_BINARY);
  _setmode(1, _O_BINARY);
#endif

  if (format_name != NULL) {
    fmt = find_stream_format_by_name(format_name);
    if (fmt == NULL) {
      fprintf(stderr, "Unknown file format name %s\n", format_name);
      return 1;
    }
  }

  if (optind == argc) {
    trace("no arguments are set.\n");

    if (opt_uncompress) {
      int skip_magic = 0;
      if (format_name == NULL) {
        fmt = find_stream_format_by_file_header(stdin);
        if (fmt == NULL) {
          return 1;
        }
        skip_magic = 1;
      }
      return fmt->uncompress(stdin, stdout, skip_magic);
    } else {
      if (isatty(1)) {
        /* stdout is a terminal */
        fprintf(stderr, "I won't write compressed data to a terminal.\n");
        fprintf(stderr, "For help, type: '%s -h'.\n", progname);
        return 1;
      }
      return fmt->compress(stdin, stdout, block_size);
    }
  }

  while (optind < argc) {
    char *infile = argv[optind++];
    size_t infilelen = strlen(infile);
    char outfile[PATH_MAX];
    FILE *infp;
    FILE *outfp;
    int skip_magic = 0;

    /* check input file and open it. */
    const char *suffix = strrchr(infile, '.');
    if (suffix != NULL) {
      stream_format_t *fmt_tmp = find_stream_format_by_suffix(suffix + 1);
      if (fmt_tmp == NULL && opt_uncompress) {
        print_error("%s has unknown suffix.\n", infile);
        continue;
      }
      if (fmt_tmp != NULL && !opt_uncompress) {
        print_error("%s already has %s suffix\n", infile, fmt_tmp->suffix);
        continue;
      }
    }

    infp = fopen(infile, "rb" OPTIMIZE_SEQUENTIAL);
    if (infp == NULL) {
      print_error("Failed to open %s for read\n", infile);
      exit(1);
    }
    if (rsize != 0) {
      trace("setvbuf(infp, NULL, _IOFBF, %ld)\n", (long)rsize);
      setvbuf(infp, NULL, _IOFBF, rsize);
    }
#ifdef HAVE_POSIX_FADVISE
    posix_fadvise(fileno(infp), 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    /* determine the file format */
    if (opt_uncompress) {
      if (format_name == NULL) {
        fmt = find_stream_format_by_file_header(infp);
        skip_magic = 1;
      }
      if (fmt == NULL) {
        exit(1);
      }
    }

    /* check output file and open it. */
    if (opt_stdout) {
      strcpy(outfile, "-");
      outfp = stdout;
    } else {
      size_t suffixlen = strlen(fmt->suffix);
      size_t reffilelen = infilelen;
      char *reffile = malloc(PATH_MAX);
      if (output_path) {
        /* if output_path is specified define reffile accordingly */
        const char *lastpath = strrchr(infile, '/');
        if (lastpath != NULL) {
          sprintf(reffile, "%s/%s", output_path, lastpath + 1);
        }
        else {
            sprintf(reffile, "%s", output_path);
        }
        reffilelen = strlen(reffile);
      }
      else {
          sprintf(reffile, "%s", infile);
      }
      if (opt_uncompress) {
        /* check suffix */
        const char *suffix = strrchr(reffile, '.');
        int remove_suffix = (suffix != NULL && strcmp(suffix + 1, fmt->suffix) == 0);
        size_t new_size = reffilelen + (remove_suffix ? (- suffixlen) : 4);
        if (new_size >= sizeof(outfile)) {
          print_error("%s has too long file name.\n", reffile);
          exit(1);
        }
        if (remove_suffix) {
          memcpy(outfile, reffile, reffilelen - suffixlen - 1);
          outfile[reffilelen - suffixlen - 1] = '\0';
        } else {
          fprintf(stderr, "%s: Can't guess original name for %s -- using %s.out\n",
                  progname, reffile, reffile);
          snprintf(outfile, sizeof(outfile), "%s.out", reffile);
        }
      } else {
        if (reffilelen + suffixlen + 2 >= sizeof(outfile)) {
          print_error("%s has too long file name.\n", reffile);
          exit(1);
        }
        sprintf(outfile, "%s.%s", reffile, fmt->suffix);
      }
      outfp = fopen(outfile, "wb" OPTIMIZE_SEQUENTIAL);
      if (outfp == NULL) {
        print_error("Failed to open %s for write\n", outfile);
        exit(1);
      }
    }
    if (wsize != 0) {
      trace("setvbuf(outfp, NULL, _IOFBF, %ld)\n", (long)wsize);
      setvbuf(outfp, NULL, _IOFBF, wsize);
    }

    if (opt_uncompress) {
      trace("uncompress %s\n", infile);
      if (fmt->uncompress(infp, outfp, skip_magic) != 0) {
        if (outfp != stdout) {
          unlink(outfile);
        }
        return 1;
      }
    } else {
      trace("compress %s\n", infile);
      if (fmt->compress(infp, outfp, block_size) != 0) {
        if (outfp != stdout) {
          unlink(outfile);
        }
        return 1;
      }
    }

    if (!opt_stdout) {
      fflush(outfp);
      copy_file_attributes(fileno(infp), fileno(outfp), outfile);
    }

    fclose(infp);
    if (outfp != stdout) {
      fclose(outfp);
    }

    if (!opt_keep) {
      int rv = unlink(infile);
      trace("unlink(\"%s\") => %d (errno = %d)\n",
            infile, rv, rv ? errno : 0);
    }
  }
  return 0;
}

static void copy_file_attributes(int infd, int outfd, const char *outfile)
{
#ifdef WIN32
  BY_HANDLE_FILE_INFORMATION fi;
  BOOL bOk;

  bOk = GetFileInformationByHandle((HANDLE)_get_osfhandle(infd), &fi);
  trace("GetFileInformationByHandle(...) => %s\n", bOk ? "TRUE" : "FALSE");
  if (bOk) {
    bOk = SetFileTime((HANDLE)_get_osfhandle(outfd), NULL, &fi.ftLastAccessTime, &fi.ftLastWriteTime);
    trace("SetFileTime(...) => %s\n", bOk ? "TRUE" : "FALSE");
    bOk = SetFileAttributesA(outfile, fi.dwFileAttributes);
    trace("SetFileAttributesA(...) => %s\n", bOk ? "TRUE" : "FALSE");
  }
#else
  struct stat sbuf;
#ifdef HAVE_FUTIMENS
  struct timespec times[2];
#else
  struct timeval times[2];
#endif
  int rv;

  if ((rv = fstat(infd, &sbuf)) != 0) {
    trace("fstat(%d, &sbuf) => %d (errno = %d)\n",
          infd, rv, errno);
    return;
  }

  /* copy file times. */
#ifdef HAVE_FUTIMENS
  times[0].tv_sec = sbuf.st_atime;
  times[0].tv_nsec = SNZ_ST_TIME_NSEC(sbuf, a);
  times[1].tv_sec = sbuf.st_mtime;
  times[1].tv_nsec = SNZ_ST_TIME_NSEC(sbuf, m);
  rv = futimens(outfd, times);
  trace("futimens(%d, [{%ld, %ld}, {%ld, %ld}]) => %d\n",
        outfd, times[0].tv_sec, times[0].tv_nsec,
        times[1].tv_sec, times[1].tv_nsec, rv);
#else /* HAVE_FUTIMENS */
  times[0].tv_sec = sbuf.st_atime;
  times[0].tv_usec = SNZ_ST_TIME_NSEC(sbuf, a) / 1000;
  times[1].tv_sec = sbuf.st_mtime;
  times[1].tv_usec = SNZ_ST_TIME_NSEC(sbuf, m) / 1000;
#ifdef HAVE_FUTIMES
  rv = futimes(outfd, times);
  trace("futimes(%d, [{%ld, %ld}, {%ld, %ld}]) => %d\n",
        outfd, times[0].tv_sec, times[0].tv_usec,
        times[1].tv_sec, times[1].tv_usec, rv);
#else /* HAVE_FUTIMES */
  rv = utimes(outfile, times);
  trace("utimes(\"%s\", [{%ld, %ld}, {%ld, %ld}]) => %d\n",
        outfile, times[0].tv_sec, times[0].tv_usec,
        times[1].tv_sec, times[1].tv_usec, rv);
#endif /* HAVE_FUTIMES */
#endif /* HAVE_FUTIMENS */

  /* copy other attributes */
  rv = fchown(outfd, sbuf.st_uid, sbuf.st_gid);
  trace("fchown(%d, %d, %d) => %d\n",
        outfd, sbuf.st_uid, sbuf.st_gid, rv);
  rv = fchmod(outfd, sbuf.st_mode);
  trace("fchmod(%d, 0%o) => %d\n",
        outfd, sbuf.st_mode, rv);
#endif
}

static void show_usage(const char *progname, int exit_code)
{
  int idx;
  int max_name_len;
  int max_suffix_len;

  fprintf(stderr,
          PACKAGE_STRING "\n"
          "\n"
          "  Usage: %s [option ...] [file ...]\n"
          "\n"
          "  general options:\n"
          "   -c       output to standard output, keep original files unchanged\n"
          "   -d       decompress\n"
          "   -k       keep (don't delete) input files\n"
          "   -o path  output to the specified path, keep original files unchanged\n"
          "   -t name  file format name. see below. The default format is %s.\n"
          "   -h       give this help\n"
          "\n"
          "  raw_format option:\n"
          "   -s size  size of input data when compressing.\n"
          "            The default value is the file size if available.\n"
          "\n"
          "  tuning options:\n"
          "   -b num   internal block size in bytes\n"
          "   -B num   internal block size. 'num'-th power of two.\n"
          "   -R num   size of read buffer in bytes\n"
          "   -W num   size of write buffer in bytes\n"
          "   -T       trace for debug\n"
          "\n"
          "  supported formats:\n",
          progname, DEFAULT_FORMAT.name);

  max_name_len = strlen("name");
  max_suffix_len = strlen("suffix");
  for (idx = 0; idx < NUM_OF_STREAM_FORMATS; idx++) {
    stream_format_t *fmt = stream_formats[idx];
    if (max_name_len < strlen(fmt->name)) {
      max_name_len = strlen(fmt->name);
    }
    if (max_suffix_len < strlen(fmt->suffix)) {
      max_suffix_len = strlen(fmt->suffix);
    }
  }
  fprintf(stderr, "    %*s  %*s  URL\n", -max_name_len, "NAME", -max_suffix_len, "SUFFIX");
  fprintf(stderr, "    %*s  %*s  ---\n", -max_name_len, "----", -max_suffix_len, "------");
  for (idx = 0; idx < NUM_OF_STREAM_FORMATS; idx++) {
    stream_format_t *fmt = stream_formats[idx];
    fprintf(stderr, "    %*s  %*s  %s\n", -max_name_len, fmt->name, -max_suffix_len, fmt->suffix, fmt->url);
  }
  exit(exit_code);
}

int work_buffer_init(work_buffer_t *wb, size_t block_size)
{
  wb->uclen = block_size;
  wb->uc = malloc(wb->uclen);
  if (wb->uc == NULL) {
    fprintf(stderr, "Out of memory\n");
    exit(1);
  }
  wb->clen = snappy_max_compressed_length(wb->uclen);
  wb->c = malloc(wb->clen);
  if (wb->c == NULL) {
    fprintf(stderr, "Out of memory\n");
    exit(1);
  }
  trace("max length of compressed data = %lu\n", wb->clen);
  trace("max length of uncompressed data = %lu\n", wb->uclen);
  return 0;
}

void work_buffer_free(work_buffer_t *wb)
{
  free(wb->c);
  free(wb->uc);
}

void work_buffer_resize(work_buffer_t *wb, size_t clen, size_t uclen)
{
  if (clen != 0) {
    wb->clen = clen;
    wb->c = realloc(wb->c, clen);
    if (wb->c == NULL) {
      fprintf(stderr, "Out of memory\n");
      exit(1);
    }
  }
  if (uclen != 0) {
    wb->uclen = uclen;
    wb->uc = realloc(wb->uc, uclen);
    if (wb->uc == NULL) {
      fprintf(stderr, "Out of memory\n");
      exit(1);
    }
  }
}

int write_full(int fd, const void *buf, size_t count)
{
  const char *ptr = (const char *)buf;

  while (count > 0) {
    int rv = write(fd, ptr, count);
    if (rv == -1) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    ptr += rv;
    count -= rv;
  }
  return (ptr - (const char *)buf);
}
