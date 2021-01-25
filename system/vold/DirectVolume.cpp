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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fnmatch.h>

#include <linux/kdev_t.h>

#define LOG_TAG "DirectVolume"

#include <cutils/log.h>
#include <sysutils/NetlinkEvent.h>

#include "DirectVolume.h"
#include "VolumeManager.h"
#include "ResponseCode.h"
#include "cryptfs.h"

#define PARTITION_DEBUG

PathInfo::PathInfo(const char *p) {
    warned = false;
    pattern = strdup(p);
    
    if (!strchr(pattern, '*')) {
        patternType = prefix;
    } else {
        patternType = wildcard;
    }
}

PathInfo::~PathInfo() {
    free(pattern);
}

bool PathInfo::match(const char *path) {
    switch (patternType) {
        case prefix: {
            bool ret = (strncmp(path, pattern, strlen(pattern)) == 0);
            if (!warned && ret && (strlen(pattern) != strlen(path))) {
                SLOGW("Deprecated implied prefix pattern detected, please use '%s*' instead", pattern);
                warned = true;
            }
            return ret;
        }
        case wildcard:
            return fnmatch(pattern, path, 0) == 0;
    }
    SLOGE("Bad matching type");
    return false;
}

DirectVolume::DirectVolume(VolumeManager *vm, const fstab_rec* rec, int flags) :
    Volume(vm, rec, flags) {
    SLOGD("new DirectVolume %s ", rec->label);
    
    mPaths = new PathCollection();
    for (int i = 0; i < MAX_PARTITIONS; i++)
        mPartMinors[i] = -1;
    mDiskMajor = -1;
    mDiskMinor = -1;
    mDiskNumParts = 0;
    mIsDecrypted = 0;
    mDevPath = NULL;
    
    if (strcmp(rec->mount_point, "auto") != 0) {
        ALOGE("Vold managed volumes must have auto mount point; ignoring %s",
              rec->mount_point);
    }
    
    char mount[PATH_MAX];
    
    snprintf(mount, PATH_MAX, "%s/%s", Volume::MEDIA_DIR, rec->label);
    mMountpoint = strdup(mount);
    snprintf(mount, PATH_MAX, "%s/%s", Volume::FUSE_DIR, rec->label);
    mFuseMountpoint = strdup(mount);
    
    setState(Volume::State_NoMedia);
}

DirectVolume::~DirectVolume() {
    PathCollection::iterator it;
    
    for (it = mPaths->begin(); it != mPaths->end(); ++it)
        delete *it;
    delete mPaths;
    
    free(mDevPath);
    mDevPath = NULL;
}

int DirectVolume::addPath(const char *path) {
    mPaths->push_back(new PathInfo(path));
    return 0;
}

dev_t DirectVolume::getDiskDevice() {
    return MKDEV(mDiskMajor, mDiskMinor);
}

dev_t DirectVolume::getShareDevice() {
    if (mPartIdx != -1) {
        return MKDEV(mDiskMajor, mPartIdx);
    } else {
        return MKDEV(mDiskMajor, mDiskMinor);
    }
}

void DirectVolume::handleVolumeShared() {
    setState(Volume::State_Shared);
}

void DirectVolume::handleVolumeUnshared() {
    setState(Volume::State_Idle);
}

int DirectVolume::handleBlockEvent(NetlinkEvent *evt) {
    const char *dp = evt->findParam("DEVPATH");
    int action = evt->getAction();
    const char* actionStr = action == NetlinkEvent::NlActionAdd ? "Add" : (action == NetlinkEvent::NlActionRemove ? "Remove" : (action == NetlinkEvent::NlActionChange ? "Change" : "UNKNOWN!!!"));
    SLOGD("DirectVolume %s state %s handleBlockEvent %s mDevPath %s devpath %s", getLabel(), getStateStr(), actionStr, mDevPath, dp);
    
    
    PathCollection::iterator  it;
    for (it = mPaths->begin(); it != mPaths->end(); ++it) {
        if ((*it)->match(dp)) {
            /* We can handle this disk */
            const char *devtype = evt->findParam("DEVTYPE");
            
            if (mDevPath != NULL) {
                if (strcmp(dp, mDevPath)) {
                    errno = ENODEV;
                    return -1;
                }
            }
            
            if (action == NetlinkEvent::NlActionAdd) {
                SLOGI("NlActionAdd");
                int st = getState();
                if (!(st == Volume::State_NoMedia || st == Volume::State_Pending || st == Volume::State_Idle)) {
                    errno = ENODEV;
                    return -1;
                }
                
                SLOGD("DirectVolume %s NlActionAdd devtype %s @ %s", getLabel(), devtype, dp);
                
                
                int major = atoi(evt->findParam("MAJOR"));
                int minor = atoi(evt->findParam("MINOR"));
                char nodepath[255];
                
                snprintf(nodepath,
                         sizeof(nodepath), "/dev/block/vold/%d:%d",
                         major, minor);
                if (createDeviceNode(nodepath, major, minor)) {
                    SLOGE("Error making device node '%s' (%s)", nodepath,
                          strerror(errno));
                }
                if (!strcmp(devtype, "disk")) {
                    handleDiskAdded(dp, evt);
#ifdef MOUNT_MULTI_PART
                    return -1;
#endif
                }
                handlePartitionAdded(dp, evt);
                
                free(mDevPath);
                mDevPath = strdup(dp);
                
                /* Send notification iff disk is ready (ie all partitions found) */
                if (getState() == Volume::State_Idle) {
                    char msg[255];
                    
                    snprintf(msg, sizeof(msg),
                             "Volume %s %s disk inserted (%d:%d)", getLabel(),
                             getFuseMountpoint(), mDiskMajor, mDiskMinor);
                    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskInserted,
                                                         msg, false);
                }
            } else if (mDevPath == NULL) {
                errno = ENODEV;
                return -1;
            }
            
            if (action == NetlinkEvent::NlActionRemove) {
                free(mDevPath);
                mDevPath = NULL;
                
                SLOGD("DirectVolume %s NlActionRemove @ %s", getLabel(), dp);
                
                handlePartitionRemoved(dp, evt);
                handleDiskRemoved(dp, evt);
            } else if (action == NetlinkEvent::NlActionChange) {
                SLOGD("DirectVolume %s NlActionChange", getLabel());
                if (!strcmp(devtype, "disk")) {
                    handleDiskChanged(dp, evt);
                } else {
                    handlePartitionChanged(dp, evt);
                }
            }
            
            return 0;
        }
    }
    errno = ENODEV;
    return -1;
}

void DirectVolume::handleDiskAdded(const char * devpath, NetlinkEvent *evt) {
    mDiskMajor = atoi(evt->findParam("MAJOR"));
    mDiskMinor = atoi(evt->findParam("MINOR"));
    const char *part_str = evt->findParam("NPARTS");
    
    SLOGI("dev %s handleDiskAdded major %d minor %d part_num %s", devpath, mDiskMajor, mDiskMinor, part_str);
    
    if (part_str) {
        mDiskNumParts = atoi(part_str);
#ifdef MOUNT_MULTI_PART
        if (mDiskNumParts > 1) {
            mDiskNumParts = 1;
        }
#endif
    } else {
        SLOGW("Kernel block uevent missing 'NPARTS'");
        mDiskNumParts = 1;
    }
    
    for (int i = 0; i < MAX_PARTITIONS; i++)
        mPartMinors[i] = -1;
        
    if (mDiskNumParts == 0) {
#ifdef PARTITION_DEBUG
        SLOGD("Dv::diskIns - No partitions - good to go son!");
#endif
        setState(Volume::State_Idle);
    } else {
#ifdef PARTITION_DEBUG
        SLOGD("Dv::diskIns - waiting for partitions");
#endif
        setState(Volume::State_Pending);
    }
}

void DirectVolume::handlePartitionAdded(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    const char *part_str = evt->findParam("PARTN");
    
    SLOGI("%s handlePartitionAdded dev %s mDiskMajor %d major %d minor %d part_num %s", getLabel(), devpath, mDiskMajor, major, minor, part_str);
    
    int part_num;
    if (part_str) {
        part_num = atoi(part_str);
#ifdef MOUNT_MULTI_PART
        if (part_num > 1) {
            part_num = 1;
        }
#endif
    } else {
        SLOGW("Kernel block uevent missing 'PARTN'");
        part_num = 1;
    }
    
#ifndef MOUNT_MULTI_PART
    if (part_num > MAX_PARTITIONS || part_num < 1) {
        SLOGE("Invalid 'PARTN' value");
        return;
    }
    
    if (part_num > mDiskNumParts) {
        mDiskNumParts = part_num;
    }
#endif
    
    if (major != mDiskMajor) {
        SLOGE("Partition '%s' has a different major than its disk!", devpath);
        return;
    }
    
    mPartMinors[part_num - 1] = minor;
    
#ifdef PARTITION_DEBUG
    SLOGD("Dv:partAdd: Got all partitions - ready to rock!");
#endif
    if (getState() != Volume::State_Formatting) {
        setState(Volume::State_Idle);
#ifdef PATCH_FOR_SLSIAP
        if (mRetryMount == true || !strncmp(getLabel(), "usbdisk", 7)) {
#else
        SLOGD("Dv:partAdd: mRetryMount %d", mRetryMount);
        if (mRetryMount == true) {
#endif
            mRetryMount = false;
            mountVol();
        }
    }
}

void DirectVolume::handleDiskChanged(const char * devpath,
                                     NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    
    if ((major != mDiskMajor) || (minor != mDiskMinor)) {
        return;
    }
    
    SLOGI("Volume %s disk %s has changed", getLabel(), devpath);
    const char *tmp = evt->findParam("NPARTS");
    if (tmp) {
        mDiskNumParts = atoi(tmp);
#ifdef MOUNT_MULTI_PART
        if (mDiskNumParts > 1) {
            mDiskNumParts = 1;
        }
#endif
    } else {
        SLOGW("Kernel block uevent missing 'NPARTS'");
        mDiskNumParts = 1;
    }
    
    for (int i = 0; i < MAX_PARTITIONS; i++)
        mPartMinors[i] = -1;
        
    if (getState() != Volume::State_Formatting) {
        if (mDiskNumParts == 0) {
            setState(Volume::State_Idle);
        } else {
            setState(Volume::State_Pending);
        }
    }
}

void DirectVolume::handlePartitionChanged(const char * devpath,
        NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    SLOGD("Volume %s devpath %s @ %s partition %d:%d changed\n", getLabel(), devpath, getMountpoint(), major, minor);
}

void DirectVolume::handleDiskRemoved(const char * /*devpath*/,
                                     NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    char msg[255];
    bool enabled;
    
    if (mVm->shareEnabled(getLabel(), "ums", &enabled) == 0 && enabled) {
        mVm->unshareVolume(getLabel(), "ums");
    }
    
    SLOGD("handleDiskRemoved Volume %s %s disk %d:%d removed\n", getLabel(), getMountpoint(), major, minor);
    snprintf(msg, sizeof(msg), "Volume %s %s disk removed (%d:%d)",
             getLabel(), getFuseMountpoint(), major, minor);
    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskRemoved,
                                         msg, false);
    setState(Volume::State_NoMedia);
}

void DirectVolume::handlePartitionRemoved(const char * /*devpath*/,
        NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    char msg[255];
    int state;
    
    SLOGD("handlePartitionRemoved Volume %s %s partition %d:%d removed\n", getLabel(), getMountpoint(), major, minor);
    
    /*
     * The framework doesn't need to get notified of
     * partition removal unless it's mounted. Otherwise
     * the removal notification will be sent on the Disk
     * itself
     */
    state = getState();
    if (state != Volume::State_Mounted && state != Volume::State_Shared) {
        return;
    }
    
    if ((dev_t) MKDEV(major, minor) == mCurrentlyMountedKdev) {
        /*
         * Yikes, our mounted partition is going away!
         */
        
        bool providesAsec = (getFlags() & VOL_PROVIDES_ASEC) != 0;
        if (providesAsec && mVm->cleanupAsec(this, true)) {
            SLOGE("Failed to cleanup ASEC - unmount will probably fail!");
        }
        
        snprintf(msg, sizeof(msg), "Volume %s %s bad removal (%d:%d)",
                 getLabel(), getFuseMountpoint(), major, minor);
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeBadRemoval,
                                             msg, false);
                                             
        if (Volume::unmountVol(true, false)) {
            SLOGE("Failed to unmount volume on bad removal (%s)",
                  strerror(errno));
            // XXX: At this point we're screwed for now
        } else {
            SLOGD("Crisis averted");
        }
    } else if (state == Volume::State_Shared) {
        /* removed during mass storage */
        snprintf(msg, sizeof(msg), "Volume %s bad removal (%d:%d)",
                 getLabel(), major, minor);
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeBadRemoval,
                                             msg, false);
                                             
        if (mVm->unshareVolume(getLabel(), "ums")) {
            SLOGE("Failed to unshare volume on bad removal (%s)",
                  strerror(errno));
        } else {
            SLOGD("Crisis averted");
        }
    }
}

/*
 * Called from base to get a list of devicenodes for mounting
 */
int DirectVolume::getDeviceNodes(dev_t *devs, int max) {
    SLOGI("%s getDeviceNodes mPartIdx %d mDiskNumParts %d mDiskMajor %d mPartMinors %d", getLabel(), mPartIdx, mDiskNumParts, mDiskMajor, mPartMinors[0]);
    if (mPartIdx == -1) {
        // If the disk has no partitions, try the disk itself
        if (!mDiskNumParts) {
            devs[0] = MKDEV(mDiskMajor, mDiskMinor);
            return 1;
        }
        
        int i;
        for (i = 0; i < mDiskNumParts && i < max; i++) {
            SLOGI("getDeviceNodes MKDEV mDiskMajor %d mPartMinors %d", mDiskMajor, mPartMinors[i]);
            devs[i] = MKDEV(mDiskMajor, mPartMinors[i]);
        }
        return mDiskNumParts;
    }
    devs[0] = MKDEV(mDiskMajor, mPartMinors[mPartIdx - 1]);
    return 1;
}

/*
 * Called from base to update device info,
 * e.g. When setting up an dm-crypt mapping for the sd card.
 */
int DirectVolume::updateDeviceInfo(char *new_path, int new_major, int new_minor) {
    PathCollection::iterator it;
    
    if (mPartIdx == -1) {
        SLOGE("Can only change device info on a partition\n");
        return -1;
    }
    
    /*
     * This is to change the sysfs path associated with a partition, in particular,
     * for an internal SD card partition that is encrypted.  Thus, the list is
     * expected to be only 1 entry long.  Check that and bail if not.
     */
    if (mPaths->size() != 1) {
        SLOGE("Cannot change path if there are more than one for a volume\n");
        return -1;
    }
    
    it = mPaths->begin();
    delete *it; /* Free the string storage */
    mPaths->erase(it); /* Remove it from the list */
    addPath(new_path); /* Put the new path on the list */
    
    /* Save away original info so we can restore it when doing factory reset.
     * Then, when doing the format, it will format the original device in the
     * clear, otherwise it just formats the encrypted device which is not
     * readable when the device boots unencrypted after the reset.
     */
    mOrigDiskMajor = mDiskMajor;
    mOrigDiskMinor = mDiskMinor;
    mOrigPartIdx = mPartIdx;
    memcpy(mOrigPartMinors, mPartMinors, sizeof(mPartMinors));
    
    mDiskMajor = new_major;
    mDiskMinor = new_minor;
    /* Ugh, virual block devices don't use minor 0 for whole disk and minor > 0 for
     * partition number.  They don't have partitions, they are just virtual block
     * devices, and minor number 0 is the first dm-crypt device.  Luckily the first
     * dm-crypt device is for the userdata partition, which gets minor number 0, and
     * it is not managed by vold.  So the next device is minor number one, which we
     * will call partition one.
     */
    mPartIdx = new_minor;
    mPartMinors[new_minor - 1] = new_minor;
    
    mIsDecrypted = 1;
    
    return 0;
}

/*
 * Called from base to revert device info to the way it was before a
 * crypto mapping was created for it.
 */
void DirectVolume::revertDeviceInfo(void) {
    if (mIsDecrypted) {
        mDiskMajor = mOrigDiskMajor;
        mDiskMinor = mOrigDiskMinor;
        mPartIdx = mOrigPartIdx;
        memcpy(mPartMinors, mOrigPartMinors, sizeof(mPartMinors));
        
        mIsDecrypted = 0;
    }
    
    return;
}

/*
 * Called from base to give cryptfs all the info it needs to encrypt eligible volumes
 */
int DirectVolume::getVolInfo(struct volume_info *v) {
    strcpy(v->label, mLabel);
    strcpy(v->mnt_point, mMountpoint);
    v->flags = getFlags();
    /* Other fields of struct volume_info are filled in by the caller or cryptfs.c */
    
    return 0;
}
