#include "MSCUF2.h"
#include "FAT.h"

#if CONFIG_ENABLED(DEVICE_USB)

#define UF2_DEFINE_HANDOVER 1
#include "uf2format.h"

#include "CodalCompat.h"
#include "CodalDmesg.h"
#include "CodalDevice.h"

#define NUM_FAT_BLOCKS 65000

#define SECTORS_PER_FAT FAT_SECTORS_PER_FAT(NUM_FAT_BLOCKS)
#define START_FAT0 FAT_START_FAT0(NUM_FAT_BLOCKS)
#define START_FAT1 FAT_START_FAT1(NUM_FAT_BLOCKS)
#define START_ROOTDIR FAT_START_ROOTDIR(NUM_FAT_BLOCKS)
#define START_CLUSTERS FAT_START_CLUSTERS(NUM_FAT_BLOCKS)

#define LOG DMESG

namespace codal
{

static const FAT_BootBlock BootBlock = {
    {0xeb, 0x3c, 0x90},                       // JumpInstruction
    {'C', 'O', 'D', 'A', 'L', ' ', '0', '0'}, // OEMInfo
    512,                                      // SectorSize
    1,                                        // SectorsPerCluster
    FAT_RESERVED_SECTORS,                     // ReservedSectors
    2,                                        // FATCopies
    (FAT_ROOT_DIR_SECTORS * 512 / 32),        // RootDirectoryEntries
    NUM_FAT_BLOCKS - 2,                       // TotalSectors16
    0xF8,                                     // MediaDescriptor
    FAT_SECTORS_PER_FAT(NUM_FAT_BLOCKS),      // SectorsPerFAT
    1,                                        // SectorsPerTrack
    1,                                        // Heads
    0,                                        // HiddenSectors
    0,                                        // TotalSectors32
    0,                                        // PhysicalDriveNum
    0,                                        // Reserved
    0x29,                                     // ExtendedBootSig
    0x00420042,                               // VolumeSerialNumber
    "",                                       // VolumeLabel
    {'F', 'A', 'T', '1', '6', ' ', ' ', ' '}, // FilesystemIdentifier
};

uint32_t MSCUF2::getCapacity()
{
    return NUM_FAT_BLOCKS;
}

static int numClusters(UF2FileEntry *p)
{
    // at least one cluster!
    return (int)(p->size + 512) / 512;
}

static int numDirEntries(UF2FileEntry *p)
{
    return 1 + (strlen(p->filename) + 1 + 12) / 13;
}

static uint8_t fatChecksum(const char *name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i)
        sum = ((sum & 1) << 7) + (sum >> 1) + *name++;
    return sum;
}

// note that ptr might be unaligned
static const char *copyVFatName(const char *ptr, void *dest, int len)
{
    uint8_t *dst = (uint8_t *)dest;

    for (int i = 0; i < len; ++i)
    {
        if (ptr == NULL)
        {
            *dst++ = 0xff;
            *dst++ = 0xff;
        }
        else
        {
            char c = *ptr;
            if (c && strchr("/?<>\\:*|^", c))
                c = '_';
            *dst++ = c;
            *dst++ = 0;
            if (*ptr)
                ptr++;
            else
                ptr = NULL;
        }
    }

    return ptr;
}

static int filechar(int c)
{
    if (!c)
        return 0;
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
           strchr("_-", c);
}

static void copyFsChars(char *dst, const char *src, int len)
{
    for (int i = 0; i < len; ++i)
    {
        if (filechar(*src))
        {
            char c = *src++;
            if ('a' <= c && c <= 'z')
                c -= 32;
            dst[i] = c;
        }
        else
        {
            if (*src == '.')
                src = "";
            if (*src == 0)
                dst[i] = ' ';
            else
                dst[i] = '_';
            while (*src && !filechar(*src))
                src++;
        }
    }
}

void MSCUF2::readDirData(uint8_t *dest, int blkno, uint8_t dirid)
{
    DirEntry *d = (DirEntry *)dest;
    int idx = blkno * -16;

    if (dirid == 0 && idx++ == 0)
    {
        copyFsChars(d->name, volumeLabel(), 11);
        d->attrs = 0x28;
        d++;
    }

    for (auto e = files; e; e = e->next)
    {
        if (e->dirid != dirid)
            continue;
        if (idx >= 16)
            break;

        char fatname[12];

        copyFsChars(fatname, e->filename, 8);
        const char *dot = strchr(e->filename, '.');
        if (!dot)
            dot = ".";
        copyFsChars(fatname + 8, dot + 1, 3);

        {
            char buf[10];
            itoa(e->id, buf + 1);
            buf[0] = '~';
            int idlen = strlen(buf);
            memcpy(fatname + 8 - idlen, buf, idlen);
        }

        fatname[11] = 0;

       // LOG("list: %s [%s] sz:%d st:%d dir:%d", e->filename, fatname, e->size, e->startCluster,
       //     e->dirid);

        int numdirentries = numDirEntries(e);
        for (int i = 0; i < numdirentries; ++i, ++idx)
        {
            if (0 <= idx && idx < 16)
            {
                if (i == numdirentries - 1)
                {
                    memcpy(d->name, fatname, 11);
                    d->attrs = e->attrs;
                    d->size = e->size;
                    d->startCluster = e->startCluster + 2;
                    // timeToFat(e->mtime, &d->updateDate, &d->updateTime);
                    // timeToFat(e->ctime, &d->createDate, &d->createTime);
                }
                else
                {
                    VFatEntry *f = (VFatEntry *)d;
                    int seq = numdirentries - i - 2;
                    f->seqno = seq + 1; // they start at 1
                    if (i == 0)
                        f->seqno |= 0x40;
                    f->attrs = 0x0F;
                    f->type = 0x00;
                    f->checksum = fatChecksum(fatname);
                    f->startCluster = 0;

                    const char *ptr = e->filename + (13 * seq);
                    ptr = copyVFatName(ptr, f->name0, 5);
                    ptr = copyVFatName(ptr, f->name1, 6);
                    ptr = copyVFatName(ptr, f->name2, 2);
                }
                d++;
            }
        }
    }
}

#define WRITE_ENT(v)                                                                               \
    do                                                                                             \
    {                                                                                              \
        if (skip++ >= 0)                                                                           \
            *dest++ = v;                                                                           \
        if (skip >= 256)                                                                           \
            return;                                                                                \
        cl++;                                                                                      \
    } while (0)

void MSCUF2::buildBlock(uint32_t block_no, uint8_t *data)
{
    memset(data, 0, 512);
    uint32_t sectionIdx = block_no;

    if (block_no == 0)
    {
        memcpy(data, &BootBlock, sizeof(BootBlock));
        FAT_BootBlock *bb = (FAT_BootBlock *)data;
        copyFsChars(bb->VolumeLabel, volumeLabel(), 11);
        data[510] = 0x55;
        data[511] = 0xaa;
    }
    else if (block_no < START_ROOTDIR)
    {
        sectionIdx -= START_FAT0;
        // logval("sidx", sectionIdx);
        if (sectionIdx >= SECTORS_PER_FAT)
            sectionIdx -= SECTORS_PER_FAT;

        int cl = 0;
        int skip = -(sectionIdx * 256);
        auto dest = (uint16_t *)data;

        WRITE_ENT(0xfff0);
        WRITE_ENT(0xffff);
        for (auto p = files; p; p = p->next)
        {
            int n = numClusters(p) - 1;
            for (int i = 0; i < n; i++)
                WRITE_ENT(cl + 1);
            WRITE_ENT(0xffff);
        }
    }
    else if (block_no < START_CLUSTERS)
    {
        sectionIdx -= START_ROOTDIR;
        readDirData(data, sectionIdx, 0);
    }
    else
    {
        sectionIdx -= START_CLUSTERS;
        for (auto p = files; p; p = p->next)
        {
            if (p->startCluster <= sectionIdx && (int)sectionIdx < p->startCluster + numClusters(p))
            {
                sectionIdx -= p->startCluster;
                if (p->attrs & 0x10)
                    readDirData(data, sectionIdx, p->id);
                else
                    readFileBlock(p->id, sectionIdx, (char *)data);
                break;
            }
        }
    }
}

void MSCUF2::readBlocks(int blockAddr, int numBlocks)
{
    uint8_t buf[512];

    finalizeFiles();

    while (numBlocks--)
    {
        buildBlock(blockAddr, buf);
        writeBulk(buf, 512);
        blockAddr++;
    }

    finishReadWrite();
}

void MSCUF2::writeBlocks(int blockAddr, int numBlocks)
{
    uint8_t buf[512];

    while (numBlocks--)
    {
        readBulk(buf, sizeof(buf));
        if (is_uf2_block(buf))
        {
            UF2_Block *b = (UF2_Block *)buf;
            if (!(b->flags & UF2_FLAG_NOFLASH))
            {
                check_uf2_handover(buf, numBlocks, in->ep & 0xf, out->ep & 0xf, cbwTag());
            }
        }
        blockAddr++;
    }

    finishReadWrite();
}

MSCUF2::MSCUF2()
{
    files = NULL;
}

bool MSCUF2::filesFinalized()
{
    return files && files->startCluster != 0xffff;
}

void MSCUF2::finalizeFiles()
{
    if (files == NULL || filesFinalized())
        return;

    UF2FileEntry *regFiles = NULL, *dirs = NULL;

    while (files)
    {
        auto n = files->next;
        if (files->attrs & 0x10)
        {
            files->next = dirs;
            dirs = files;
        }
        else
        {
            files->next = regFiles;
            regFiles = files;
        }
        files = n;
    }

    files = regFiles;

    int cl = 0;
    for (auto p = files; p; p = p->next)
    {
        p->startCluster = cl;
        cl += numClusters(p);
        if (p->dirid)
        {
            for (auto d = dirs; d; d = d->next)
            {
                if (d->id == p->dirid)
                {
                    d->size += sizeof(DirEntry) * numDirEntries(p);
                    break;
                }
            }
        }
        if (p->next == NULL)
        {
            p->next = dirs;
            dirs = NULL;
        }
    }
}

UF2FileEntry *MSCUF2::addFileCore(uint16_t id, const char *filename, uint32_t size)
{
    if (filesFinalized())
        target_panic(DEVICE_USB_ERROR);

    auto f = (UF2FileEntry *)malloc(sizeof(UF2FileEntry) + strlen(filename) + 1);
    memset(f, 0, sizeof(UF2FileEntry));
    strcpy(f->filename, filename);
    f->size = size;
    f->id = id;
    f->next = files;
    f->startCluster = 0xffff;
    files = f;
    return f;
}

void MSCUF2::addFile(uint16_t id, const char *filename, uint32_t size, uint8_t dirid)
{
    auto f = addFileCore(id, filename, size);
    f->dirid = dirid;
}

void MSCUF2::addDirectory(uint8_t id, const char *dirname)
{
    auto f = addFileCore(id, dirname, 0);
    f->attrs = 0x10;
}

void MSCUF2::addFiles()
{
    addFile(1, "info_uf2.txt", strlen(uf2_info()));
    addFile(2, "index.html", strlen(indexHTML()));
    addFile(3, "current.uf2", internalFlashSize() * 2);
#if DEVICE_DMESG_BUFFER_SIZE > 0
    addFile(4, "dmesg.txt", DEVICE_DMESG_BUFFER_SIZE);
#endif
}

void MSCUF2::readFileBlock(uint16_t id, int blockAddr, char *dst)
{
    uint32_t addr;

    switch (id)
    {
    case 1:
        strcpy(dst, uf2_info());
        break;
    case 2:
        strcpy(dst, indexHTML());
        break;
    case 3:
        addr = blockAddr * 256;
        if (addr < internalFlashSize())
        {
            UF2_Block *bl = (UF2_Block *)dst;
            bl->magicStart0 = UF2_MAGIC_START0;
            bl->magicStart1 = UF2_MAGIC_START1;
            bl->magicEnd = UF2_MAGIC_END;
            bl->blockNo = blockAddr;
            bl->numBlocks = internalFlashSize() / 256;
            bl->targetAddr = addr;
            bl->payloadSize = 256;
            memcpy(bl->data, (void *)addr, bl->payloadSize);
        }
        break;
#if DEVICE_DMESG_BUFFER_SIZE > 0
    case 4:
        addr = blockAddr * 512;
        for (uint32_t i = 0; i < 512; ++i)
        {
            if (addr < codalLogStore.ptr)
                *dst++ = codalLogStore.buffer[addr++];
            else
                *dst++ = '\n';
        }
        break;
#endif
    }
}
}

#endif
