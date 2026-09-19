/*
 * Minimal stdio on top of playdate->file, plus console output.
 * See pd_compat.h. Only what WinQuake actually calls is implemented.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pd_port.h"

typedef struct {
	SDFile *f;
	int writable;
	int eof;
	unsigned char buf[512];
	int pos, len;	/* read buffer window */
	int unget;	/* pushed-back char or -1 */
} pdq_file_t;

#define PDF(fp) ((pdq_file_t *) (fp))

void pdq_log_line(const char *text)
{
	char out[512];
	size_t n = 0;

	/* Strip ANSI colour codes and CR/LF; the console adds its own newline */
	for (const char *p = text; *p && n < sizeof(out) - 1; p++) {
		if (*p == '\033' && p[1] == '[') {
			p += 2;
			while (*p && *p != 'm')
				p++;
			if (!*p)
				break;
			continue;
		}
		if (*p == '\r' || *p == '\n')
			continue;
		out[n++] = ((unsigned char) *p >= 128) ? '?' : *p;
	}
	out[n] = '\0';
	if (n)
		qembd_pd->system->logToConsole("%s", out);
}

int pdq_printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (qembd_pd)
		pdq_log_line(buf);
	return r;
}

int pdq_unlink(const char *path)
{
	char norm[256];

	return qembd_pd->file->unlink(pdq_path(path, norm, sizeof(norm)), 0);
}

FILE *pdq_fopen(const char *path, const char *mode)
{
	FileOptions opt;
	pdq_file_t *pf;
	SDFile *f;
	char norm[256];

	if (mode[0] == 'w') {
		pdq_mkdirs(path);
		opt = kFileWrite;
	} else if (mode[0] == 'a') {
		pdq_mkdirs(path);
		opt = kFileAppend;
	} else {
		opt = kFileRead | kFileReadData;
	}

	f = qembd_pd->file->open(pdq_path(path, norm, sizeof(norm)), opt);
	if (!f)
		return NULL;
	pf = calloc(1, sizeof(*pf));
	if (!pf) {
		qembd_pd->file->close(f);
		return NULL;
	}
	pf->f = f;
	pf->writable = (opt != (kFileRead | kFileReadData));
	pf->unget = -1;
	return (FILE *) pf;
}

int pdq_fclose(FILE *fp)
{
	pdq_file_t *pf = PDF(fp);
	int r = qembd_pd->file->close(pf->f);

	free(pf);
	return r;
}

/* Fill the read buffer; returns 0 at EOF */
static int refill(pdq_file_t *pf)
{
	int n = qembd_pd->file->read(pf->f, pf->buf, sizeof(pf->buf));

	pf->pos = 0;
	pf->len = n > 0 ? n : 0;
	if (pf->len == 0)
		pf->eof = 1;
	return pf->len > 0;
}

int pdq_fgetc(FILE *fp)
{
	pdq_file_t *pf = PDF(fp);

	if (pf->unget >= 0) {
		int c = pf->unget;
		pf->unget = -1;
		return c;
	}
	if (pf->pos >= pf->len && !refill(pf))
		return EOF;
	return pf->buf[pf->pos++];
}

static void pdq_ungetc(int c, pdq_file_t *pf)
{
	if (c != EOF)
		pf->unget = c;
}

int pdq_feof(FILE *fp)
{
	pdq_file_t *pf = PDF(fp);

	/* Like stdio: only true after a read hit the end */
	return pf->eof && pf->pos >= pf->len && pf->unget < 0;
}

size_t pdq_fread(void *buf, size_t size, size_t n, FILE *fp)
{
	unsigned char *dst = buf;
	size_t want = size * n, got = 0;

	while (got < want) {
		int c = pdq_fgetc(fp);

		if (c == EOF)
			break;
		*dst++ = (unsigned char) c;
		got++;
	}
	return size ? got / size : 0;
}

size_t pdq_fwrite(const void *buf, size_t size, size_t n, FILE *fp)
{
	pdq_file_t *pf = PDF(fp);
	int w = qembd_pd->file->write(pf->f, buf, (unsigned) (size * n));

	return (w > 0 && size) ? (size_t) w / size : 0;
}

int pdq_fseek(FILE *fp, long off, int whence)
{
	pdq_file_t *pf = PDF(fp);

	if (whence == SEEK_CUR)
		off -= (pf->len - pf->pos) + (pf->unget >= 0);
	pf->pos = pf->len = 0;
	pf->unget = -1;
	pf->eof = 0;
	return qembd_pd->file->seek(pf->f, (int) off, whence);
}

int pdq_fflush(FILE *fp)
{
	pdq_file_t *pf = PDF(fp);

	if (pf->writable)
		qembd_pd->file->flush(pf->f);
	return 0;
}

int pdq_fprintf(FILE *fp, const char *fmt, ...)
{
	char stackbuf[512], *buf = stackbuf;
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
	va_end(ap);
	if (n >= (int) sizeof(stackbuf)) {
		buf = malloc(n + 1);
		if (!buf)
			return -1;
		va_start(ap, fmt);
		vsnprintf(buf, n + 1, fmt, ap);
		va_end(ap);
	}

	if (fp == stdout || fp == stderr)
		pdq_log_line(buf);
	else
		pdq_fwrite(buf, 1, (size_t) n, fp);

	if (buf != stackbuf)
		free(buf);
	return n;
}

/*
 * fscanf supporting %i %d %f %s (with optional width) and whitespace, which
 * is all WinQuake's save/load code uses. Every conversion skips leading
 * whitespace and reads one whitespace-delimited token.
 */
int pdq_fscanf(FILE *fp, const char *fmt, ...)
{
	pdq_file_t *pf = PDF(fp);
	va_list ap;
	int assigned = 0;

	va_start(ap, fmt);
	while (*fmt) {
		if (isspace((unsigned char) *fmt)) {
			int c;
			do
				c = pdq_fgetc(fp);
			while (c != EOF && isspace(c));
			pdq_ungetc(c, pf);
			fmt++;
			continue;
		}
		if (*fmt != '%') {
			int c = pdq_fgetc(fp);
			if (c != (unsigned char) *fmt) {
				pdq_ungetc(c, pf);
				break;
			}
			fmt++;
			continue;
		}

		fmt++;
		int width = 0;
		while (isdigit((unsigned char) *fmt))
			width = width * 10 + (*fmt++ - '0');
		char conv = *fmt++;
		char tok[128];
		int max = (int) sizeof(tok) - 1, n = 0, c;

		if (width && width < max)
			max = width;
		do
			c = pdq_fgetc(fp);
		while (c != EOF && isspace(c));
		if (c == EOF)
			break;
		while (c != EOF && !isspace(c) && n < max) {
			tok[n++] = (char) c;
			c = pdq_fgetc(fp);
		}
		pdq_ungetc(c, pf);
		tok[n] = '\0';

		if (conv == 'i' || conv == 'd') {
			*va_arg(ap, int *) = (int) strtol(tok, NULL, conv == 'i' ? 0 : 10);
		} else if (conv == 'f') {
			*va_arg(ap, float *) = strtof(tok, NULL);
		} else if (conv == 's') {
			strcpy(va_arg(ap, char *), tok);
		} else {
			break;
		}
		assigned++;
	}
	va_end(ap);

	return (assigned == 0 && pf->eof) ? EOF : assigned;
}
