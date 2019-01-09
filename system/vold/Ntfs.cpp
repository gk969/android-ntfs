/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include <linux/kdev_t.h>

#define LOG_TAG "Vold"

#include <cutils/log.h>
#include <cutils/properties.h>
#include <logwrap/logwrap.h>

#include "Ntfs.h"

#define UNUSED __attribute__((unused))

#ifdef HAS_NTFS_3G
static char NTFS_3G_PATH[] = "/system/bin/ntfs-3g";
static char NTFSFIX_3G_PATH[] = "/system/bin/ntfsfix";
static char MKNTFS_3G_PATH[] = "/system/bin/mkntfs";
#endif /* HAS_NTFS_3G */

extern "C" int mount(const char *, const char *, const char *, unsigned long, const void *);

int Ntfs::check(const char *fsPath UNUSED) {
#ifndef HAS_NTFS_3G
    SLOGW("Skipping NTFS check\n");
    return 0;
#else
    bool rw = true;
    
    SLOGI("check access to %s", NTFSFIX_3G_PATH);
    if (access(NTFSFIX_3G_PATH, X_OK)) {
        SLOGW("Skipping fs checks\n");
        return 0;
    }
    SLOGI("access to %s X_OK", NTFSFIX_3G_PATH);
    

    int pass = 1;
    int rc = 0;
    int status;
    do {
        const char *args[4];
        args[0] = NTFSFIX_3G_PATH;
        args[1] = "-n";
        args[2] = fsPath;
        args[3] = NULL;

        rc = android_fork_execvp(3, (char **)args, &status, false, true);

        if (rc != 0) {
            SLOGE("NTFS check failed due to logwrap error");
            errno = EIO;
            return -1;
        }

        if (!WIFEXITED(status)) {
            SLOGE("NTFS check did not exit properly");
            errno = EIO;
            return -1;
        }

        status = WEXITSTATUS(status);

        switch (status) {
            case 0:
                SLOGI("NTFS check completed OK");
                return 0;
            case 1:
                SLOGE("NTFS check failed (not a NTFS filesystem)");
                errno = ENODATA;
                return -1;
            default:
                SLOGE("NTFS check failed (unknown exit code %d)", rc);
                errno = EIO;
                return -1;
        }
    } while (0);

    return 0;
#endif
}

int Ntfs::doMount(const char *fsPath, const char *mountPoint,
                 bool ro UNUSED, bool remount UNUSED, int ownerUid, int ownerGid,
                 int permMask, bool createLost UNUSED) {
#ifndef HAS_NTFS_3G
    int rc;
    unsigned long flags;
    char mountData[255];

    flags = MS_NODEV | MS_NOEXEC | MS_NOSUID | MS_DIRSYNC;

    flags |= (ro ? MS_RDONLY : 0);
    flags |= (remount ? MS_REMOUNT : 0);

    permMask = 0;

    sprintf(mountData,
            "nls=utf8,uid=%d,gid=%d,fmask=%o,dmask=%o",
            ownerUid, ownerGid, permMask, permMask);

    rc = mount(fsPath, mountPoint, "ntfs", flags, mountData);

    if (rc && errno == EROFS) {
        SLOGE("%s appears to be a read only filesystem - retrying mount RO", fsPath);
        flags |= MS_RDONLY;
        rc = mount(fsPath, mountPoint, "ntfs", flags, mountData);
    }

    return rc;
#else
    int rc;
    int status;
    const char *args[11];
    char mountData[255];

    sprintf(mountData,
            "locale=utf8,uid=%d,gid=%d,fmask=%o,dmask=%o",
            ownerUid, ownerGid, permMask, permMask);

    args[0] = NTFS_3G_PATH;
    args[1] = fsPath;
    args[2] = mountPoint;
    args[3] = "-o";
    args[4] = mountData;
    args[5] = NULL;
    rc = android_fork_execvp(5, (char **)args, &status, false, true);

    if (rc != 0) {
        SLOGE("NTFS check failed due to logwrap error");
        errno = EIO;
        return -1;
    }

    if (!WIFEXITED(status)) {
        SLOGE("NTFS check did not exit properly");
        errno = EIO;
        return -1;
    }

    status = WEXITSTATUS(status);

    return status;
#endif /* HAS_NTFS_3G */
}

int Ntfs::format(const char *fsPath UNUSED, unsigned int numSectors UNUSED) {
#ifndef HAS_NTFS_3G
    SLOGE("Skipping NTFS format\n");
    errno = EIO;
    return -1;
#else
    char * label = NULL;
    int fd;
    int argc;
    const char *args[10];
    int rc;
    int status;

    args[0] = MKNTFS_3G_PATH;
    args[1] = "-f";

    if (numSectors) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%u", numSectors);
        const char *size = tmp;
        args[2] = "-s";
        args[3] = size;
        if (label == NULL) {
            args[4] = fsPath;
            args[5] = NULL;
            argc = 6;
        } else {
            args[4] = "-L";
            args[5] = label;
            args[6] = fsPath;
            args[7] = NULL;
            argc = 7;
        }
    } else {
        if (label == NULL) {
            args[2] = fsPath;
            args[3] = NULL;
            argc = 3;
        } else {
            args[2] = "-L";
            args[3] = label;
            args[4] = fsPath;
            args[5] = NULL;
            argc = 5;
        }
    }
    rc = android_fork_execvp(argc, (char **)args, &status, false, true);

    if (rc != 0) {
        SLOGE("NTFS format failed due to logwrap error");
        errno = EIO;
        return -1;
    }

    if (!WIFEXITED(status)) {
        SLOGE("NTFS format did not exit properly");
        errno = EIO;
        return -1;
    }

    status = WEXITSTATUS(status);

    if (status == 0) {
        SLOGI("NTFS formatted OK");
    } else {
        SLOGE("NTFS Format failed (unknown exit code %d)", rc);
        errno = EIO;
        return -1;
    }
    return 0;
#endif

}
