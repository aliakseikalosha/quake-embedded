/*
 * File I/O for the Playdate.
 *
 * Reads look in the game's Data folder first and then in the .pdx bundle
 * itself (kFileRead | kFileReadData), so pak files can either be shipped in
 * Source/ or copied to the Data folder later. Writes always go to Data.
 */

#include <quakembd.h>
#include <string.h>
#include "pd_port.h"

#define MAX_HANDLES 16

static SDFile *handles[MAX_HANDLES];

static int alloc_handle(SDFile *f)
{
	for (int i = 0; i < MAX_HANDLES; i++) {
		if (!handles[i]) {
			handles[i] = f;
			return i;
		}
	}
	return -1;
}

static SDFile *get(int handle)
{
	return (handle >= 0 && handle < MAX_HANDLES) ? handles[handle] : NULL;
}

const char *pdq_path(const char *path, char *out, size_t size)
{
	size_t n = 0;

	/* Drop leading "./" and "/", collapse repeated slashes */
	while (path[0] == '/' || (path[0] == '.' && path[1] == '/'))
		path += path[0] == '/' ? 1 : 2;
	for (; *path && n < size - 1; path++) {
		if (*path == '/' && n && out[n - 1] == '/')
			continue;
		out[n++] = *path;
	}
	out[n] = '\0';
	return out;
}

void pdq_mkdirs(const char *path)
{
	char tmp[256];

	pdq_path(path, tmp, sizeof(tmp));
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			qembd_pd->file->mkdir(tmp);
			*p = '/';
		}
	}
}

int Sys_FileOpenRead(char *path, int *handle)
{
	char norm[256];
	SDFile *f = qembd_pd->file->open(pdq_path(path, norm, sizeof(norm)),
	                                 kFileRead | kFileReadData);
	int h;

	*handle = -1;
	if (!f)
		return -1;
	h = alloc_handle(f);
	if (h < 0) {
		qembd_pd->file->close(f);
		qembd_error("Out of file handles opening %s", path);
		return -1;
	}
	*handle = h;

	/* Size: stat() only sees Data-folder files, so measure by seeking */
	qembd_pd->file->seek(f, 0, SEEK_END);
	int size = qembd_pd->file->tell(f);
	qembd_pd->file->seek(f, 0, SEEK_SET);
	return size;
}

int Sys_FileOpenWrite(char *path)
{
	char norm[256];
	SDFile *f;
	int h;

	pdq_mkdirs(path);
	f = qembd_pd->file->open(pdq_path(path, norm, sizeof(norm)), kFileWrite);
	if (!f) {
		qembd_error("Error opening %s: %s", path, qembd_pd->file->geterr());
		return -1;
	}
	h = alloc_handle(f);
	if (h < 0) {
		qembd_pd->file->close(f);
		return -1;
	}
	return h;
}

void Sys_FileClose(int handle)
{
	SDFile *f = get(handle);

	if (f) {
		qembd_pd->file->close(f);
		handles[handle] = NULL;
	}
}

void Sys_FileSeek(int handle, int position)
{
	SDFile *f = get(handle);

	if (f)
		qembd_pd->file->seek(f, position, SEEK_SET);
}

int Sys_FileRead(int handle, void *dest, int count)
{
	SDFile *f = get(handle);

	return f ? qembd_pd->file->read(f, dest, count) : -1;
}

int Sys_FileWrite(int handle, void *src, int count)
{
	SDFile *f = get(handle);

	return f ? qembd_pd->file->write(f, src, count) : -1;
}

int Sys_FileTime(char *path)
{
	char norm[256];
	FileStat st;

	/* Existing file -> any non-negative value; Quake only compares to -1 */
	if (qembd_pd->file->stat(pdq_path(path, norm, sizeof(norm)), &st) == 0)
		return 1;
	return -1;
}

void Sys_mkdir(char *path)
{
	pdq_mkdirs(path);
	qembd_pd->file->mkdir(pdq_path(path, (char[256]){0}, 256));
}

void Sys_FileSync(int handle)
{
	SDFile *f = get(handle);

	if (f)
		qembd_pd->file->flush(f);
}

void Sys_File_gets(int handle, char *buf, int len)
{
	SDFile *f = get(handle);
	int i = 0;
	char c;

	if (!f || len <= 0)
		return;
	while (i < len - 1 && qembd_pd->file->read(f, &c, 1) == 1) {
		buf[i++] = c;
		if (c == '\n')
			break;
	}
	buf[i] = '\0';
}
