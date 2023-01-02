#ifndef _VFS_H_
#define _VFS_H_

#include <cstdint>
#include <memory>

#include "CardImage.h"
#include "CardVolume.h"
#include "FileEntry.h"
#include "fatfs/ff.h"

class Vfs {
   public:
    ~Vfs();

    void* Malloc(int size);
    void Free(void* buffer);
    void* Nullptr();

    void AllocateImage(unsigned int blockCount);
    bool MountImage(unsigned int slot);
    void UnmountImage(unsigned int slot);

    int GetPendingImageSize() const;
    int GetSize(unsigned int slot) const;
    void* GetPendingImage() const;
    void* GetImage(unsigned int slot) const;
    void* GetDirtyPages(unsigned int slot) const;

    int RenameFile(const char* from, const char* to);
    int ChmodFile(const char* path, int attr, int mask);
    int StatFile(const char* path);
    const FileEntry& GetEntry();

   private:
    std::unique_ptr<CardImage> cardImages[FF_VOLUMES];
    std::unique_ptr<CardVolume> cardVolumes[FF_VOLUMES];
    std ::unique_ptr<uint8_t[]> pendingImage;
    size_t pendingImageSize{0};

    FATFS fs[FF_VOLUMES];
    FileEntry fileEntry;
};

#endif  // _VFS_H_
