/*
 * Copyright (c) 2011 - 2012, Simon Fraser University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification,
 * are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list
 * of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 * this
 *   list of conditions and the following disclaimer in the documentation and/or
 * other
 *   materials provided with the distribution.
 * - Neither the name of the Simon Fraser University nor the names of its
 * contributors may be
 *   used to endorse or promote products derived from this software without
 * specific
 *   prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Author         : Ibrahim Numanagic
 * Email          : inumanag AT sfu DOT ca
 * Last Update    : 25. vii 2012.
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arithmetic.h"
#include "buffio.h"
#include "const.h"
#include "decompress.h"
#include "names.h"
#include "reads.h"

static const char *last_strstr(const char *haystack, const char *needle) {
  if (*needle == '\0')
    return (char *)haystack;

  const char *result = NULL;
  for (;;) {
    const char *p = strstr(haystack, needle);
    if (p == NULL)
      break;
    result = p;
    haystack = p + 1;
  }

  return result;
}

void get_file_name(char *name, char c) {
  char *p;
  if (p = (char *)last_strstr((const char *)name, ".scalce")) {
    *(p + 7) = c;
  }
}

void decompress(const char *path, const char *out) {
  uint8_t buffer[MAXLINE], buffer2[MAXLINE];

  aho_trie *trie = _pattern_path[0] ? read_patterns_from_file(_pattern_path)
                                    : read_patterns();
  ac_init();

  int32_t len[2];
  int64_t nl[2], phredOff[2];

  int mode[2] = {IO_SYS, IO_SYS};
  uint8_t c[2];

  buffered_file fR[2], fQ[2], fN[2], fL[2];

  LOG("using %d threads...\n", _thread_count);
  strncpy((char *)buffer, (char *)path, MAXLINE);
  for (int i = 0; i < 1 + (_use_second_file | _interleave); i++) {
    /* file type detection */
    get_file_name((char *)buffer, 'r');
    FILE *ft = fopen((char *)buffer, "rb");
    if (!ft) {
      LOG("Cannot open file %s (error code %d)\n", buffer, errno);
      exit(errno);
    }
    fread(c, 1, 2, ft);
    int mode = IO_SYS;
    if (c[0] == 0x1f && c[1] == 0x8b) {
      mode = IO_GZIP;
      //			if (_thread_count > 1)
      //				mode = IO_PGZIP;
    }
    if (c[0] == 0x42 && c[1] == 0x5a)
      mode = IO_BZIP;
    fclose(ft);
    LOG("Paired-end %d, file format detected: %s\n", i,
        (mode == IO_GZIP
             ? "gzip"
             : (mode == IO_BZIP ? "bzip"
                                : (mode == IO_PGZIP ? "pigz" : "plain"))));

    /* initialization */
    f_init(fR + i, mode);
    f_init(fN + i, mode);

    /* open read, name and quality file */
    get_file_name((char *)buffer, 'r');
    f_open(fR + i, (char *)buffer, IO_READ);
    if (!f_alive(fR + i))
      ERROR("Cannot find read file %s!\n", buffer);
    get_file_name((char *)buffer, 'n');
    f_open(fN + i, (char *)buffer, IO_READ);
    if (!f_alive(fN + i) && _use_names)
      ERROR("Cannot find name file %s! Use -n parameter if you want to skip "
            "name file lookup.\n",
            buffer);

#ifdef PACBIO
    f_init(fL + i, mode);
    get_file_name((char *)buffer, 'l');
    f_open(fL + i, (char *)buffer, IO_READ);
    if (!f_alive(fL + i))
      ERROR("Cannot find name file %s!\n", buffer);
#endif

    /* read magic */
    f_read(fR + i, buffer2, 8);

    buffer2[8] = 0;
    LOG("File magic: %s\n", buffer2 + 6);
    _no_ac = 0;
    if (buffer2[6] == '2' && buffer2[7] >= '2')
      f_read(fR + i, &_no_ac, sizeof(int));

    f_init(fQ + i, _no_ac ? mode : IO_SYS);
    get_file_name((char *)buffer, 'q');
    f_open(fQ + i, (char *)buffer, IO_READ);
    if (!f_alive(fQ + i))
      ERROR("Cannot find quality file %s!\n", buffer);

    if (_compress_qualities)
      f_read(fQ + i, buffer, 8);
    f_read(fN + i, buffer, 8);
    /* read metadata */
    f_read(fR + i, len + i, sizeof(int32_t));

    if (_compress_qualities)
      f_read(fQ + i, phredOff + i, sizeof(int64_t));

    if (_compress_qualities && !_no_ac) {
      for (int x = 0; x < AC_DEPTH; x++) // write stat stuff
        for (int j = 0; j < AC_DEPTH; j++)
          for (int k = 0; k < AC_DEPTH; k++) {
            uint32_t y;
            f_read(fQ + i, &y, sizeof(uint32_t));
            ac_freq4[i][(x * AC_DEPTH + j) * AC_DEPTH + k] = y;
          }
    }

    if (!i)
      strncpy((char *)buffer, get_second_file(path), MAXLINE);
  }
  if (!_use_names)
    LOG("Using library name %s\n", _library_name);
  // if ((_interleave|_use_second_file) && nl[1] != nl[0])
  //	ERROR("Paired-ends do not have the same number of reads.\n");

  /* prepare output files */
  buffered_file fo[2], *pfo[2];
  int file = 0;
  for (int i = 0; i < 1 + _use_second_file; i++) {
    f_init(fo + i, IO_SYS);
    pfo[i] = fo + i;

    //	if (_split_reads)
    //		snprintf((char*)buffer, MAXLINE, "%s_%d.part%02d.fastq", out,i+1,
    //file++);
    if (!strcmp("-", out))
      snprintf((char *)buffer, MAXLINE, "-");
    else {
      if (_split_reads)
        snprintf((char *)buffer, MAXLINE, "%s.%d_%d.fastq", out, 1, i + 1);
      else
        snprintf((char *)buffer, MAXLINE, "%s_%d.fastq", out, i + 1);
    }
    f_open(fo + i, (char *)buffer, IO_WRITE);
  }
  //	if (_interleave)
  //		pfo[1] = fo;

  int bc[2];
  bc[0] = SZ_QUAL(len[0]);
  bc[1] = SZ_QUAL(len[1]);
  //	LOG("--->%d\n",bc[0]);
  char alphabet[] = "ACGT";
  uint8_t l[MAXLINE], o[MAXLINE], ox[MAXLINE], chr, off;
  char names = 0;
  int64_t nameidx = 0;
  //	int64_t limit = (_split_reads ? _split_reads : nl[0]);
  char library[MAXLINE + 1];
  if (!_use_names) {
    strncpy(library, _library_name, MAXLINE);
    nameidx = 0;
  } else { // both ends should use same library name and starting number
    f_read(fN, &names, 1);
    if (_use_second_file | _interleave) /* repeat */
      f_read(fN + 1, &names, 1);
    if (!names) {
      f_read(fN, &nameidx, sizeof(int64_t));
      int t = f_read(fN, library, MAXLINE);
      library[t] = 0;

      if (_use_second_file | _interleave) { /* repeat */
        f_read(fN + 1, &nameidx, sizeof(int64_t));
        int t = f_read(fN + 1, library, MAXLINE);
        library[t] = 0;
      }
    }
  }

  int sz_meta = 1;
  if (len[0] > 255)
    sz_meta = 2;
#ifdef PACBIO
  sz_meta = 4;
#endif
  int64_t read_next_read_info = 0;

  for (int F = 0; F < 1 + (_interleave | _use_second_file); F++) {
    int64_t reads_so_far = 0, files_so_far = 1;

    int32_t core, corlen;
    uint32_t n_id = 0, n_lane = 0, n_i, n_l, n_x, n_y;
    nameidx = 0;
    /* init qq */
    if (_compress_qualities && !_no_ac) {
      set_ac_stat(ac_freq3[F], ac_freq4[F]);
      ac_read(fQ + F, 0, 0);
    }

    for (int64_t K = 0;; K++) {

      /* names */
      if (K == read_next_read_info && !F) {
        uint64_t len = 0;
        if (f_read(fR + F, &core, sizeof(int32_t)) != sizeof(int32_t))
          break;
        f_read(fR + F, &len, sizeof(int64_t));
        read_next_read_info += len;

        corlen = (core == MAXBIN - 1) ? 0 : strlen(patterns[core]);
        n_id = n_lane = 0;
        //	if(corlen)LOG("i=%d, next=%d, core=%s[%d]\n", K,
        //read_next_read_info, patterns[core],core);
      } else if (F && K == read_next_read_info)
        break;

      if (_split_reads && reads_so_far == _split_reads) {
        LOG("Created part %d/%d as %s with %d reads\n", files_so_far, F + 1,
            pfo[F]->file_name, reads_so_far);

        reads_so_far = 0;
        files_so_far++;

        f_close(pfo[F]);
        snprintf((char *)buffer, MAXLINE, "%s.%d_%d.fastq", out, files_so_far,
                 F + 1);
        f_open(pfo[F], (char *)buffer, IO_WRITE);
      }

      /* names */
      if (names) {
        f_read(fN + F, &chr, 1);
        buffer[0] = '@';
        f_read(fN + F, buffer + 1, chr);

        if ((_interleave | _use_second_file) && chr > 0 &&
            buffer[chr - 1] == '/')
          buffer[chr] = F + 1 + '0';
        buffer[chr + 1] = '\n';
        f_write(pfo[F], buffer, chr + 2);
      } else {
        snprintf((char *)buffer, MAXLINE, "@%s.%lld\n", library, nameidx);
        f_write(pfo[F], buffer, strlen((char *)buffer));
      }
      //			if(_interleave && (!names ||  (chr > 0 &&
      //buffer[chr] != F + 1 - '0' && buffer[chr - 1] != '/'))) {
      //				snprintf((char*)buffer,3,"/%d",F+1);
      //				f_write(pfo[F],buffer, 2);
      //			}
      //			chr='\n';
      //			f_write(pfo[F],&chr,1);

      /* quals */

      uint32_t lenOfRd = len[F];
#ifdef PACBIO
      if (F)
        f_read(fL + F, &lenOfRd, sizeof(uint32_t));
      f_read(fL, &lenOfRd, sizeof(uint32_t));
      bc[F] = SZ_QUAL(lenOfRd);
//	LOG("%d %d %d\n", lenOfRd, bc[F], F);
#endif

      if (_compress_qualities) {
        if (!_no_ac)
          ac_read(fQ + F, buffer, bc[F]);
        else
          f_read(fQ + F, buffer, bc[F]);
      }

      /* reads */
      int64_t end = 0;
      int r = f_read(fR + F, o, SZ_READ(lenOfRd - corlen));
      int lc = 0;
      if (!F) {
        f_read(fR + F, &end, sz_meta);
        assert(end != 0 || corlen == 0);
        if (end) {
          for (int i = lenOfRd - end; i < lenOfRd - corlen; i++)
            l[lc++] = alphabet[o[i >> 2] >> ((~i & 3) << 1) & 3];
          for (int i = 0; i < corlen; i++)
            l[lc++] = patterns[core][i];
        }
      }
      for (int i = 0; i < lenOfRd - end; i++)
        l[lc++] = alphabet[o[i >> 2] >> ((~i & 3) << 1) & 3];

      if (_compress_qualities)
        for (int i = 0; i < lenOfRd; i++) {
#ifndef PACBIO
          if (!buffer[i])
            l[i] = 'N';
#endif
          buffer[i] += phredOff[F];
        }

      l[lenOfRd] = '\n';
      f_write(pfo[F], l, lenOfRd + 1);
      if (_compress_qualities) {
        char cx;
        cx = '+';
        f_write(pfo[F], &cx, 1);
        cx = '\n';
        f_write(pfo[F], &cx, 1);
        buffer[lenOfRd] = '\n';
        f_write(pfo[F], buffer, lenOfRd + 1);
      }
      nameidx++;
      reads_so_far++;
    }
    LOG("Created part %d/%d as %s with %lld reads\n", files_so_far, F + 1,
        pfo[F]->file_name, reads_so_far);
  }
  //	int lR = (_split_reads? _split_reads-limit : nl[0]-limit);

  f_free(fR);
  f_free(fQ);
  f_free(fN);
#ifdef PACBIO
  f_free(fL);
#endif
  f_free(fo);
  if (_interleave | _use_second_file) {
    f_free(fR + 1);
    f_free(fN + 1);
    f_free(fQ + 1);
#ifdef PACBIO
    f_free(fL + 1);
#endif
  }
  if (_use_second_file)
    f_free(fo + 1);
  if (_compress_qualities && !_no_ac)
    ac_finish();
  _time_elapsed = (TIME - _time_elapsed) / 1000000;
  LOG("\tTime elapsed:     %02d:%02d:%02d\n", _time_elapsed / 3600,
      (_time_elapsed / 60) % 60, _time_elapsed % 60);
}
