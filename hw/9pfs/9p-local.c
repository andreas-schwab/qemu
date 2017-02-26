/*
 * 9p Posix callback
 *
 * Copyright IBM, Corp. 2010
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "9p.h"
#include "9p-local.h"
#include "9p-xattr.h"
#include "9p-util.h"
#include <arpa/inet.h>
#include <pwd.h>
#include <grp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "qemu/xattr.h"
#include <libgen.h>
#include <linux/fs.h>
#ifdef CONFIG_LINUX_MAGIC_H
#include <linux/magic.h>
#endif
#include <sys/ioctl.h>

#ifndef XFS_SUPER_MAGIC
#define XFS_SUPER_MAGIC  0x58465342
#endif
#ifndef EXT2_SUPER_MAGIC
#define EXT2_SUPER_MAGIC 0xEF53
#endif
#ifndef REISERFS_SUPER_MAGIC
#define REISERFS_SUPER_MAGIC 0x52654973
#endif
#ifndef BTRFS_SUPER_MAGIC
#define BTRFS_SUPER_MAGIC 0x9123683E
#endif

typedef struct {
    int mountfd;
} LocalData;

int local_open_nofollow(FsContext *fs_ctx, const char *path, int flags,
                        mode_t mode)
{
    LocalData *data = fs_ctx->private;

    /* All paths are relative to the path data->mountfd points to */
    while (*path == '/') {
        path++;
    }

    return relative_openat_nofollow(data->mountfd, path, flags, mode);
}

int local_opendir_nofollow(FsContext *fs_ctx, const char *path)
{
    return local_open_nofollow(fs_ctx, path, O_DIRECTORY | O_RDONLY, 0);
}

#define VIRTFS_META_DIR ".virtfs_metadata"

static const char *local_mapped_attr_path(FsContext *ctx,
                                          const char *path, char *buffer)
{
    char *dir_name;
    char *tmp_path = g_strdup(path);
    char *base_name = basename(tmp_path);

    /* NULL terminate the directory */
    dir_name = tmp_path;
    *(base_name - 1) = '\0';

    snprintf(buffer, PATH_MAX, "%s/%s/%s/%s",
             ctx->fs_root, dir_name, VIRTFS_META_DIR, base_name);
    g_free(tmp_path);
    return buffer;
}

#define ATTR_MAX 100
static void local_mapped_file_attr(FsContext *ctx, const char *path,
                                   struct stat *stbuf)
{
    FILE *fp;
    char buf[ATTR_MAX];
    char attr_path[PATH_MAX];

    local_mapped_attr_path(ctx, path, attr_path);
    fp = fopen(attr_path, "r");
    if (!fp) {
        return;
    }
    memset(buf, 0, ATTR_MAX);
    while (fgets(buf, ATTR_MAX, fp)) {
        if (!strncmp(buf, "virtfs.uid", 10)) {
            stbuf->st_uid = atoi(buf+11);
        } else if (!strncmp(buf, "virtfs.gid", 10)) {
            stbuf->st_gid = atoi(buf+11);
        } else if (!strncmp(buf, "virtfs.mode", 11)) {
            stbuf->st_mode = atoi(buf+12);
        } else if (!strncmp(buf, "virtfs.rdev", 11)) {
            stbuf->st_rdev = atoi(buf+12);
        }
        memset(buf, 0, ATTR_MAX);
    }
    fclose(fp);
}

static int local_lstat(FsContext *fs_ctx, V9fsPath *fs_path, struct stat *stbuf)
{
    int err;
    char buffer[PATH_MAX];
    char *path = fs_path->data;

    err =  lstat(rpath(fs_ctx, path, buffer), stbuf);
    if (err) {
        return err;
    }
    if (fs_ctx->export_flags & V9FS_SM_MAPPED) {
        /* Actual credentials are part of extended attrs */
        uid_t tmp_uid;
        gid_t tmp_gid;
        mode_t tmp_mode;
        dev_t tmp_dev;
        if (getxattr(rpath(fs_ctx, path, buffer), "user.virtfs.uid", &tmp_uid,
                    sizeof(uid_t)) > 0) {
            stbuf->st_uid = tmp_uid;
        }
        if (getxattr(rpath(fs_ctx, path, buffer), "user.virtfs.gid", &tmp_gid,
                    sizeof(gid_t)) > 0) {
            stbuf->st_gid = tmp_gid;
        }
        if (getxattr(rpath(fs_ctx, path, buffer), "user.virtfs.mode",
                    &tmp_mode, sizeof(mode_t)) > 0) {
            stbuf->st_mode = tmp_mode;
        }
        if (getxattr(rpath(fs_ctx, path, buffer), "user.virtfs.rdev", &tmp_dev,
                        sizeof(dev_t)) > 0) {
                stbuf->st_rdev = tmp_dev;
        }
    } else if (fs_ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        local_mapped_file_attr(fs_ctx, path, stbuf);
    }
    return err;
}

static int local_create_mapped_attr_dir(FsContext *ctx, const char *path)
{
    int err;
    char attr_dir[PATH_MAX];
    char *tmp_path = g_strdup(path);

    snprintf(attr_dir, PATH_MAX, "%s/%s/%s",
             ctx->fs_root, dirname(tmp_path), VIRTFS_META_DIR);

    err = mkdir(attr_dir, 0700);
    if (err < 0 && errno == EEXIST) {
        err = 0;
    }
    g_free(tmp_path);
    return err;
}

static int local_set_mapped_file_attr(FsContext *ctx,
                                      const char *path, FsCred *credp)
{
    FILE *fp;
    int ret = 0;
    char buf[ATTR_MAX];
    char attr_path[PATH_MAX];
    int uid = -1, gid = -1, mode = -1, rdev = -1;

    fp = fopen(local_mapped_attr_path(ctx, path, attr_path), "r");
    if (!fp) {
        goto create_map_file;
    }
    memset(buf, 0, ATTR_MAX);
    while (fgets(buf, ATTR_MAX, fp)) {
        if (!strncmp(buf, "virtfs.uid", 10)) {
            uid = atoi(buf+11);
        } else if (!strncmp(buf, "virtfs.gid", 10)) {
            gid = atoi(buf+11);
        } else if (!strncmp(buf, "virtfs.mode", 11)) {
            mode = atoi(buf+12);
        } else if (!strncmp(buf, "virtfs.rdev", 11)) {
            rdev = atoi(buf+12);
        }
        memset(buf, 0, ATTR_MAX);
    }
    fclose(fp);
    goto update_map_file;

create_map_file:
    ret = local_create_mapped_attr_dir(ctx, path);
    if (ret < 0) {
        goto err_out;
    }

update_map_file:
    fp = fopen(attr_path, "w");
    if (!fp) {
        ret = -1;
        goto err_out;
    }

    if (credp->fc_uid != -1) {
        uid = credp->fc_uid;
    }
    if (credp->fc_gid != -1) {
        gid = credp->fc_gid;
    }
    if (credp->fc_mode != -1) {
        mode = credp->fc_mode;
    }
    if (credp->fc_rdev != -1) {
        rdev = credp->fc_rdev;
    }


    if (uid != -1) {
        fprintf(fp, "virtfs.uid=%d\n", uid);
    }
    if (gid != -1) {
        fprintf(fp, "virtfs.gid=%d\n", gid);
    }
    if (mode != -1) {
        fprintf(fp, "virtfs.mode=%d\n", mode);
    }
    if (rdev != -1) {
        fprintf(fp, "virtfs.rdev=%d\n", rdev);
    }
    fclose(fp);

err_out:
    return ret;
}

static int local_set_xattr(const char *path, FsCred *credp)
{
    int err;

    if (credp->fc_uid != -1) {
        err = setxattr(path, "user.virtfs.uid", &credp->fc_uid, sizeof(uid_t),
                0);
        if (err) {
            return err;
        }
    }
    if (credp->fc_gid != -1) {
        err = setxattr(path, "user.virtfs.gid", &credp->fc_gid, sizeof(gid_t),
                0);
        if (err) {
            return err;
        }
    }
    if (credp->fc_mode != -1) {
        err = setxattr(path, "user.virtfs.mode", &credp->fc_mode,
                sizeof(mode_t), 0);
        if (err) {
            return err;
        }
    }
    if (credp->fc_rdev != -1) {
        err = setxattr(path, "user.virtfs.rdev", &credp->fc_rdev,
                sizeof(dev_t), 0);
        if (err) {
            return err;
        }
    }
    return 0;
}

static int local_post_create_passthrough(FsContext *fs_ctx, const char *path,
                                         FsCred *credp)
{
    char buffer[PATH_MAX];

    if (lchown(rpath(fs_ctx, path, buffer), credp->fc_uid,
                credp->fc_gid) < 0) {
        /*
         * If we fail to change ownership and if we are
         * using security model none. Ignore the error
         */
        if ((fs_ctx->export_flags & V9FS_SEC_MASK) != V9FS_SM_NONE) {
            return -1;
        }
    }

    if (chmod(rpath(fs_ctx, path, buffer), credp->fc_mode & 07777) < 0) {
        return -1;
    }
    return 0;
}

static ssize_t local_readlink(FsContext *fs_ctx, V9fsPath *fs_path,
                              char *buf, size_t bufsz)
{
    ssize_t tsize = -1;
    char buffer[PATH_MAX];
    char *path = fs_path->data;

    if ((fs_ctx->export_flags & V9FS_SM_MAPPED) ||
        (fs_ctx->export_flags & V9FS_SM_MAPPED_FILE)) {
        int fd;
        fd = open(rpath(fs_ctx, path, buffer), O_RDONLY | O_NOFOLLOW);
        if (fd == -1) {
            return -1;
        }
        do {
            tsize = read(fd, (void *)buf, bufsz);
        } while (tsize == -1 && errno == EINTR);
        close(fd);
        return tsize;
    } else if ((fs_ctx->export_flags & V9FS_SM_PASSTHROUGH) ||
               (fs_ctx->export_flags & V9FS_SM_NONE)) {
        tsize = readlink(rpath(fs_ctx, path, buffer), buf, bufsz);
    }
    return tsize;
}

static int local_close(FsContext *ctx, V9fsFidOpenState *fs)
{
    return close(fs->fd);
}

static int local_closedir(FsContext *ctx, V9fsFidOpenState *fs)
{
    return closedir(fs->dir.stream);
}

static int local_open(FsContext *ctx, V9fsPath *fs_path,
                      int flags, V9fsFidOpenState *fs)
{
    int fd;

    fd = local_open_nofollow(ctx, fs_path->data, flags, 0);
    if (fd == -1) {
        return -1;
    }
    fs->fd = fd;
    return fs->fd;
}

static int local_opendir(FsContext *ctx,
                         V9fsPath *fs_path, V9fsFidOpenState *fs)
{
    int dirfd;
    DIR *stream;

    dirfd = local_opendir_nofollow(ctx, fs_path->data);
    if (dirfd == -1) {
        return -1;
    }

    stream = fdopendir(dirfd);
    if (!stream) {
        return -1;
    }
    fs->dir.stream = stream;
    return 0;
}

static void local_rewinddir(FsContext *ctx, V9fsFidOpenState *fs)
{
    rewinddir(fs->dir.stream);
}

static off_t local_telldir(FsContext *ctx, V9fsFidOpenState *fs)
{
    return telldir(fs->dir.stream);
}

static int local_readdir_r(FsContext *ctx, V9fsFidOpenState *fs,
                           struct dirent *entry,
                           struct dirent **result)
{
    int ret;

again:
    ret = readdir_r(fs->dir.stream, entry, result);
    if (ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        if (!ret && *result != NULL &&
            !strcmp(entry->d_name, VIRTFS_META_DIR)) {
            /* skp the meta data directory */
            goto again;
        }
    }
    return ret;
}

static void local_seekdir(FsContext *ctx, V9fsFidOpenState *fs, off_t off)
{
    seekdir(fs->dir.stream, off);
}

static ssize_t local_preadv(FsContext *ctx, V9fsFidOpenState *fs,
                            const struct iovec *iov,
                            int iovcnt, off_t offset)
{
#ifdef CONFIG_PREADV
    return preadv(fs->fd, iov, iovcnt, offset);
#else
    int err = lseek(fs->fd, offset, SEEK_SET);
    if (err == -1) {
        return err;
    } else {
        return readv(fs->fd, iov, iovcnt);
    }
#endif
}

static ssize_t local_pwritev(FsContext *ctx, V9fsFidOpenState *fs,
                             const struct iovec *iov,
                             int iovcnt, off_t offset)
{
    ssize_t ret
;
#ifdef CONFIG_PREADV
    ret = pwritev(fs->fd, iov, iovcnt, offset);
#else
    int err = lseek(fs->fd, offset, SEEK_SET);
    if (err == -1) {
        return err;
    } else {
        ret = writev(fs->fd, iov, iovcnt);
    }
#endif
#ifdef CONFIG_SYNC_FILE_RANGE
    if (ret > 0 && ctx->export_flags & V9FS_IMMEDIATE_WRITEOUT) {
        /*
         * Initiate a writeback. This is not a data integrity sync.
         * We want to ensure that we don't leave dirty pages in the cache
         * after write when writeout=immediate is sepcified.
         */
        sync_file_range(fs->fd, offset, ret,
                        SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE);
    }
#endif
    return ret;
}

static int local_chmod(FsContext *fs_ctx, V9fsPath *fs_path, FsCred *credp)
{
    char buffer[PATH_MAX];
    char *path = fs_path->data;

    if (fs_ctx->export_flags & V9FS_SM_MAPPED) {
        return local_set_xattr(rpath(fs_ctx, path, buffer), credp);
    } else if (fs_ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        return local_set_mapped_file_attr(fs_ctx, path, credp);
    } else if ((fs_ctx->export_flags & V9FS_SM_PASSTHROUGH) ||
               (fs_ctx->export_flags & V9FS_SM_NONE)) {
        return chmod(rpath(fs_ctx, path, buffer), credp->fc_mode);
    }
    return -1;
}

static int local_mknod(FsContext *fs_ctx, V9fsPath *dir_path,
                       const char *name, FsCred *credp)
{
    char *path;
    int err = -1;
    int serrno = 0;
    V9fsString fullname;
    char buffer[PATH_MAX];

    v9fs_string_init(&fullname);
    v9fs_string_sprintf(&fullname, "%s/%s", dir_path->data, name);
    path = fullname.data;

    /* Determine the security model */
    if (fs_ctx->export_flags & V9FS_SM_MAPPED) {
        err = mknod(rpath(fs_ctx, path, buffer),
                SM_LOCAL_MODE_BITS|S_IFREG, 0);
        if (err == -1) {
            goto out;
        }
        err = local_set_xattr(rpath(fs_ctx, path, buffer), credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    } else if (fs_ctx->export_flags & V9FS_SM_MAPPED_FILE) {

        err = mknod(rpath(fs_ctx, path, buffer),
                    SM_LOCAL_MODE_BITS|S_IFREG, 0);
        if (err == -1) {
            goto out;
        }
        err = local_set_mapped_file_attr(fs_ctx, path, credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    } else if ((fs_ctx->export_flags & V9FS_SM_PASSTHROUGH) ||
               (fs_ctx->export_flags & V9FS_SM_NONE)) {
        err = mknod(rpath(fs_ctx, path, buffer), credp->fc_mode,
                credp->fc_rdev);
        if (err == -1) {
            goto out;
        }
        err = local_post_create_passthrough(fs_ctx, path, credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    }
    goto out;

err_end:
    remove(rpath(fs_ctx, path, buffer));
    errno = serrno;
out:
    v9fs_string_free(&fullname);
    return err;
}

static int local_mkdir(FsContext *fs_ctx, V9fsPath *dir_path,
                       const char *name, FsCred *credp)
{
    char *path;
    int err = -1;
    int serrno = 0;
    V9fsString fullname;
    char buffer[PATH_MAX];

    v9fs_string_init(&fullname);
    v9fs_string_sprintf(&fullname, "%s/%s", dir_path->data, name);
    path = fullname.data;

    /* Determine the security model */
    if (fs_ctx->export_flags & V9FS_SM_MAPPED) {
        err = mkdir(rpath(fs_ctx, path, buffer), SM_LOCAL_DIR_MODE_BITS);
        if (err == -1) {
            goto out;
        }
        credp->fc_mode = credp->fc_mode|S_IFDIR;
        err = local_set_xattr(rpath(fs_ctx, path, buffer), credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    } else if (fs_ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        err = mkdir(rpath(fs_ctx, path, buffer), SM_LOCAL_DIR_MODE_BITS);
        if (err == -1) {
            goto out;
        }
        credp->fc_mode = credp->fc_mode|S_IFDIR;
        err = local_set_mapped_file_attr(fs_ctx, path, credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    } else if ((fs_ctx->export_flags & V9FS_SM_PASSTHROUGH) ||
               (fs_ctx->export_flags & V9FS_SM_NONE)) {
        err = mkdir(rpath(fs_ctx, path, buffer), credp->fc_mode);
        if (err == -1) {
            goto out;
        }
        err = local_post_create_passthrough(fs_ctx, path, credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    }
    goto out;

err_end:
    remove(rpath(fs_ctx, path, buffer));
    errno = serrno;
out:
    v9fs_string_free(&fullname);
    return err;
}

static int local_fstat(FsContext *fs_ctx, int fid_type,
                       V9fsFidOpenState *fs, struct stat *stbuf)
{
    int err, fd;

    if (fid_type == P9_FID_DIR) {
        fd = dirfd(fs->dir.stream);
    } else {
        fd = fs->fd;
    }

    err = fstat(fd, stbuf);
    if (err) {
        return err;
    }
    if (fs_ctx->export_flags & V9FS_SM_MAPPED) {
        /* Actual credentials are part of extended attrs */
        uid_t tmp_uid;
        gid_t tmp_gid;
        mode_t tmp_mode;
        dev_t tmp_dev;

        if (fgetxattr(fd, "user.virtfs.uid",
                      &tmp_uid, sizeof(uid_t)) > 0) {
            stbuf->st_uid = tmp_uid;
        }
        if (fgetxattr(fd, "user.virtfs.gid",
                      &tmp_gid, sizeof(gid_t)) > 0) {
            stbuf->st_gid = tmp_gid;
        }
        if (fgetxattr(fd, "user.virtfs.mode",
                      &tmp_mode, sizeof(mode_t)) > 0) {
            stbuf->st_mode = tmp_mode;
        }
        if (fgetxattr(fd, "user.virtfs.rdev",
                      &tmp_dev, sizeof(dev_t)) > 0) {
                stbuf->st_rdev = tmp_dev;
        }
    } else if (fs_ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return err;
}

static int local_open2(FsContext *fs_ctx, V9fsPath *dir_path, const char *name,
                       int flags, FsCred *credp, V9fsFidOpenState *fs)
{
    char *path;
    int fd = -1;
    int err = -1;
    int serrno = 0;
    V9fsString fullname;
    char buffer[PATH_MAX];

    v9fs_string_init(&fullname);
    v9fs_string_sprintf(&fullname, "%s/%s", dir_path->data, name);
    path = fullname.data;

    /* Determine the security model */
    if (fs_ctx->export_flags & V9FS_SM_MAPPED) {
        fd = open(rpath(fs_ctx, path, buffer), flags, SM_LOCAL_MODE_BITS);
        if (fd == -1) {
            err = fd;
            goto out;
        }
        credp->fc_mode = credp->fc_mode|S_IFREG;
        /* Set cleint credentials in xattr */
        err = local_set_xattr(rpath(fs_ctx, path, buffer), credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    } else if (fs_ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        fd = open(rpath(fs_ctx, path, buffer), flags, SM_LOCAL_MODE_BITS);
        if (fd == -1) {
            err = fd;
            goto out;
        }
        credp->fc_mode = credp->fc_mode|S_IFREG;
        /* Set client credentials in .virtfs_metadata directory files */
        err = local_set_mapped_file_attr(fs_ctx, path, credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    } else if ((fs_ctx->export_flags & V9FS_SM_PASSTHROUGH) ||
               (fs_ctx->export_flags & V9FS_SM_NONE)) {
        fd = open(rpath(fs_ctx, path, buffer), flags, credp->fc_mode);
        if (fd == -1) {
            err = fd;
            goto out;
        }
        err = local_post_create_passthrough(fs_ctx, path, credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    }
    err = fd;
    fs->fd = fd;
    goto out;

err_end:
    close(fd);
    remove(rpath(fs_ctx, path, buffer));
    errno = serrno;
out:
    v9fs_string_free(&fullname);
    return err;
}


static int local_symlink(FsContext *fs_ctx, const char *oldpath,
                         V9fsPath *dir_path, const char *name, FsCred *credp)
{
    int err = -1;
    int serrno = 0;
    char *newpath;
    V9fsString fullname;
    char buffer[PATH_MAX];

    v9fs_string_init(&fullname);
    v9fs_string_sprintf(&fullname, "%s/%s", dir_path->data, name);
    newpath = fullname.data;

    /* Determine the security model */
    if (fs_ctx->export_flags & V9FS_SM_MAPPED) {
        int fd;
        ssize_t oldpath_size, write_size;
        fd = open(rpath(fs_ctx, newpath, buffer), O_CREAT|O_EXCL|O_RDWR,
                SM_LOCAL_MODE_BITS);
        if (fd == -1) {
            err = fd;
            goto out;
        }
        /* Write the oldpath (target) to the file. */
        oldpath_size = strlen(oldpath);
        do {
            write_size = write(fd, (void *)oldpath, oldpath_size);
        } while (write_size == -1 && errno == EINTR);

        if (write_size != oldpath_size) {
            serrno = errno;
            close(fd);
            err = -1;
            goto err_end;
        }
        close(fd);
        /* Set cleint credentials in symlink's xattr */
        credp->fc_mode = credp->fc_mode|S_IFLNK;
        err = local_set_xattr(rpath(fs_ctx, newpath, buffer), credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    } else if (fs_ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        int fd;
        ssize_t oldpath_size, write_size;
        fd = open(rpath(fs_ctx, newpath, buffer), O_CREAT|O_EXCL|O_RDWR,
                  SM_LOCAL_MODE_BITS);
        if (fd == -1) {
            err = fd;
            goto out;
        }
        /* Write the oldpath (target) to the file. */
        oldpath_size = strlen(oldpath);
        do {
            write_size = write(fd, (void *)oldpath, oldpath_size);
        } while (write_size == -1 && errno == EINTR);

        if (write_size != oldpath_size) {
            serrno = errno;
            close(fd);
            err = -1;
            goto err_end;
        }
        close(fd);
        /* Set cleint credentials in symlink's xattr */
        credp->fc_mode = credp->fc_mode|S_IFLNK;
        err = local_set_mapped_file_attr(fs_ctx, newpath, credp);
        if (err == -1) {
            serrno = errno;
            goto err_end;
        }
    } else if ((fs_ctx->export_flags & V9FS_SM_PASSTHROUGH) ||
               (fs_ctx->export_flags & V9FS_SM_NONE)) {
        err = symlink(oldpath, rpath(fs_ctx, newpath, buffer));
        if (err) {
            goto out;
        }
        err = lchown(rpath(fs_ctx, newpath, buffer), credp->fc_uid,
                     credp->fc_gid);
        if (err == -1) {
            /*
             * If we fail to change ownership and if we are
             * using security model none. Ignore the error
             */
            if ((fs_ctx->export_flags & V9FS_SEC_MASK) != V9FS_SM_NONE) {
                serrno = errno;
                goto err_end;
            } else
                err = 0;
        }
    }
    goto out;

err_end:
    remove(rpath(fs_ctx, newpath, buffer));
    errno = serrno;
out:
    v9fs_string_free(&fullname);
    return err;
}

static int local_link(FsContext *ctx, V9fsPath *oldpath,
                      V9fsPath *dirpath, const char *name)
{
    int ret;
    V9fsString newpath;
    char buffer[PATH_MAX], buffer1[PATH_MAX];

    v9fs_string_init(&newpath);
    v9fs_string_sprintf(&newpath, "%s/%s", dirpath->data, name);

    ret = link(rpath(ctx, oldpath->data, buffer),
               rpath(ctx, newpath.data, buffer1));

    /* now link the virtfs_metadata files */
    if (!ret && (ctx->export_flags & V9FS_SM_MAPPED_FILE)) {
        /* Link the .virtfs_metadata files. Create the metada directory */
        ret = local_create_mapped_attr_dir(ctx, newpath.data);
        if (ret < 0) {
            goto err_out;
        }
        ret = link(local_mapped_attr_path(ctx, oldpath->data, buffer),
                   local_mapped_attr_path(ctx, newpath.data, buffer1));
        if (ret < 0 && errno != ENOENT) {
            goto err_out;
        }
    }
err_out:
    v9fs_string_free(&newpath);
    return ret;
}

static int local_truncate(FsContext *ctx, V9fsPath *fs_path, off_t size)
{
    char buffer[PATH_MAX];
    char *path = fs_path->data;

    return truncate(rpath(ctx, path, buffer), size);
}

static int local_rename(FsContext *ctx, const char *oldpath,
                        const char *newpath)
{
    int err;
    char buffer[PATH_MAX], buffer1[PATH_MAX];

    if (ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        err = local_create_mapped_attr_dir(ctx, newpath);
        if (err < 0) {
            return err;
        }
        /* rename the .virtfs_metadata files */
        err = rename(local_mapped_attr_path(ctx, oldpath, buffer),
                     local_mapped_attr_path(ctx, newpath, buffer1));
        if (err < 0 && errno != ENOENT) {
            return err;
        }
    }
    return rename(rpath(ctx, oldpath, buffer), rpath(ctx, newpath, buffer1));
}

static int local_chown(FsContext *fs_ctx, V9fsPath *fs_path, FsCred *credp)
{
    char buffer[PATH_MAX];
    char *path = fs_path->data;

    if ((credp->fc_uid == -1 && credp->fc_gid == -1) ||
        (fs_ctx->export_flags & V9FS_SM_PASSTHROUGH) ||
        (fs_ctx->export_flags & V9FS_SM_NONE)) {
        return lchown(rpath(fs_ctx, path, buffer),
                      credp->fc_uid, credp->fc_gid);
    } else if (fs_ctx->export_flags & V9FS_SM_MAPPED) {
        return local_set_xattr(rpath(fs_ctx, path, buffer), credp);
    } else if (fs_ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        return local_set_mapped_file_attr(fs_ctx, path, credp);
    }
    return -1;
}

static int local_utimensat(FsContext *s, V9fsPath *fs_path,
                           const struct timespec *buf)
{
    char buffer[PATH_MAX];
    char *path = fs_path->data;

    return qemu_utimens(rpath(s, path, buffer), buf);
}

static int local_unlinkat_common(FsContext *ctx, int dirfd, const char *name,
                                 int flags)
{
    int ret = -1;

    if (ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        int map_dirfd;

        if (flags == AT_REMOVEDIR) {
            int fd;

            fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_PATH);
            if (fd == -1) {
                goto err_out;
            }
            /*
             * If directory remove .virtfs_metadata contained in the
             * directory
             */
            ret = unlinkat(fd, VIRTFS_META_DIR, AT_REMOVEDIR);
            close_preserve_errno(fd);
            if (ret < 0 && errno != ENOENT) {
                /*
                 * We didn't had the .virtfs_metadata file. May be file created
                 * in non-mapped mode ?. Ignore ENOENT.
                 */
                goto err_out;
            }
        }
        /*
         * Now remove the name from parent directory
         * .virtfs_metadata directory.
         */
        map_dirfd = openat_dir(dirfd, VIRTFS_META_DIR);
        ret = unlinkat(map_dirfd, name, 0);
        close_preserve_errno(map_dirfd);
        if (ret < 0 && errno != ENOENT) {
            /*
             * We didn't had the .virtfs_metadata file. May be file created
             * in non-mapped mode ?. Ignore ENOENT.
             */
            goto err_out;
        }
    }

    ret = unlinkat(dirfd, name, flags);
err_out:
    return ret;
}

static int local_remove(FsContext *ctx, const char *path)
{
    struct stat stbuf;
    char *dirpath = g_path_get_dirname(path);
    char *name = g_path_get_basename(path);
    int flags = 0;
    int dirfd;
    int err = -1;

    dirfd = local_opendir_nofollow(ctx, dirpath);
    if (dirfd) {
        goto out;
    }

    if (fstatat(dirfd, path, &stbuf, AT_SYMLINK_NOFOLLOW) < 0) {
        goto err_out;
    }

    if (S_ISDIR(stbuf.st_mode)) {
        flags |= AT_REMOVEDIR;
    }

    err = local_unlinkat_common(ctx, dirfd, name, flags);
err_out:
    close_preserve_errno(dirfd);
out:
    g_free(name);
    g_free(dirpath);
    return err;
}

static int local_fsync(FsContext *ctx, int fid_type,
                       V9fsFidOpenState *fs, int datasync)
{
    int fd;

    if (fid_type == P9_FID_DIR) {
        fd = dirfd(fs->dir.stream);
    } else {
        fd = fs->fd;
    }

    if (datasync) {
        return qemu_fdatasync(fd);
    } else {
        return fsync(fd);
    }
}

static int local_statfs(FsContext *s, V9fsPath *fs_path, struct statfs *stbuf)
{
    char buffer[PATH_MAX];
    char *path = fs_path->data;

    return statfs(rpath(s, path, buffer), stbuf);
}

static ssize_t local_lgetxattr(FsContext *ctx, V9fsPath *fs_path,
                               const char *name, void *value, size_t size)
{
    char *path = fs_path->data;

    return v9fs_get_xattr(ctx, path, name, value, size);
}

static ssize_t local_llistxattr(FsContext *ctx, V9fsPath *fs_path,
                                void *value, size_t size)
{
    char *path = fs_path->data;

    return v9fs_list_xattr(ctx, path, value, size);
}

static int local_lsetxattr(FsContext *ctx, V9fsPath *fs_path, const char *name,
                           void *value, size_t size, int flags)
{
    char *path = fs_path->data;

    return v9fs_set_xattr(ctx, path, name, value, size, flags);
}

static int local_lremovexattr(FsContext *ctx, V9fsPath *fs_path,
                              const char *name)
{
    char *path = fs_path->data;

    return v9fs_remove_xattr(ctx, path, name);
}

static int local_name_to_path(FsContext *ctx, V9fsPath *dir_path,
                              const char *name, V9fsPath *target)
{
    if (dir_path) {
        v9fs_path_sprintf(target, "%s/%s", dir_path->data, name);
    } else {
        v9fs_path_sprintf(target, "%s", name);
    }
    return 0;
}

static int local_renameat(FsContext *ctx, V9fsPath *olddir,
                          const char *old_name, V9fsPath *newdir,
                          const char *new_name)
{
    int ret;
    V9fsString old_full_name, new_full_name;

    v9fs_string_init(&old_full_name);
    v9fs_string_init(&new_full_name);

    v9fs_string_sprintf(&old_full_name, "%s/%s", olddir->data, old_name);
    v9fs_string_sprintf(&new_full_name, "%s/%s", newdir->data, new_name);

    ret = local_rename(ctx, old_full_name.data, new_full_name.data);
    v9fs_string_free(&old_full_name);
    v9fs_string_free(&new_full_name);
    return ret;
}

static int local_unlinkat(FsContext *ctx, V9fsPath *dir,
                          const char *name, int flags)
{
    int ret;
    int dirfd;

    dirfd = local_opendir_nofollow(ctx, dir->data);
    if (dirfd == -1) {
        return -1;
    }

    ret = local_unlinkat_common(ctx, dirfd, name, flags);
    close_preserve_errno(dirfd);
    return ret;
}

static int local_ioc_getversion(FsContext *ctx, V9fsPath *path,
                                mode_t st_mode, uint64_t *st_gen)
{
    int err;
#ifdef FS_IOC_GETVERSION
    V9fsFidOpenState fid_open;

    /*
     * Do not try to open special files like device nodes, fifos etc
     * We can get fd for regular files and directories only
     */
    if (!S_ISREG(st_mode) && !S_ISDIR(st_mode)) {
            return 0;
    }
    err = local_open(ctx, path, O_RDONLY, &fid_open);
    if (err < 0) {
        return err;
    }
    err = ioctl(fid_open.fd, FS_IOC_GETVERSION, st_gen);
    local_close(ctx, &fid_open);
#else
    err = -ENOTTY;
#endif
    return err;
}

static int local_init(FsContext *ctx)
{
    struct statfs stbuf;
    LocalData *data = g_malloc(sizeof(*data));

    data->mountfd = open(ctx->fs_root, O_DIRECTORY | O_RDONLY);
    if (data->mountfd == -1) {
        goto err;
    }

#ifdef FS_IOC_GETVERSION
    /*
     * use ioc_getversion only if the ioctl is definied
     */
    if (fstatfs(data->mountfd, &stbuf) < 0) {
        close_preserve_errno(data->mountfd);
        goto err;
    }
    switch (stbuf.f_type) {
    case EXT2_SUPER_MAGIC:
    case BTRFS_SUPER_MAGIC:
    case REISERFS_SUPER_MAGIC:
    case XFS_SUPER_MAGIC:
        ctx->exops.get_st_gen = local_ioc_getversion;
        break;
    }
#endif

    if (ctx->export_flags & V9FS_SM_PASSTHROUGH) {
        ctx->xops = passthrough_xattr_ops;
    } else if (ctx->export_flags & V9FS_SM_MAPPED) {
        ctx->xops = mapped_xattr_ops;
    } else if (ctx->export_flags & V9FS_SM_NONE) {
        ctx->xops = none_xattr_ops;
    } else if (ctx->export_flags & V9FS_SM_MAPPED_FILE) {
        /*
         * xattr operation for mapped-file and passthrough
         * remain same.
         */
        ctx->xops = passthrough_xattr_ops;
    }
    ctx->export_flags |= V9FS_PATHNAME_FSCONTEXT;

    ctx->private = data;
    return 0;

err:
    g_free(data);
    return -1;
}

static void local_cleanup(FsContext *ctx)
{
    LocalData *data = ctx->private;

    close(data->mountfd);
    g_free(data);
}

static int local_parse_opts(QemuOpts *opts, struct FsDriverEntry *fse)
{
    const char *sec_model = qemu_opt_get(opts, "security_model");
    const char *path = qemu_opt_get(opts, "path");

    if (!sec_model) {
        fprintf(stderr, "security model not specified, "
                "local fs needs security model\nvalid options are:"
                "\tsecurity_model=[passthrough|mapped|none]\n");
        return -1;
    }

    if (!strcmp(sec_model, "passthrough")) {
        fse->export_flags |= V9FS_SM_PASSTHROUGH;
    } else if (!strcmp(sec_model, "mapped") ||
               !strcmp(sec_model, "mapped-xattr")) {
        fse->export_flags |= V9FS_SM_MAPPED;
    } else if (!strcmp(sec_model, "none")) {
        fse->export_flags |= V9FS_SM_NONE;
    } else if (!strcmp(sec_model, "mapped-file")) {
        fse->export_flags |= V9FS_SM_MAPPED_FILE;
    } else {
        fprintf(stderr, "Invalid security model %s specified, valid options are"
                "\n\t [passthrough|mapped-xattr|mapped-file|none]\n",
                sec_model);
        return -1;
    }

    if (!path) {
        fprintf(stderr, "fsdev: No path specified.\n");
        return -1;
    }
    fse->path = g_strdup(path);

    return 0;
}

FileOperations local_ops = {
    .parse_opts = local_parse_opts,
    .init  = local_init,
    .cleanup = local_cleanup,
    .lstat = local_lstat,
    .readlink = local_readlink,
    .close = local_close,
    .closedir = local_closedir,
    .open = local_open,
    .opendir = local_opendir,
    .rewinddir = local_rewinddir,
    .telldir = local_telldir,
    .readdir_r = local_readdir_r,
    .seekdir = local_seekdir,
    .preadv = local_preadv,
    .pwritev = local_pwritev,
    .chmod = local_chmod,
    .mknod = local_mknod,
    .mkdir = local_mkdir,
    .fstat = local_fstat,
    .open2 = local_open2,
    .symlink = local_symlink,
    .link = local_link,
    .truncate = local_truncate,
    .rename = local_rename,
    .chown = local_chown,
    .utimensat = local_utimensat,
    .remove = local_remove,
    .fsync = local_fsync,
    .statfs = local_statfs,
    .lgetxattr = local_lgetxattr,
    .llistxattr = local_llistxattr,
    .lsetxattr = local_lsetxattr,
    .lremovexattr = local_lremovexattr,
    .name_to_path = local_name_to_path,
    .renameat  = local_renameat,
    .unlinkat = local_unlinkat,
};
