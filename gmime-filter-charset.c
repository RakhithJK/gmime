/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  Authors: Jeffrey Stedfast <fejj@ximian.com>
 *
 *  Copyright 2002 Ximian, Inc. (www.ximian.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 *
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>

#include "gmime-filter-charset.h"
#include "gmime-charset.h"
#include "gmime-iconv.h"


static void filter_destroy (GMimeFilter *filter);
static GMimeFilter *filter_copy (GMimeFilter *filter);
static void filter_filter (GMimeFilter *filter, char *in, size_t len, 
			   size_t prespace, char **out, 
			   size_t *outlen, size_t *outprespace);
static void filter_complete (GMimeFilter *filter, char *in, size_t len, 
			     size_t prespace, char **out, 
			     size_t *outlen, size_t *outprespace);
static void filter_reset (GMimeFilter *filter);

static GMimeFilter filter_template = {
	NULL, NULL, NULL, NULL,
	0, 0, NULL, 0, 0,
	filter_destroy,
	filter_copy,
	filter_filter,
	filter_complete,
	filter_reset,
};


/**
 * g_mime_filter_charset_new:
 * @from_charset:
 * @to_charset:
 *
 * Creates a new GMimeFilterCharset filter.
 *
 * Returns a new charset filter.
 **/
GMimeFilter *
g_mime_filter_charset_new (const char *from_charset, const char *to_charset)
{
	GMimeFilterCharset *new;
	iconv_t cd;
	
	cd = g_mime_iconv_open (to_charset, from_charset);
	if (cd == (iconv_t) -1)
		return NULL;
	
	new = g_new (GMimeFilterCharset, 1);
	new->from_charset = g_strdup (from_charset);
	new->to_charset = g_strdup (to_charset);
	new->cd = cd;
	
	g_mime_filter_construct (GMIME_FILTER (new), &filter_template);
	
	return GMIME_FILTER (new);
}


static void
filter_destroy (GMimeFilter *filter)
{
	GMimeFilterCharset *charset = (GMimeFilterCharset *) filter;
	
	g_free (charset->from_charset);
	g_free (charset->to_charset);
	if (charset->cd != (iconv_t) -1)
		g_mime_iconv_close (charset->cd);
	g_free (filter);
}

static GMimeFilter *
filter_copy (GMimeFilter *filter)
{
	GMimeFilterCharset *charset = (GMimeFilterCharset *) filter;
	
	return g_mime_filter_charset_new (charset->from_charset, charset->to_charset);
}

static void
filter_filter (GMimeFilter *filter, char *in, size_t len, size_t prespace,
	       char **out, size_t *outlen, size_t *outprespace)
{
	GMimeFilterCharset *charset = (GMimeFilterCharset *) filter;
	size_t inleft, outleft, converted = 0;
	char *inbuf;
	char *outbuf;
	
	if (charset->cd == (iconv_t) -1)
		goto noop;
	
	g_mime_filter_set_size (filter, len * 5 + 16, FALSE);
	outbuf = filter->outbuf;
	outleft = filter->outsize;
	
	inbuf = in;
	inleft = len;
	
	do {
		converted = iconv (charset->cd, &inbuf, &inleft, &outbuf, &outleft);
		if (converted == (size_t) -1) {
			if (errno != E2BIG && errno != EINVAL)
				break;
			
			if (errno == EILSEQ) {
				/*
				 * EILSEQ An invalid multibyte sequence has been  encountered
				 *        in the input.
				 *
				 * What we do here is eat the invalid bytes in the sequence and continue
				 */
				
				inbuf++;
				inleft--;
			} else {
				/* unknown error condition */
				goto noop;
			}
		}
	} while (((int) inleft) > 0);
	
	if (((int) inleft) > 0) {
		/* We've either got an E2BIG or EINVAL. Save the
                   remainder of the buffer as we'll process this next
                   time through */
		g_mime_filter_backup (filter, inbuf, inleft);
	}
	
	*out = filter->outbuf;
	*outlen = converted;
	*outprespace = filter->outpre;
	
	return;
	
 noop:
	
	*out = in;
	*outlen = len;
	*outprespace = prespace;
}

static void 
filter_complete (GMimeFilter *filter, char *in, size_t len, size_t prespace,
		 char **out, size_t *outlen, size_t *outprespace)
{
	GMimeFilterCharset *charset = (GMimeFilterCharset *) filter;
	size_t inleft, outleft, converted = 0;
	char *inbuf;
	char *outbuf;
	
	if (charset->cd == (iconv_t) -1)
		goto noop;
	
	g_mime_filter_set_size (filter, len * 5 + 16, FALSE);
	outbuf = filter->outbuf;
	outleft = filter->outsize;
	
	inbuf = in;
	inleft = len;
	
	if (inleft > 0) {
		do {
			converted = iconv (charset->cd, &inbuf, &inleft, &outbuf, &outleft);
			if (converted != (size_t) -1)
				continue;
			
			if (errno == E2BIG) {
				/*
				 * E2BIG   There is not sufficient room at *outbuf.
				 *
				 * We just need to grow our outbuffer and try again.
				 */
				
				converted = outbuf - filter->outbuf;
				g_mime_filter_set_size (filter, inleft * 5 + filter->outsize + 16, TRUE);
				outbuf = filter->outbuf + converted;
				outleft = filter->outsize - converted;
			} else if (errno == EILSEQ) {
				/*
				 * EILSEQ An invalid multibyte sequence has been  encountered
				 *        in the input.
				 *
				 * What we do here is eat the invalid bytes in the sequence and continue
				 */
				
				inbuf++;
				inleft--;
			} else if (errno == EINVAL) {
				/*
				 * EINVAL  An  incomplete  multibyte sequence has been encoun�
				 *         tered in the input.
				 *
				 * We assume that this can only happen if we've run out of
				 * bytes for a multibyte sequence, if not we're in trouble.
				 */
				
				break;
			} else
				goto noop;
			
		} while (((int) inleft) > 0);
	}
	
	/* flush the iconv conversion */
	iconv (charset->cd, NULL, NULL, &outbuf, &outleft);
	
	*out = filter->outbuf;
	*outlen = filter->outsize - outleft;
	*outprespace = filter->outpre;
	
	return;
	
 noop:
	
	*out = in;
	*outlen = len;
	*outprespace = prespace;
}

static void
filter_reset (GMimeFilter *filter)
{
	GMimeFilterCharset *charset = (GMimeFilterCharset *) filter;
	
	if (charset->cd != (iconv_t) -1)
		iconv (charset->cd, NULL, NULL, NULL, NULL);
}
