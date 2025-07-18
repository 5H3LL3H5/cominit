// SPDX-License-Identifier: MIT
/**
 * @file meta.c
 * @brief Implementation of partition metadata handling.
 */
#include "meta.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

#include "crypto.h"
#include "keyring.h"
#include "output.h"

#define COMINIT_ROOTFS_FEATURE_NONE "none"  ///< Human-readable string indicating no use of device mapper features.
#define COMINIT_ROOTFS_FEATURE_VERITY "dm-verity"        ///< Human-readable string indicating use of dm-verity.
#define COMINIT_ROOTFS_FEATURE_INTEGRITY "dm-integrity"  ///< Human-readable string indicating use of dm-integrity.
#define COMINIT_ROOTFS_FEATURE_CRYPT "dm-crypt"          ///< Human-readable string indicating use of dm-crypt.

/**
 * Parse a metadata string.
 *
 * Given a null-terminated metadata string formatted as defined in README.md, fill the fields of \a meta accordingly.
 *
 * @param meta     The metadata structure to fill.
 * @param metaStr  The metadata string to parse.
 *
 * @return  0 on success, -1 otherwise
 */
static inline int cominitParseMetadata(cominitRfsMetaData_t *meta, char *metaStr);
/**
 * Generate a device mapper table from dm-verity partition metadata.
 *
 * Given the first device mapper table part of the partition metadata (see README.md), construct
 * cominitRfsMetaData_t::dmTableVerint in \a meta accordingly. Called by cominitParseMetadata() if the partition uses
 * dm-verity.
 *
 * @param meta       The metadata structure to hold the device mapper table string to be generated.
 * @param dmMetaStr  The first device mapper table part from the metadata string.
 *
 * @return  0 on success, -1 otherwise
 */
static inline int cominitGenVerityDmTbl(cominitRfsMetaData_t *meta, char *dmMetaStr);
/**
 * Generate a device mapper table from dm-integrity partition metadata.
 *
 * Given the first device mapper table part of the partition metadata (see README.md), construct
 * cominitRfsMetaData_t::dmTableVerint in \a meta accordingly. Called by cominitParseMetadata() if the partition uses
 * dm-integrity.
 *
 * @param meta       The metadata structure to hold the device mapper table string to be generated.
 * @param dmMetaStr  The first device mapper table part from the metadata string.
 *
 * @return  0 on success, -1 otherwise
 */
static inline int cominitGenIntegrityDmTbl(cominitRfsMetaData_t *meta, char *dmMetaStr);
/**
 * Read an exact amount of Bytes from a file descriptor at an offset.
 *
 * @param buf     The buffer to write the data to, must be at least \a len large.
 * @param fd      The open file descriptor to read from.
 * @param offset  The offset in Bytes in \a fd where to start reading. Only positive values (or 0) are allowed and
 *                \a fd must support seeking if it is >0.
 * @param len     The amount of Bytes to read.
 *
 * @return  0 on success, -1 otherwise
 */
static int cominitBinReadall(uint8_t *buf, int fd, off_t offset, size_t len);

int cominitLoadVerifyMetadata(cominitRfsMetaData_t *meta, const char *keyfile) {
    uint8_t metabuf[COMINIT_PART_META_DATA_SIZE] = {0};
    uint64_t partSize = 0;

    int partFd = open(meta->devicePath, O_RDONLY);
    if (partFd == -1) {
        cominitErrnoPrint("Could not open \'%s\' for reading.", meta->devicePath);
        return -1;
    }

    if (cominitGetPartSize(&partSize, partFd) == -1) {
        cominitErrPrint("Could not determine size of partition \'%s\'.", meta->devicePath);
        close(partFd);
        return -1;
    }

    off_t metadataOffset = (partSize - COMINIT_PART_META_DATA_SIZE);
    if (cominitBinReadall(metabuf, partFd, metadataOffset, sizeof(metabuf)) == -1) {
        cominitErrPrint("Could not read %zu Bytes from offset %ld in \'%s\'.", sizeof(metabuf), metadataOffset,
                        meta->devicePath);
        close(partFd);
        return -1;
    }
    close(partFd);

    size_t metaLen = strnlen((const char *)metabuf, sizeof(metabuf));
    if (metaLen >= sizeof(metabuf) - COMINIT_PART_META_SIG_LENGTH - 1) {
        cominitErrPrint("Could not interpret metadata from \'%s\' at offset %ld. It seems to be corrupted.",
                        meta->devicePath, metadataOffset);
        return -1;
    }

    uint8_t *pSig = metabuf + metaLen + 1;
    if (cominitCryptoVerifySignature(metabuf, metaLen + 1, pSig, keyfile) == -1) {
        cominitErrPrint("Verification of metadata signature on partition \'%s\' failed.", meta->devicePath);
        return -1;
    }

    if (cominitParseMetadata(meta, (char *)metabuf) == -1) {
        cominitErrPrint("Parsing of partition metadata failed.");
        return -1;
    }
    return 0;
}

static int cominitBinReadall(uint8_t *buf, int fd, off_t offset, size_t len) {
    if (buf == NULL) {
        cominitErrPrint("Return buffer must not be NULL.");
        return -1;
    }
    if (offset < 0) {
        cominitErrPrint("Offset must not be negative.");
        return -1;
    }
    if (lseek(fd, offset, SEEK_SET) == -1) {
        cominitErrnoPrint("Could not seek to position %ld in given file descriptor.", offset);
        return -1;
    }
    while (len > 0) {
        ssize_t bytesRead = read(fd, buf, len);
        if (bytesRead < 0) {
            cominitErrnoPrint("Could not read from given file descriptor.");
            return -1;
        }
        len -= bytesRead;
    }
    return 0;
}

int cominitGetPartSize(uint64_t *partSize, int fd) {
    if (partSize == NULL) {
        cominitErrPrint("Return pointer must not be NULL.");
        return -1;
    }
    if (ioctl(fd, (int)BLKGETSIZE64, partSize) == -1) {
        cominitErrnoPrint("Could not determine size of partition.");
        return -1;
    }
    return 0;
}

static inline int cominitParseMetadata(cominitRfsMetaData_t *meta, char *metaStr) {
    char *strtokState = NULL;
    char *runner = metaStr;
    char *dmTblVerintStr = NULL;
    char *dmTblCryptStr = NULL;

    if (meta == NULL || metaStr == NULL) {
        cominitErrPrint("Input parameters must not be NULL.");
        return -1;
    }

    // Check metadata version
    if (strncmp(runner, COMINIT_PART_META_DATA_VERSION, sizeof(COMINIT_PART_META_DATA_VERSION) - 1) != 0) {
        cominitErrPrint("Wrong format of partition metadata.");
        return -1;
    }

    // Find beginning of first device mapper table (verity/integrity)
    dmTblVerintStr = strchr(metaStr, 0xFF);
    if (dmTblVerintStr == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    dmTblVerintStr++[0] = '\0';

    // Find beginning of second device mapper table (crypt)
    dmTblCryptStr = strchr(dmTblVerintStr, 0xFF);
    if (dmTblCryptStr == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    dmTblCryptStr++[0] = '\0';

    // Jump over version number and get fs type
    runner = strtok_r(metaStr, " ", &strtokState);
    if (runner == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    runner = strtok_r(NULL, " ", &strtokState);
    if (runner == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    strncpy(meta->fsType, runner, sizeof(meta->fsType));  // Use of strtok(_r) guarantees \0 at end of token.
    meta->fsType[sizeof(meta->fsType) - 1] = '\0';

    runner = strtok_r(NULL, " ", &strtokState);
    if (runner == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    if (strcmp(runner, "ro") == 0) {
        meta->ro = true;
    } else if (strcmp(runner, "rw") == 0) {
        meta->ro = false;
    } else {
        cominitErrPrint("Unsupported value for filesystem mode: \'%s\'. Must be 'ro' or 'rw'.", runner);
        return -1;
    }

    runner = strtok_r(NULL, " ", &strtokState);
    if (runner == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    if (strcmp(runner, "plain") == 0) {
        meta->crypt = COMINIT_CRYPTOPT_NONE;
    } else if (strcmp(runner, "verity") == 0) {
        meta->crypt = COMINIT_CRYPTOPT_VERITY;
    } else if (strcmp(runner, "integrity") == 0) {
        meta->crypt = COMINIT_CRYPTOPT_INTEGRITY;
    } else if (strcmp(runner, "crypt") == 0) {
        meta->crypt = COMINIT_CRYPTOPT_CRYPT;
    } else if (strcmp(runner, "crypt-integrity") == 0) {
        meta->crypt = COMINIT_CRYPTOPT_CRYPT | COMINIT_CRYPTOPT_INTEGRITY;
    } else if (strcmp(runner, "crypt-verity") == 0) {
        meta->crypt = COMINIT_CRYPTOPT_CRYPT | COMINIT_CRYPTOPT_VERITY;
    } else {
        cominitErrPrint("Unsupported value for crypt type: \'%s\'.", runner);
        return -1;
    }

    if ((meta->crypt ^ (COMINIT_CRYPTOPT_VERITY | COMINIT_CRYPTOPT_INTEGRITY)) == 0) {
        cominitErrPrint("Dm-verity and dm-integrity cannot be combined.");
        return -1;
    }

    cominitInfoPrint("Using rootfs \'%s\' with filesystem \"%s\"%s.", meta->devicePath, meta->fsType,
                     (meta->ro) ? ", read-only" : ", read-write");

    cominitInfoPrint("Rootfs cryptographic features: %s%s%s%s",
                     (meta->crypt == COMINIT_CRYPTOPT_NONE) ? COMINIT_ROOTFS_FEATURE_NONE " " : "",
                     (meta->crypt & COMINIT_CRYPTOPT_VERITY) ? COMINIT_ROOTFS_FEATURE_VERITY " " : "",
                     (meta->crypt & COMINIT_CRYPTOPT_INTEGRITY) ? COMINIT_ROOTFS_FEATURE_INTEGRITY " " : "",
                     (meta->crypt & COMINIT_CRYPTOPT_CRYPT) ? COMINIT_ROOTFS_FEATURE_CRYPT : "");

    // Default case (plain) means two empty strings as device mapper tables.
    meta->dmTableVerint[0] = '\0';
    meta->dmTableCrypt[0] = '\0';

    if (meta->crypt == COMINIT_CRYPTOPT_VERITY && cominitGenVerityDmTbl(meta, dmTblVerintStr) == -1) {
        cominitErrPrint("Could not generate device mapper table for dm-verity rootfs.");
        return -1;
    }

    if (meta->crypt == COMINIT_CRYPTOPT_INTEGRITY && cominitGenIntegrityDmTbl(meta, dmTblVerintStr) == -1) {
        cominitErrPrint("Could not generate device mapper table for dm-integrity rootfs.");
        return -1;
    }
    // TODO: Add case(s) for dm-crypt
    return 0;
}

static inline int cominitGenVerityDmTbl(cominitRfsMetaData_t *meta, char *dmMetaStr) {
    if (meta == NULL || dmMetaStr == NULL) {
        cominitErrPrint("Input parameters must not be NULL.");
        return -1;
    }
    if (meta->crypt != COMINIT_CRYPTOPT_VERITY) {
        cominitErrPrint("This function must only be called for a dm-integrity rootfs.");
        return -1;
    }

    char *verityVersion, *verityTblTail, *runner;
    char *strtokState = NULL;

    // start of first device mapper table / verity version
    verityVersion = strtok_r(dmMetaStr, " ", &strtokState);
    if (verityVersion == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }

    // rest of first device mapper table
    verityTblTail = strtok_r(NULL, "", &strtokState);
    if (verityTblTail == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }

    int n = snprintf(meta->dmTableVerint, sizeof(meta->dmTableVerint), "%s %s %s %s", verityVersion, meta->devicePath,
                     meta->devicePath, verityTblTail);
    if (n < 0) {
        cominitErrnoPrint("Error formatting device mapper table.");
        return -1;
    }
    if ((size_t)n >= sizeof(meta->dmTableVerint)) {
        cominitErrPrint("Device mapper table size too large.");
        return -1;
    }

    // data block size to get dm volume data size
    runner = strtok_r(verityTblTail, " ", &strtokState);
    if (runner == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    meta->dmVerintDataSizeBytes = strtoull(runner, NULL, 10);

    // jump over hash block size
    runner = strtok_r(NULL, " ", &strtokState);
    if (runner == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    // number of data blocks to get dm volume data size
    runner = strtok_r(NULL, " ", &strtokState);
    if (runner == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    meta->dmVerintDataSizeBytes *= strtoull(runner, NULL, 10);

    // jump over hash start block
    runner = strtok_r(NULL, " ", &strtokState);
    if (runner == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }

    // hash algorithm
    runner = strtok_r(NULL, " ", &strtokState);
    if (runner == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    cominitInfoPrint("dm-verity hash algorithm: %s", runner);

    return 0;
}

static inline int cominitGenIntegrityDmTbl(cominitRfsMetaData_t *meta, char *dmMetaStr) {
    if (meta == NULL || dmMetaStr == NULL) {
        cominitErrPrint("Input parameters must not be NULL.");
        return -1;
    }
    if (meta->crypt != COMINIT_CRYPTOPT_INTEGRITY) {
        cominitErrPrint("This function must only be called for a dm-integrity rootfs.");
        return -1;
    }
    char *blocks, *blksize, *numOptStr, *addOpts;
    char procAddOpts[COMINIT_DM_TABLE_SIZE_MAX];
    char *strtokState = NULL;

    // number of data blocks to get dm volume data size
    blocks = strtok_r(dmMetaStr, " ", &strtokState);
    if (blocks == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    meta->dmVerintDataSizeBytes = strtoull(blocks, NULL, 10);

    // data blocksize to get dm volume data size
    blksize = strtok_r(NULL, " ", &strtokState);
    if (blksize == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    meta->dmVerintDataSizeBytes *= strtoull(blksize, NULL, 10);

    // number of additional options from metadata
    numOptStr = strtok_r(NULL, " ", &strtokState);
    if (numOptStr == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    unsigned long numOpts = strtoul(numOptStr, NULL, 10);

    // rest of dm-integrity table comes from metadata
    addOpts = strtok_r(NULL, "", &strtokState);
    if (addOpts == NULL) {
        cominitErrPrint("Unexpected end of metadata string.");
        return -1;
    }
    // now we have to post-process addOpts in case we need to load any keys from the keyring
    strtokState = NULL;
    char *opt = strtok_r(addOpts, " ", &strtokState);
    char *procOpt = procAddOpts;
    *procOpt = '\0';

    const char *keyOpts[] = {"internal_hash:", "journal_crypt:", "journal_mac:"};
    while (opt != NULL) {
        int n = sizeof(procAddOpts) - (procOpt - procAddOpts);
        int ret;
        bool optionalKey = false;
        for (size_t i = 0; i < sizeof(keyOpts) / sizeof(*keyOpts); i++) {
            if (strncmp(opt, keyOpts[i], strlen(keyOpts[i])) == 0) {
                optionalKey = true;
                char hashTmp[32] = {'\0'};
                char *hashPtr = opt + strlen(keyOpts[0]);
                size_t copyLen = strcspn(hashPtr, ":");
                strncpy(hashTmp, hashPtr, (copyLen < sizeof(hashTmp)) ? copyLen : sizeof(hashTmp) - 1);
                hashTmp[sizeof(hashTmp) - 1] = '\0';
                cominitInfoPrint("Dm-integrity algorithm for %s %s", keyOpts[i], hashTmp);
                break;
            }
        }
        char *keyDesc = (optionalKey) ? strstr(opt, "::") : NULL;
        if (keyDesc != NULL && strlen(keyDesc) > 2) {
            uint8_t keyBytes[COMINIT_KEYRING_PAYLOAD_MAX_SIZE];
            char keyHex[2 * COMINIT_KEYRING_PAYLOAD_MAX_SIZE + 1];
            keyDesc[1] = '\0';
            keyDesc += 2;

            cominitInfoPrint("Dm-integrity will use key \'%s\' from Kernel keyring.", keyDesc);
            ssize_t keyLen = cominitGetKey(keyBytes, COMINIT_KEYRING_PAYLOAD_MAX_SIZE, keyDesc);
            if (keyLen < 1) {
                cominitErrPrint("Could not get key payload for key \'%s\'.", keyDesc);
                return -1;
            }
            if (cominitBytesToHex(keyHex, keyBytes, keyLen) == -1) {
                cominitErrPrint("Could not convert key payload to hexadecimal format.");
                return -1;
            }
            ret = snprintf(procOpt, n, "%s%s ", opt, keyHex);
        } else {
            ret = snprintf(procOpt, n, "%s ", opt);
        }

        if (ret < 0) {
            cominitErrPrint("Could not format the final string including the key for option \'%s\'", opt);
            return -1;
        }
        if (ret >= n) {
            cominitErrPrint("Not enough space left in device mapper table.");
            return -1;
        }
        procOpt += ret;
        opt = strtok_r(NULL, " ", &strtokState);
    }

    // Construct device mapper table
    int n = snprintf(meta->dmTableVerint, sizeof(meta->dmTableVerint), "%s 0 - J %lu block_size:%s %s",
                     meta->devicePath, numOpts + 1, blksize, procAddOpts);
    if (n < 0) {
        cominitErrnoPrint("Error formatting device mapper table.");
        return -1;
    }
    if ((size_t)n >= sizeof(meta->dmTableVerint)) {
        cominitErrPrint("Device mapper table size too large.");
        return -1;
    }
    return 0;
}

int cominitBytesToHex(char *dest, const uint8_t *src, size_t n) {
    if (dest == NULL || src == NULL) {
        cominitErrPrint("Input parameters must not be NULL.");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (snprintf(dest + 2 * i, 3, "%02x", src[i]) < 0) {
            cominitErrPrint("Could not write hexadecimal Bytes to destination.");
            return -1;
        }
    }

    return 0;
}
